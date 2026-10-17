<script setup>
import { onMounted, onUnmounted, ref, nextTick, watch, reactive, computed } from 'vue';
import { useRouter, onBeforeRouteLeave } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState, normalizeProjectPath } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';
import { projectLauncherService, cancelPendingProjectOpen, getProjectSelectionVersion } from '@/services/projectLauncherService.js';
import { trackWorldSessionWork, notifyWorldError, registerWorldSessionSave } from '@/services/worldSessionLifecycle.js';
import { createStoryNavigationController, NAVIGATION_KEY } from '../../../../game/frontend/storyNavigation.mjs';
// 剧情 HUD 样式：复刻 ui2.html《自然荒野 · 战术协同系统 HUD》
import './StoryWorld.hud.css';
// 中央「机体投影」用的人物图（537×817，比例与投影仓一致，不会裁切）
import HERO_FIGURE from '@/assets/hero-figure.png';
// 行囊（Tab 背包）样式：复刻 bag2.html《自然荒野 · 战术编成与收纳库》，收在 .story-bag 之下
import './StoryWorld.bag.css';

import { ensureStoryCharacters, syncPlacementActors } from '../../../../game/frontend/storyActors.mjs';
import { createPlayerController, VIEW_LABELS } from '../../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../../game/frontend/playerSave.mjs';
import { createStoryGameplay, worldBounds, distanceToBounds, pickupDistance,
  contextualInteraction } from '../../../../game/frontend/storyGameplay.mjs';
import { STORY_CHARACTERS, PROPHET_GUID, resolveStoryAssetPath,
  sceneSnapshot } from '../../../../game/frontend/storyCharacters.mjs';
import { MAX_PLACEMENTS, PLACED_BLOCK_REACH, PLACEMENT_DISTANCE, blockSpawnTransform, cycleSlot, findProp,
  hotbarSlots as hotbarSlotsFor, nearestPlacedBlock, nextPlacementIndex, placementGuid,
  removePlacement, sanitizePlacements } from '../../../../game/frontend/storyProps.mjs';
import { PROPHET_INTERACTION, prophetAvailable, prophetDistance,
  normalizeDialogue } from '../../../../game/frontend/prophetDialogue.mjs';
import { ensureStoryCube, cubeTransformOf, playerFacingYaw } from '../../../../game/frontend/storyCube.mjs';
import { ensureStoryMap } from '../../../../game/frontend/storyMap.mjs';
import { heightAt } from '../../../../game/frontend/storyMapHeight.mjs';
import { loadStoryMapHeight, mapHeightUrl } from '../../../../game/frontend/storyMapHeightLoader.mjs';
import { collectSwayTargets, createWorldRuleRunner,
  findFragment } from '../../../../game/frontend/storyWorldRules.mjs';
import { ADJUST_ACTIONS, FRAGMENT_ADJUST_ACTIONS, PROPHET_SCREENS, adjustFeedback,
  adjustFragmentRule, fragmentAdjustPanel, fragmentPanel,
  nextObjectTransform, objectLabel, prophetKeyAction, selectableObjects,
  toggleFragment } from '../../../../game/frontend/storyProphetActions.mjs';

const router = useRouter();
const surface = ref(null);
const inventoryOpen = ref(false);
const inventoryPanel = ref(null);
const selectedSlot = ref(0);
const navigationPending = ref(false);
const initializing = ref(true);
const cooldownTick = ref(0);
let cooldownTimer = null;
const gameplayState = ref(null);
const bossNearby = ref(false);
const canPickup = ref(false);
const feedback = ref('');
const gameplayError = ref('');
const toastText = ref('');
const fxLayer = ref(null);
const actionBusy = ref(false);
let feedbackTimer = null;
let toastTimer = null;
let bossActor = null;
let bossBounds = null;
let sceneId = null;
let visualSignature = null;
let lastHitHp = null;
let lastHitRage = null;
let lastHitRevision = null;
const revision = worldModeState.revision;
const projectPath = worldModeState.projectPath;
// 世界角色镜像：gameplay 是普通对象，computed 追踪不到，需显式 ref 才能驱动 HUD
const roleRef = ref(null);
// 视角切换用的重算信号（相机控制器同样不是响应式对象）
const viewTick = ref(0);
// 先知交互
const dialogueOpen = ref(false);
const dialogueLine = ref(0);
// F 的唯一裁决：提示（controlHints）与按键（interact）都读它，两者不可能各说各话。
const fKeyAction = ref(contextualInteraction());
const dialogue = normalizeDialogue(null);
let prophetActor = null;
// 先知旁的方块与世界规则（李淳珺任务）：方块是玩家可调整的场景道具，不是角色。
let playerActor = null;
let cubeActor = null;
let cubeTransform = null;
let dialoguePanel = null;
// 小世界陈列：布局由 placements 驱动，物品栏（下方）直接在前方落物
const placements = ref([]);
// 物品栏选中的格子：只在世界里保留，切换世界会重建组件，因此无需持久化。
const hotbarIndex = ref(0);
let visualPlacements = null;
let disposed = false;
let cameraReady = false;
let playerSave = null;
let savingPlayer = false;
let initialization = Promise.resolve();
let exitPending = null;
let resizeObserver = null;
let leaving = false;
let dpiQuery = null;
let focused = document.hasFocus();
const current = () => !disposed && !leaving && worldModeState.status === 'ready'
  && worldModeState.mode === 'story' && worldModeState.revision === revision;
/**
 * 物体的地面线：与角色、玩家控制器同一个来源——烘好的地图高度网格。
 * 摆放、掉落和先知方块都读这一个函数（`heightAt` 在没有网格时返回 null，各调用点便退回
 * 它们原来的常量高度），因此世界上所有东西站的是同一条地面，而不是三套各自算的 y。
 */
const groundYAt = (x, z) => heightAt(x, z);
const camera = createStoryCameraController({
  createControls: createPlayerController,
  // 玩家的地面线：地图是 phantom（引擎不为它建碰撞体），脚下多高只能由烘好的高度网格回答。
  // `heightAt` 在没有网格时返回 null，控制器便退回原来的固定地板——这条绑定必须在
  // createStoryCameraController 里继续往下传，否则控制器收不到它（这正是之前的原因）。
  getGroundY: groundYAt,
  onError: error => {
    if (current()) gameplayError.value = error.message;
    notifyWorldError(error);
  },
  getBridge: () => current() ? window.coronaBridge : null,
  isCurrent: current,
  isInputLocked: () => initializing.value || !focused || document.hidden || navigation.busy || savingPlayer || inventoryOpen.value || dialogueOpen.value,
  onPlayerChanged: updateProximity,
  getRect: () => surface.value?.getBoundingClientRect(),
  getPixelRatio: () => window.devicePixelRatio,
});
const unwrap = (response) => response?.data ?? response;
function updateProximity() {
  if (!current()) return;
  const data = gameplayState.value, player = camera.snapshotPlayer();
  bossNearby.value = Boolean(data?.role === 'main' && data.state.boss.hp > 0
    && distanceToBounds(player?.position, bossBounds) <= data.config.bossBarRadius);
  const reach = reachableDrop(data, player);
  canPickup.value = Boolean(reach.drop) && reach.distance <= (data?.config?.pickupRange ?? 0);
  // 提示与 F 键读的是同一个函数、同一份现算的距离；缓存只用于渲染这一行提示。
  fKeyAction.value = interactionFor(data, player);
}
// 今天还剩下的世界碎片：只有主世界会掉落，被拾走之后就不再是候选。
function reachableDrop(data, player) {
  const drop = data?.role === 'main' ? data.state.drop : null;
  return { drop, distance: drop && !drop.collected ? pickupDistance(player, drop) : Infinity };
}
// 可被取回的方块：只有小世界有陈列物。主世界读到的是空布局，这里再按角色收一次口，
// 免得一份还没换掉的旧布局在主世界里悄悄变成候选。
function reachableBlock(data, player) {
  const found = data?.role === 'child'
    ? nearestPlacedBlock(player, placements.value, PLACED_BLOCK_REACH) : null;
  return { placement: found, distance: found ? found.distance : Infinity };
}
/**
 * F 的唯一裁决，现算现用：提示（controlHints）与按键（interact）都调用它，
 * 因此两者不可能各说各话——也不会拿上一个世界的旧结论去回答这个世界的 F。
 */
function interactionFor(data, player = camera.snapshotPlayer()) {
  const { drop, distance } = reachableDrop(data, player);
  const block = reachableBlock(data, player);
  return contextualInteraction({
    prophetDistance: data?.role === 'child' && prophetAvailable(data.role)
      ? prophetDistance(player, prophetBounds()) : Infinity,
    pickupDistance: drop ? distance : Infinity,
    blockDistance: block.distance,
    range: PROPHET_INTERACTION.range,
    // 没有配置就不许拾取（0 射程），而不是悄悄放宽成无限。
    pickupRange: data?.config?.pickupRange ?? 0,
    // 取回的射程由陈列几何自己给出：摆放与取回是同一对数字。
    blockRange: PLACED_BLOCK_REACH,
  });
}
// F 按下时现算一次：动作永远按玩家此刻真正站的地方判定，而不是最近一次渲染的结论。
function currentInteraction() {
  return current() ? interactionFor(gameplayState.value) : contextualInteraction();
}
function showFeedback(message) {
  if (!current()) return;
  feedback.value = message;
  clearTimeout(feedbackTimer);
  feedbackTimer = setTimeout(() => { feedback.value = ''; }, 2200);
}
// 提示条：只做显示，不改动任何玩法/存档流程
function showToast(message) {
  if (!message) return;
  toastText.value = message;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { toastText.value = ''; }, 1600);
}
// 受击飘字：只做显示。按 revision 判断本帧是否发生新的命中，
// 用怒气下降确认“打中但未掉血”的技能，再取掉血差值作为数字。
function spawnDamageText(value, kind) {
  const layer = fxLayer.value;
  if (!layer) return;
  const el = document.createElement('span');
  el.className = kind === 'miss' ? 'dmg dmg--miss' : 'dmg';
  el.style.left = `${(34 + Math.random() * 32).toFixed(1)}%`;
  el.style.top = `${(40 + Math.random() * 10).toFixed(1)}%`;
  el.style.setProperty('--dx', `${Math.round(Math.random() * 46 - 23)}px`);
  el.style.setProperty('--rise', `${Math.round(46 + Math.random() * 36)}px`);
  el.style.setProperty('--rot', `${(Math.random() * 12 - 6).toFixed(1)}deg`);
  el.textContent = kind === 'miss' ? '未中' : String(value);
  layer.appendChild(el);
  el.addEventListener('animationend', () => el.remove(), { once: true });
  setTimeout(() => el.remove(), 1400);
}
function syncCombatFeedback(data) {
  if (!data) return;
  const { rage, revision: stateRevision } = data.state;
  const hp = data.state.boss.hp;
  const skillsUsed = lastHitRage !== null && lastHitRage > rage;
  if (stateRevision !== lastHitRevision) {
    const delta = lastHitHp === null ? 0 : Math.max(0, lastHitHp - hp);
    if (delta > 0) spawnDamageText(delta, 'hit');
    else if (skillsUsed) spawnDamageText(0, 'miss');
  }
  lastHitRevision = stateRevision;
  lastHitHp = hp;
  lastHitRage = rage;
}
watch(feedback, value => showToast(value));
watch(() => gameplayState.value?.state.revision, () => syncCombatFeedback(gameplayState.value));
// 拾得世界碎片：行囊的小世界一览随即多出第二界
watch(() => gameplayState.value?.state.inventory.worldFragment, (count, previous) => {
  if ((count || 0) > (previous || 0)) showToast('拾得 世界碎片 · 新界已辟');
});
// Save finalizers may outlive the component; validate the actual native source,
// not the new route's revision, before reconciling any old-world actor.
async function assertSource() {
  const info = unwrap(await editorApi.projectSettings.getActiveProjectInfo());
  if (info?.mode !== 'story' || normalizeProjectPath(info.project_path) !== normalizeProjectPath(projectPath)) {
    throw new Error('当前世界已改变，已取消旧世界模型更新');
  }
}
const signature = data => JSON.stringify([data.role, data.state.boss.hp === 0, data.state.drop]);
const gameplay = createStoryGameplay({
  api: editorApi, projectPath, readPlayer: camera.snapshotPlayer, readBoss: () => bossActor,
  // 掉落的碎片和角色、方块读同一个地面：地图的烘好高度网格（没有网格时退回原来的常量）。
  groundYAt,
  trackWork: trackWorldSessionWork,
  onState: data => {
    // 即使 current() 为假也镜像已确认的世界角色：HUD 要跟随引擎真正确认的世界，
    // 而不是可能已经过期的渲染守卫。
    roleRef.value = data.role;
    if (current()) { gameplayState.value = data; updateProximity(); }
  },
  onFeedback: showFeedback,
  reconcile: async data => {
    const next = signature(data), nextPlacements = placementSignature(placements.value);
    if (next === visualSignature && nextPlacements === visualPlacements) return;
    if (next !== visualSignature) {
      await ensureStoryCharacters({ api: editorApi, sceneId, frontendUrl: window.location.href,
        gameplay: data, combatOnly: true, assertSource, projectPath });
      visualSignature = next;
    }
    // 布局是权威来源：被撤回的陈列物在此退场。中途换世界会抛出中止，那不是存档失败。
    if (nextPlacements !== visualPlacements) {
      try {
        await syncPlacementActors({ api: editorApi, sceneId, frontendUrl: window.location.href,
          placements: placements.value, assertSource, isCurrent: current });
        visualPlacements = nextPlacements;
      } catch (error) {
        // 中止说明世界已被替换；同一情形下上面的角色分支也会被跳过，因此不应向上抛。
        if (error.name !== 'AbortError' && current()) {
          gameplayError.value = `陈列同步失败：${error.message}`;
        }
      }
    }
    if (current()) updateProximity();
  },
});
async function runAction(action) {
  if (!current() || !focused || document.hidden || inventoryOpen.value
    || navigation.busy || savingPlayer || actionBusy.value || !cameraReady) return;
  actionBusy.value = true;
  try {
    await action();
    if (current()) gameplayError.value = '';
  } catch (error) {
    if (current()) gameplayError.value = error.message || '保存失败，请重试';
  } finally {
    actionBusy.value = false;
    if (current()) updateProximity();
  }
}
function toggleInventory() {
  if (initializing.value || navigation.busy || navigationPending.value || savingPlayer) return;
  camera.resetInput();
  inventoryOpen.value = !inventoryOpen.value;
  void nextTick(() => {
    if (!current()) return;
    if (inventoryOpen.value) inventoryPanel.value?.querySelector('[data-inventory-close]')?.focus();
    else surface.value?.focus();
  });
}
function onInventoryFocus(event) {
  if (inventoryOpen.value && inventoryPanel.value && !inventoryPanel.value.contains(event.target)) {
    inventoryPanel.value.querySelector('[data-inventory-close]')?.focus();
  }
}
function onInventoryKey(event) {
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.code)) return;
  event.preventDefault(); event.stopPropagation();
  const panel = inventoryPanel.value;
  const slots = [...(panel?.querySelectorAll('.inventory-slot') || [])];
  if (!slots.length || navigationPending.value) return;
  const close = panel.querySelector('[data-inventory-close]');
  const travel = panel.querySelector('[data-inventory-travel]:not(:disabled)')
    || panel.querySelector('[data-inventory-travel-focus]');
  const active = document.activeElement;
  const index = slots.indexOf(active);
  let target;
  if (event.code === 'Home') target = 0;
  else if (event.code === 'End') target = slots.length - 1;
  else if (index < 0) {
    target = active === close ? 0 : slots.length - 5 + selectedSlot.value % 5;
  } else {
    const step = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -5, ArrowDown: 5 }[event.code];
    target = index + step;
    // Tab/Esc still close the bag. Arrows reach both actions without a mouse.
    if (target < 0) { close?.focus(); return; }
    if (target >= slots.length) { travel?.focus(); return; }
  }
  selectedSlot.value = target;
  slots[target]?.focus();
}
function skillStatus(id) {
  void cooldownTick.value;
  return gameplay.skillStatus(id);
}
function cooldownSeconds(id) {
  const remaining = skillStatus(id).remainingMs;
  return remaining > 120 ? (remaining / 1000).toFixed(1) : '';
}
function ragePercent() {
  const data = gameplayState.value;
  if (!data?.config.rageMax) return 0;
  return Math.max(0, Math.min(100, 100 * data.state.rage / data.config.rageMax));
}
function bossPercent() {
  const data = gameplayState.value;
  if (!data?.config.bossHp) return 0;
  return Math.max(0, Math.min(100, 100 * data.state.boss.hp / data.config.bossHp));
}
async function navigateWorld() {
  const data = gameplayState.value;
  if (!current() || !inventoryOpen.value || !data || navigationPending.value || navigation.busy
    || (data.role === 'main' && data.state.boss.hp > 0)) return;
  navigationPending.value = true;
  gameplayError.value = '';
  try { await navigation.navigate(data.role === 'main' ? 'enter' : 'exit'); }
  finally { navigationPending.value = false; }
}
/* =========================================================
   行囊（Tab 背包）：由 111/bag.html 复刻的界面
   —— 纯界面状态，不写任何玩法存档。
   ========================================================= */
const BAG_UI_KEY = '__corona_story_bag_ui_v1__';
const BAG_COLS = 12;
const BAG_TOTAL = 42;

const RARITY = {
  common: { label: '凡品', weight: 1 },
  fine: { label: '良品', weight: 2 },
  rare: { label: '珍品', weight: 3 },
  epic: { label: '秘宝', weight: 4 },
};
/* 剧情 HUD 的「气刃大回旋」按钮对应引擎里的 heavy 战技（键位 E）。 */
const HEAVY_SKILL_ID = 'heavy';
const heavySkill = computed(() => gameplayState.value?.config?.skills?.[HEAVY_SKILL_ID] || null);
/* 披挂位：左四（头盔 / 胸甲 / 腿甲 / 战靴）+ 右一（武器）。
   顺序即列内自上而下的排布。 */
const GEAR_SLOTS = [
  { id: 'helm', name: '头盔', types: ['重甲', '轻甲'] },
  { id: 'chest', name: '胸甲', types: ['重甲', '轻甲'] },
  { id: 'legs', name: '腿甲', types: ['重甲', '轻甲'] },
  { id: 'boots', name: '战靴', types: ['靴履'] },
  { id: 'weapon', name: '武器', types: ['长兵', '刀剑', '弓弩', '法器'] },
];
const SLOT_BY_ID = {};
GEAR_SLOTS.forEach(slot => { SLOT_BY_ID[slot.id] = slot; });
const BAG_TABS = [
  { id: 'all', name: '全部' },
  { id: 'equip', name: '兵甲' },
  { id: 'usable', name: '丹散' },
  { id: 'mat', name: '材料' },
];
/* 界面自带的器物表；其中「世界碎片」一格接真实存档数量 */
const BAG_ITEMS = [
  { u: 'armor-1', name: '影纹鳞甲', type: '重甲', cat: 'equip', slot: 'chest', rarity: 'epic', glyph: 'g-armor',
    stats: [['护体', '+186'], ['筋骨', '+24'], ['抗蚀', '+18%']],
    flavor: '鳞片取自深潭老蛟，贴着火光看，纹路会自己游动。' },
  { u: 'legs-1', name: '缚地胫铠', type: '重甲', cat: 'equip', slot: 'legs', rarity: 'rare', glyph: 'g-legs-h',
    stats: [['护体', '+104'], ['负重', '+9']],
    flavor: '胫甲上留着一道旧铆痕，走过碎石地时才会响。' },
  { u: 'main-1', name: '赤霄长兵', type: '长兵', cat: 'equip', slot: 'weapon', rarity: 'epic', glyph: 'g-sword',
    stats: [['锋芒', '86 - 124'], ['力道', '+31'], ['破甲', '+12%']],
    flavor: '刃上缠着一线不散的血气，主人不问，它也不说。' },
  { u: 'helm-1', name: '兜鍪 · 静水', type: '重甲', cat: 'equip', slot: 'helm', rarity: 'rare', glyph: 'g-helmet',
    stats: [['护体', '+92'], ['定神', '+14']],
    flavor: '面甲内侧刻着半句残咒，据说是铸者留给自己的。' },
  { u: 'boots-1', name: '踏雾履', type: '靴履', cat: 'equip', slot: 'boots', rarity: 'rare', glyph: 'g-boots-l',
    stats: [['身法', '+19'], ['疾行', '+9%']],
    flavor: '踩在松针上，声音比风还迟一步。' },
  { u: 'amulet-1', name: '青囊护符', type: '长兵', cat: 'equip', slot: 'weapon', rarity: 'rare', glyph: 'g-talis',
    stats: [['定神', '+22'], ['回气', '+8/s']],
    flavor: '符纸换了七次，绳结始终没舍得拆。' },
  { u: 'belt-1', name: '缠云腰带', type: '法器', cat: 'equip', slot: 'weapon', rarity: 'fine', glyph: 'g-belt',
    stats: [['护体', '+38'], ['术法', '+12']],
    flavor: '三层素绢，一层一线都是山里人自己纺的。' },
  { u: 'ring-1', name: '残玉指环', type: '刀剑', cat: 'equip', slot: 'weapon', rarity: 'fine', glyph: 'g-ring',
    stats: [['锋芒', '+9'], ['身法', '+11']],
    flavor: '玉上有一道旧裂，握紧时微微发烫。' },
  { u: 'off-1', name: '断水短刃', type: '刀剑', cat: 'equip', slot: 'weapon', rarity: 'fine', glyph: 'g-sword',
    stats: [['锋芒', '42 - 58'], ['身法', '+11']],
    flavor: '短得只能贴身用，也近得让人来不及退。' },
  { u: 'main-2', name: '乌木猎弓', type: '弓弩', cat: 'equip', slot: 'weapon', rarity: 'fine', glyph: 'g-bow',
    stats: [['锋芒', '56 - 74'], ['力道', '+14']],
    flavor: '弦是旧弦，换过三次弓臂，准头反而更稳。' },
  { u: 'main-3', name: '镇魂法铃', type: '法器', cat: 'equip', slot: 'weapon', rarity: 'rare', glyph: 'g-staff',
    stats: [['术法', '+27'], ['定神', '+16']],
    flavor: '摇一下，活人听见的是铃，别的东西听见的是雷。' },
  { u: 'off-2', name: '开山短斧', type: '刀剑', cat: 'equip', slot: 'weapon', rarity: 'common', glyph: 'g-axe',
    stats: [['锋芒', '33 - 45'], ['力道', '+8']],
    flavor: '斧背豁了几处口，砍柴砍人都不耽误。' },
  { u: 'legs-2', name: '藤编胫衣', type: '轻甲', cat: 'equip', slot: 'legs', rarity: 'fine', glyph: 'g-legs-l',
    stats: [['护体', '+52'], ['身法', '+14']],
    flavor: '老藤浸过桐油，走夜路时几乎不发出声响。' },
  { u: 'chest-2', name: '素麻战衣', type: '轻甲', cat: 'equip', slot: 'chest', rarity: 'fine', glyph: 'g-chest-l',
    stats: [['护体', '+71'], ['回气', '+4/s']],
    flavor: '洗得发白的粗麻，针脚却密得看不出线头。' },
  { u: 'use-1', name: '回春丹', type: '丹散', cat: 'usable', rarity: 'fine', glyph: 'g-pill', qty: 12,
    stats: [['回复生命', '240'], ['念咒时长', '1.2s']],
    flavor: '入口先苦后甘，苦的那口才是药。' },
  { u: 'use-2', name: '凝神散', type: '丹散', cat: 'usable', rarity: 'fine', glyph: 'g-pill', qty: 8,
    stats: [['回复法力', '180'], ['念咒时长', '1.0s']],
    flavor: '化在舌下，像咽了一口山里的凉雾。' },
  { u: 'use-3', name: '青铜小还丹', type: '丹散', cat: 'usable', rarity: 'rare', glyph: 'g-elixir', qty: 3,
    stats: [['回复生命', '520'], ['回复法力', '260'], ['念咒时长', '1.8s']],
    flavor: '丹衣上浮着铜绿，老药师说这是它睡醒的样子。' },
  { u: 'use-4', name: '朱砂符纸', type: '符纸', cat: 'usable', rarity: 'fine', glyph: 'g-scroll', qty: 5,
    stats: [['附火', '36 / 6s']],
    flavor: '画符的人手很稳，最后那一笔却总爱往上挑。' },
  { u: 'chest-3', name: '玄鳞重铠', type: '重甲', cat: 'equip', slot: 'chest', rarity: 'rare', glyph: 'g-chest-h',
    stats: [['护体', '+148'], ['抗蚀', '+14%']],
    flavor: '每一片鳞都单独锻过，穿起来比看起来轻。' },
  { u: 'chest-4', name: '云纹法袍', type: '轻甲', cat: 'equip', slot: 'chest', rarity: 'rare', glyph: 'g-chest-l',
    stats: [['护体', '+88'], ['术法', '+31']],
    flavor: '袍角绣着半幅云纹，走起来像有雾跟着。' },
  { u: 'chest-5', name: '旧皮护心', type: '轻甲', cat: 'equip', slot: 'chest', rarity: 'common', glyph: 'g-chest-l',
    stats: [['护体', '+34']],
    flavor: '皮面磨得发亮，胸口那道补丁是后来缝的。' },
  { u: 'helm-2', name: '沉铁面甲', type: '重甲', cat: 'equip', slot: 'helm', rarity: 'epic', glyph: 'g-helm-h',
    stats: [['护体', '+132'], ['定神', '+19'], ['抗蚀', '+9%']],
    flavor: '戴上后只听得见自己的呼吸，别的都远了一层。' },
  { u: 'helm-3', name: '竹编斗笠', type: '轻甲', cat: 'equip', slot: 'helm', rarity: 'fine', glyph: 'g-helm-l',
    stats: [['护体', '+41'], ['身法', '+8']],
    flavor: '雨点打上去是闷的，正好遮住脚步。' },
  { u: 'helm-4', name: '破面铁盔', type: '重甲', cat: 'equip', slot: 'helm', rarity: 'common', glyph: 'g-helm-h',
    stats: [['护体', '+46']],
    flavor: '面甲被砍开一道口子，主人说这样看得更清楚。' },
  { u: 'boots-2', name: '沉铁战靴', type: '靴履', cat: 'equip', slot: 'boots', rarity: 'epic', glyph: 'g-boots-h',
    stats: [['护体', '+97'], ['筋骨', '+21']],
    flavor: '鞋底钉着九枚铁钉，站定了就没人推得动。' },
  { u: 'boots-3', name: '青毡软履', type: '靴履', cat: 'equip', slot: 'boots', rarity: 'fine', glyph: 'g-boots-l',
    stats: [['护体', '+36'], ['回气', '+3/s']],
    flavor: '毡面洗过许多次，颜色淡了，却更贴身。' },
  { u: 'boots-4', name: '旧布行履', type: '靴履', cat: 'equip', slot: 'boots', rarity: 'common', glyph: 'g-boots-l',
    stats: [['护体', '+22']],
    flavor: '底子补过三回，走山路依然不打滑。' },
  { u: 'legs-3', name: '铁线胫甲', type: '重甲', cat: 'equip', slot: 'legs', rarity: 'epic', glyph: 'g-legs-h',
    stats: [['护体', '+142'], ['负重', '+26']],
    flavor: '铁线一道压一道编成，弯不下腿，但也不会断。' },
  { u: 'legs-4', name: '布缚胫衣', type: '轻甲', cat: 'equip', slot: 'legs', rarity: 'common', glyph: 'g-legs-l',
    stats: [['护体', '+28']],
    flavor: '缠得紧，走远路腿肚子不容易酸。' },
  { u: 'main-4', name: '裂山重锤', type: '长兵', cat: 'equip', slot: 'weapon', rarity: 'epic', glyph: 'g-axe',
    stats: [['锋芒', '104 - 138'], ['力道', '+38'], ['破甲', '+16%']],
    flavor: '锤头补过一次铁，补痕比原来的纹路还深。' },
  { u: 'main-5', name: '细雪长刀', type: '刀剑', cat: 'equip', slot: 'weapon', rarity: 'rare', glyph: 'g-sword',
    stats: [['锋芒', '72 - 96'], ['身法', '+18']],
    flavor: '出鞘时几乎没有声音，收回去也一样。' },
  { u: 'main-6', name: '巡夜短弩', type: '弓弩', cat: 'equip', slot: 'weapon', rarity: 'common', glyph: 'g-bow',
    stats: [['锋芒', '40 - 52']],
    flavor: '弩臂上刻着巡夜的更次，最后一格是空着的。' },
  { u: 'main-7', name: '引雷法杖', type: '法器', cat: 'equip', slot: 'weapon', rarity: 'epic', glyph: 'g-staff',
    stats: [['术法', '+44'], ['定神', '+21']],
    flavor: '杖头那颗石头阴天会自己发烫，老人说它在等雨。' },
  { u: 'legs-5', name: '玄铁重胫', type: '重甲', cat: 'equip', slot: 'legs', rarity: 'rare', glyph: 'g-legs-h',
    stats: [['护体', '+118'], ['筋骨', '+17']],
    flavor: '一套两件，左胫内侧刻着铸者的名，右胫那道被人磨平了。' },
  { u: 'legs-6', name: '流沙胫缚', type: '轻甲', cat: 'equip', slot: 'legs', rarity: 'rare', glyph: 'g-legs-l',
    stats: [['护体', '+64'], ['身法', '+22']],
    flavor: '细沙灌进夹层，跑起来脚步比平时沉，落地却更稳。' },
  { u: 'legs-7', name: '猎户皮胫', type: '轻甲', cat: 'equip', slot: 'legs', rarity: 'common', glyph: 'g-legs-l',
    stats: [['护体', '+26']],
    flavor: '硝过的鹿皮，膝盖那两块加厚是自己补的。' },
  { u: 'legs-8', name: '霜纹胫铠', type: '重甲', cat: 'equip', slot: 'legs', rarity: 'epic', glyph: 'g-legs-h',
    stats: [['护体', '+162'], ['抗蚀', '+21%'], ['负重', '+14']],
    flavor: '甲面上的霜纹不是刻的，是天冷时自己长出来的。' },
  { u: 'legs-9', name: '断锋护腿', type: '重甲', cat: 'equip', slot: 'legs', rarity: 'fine', glyph: 'g-legs-h',
    stats: [['护体', '+71'], ['破甲', '+6%']],
    flavor: '边缘全是豁口，挡过的东西比主人记得的还多。' },
  { u: 'legs-10', name: '雾行胫套', type: '轻甲', cat: 'equip', slot: 'legs', rarity: 'epic', glyph: 'g-legs-l',
    stats: [['护体', '+83'], ['身法', '+27'], ['疾行', '+8%']],
    flavor: '套上后小腿总像隔着一层薄雾，冷热都迟半拍才传到。' },
  { u: 'chest-6', name: '沉铁胸铠', type: '重甲', cat: 'equip', slot: 'chest', rarity: 'common', glyph: 'g-chest-h',
    stats: [['护体', '+58']],
    flavor: '分量实在，走远路的人穿它，多半是为了心里踏实。' },
  { u: 'boots-5', name: '藤底行履', type: '靴履', cat: 'equip', slot: 'boots', rarity: 'common', glyph: 'g-boots-l',
    stats: [['护体', '+25']],
    flavor: '藤底编得密，湿了也不打滑。' },
  { u: 'boots-6', name: '鸣沙战靴', type: '靴履', cat: 'equip', slot: 'boots', rarity: 'rare', glyph: 'g-boots-h',
    stats: [['护体', '+84'], ['身法', '+16']],
    flavor: '踏在沙上会发出极轻的响声，像有人在远处数着步子。' },
  { u: 'helm-5', name: '素面轻盔', type: '轻甲', cat: 'equip', slot: 'helm', rarity: 'common', glyph: 'g-helm-l',
    stats: [['护体', '+33']],
    flavor: '没有任何纹饰，反倒是营里最不容易认错的一顶。' },
  { u: 'helm-6', name: '玄铁重盔', type: '重甲', cat: 'equip', slot: 'helm', rarity: 'rare', glyph: 'g-helm-h',
    stats: [['护体', '+108'], ['定神', '+12']],
    flavor: '内衬换过三回，铁壳始终没舍得扔。' },
  { u: 'main-8', name: '断岳长兵', type: '长兵', cat: 'equip', slot: 'weapon', rarity: 'epic', glyph: 'g-sword',
    stats: [['锋芒', '112 - 146'], ['力道', '+41'], ['破甲', '+18%']],
    flavor: '据说劈开过一道山脊，主人从没提过这事。' },
  { u: 'main-9', name: '碎星双刃', type: '刀剑', cat: 'equip', slot: 'weapon', rarity: 'rare', glyph: 'g-sword',
    stats: [['锋芒', '78 - 102'], ['身法', '+21']],
    flavor: '两刃对称，据说原本是一对，另一把在别人手里。' },
  { u: 'mat-1', name: '玄铁矿', type: 'material', cat: 'mat', rarity: 'common', glyph: 'g-ore', qty: 24,
    stats: [['锻造', '可用']],
    flavor: '敲开来，断口里有一线细细的黑光。' },
  { u: 'mat-2', name: '铜绿矿脉', type: 'material', cat: 'mat', rarity: 'fine', glyph: 'g-ore', qty: 9,
    stats: [['锻造', '上品']],
    flavor: '埋得越深，颜色越像雨后旧钟。' },
  { u: 'mat-3', name: '幽昙草', type: 'material', cat: 'mat', rarity: 'fine', glyph: 'g-herb', qty: 14,
    stats: [['炼丹', '可用']],
    flavor: '子时开，丑时谢，采药的人从不点灯。' },
  { u: 'mat-4', name: '蚀骨妖牙', type: 'material', cat: 'mat', rarity: 'rare', glyph: 'g-fang', qty: 2,
    stats: [['锻造', '秘材']],
    flavor: '牙根还带着温，剥下来时整片林子都静了。' },
  { u: 'mat-5', name: '地脉残炭', type: 'material', cat: 'mat', rarity: 'common', glyph: 'g-coal', qty: 31,
    stats: [['燃料', '耐烧']],
    flavor: '燃起来无烟，火色是很少见的青。' },
  { u: 'mat-6', name: '蚕丝云缎', type: 'material', cat: 'mat', rarity: 'fine', glyph: 'g-silk', qty: 6,
    stats: [['缝制', '上品']],
    flavor: '抖开像一小片云，落在手上几乎没有分量。' },
];
const BAG_LOADOUT = { weapon: 'main-1', helm: 'helm-1', chest: 'armor-1', legs: 'legs-1', boots: 'boots-1' };
const BAG_WEIGHT_MAX = 120;
/* 真实存档里唯一的行囊物品 */
const FRAGMENT_DEF = { u: 'corona-world-fragment', name: '世界碎片', type: '未解之物', cat: 'mat', rarity: 'epic',
  glyph: 'g-ore', real: true, stats: [['来历', '界隙所遗'], ['用途', '待考']],
  flavor: '从裂隙里落下的一角，边缘仍在缓慢地转动。' };

/* 世界图象：山形 / 月相（与 bag.html 同一套坐标） */
const WS_SHAPES = [
  { far: ['M0 85 32 45 58 85z', 'M38 85 76 24 112 85z', 'M92 85 122 47 156 85z'],
    snow: ['M70 34 76 24 82 34 76 30z', 'M107 55 122 47 137 55'],
    near: 'M0 112V97C24 87 40 95 58 91c20-4.6 34 4 58-4 18-6 32-1 44 3v22z',
    pine: 'M30 101V79' },
  { far: ['M0 84 26 56 52 84z', 'M30 84 72 20 116 84z', 'M100 84 132 52 160 84z'],
    snow: ['M65 32 72 20 79 32 72 28z'],
    near: 'M0 112V94C22 86 44 98 66 92c24-6 40 6 62 0 14-4 24 2 32 6v10z',
    pine: 'M36 103V83' },
  { far: ['M0 86 36 40 66 86z', 'M50 86 84 28 124 86z'],
    snow: ['M78 38 84 28 90 38 84 34z', 'M116 60 128 50 140 60'],
    near: 'M0 112V99C20 90 46 97 70 92c22-5 44 5 64-1 10-3 20 0 26 3v19z',
    pine: 'M28 102V76' },
  { far: ['M0 85 30 50 60 85z', 'M40 85 80 26 120 85z', 'M96 85 128 44 160 85z'],
    snow: ['M74 36 80 26 86 36 80 32z'],
    near: 'M0 112V96C26 88 54 97 78 91c24-6 50 4 72-2 6-2 10 0 10 2v21z',
    pine: 'M32 101V80' },
  { far: ['M0 87 24 60 48 87z', 'M34 87 70 22 108 87z', 'M96 87 126 56 158 87z'],
    snow: ['M63 34 70 22 77 34 70 30z'],
    near: 'M0 112V98C24 90 50 99 74 93c24-6 46 5 68-2 8-3 14 0 18 4v19z',
    pine: 'M34 102V78' },
];
const WS_MOONS = [
  { x: 121, y: 30, r: 14, cx: 129, cy: 25, cr: 12.5 },
  { x: 0, y: 0, r: 0, cx: 0, cy: 0, cr: 0 },
  { x: 124, y: 28, r: 16, cx: 0, cy: 0, cr: 0 },
  { x: 118, y: 32, r: 13, cx: 130, cy: 22, cr: 14 },
  { x: 0, y: 0, r: 0, cx: 0, cy: 0, cr: 0 },
];
/* 两个世界：
     · 主世界（index 0）—— 剧情起点，即 role === 'main' 的那个场景；
     · 小世界（index 1）—— 击败 Boss 并拾得世界碎片后才开辟（role === 'child'）。
   名称固定，不允许改名：它们是这两个场景的身份，不是可命名的存档位。
   图象（art）仍可自选，属纯界面偏好。 */
const WORLD_MAIN = { name: '主世界', tier: '主界', art: { s: 0, m: 0 } };
const WORLD_SMALL = { name: '小世界', tier: '沙盒', art: { s: 1, m: 1 } };
const WORLDS_DEF = [WORLD_MAIN];
const WORLDS_FRAGMENT = WORLD_SMALL;
/* 一览里的下标 → 场景角色。 */
const WORLD_ROLES = ['main', 'child'];

let bagUidSeq = 0;
const bag = reactive({
  tab: 'all',
  sel: null,
  gear: {},
  cells: [],
  worlds: [],
  worldCur: 0,
  tipOn: false,
  tipStyle: { left: '0px', top: '0px' },
  tipItem: null,
  toast: '',
});
let bagToastTimer = null;
const bagTip = ref(null);

function bagDef(u) { return BAG_ITEMS.find(def => def.u === u) || null; }
function bagMakeItem(def) {
  return { ...def, uid: `${def.u}#${++bagUidSeq}`, qty: def.qty || 1 };
}
function clampWorldIndex(index) {
  const i = Math.floor(Number(index));
  const last = Math.max(0, bag.worlds.length - 1);
  return Number.isFinite(i) ? Math.min(Math.max(i, 0), last) : 0;
}
/* 每件兵甲只认自己的那一个披挂位：胸甲只能进胸甲位，不按类型跨位匹配。
   （类型仍用于详情页的说明文字，但不再决定能放进哪个格子。） */
function bagSlotsFor(item) {
  if (!item || item.cat !== 'equip' || !item.slot) return [];
  return SLOT_BY_ID[item.slot] ? [item.slot] : [];
}
function bagRealFragment() {
  const count = gameplayState.value?.state?.inventory?.worldFragment || 0;
  return count > 0
    ? { ...FRAGMENT_DEF, uid: FRAGMENT_DEF.u, qty: count, real: true, realCount: count }
    : null;
}
/* 世界碎片既在囊中，也在小世界一览里辟出第二界 */
function bagFragmentOwned() {
  return (gameplayState.value?.state?.inventory?.worldFragment || 0) > 0;
}
/* 一览 = 主世界（恒有） + 小世界（拾得碎片后开辟）。
   名称与层级用代码里的固定身份，**不读存档里的名字**，
   否则旧存档会把「主世界 / 小世界」覆盖成任意旧名。 */
function bagVisibleWorlds() {
  const owned = bagFragmentOwned();
  const list = [{
    ...WORLD_MAIN,
    role: WORLD_ROLES[0],
    art: { ...(bag.worlds[0]?.art || WORLD_MAIN.art) },
  }];
  if (owned) {
    list.push({
      ...WORLD_SMALL,
      role: WORLD_ROLES[1],
      art: { ...(bag.worlds[1]?.art || WORLD_SMALL.art) },
    });
  }
  return list;
}
/* 可换场景的界：主世界 ↔ 小世界，两个都可点。 */
function bagEntryIndex() {
  return bagVisibleWorlds().length > 1 ? 1 : -1;
}
/* 碎片尚未到手：小世界不可入 */
function bagEntryLocked(index) {
  return bagVisibleWorlds()[index]?.role === 'child' && !bagFragmentOwned();
}
function bagLoad() {
  try {
    const raw = window.localStorage?.getItem(BAG_UI_KEY);
    if (!raw) return null;
    const saved = JSON.parse(raw);
    return saved && typeof saved === 'object' && saved.version === 1 ? saved : null;
  } catch { return null; }
}
function bagSave() {
  try {
    window.localStorage?.setItem(BAG_UI_KEY, JSON.stringify({ version: 1,
      tab: bag.tab,
      gear: Object.fromEntries(Object.entries(bag.gear).filter(([, item]) => item).map(([id, item]) => [id, item.u])),
      cells: bag.cells.map(item => (item && !item.real ? { u: item.u, qty: item.qty } : null)),
      /* 只存图象偏好：世界名称是固定身份，不写档也不读档。 */
      worlds: bag.worlds.slice(0, 2).map(world => ({ art: { ...world.art } })),
      worldCur: bag.worldCur,    }));
  } catch { /* 界面状态不写存档，失败即忽略 */ }
}
function bagInit() {
  const saved = bagLoad();
  if (saved) {
    bag.tab = BAG_TABS.some(tab => tab.id === saved.tab) ? saved.tab : 'all';
    GEAR_SLOTS.forEach(slot => {
      const def = bagDef(saved.gear?.[slot.id]);
      if (def) bag.gear[slot.id] = bagMakeItem(def);
    });
    /* 旧存档可能把器物挂在已取消的披挂位上（护符/副兵/腰系/指环）。
       这些器物不能丢：能放进新披挂位的就转过去，放不进的退回格阵。 */
    const orphaned = Object.entries(saved.gear || {})
      .filter(([slotId, u]) => u && !SLOT_BY_ID[slotId])
      .map(([, u]) => bagDef(u)).filter(Boolean);
    bag.cells = Array.from({ length: BAG_TOTAL }, (_, i) => {
      const entry = saved.cells?.[i];
      const def = entry && bagDef(entry.u);
      if (!def) return null;
      const item = bagMakeItem(def);
      if (entry.qty > 1) item.qty = entry.qty;
      return item;
    });
    for (const def of orphaned) {
      const free = bagSlotsFor(def).find(id => !bag.gear[id]);
      if (free) bag.gear[free] = bagMakeItem(def);
      else {
        const empty = bag.cells.indexOf(null);
        if (empty !== -1) bag.cells[empty] = bagMakeItem(def);
      }
    }
    if (Array.isArray(saved.worlds) && saved.worlds.length) {
      /* 只恢复图象；旧存档里的名字一律丢弃（当时的「無常劫 / 界隙」等已作废）。 */
      bag.worlds = saved.worlds.slice(0, 2).map(world => ({
        art: { s: Number(world?.art?.s) || 0, m: Number(world?.art?.m) || 0 },
      }));
      bag.worldCur = clampWorldIndex(saved.worldCur);
    }
  }
  if (!bag.worlds.length) {
    bag.worlds = [WORLD_MAIN, WORLD_SMALL].map(world => ({ art: { ...world.art } }));
    bag.worldCur = 0;
  }
  // 第二界的外观数据位：拾得碎片后它才会出现在一览里
  while (bag.worlds.length < 2) {
    bag.worlds.push({ art: { ...WORLD_SMALL.art } });
  }
  if (!bag.cells.length) {
    // 未披挂的兵甲 + 消耗品 + 材料，乱序铺进格阵（与 bag.html 同一铺陈方式）
    const equipDefs = BAG_ITEMS.filter(def => def.cat === 'equip');
    const pool = equipDefs.filter(def => !Object.values(BAG_LOADOUT).includes(def.u)).map(bagMakeItem)
      .concat(BAG_ITEMS.filter(def => def.cat !== 'equip').map(bagMakeItem));
    for (let i = pool.length - 1; i > 0; i--) {
      const j = Math.floor(Math.random() * (i + 1));
      [pool[i], pool[j]] = [pool[j], pool[i]];
    }
    const floorCount = Math.min(pool.length, Math.round(BAG_TOTAL * 0.78));
    bag.cells = Array.from({ length: BAG_TOTAL }, (_, i) => {
      const mustFill = (BAG_TOTAL - i) <= pool.length;
      const wantFill = pool.length
        && (i < floorCount || Math.random() < 0.55 + (Math.floor(i / BAG_COLS) < 2 ? 0.12 : 0));
      return mustFill || wantFill ? pool.pop() : null;
    });
    if (!Object.keys(bag.gear).length) {
      Object.entries(BAG_LOADOUT).forEach(([slotId, u]) => {
        const def = bagDef(u);
        if (def) bag.gear[slotId] = bagMakeItem(def);
      });
    }
  }
}
function bagCellItems() {
  const source = bag.cells;
  return Array.from({ length: BAG_TOTAL }, (_, i) => {
    if (i === 0) { const real = bagRealFragment(); if (real) return real; }
    return source[i] || null;
  });
}
function bagPassFilter(item) {
  if (!item) return false;
  if (item.real) return bag.tab === 'all' || bag.tab === 'mat';
  return bag.tab === 'all' || item.cat === bag.tab;
}
/* 分类页签右侧的件数：已披挂的兵甲也算进「全部 / 兵甲」。 */
function bagTabCount(tabId) {
  const all = [ ...Object.values(bag.gear).filter(Boolean), ...bagCellItems().filter(Boolean) ];
  if (tabId === 'all') return all.reduce((sum, item) => sum + (item.qty || 1), 0);
  return all.filter(item => (item.real ? tabId === 'mat' : item.cat === tabId))
    .reduce((sum, item) => sum + (item.qty || 1), 0);
}
/* 器物 glyph → 图标 id。每种兵甲有自己的精细图标
   （头盔分重/轻，胸甲分重/轻，腿甲分重/轻，战靴分重/轻，武器分利器/重兵/弓弩/法器），
   不再让同一类装备共用一张线条图。 */
const BAG_GLYPH_TO_ICON = {
  // 头盔
  'g-helm-h': 'i-helm-h',
  'g-helm-l': 'i-helm-l',
  'g-helmet': 'i-helm-h',
  // 胸甲
  'g-chest-h': 'i-chest-h',
  'g-chest-l': 'i-chest-l',
  'g-armor': 'i-chest-h',
  'g-body': 'i-chest-l',
  // 腿甲
  'g-legs-h': 'i-legs-h',
  'g-legs-l': 'i-legs-l',
  // 战靴
  'g-boots-h': 'i-boots-h',
  'g-boots-l': 'i-boots-l',
  'g-boots': 'i-boots-l',
  // 武器
  'g-sword': 'i-weapon-b',
  'g-axe': 'i-weapon-h',
  'g-bow': 'i-weapon-r',
  'g-staff': 'i-weapon-x',
  // 佩饰 / 丹散 / 材料
  'g-ring': 'i-ring-d',
  'g-talis': 'i-talis-d',
  'g-pill': 'i-pill-d',
  'g-elixir': 'i-elixir-d',
  'g-scroll': 'i-scroll-d',
  'g-ore': 'i-ore-d',
  'g-herb': 'i-herb-d',
  'g-silk': 'i-silk-d',
  'g-coal': 'i-coal-d',
  'g-fang': 'i-fang-d',
  'g-belt': 'i-silk-d',
};
function bagGlyphId(glyph) {
  return BAG_GLYPH_TO_ICON[glyph] || 'i-material';
}
/* 器物图标：自包含的行内 SVG（几何 + 行内 fill/stroke），
   不经 <use>、不引用 url(#...)：这套 CEF 不会把渐变引用带过 <use> 克隆，
   之前所有图标因此丢失填充、缩成一团轮廓。 */
/* 剧情 HUD 的战技图标：同样做成自包含行内 SVG。
   此前模板引用 #i-shield / #i-mirage / #i-stasis，而 HUD 的 defs 里只有
   i-heal 与 i-burst —— 这三个引用根本不存在，所以战技钮的图标一直是空的。 */
const SKILL_ICON_INNER = {
  // 重斩：护盾（防御 → 蓄力重击）
  shield: '<polygon points="24,6 40,16 34,36 24,44 14,36 8,16" fill="rgba(158,203,214,.28)"/>'
    + '<polygon points="24,6 40,16 34,36 24,44 14,36 8,16"/>'
    + '<line x1="24" y1="16" x2="24" y2="44"/>',
  // 横扫：多重残影（横挥的多段轨迹）
  mirage: '<rect x="14" y="14" width="20" height="20" rx="2" fill="rgba(230,213,167,.34)"/>'
    + '<rect x="14" y="14" width="20" height="20" rx="2"/>'
    + '<rect x="8" y="8" width="20" height="20" rx="2" opacity="0.45" fill="rgba(230,213,167,.2)"/>'
    + '<rect x="8" y="8" width="20" height="20" rx="2" opacity="0.45"/>'
    + '<rect x="20" y="20" width="20" height="20" rx="2" opacity="0.45" fill="rgba(230,213,167,.2)"/>'
    + '<rect x="20" y="20" width="20" height="20" rx="2" opacity="0.45"/>',
  // 其它战技：定身符（双竖线 + 环）
  stasis: '<circle cx="24" cy="24" r="16" fill="rgba(158,203,214,.22)"/>'
    + '<circle cx="24" cy="24" r="16"/>'
    + '<line x1="18" y1="14" x2="18" y2="34"/><line x1="30" y1="14" x2="30" y2="34"/>',
};
function skillIconInner(id) {
  return SKILL_ICON_INNER[id] || SKILL_ICON_INNER.stasis;
}

function bagIconHTML(item) {
  if (!item) return '';
  return GEAR_ICONS[bagGlyphId(item.glyph)] || GEAR_ICONS['i-material'];
}

const GEAR_ICONS = {
  "i-helm-h": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 5C15.7 5 9.5 11.6 9.5 20v6h29v-6C38.5 11.6 32.3 5 24 5Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M24 5c8.3 0 14.5 6.6 14.5 15v6H24Z\" fill=\"rgba(120,92,38,.45)\"/><path d=\"M24 31c-6.4 0-11.6 3.6-13.4 9h26.8C35.6 34.6 30.4 31 24 31Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M24 5C15.7 5 9.5 11.6 9.5 20v6h29v-6C38.5 11.6 32.3 5 24 5Z\"/><path d=\"M24 31c-6.4 0-11.6 3.6-13.4 9h26.8C35.6 34.6 30.4 31 24 31Z\"/><path d=\"M21.6 26v14\"/><path d=\"M10.6 26h7.4M30 26h7.4\"/><path d=\"M24 5v7\"/></svg>",
  "i-helm-l": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 10c-7.4 0-13 6.4-14.6 15.4L24 31l14.6-5.6C37 16.4 31.4 10 24 10Z\" fill=\"rgba(244,248,240,.40)\"/><path d=\"M24 10c7.4 0 13 6.4 14.6 15.4L24 31Z\" fill=\"rgba(150,164,144,.35)\"/><path d=\"M6 25.4c5.6 4.8 12 6.9 18 6.9s12.4-2.1 18-6.9c-5.6 2-12 3-18 3s-12.4-1-18-3Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M24 10c-7.4 0-13 6.4-14.6 15.4L24 31l14.6-5.6C37 16.4 31.4 10 24 10Z\"/><path d=\"M6 25.4c5.6 4.8 12 6.9 18 6.9s12.4-2.1 18-6.9c-5.6 2-12 3-18 3s-12.4-1-18-3Z\"/><path d=\"M17 32c1.6 5.6 4 9.6 7 12 3-2.4 5.4-6.4 7-12\"/><path d=\"M24 10v21\"/></svg>",
  "i-chest-h": "<svg viewBox=\"0 0 48 48\"><path d=\"M14.5 12.5 24 9.5l9.5 3 2 9.5c0 9-5 15.5-11.5 19.5C17.5 37.5 12.5 31 12.5 22Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M24 9.5l9.5 3 2 9.5c0 9-5 15.5-11.5 19.5Z\" fill=\"rgba(120,92,38,.45)\"/><path d=\"M24 20c-4.4 0-7 3-7 7 0 5 3.4 9 7 11.5 3.6-2.5 7-6.5 7-11.5 0-4-2.6-7-7-7Z\" fill=\"rgba(230,213,167,.55)\"/><path d=\"M14.5 12.5 24 9.5l9.5 3 2 9.5c0 9-5 15.5-11.5 19.5C17.5 37.5 12.5 31 12.5 22Z\"/><path d=\"M24 20c-4.4 0-7 3-7 7 0 5 3.4 9 7 11.5 3.6-2.5 7-6.5 7-11.5 0-4-2.6-7-7-7Z\"/><path d=\"M24 20v18.5\"/><path d=\"M12.5 22h7M35.5 22h-7\"/></svg>",
  "i-chest-l": "<svg viewBox=\"0 0 48 48\"><path d=\"M15 13 24 10l9 3 2 10c0 8-4.5 13.5-11 16.5-6.5-3-11-8.5-11-16.5Z\" fill=\"rgba(244,248,240,.40)\"/><path d=\"M24 10l9 3 2 10c0 8-4.5 13.5-11 16.5Z\" fill=\"rgba(150,164,144,.35)\"/><path d=\"M15 13 24 10l9 3 2 10c0 8-4.5 13.5-11 16.5-6.5-3-11-8.5-11-16.5Z\"/><path d=\"M20 13.5 24 20l4-6.5\"/><path d=\"M24 20v19\"/><path d=\"M15.5 26h5M27.5 26h5\"/></svg>",
  "i-legs-h": "<svg viewBox=\"0 0 48 48\"><path d=\"M15 8.5h8c1 5 1.4 10 1 15l-1 6h-8l-1-6c-.4-5 0-10 1-15Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M25 8.5h8c1 5 1.4 10 1 15l-1 6h-8l-1-6c-.4-5 0-10 1-15Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M25 8.5h8c1 5 1.4 10 1 15l-1 6h-8Z\" fill=\"rgba(120,92,38,.45)\"/><path d=\"M11.6 8.6C17.4 5 30.6 5 36.4 8.6c-5.8 3.4-19 3.4-24.8 0Z\" fill=\"rgba(230,213,167,.55)\"/><circle cx=\"19\" cy=\"20\" r=\"3.6\" fill=\"rgba(230,213,167,.55)\"/><circle cx=\"29.5\" cy=\"20\" r=\"3.6\" fill=\"rgba(230,213,167,.55)\"/><path d=\"M15 8.5h8c1 5 1.4 10 1 15l-1 6h-8l-1-6c-.4-5 0-10 1-15Z\"/><path d=\"M25 8.5h8c1 5 1.4 10 1 15l-1 6h-8l-1-6c-.4-5 0-10 1-15Z\"/><path d=\"M11.6 8.6C17.4 5 30.6 5 36.4 8.6c-5.8 3.4-19 3.4-24.8 0Z\"/><circle cx=\"19\" cy=\"20\" r=\"3.6\"/><circle cx=\"29.5\" cy=\"20\" r=\"3.6\"/><path d=\"M15.4 30.5h7.2M25.4 30.5h7.2\"/></svg>",
  "i-legs-l": "<svg viewBox=\"0 0 48 48\"><path d=\"M16 9h6.5l1.4 8-1 22H16c-1-10-1-20 0-30Z\" fill=\"rgba(244,248,240,.40)\"/><path d=\"M25.5 9H32c1 10 1 20 0 30h-7.9l-1-22Z\" fill=\"rgba(244,248,240,.40)\"/><path d=\"M25.5 9H32c1 10 1 20 0 30H25.5Z\" fill=\"rgba(150,164,144,.35)\"/><path d=\"M16 9h6.5l1.4 8-1 22H16c-1-10-1-20 0-30Z\"/><path d=\"M25.5 9H32c1 10 1 20 0 30h-7.9l-1-22Z\"/><path d=\"M16.4 17h7M16.4 25h7M16.4 33h7M25.6 17h7M25.6 25h7M25.6 33h7\"/></svg>",
  "i-boots-h": "<svg viewBox=\"0 0 48 48\"><path d=\"M13 11h8v17.5l6.5 6.5v6.5H13Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M25 12h8v13l6 10v6.5H25Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M25 12h8v13l6 10v6.5H25Z\" fill=\"rgba(120,92,38,.45)\"/><path d=\"M11.5 9.5c4.4-1.6 15.6-1.6 20 0 .8 2.2.8 3.6 0 5.4-4.4-1.6-15.6-1.6-20 0-.8-1.8-.8-3.2 0-5.4Z\" fill=\"rgba(230,213,167,.55)\"/><path d=\"M11.5 38.5h31v3.5h-31Z\" fill=\"rgba(120,92,38,.45)\"/><path d=\"M13 11h8v17.5l6.5 6.5v6.5H13Z\"/><path d=\"M25 12h8v13l6 10v6.5H25Z\"/><path d=\"M11.5 9.5c4.4-1.6 15.6-1.6 20 0 .8 2.2.8 3.6 0 5.4-4.4-1.6-15.6-1.6-20 0-.8-1.8-.8-3.2 0-5.4Z\"/><path d=\"M11.5 38.5h31v3.5h-31Z\"/><path d=\"M25 25h8\"/></svg>",
  "i-boots-l": "<svg viewBox=\"0 0 48 48\"><path d=\"M14 13h7.5v15.5l6 7v5H14Z\" fill=\"rgba(244,248,240,.40)\"/><path d=\"M25 14h7.5v12l5.5 9.5v5H25Z\" fill=\"rgba(244,248,240,.40)\"/><path d=\"M25 14h7.5v12l5.5 9.5v5H25Z\" fill=\"rgba(150,164,144,.35)\"/><path d=\"M11 38h29l-1.5 4h-26Z\" fill=\"rgba(150,164,144,.35)\"/><path d=\"M14 13h7.5v15.5l6 7v5H14Z\"/><path d=\"M25 14h7.5v12l5.5 9.5v5H25Z\"/><path d=\"M11 38h29l-1.5 4h-26Z\"/><path d=\"M14 19h7.5M25 20h7.5M25 27h7.5\"/></svg>",
  "i-weapon-b": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 2.6 30 13.5v20.5h-12V13.5Z\" fill=\"rgba(255,255,255,.62)\"/><path d=\"M24 2.6 30 13.5v20.5h-6Z\" fill=\"rgba(124,134,118,.45)\"/><path d=\"M24 2.6 30 13.5v20.5h-12V13.5Z\"/><path d=\"M9.5 33.5c5-3 24-3 29 0-5 3-24 3-29 0Z\" fill=\"rgba(230,213,167,.55)\"/><path d=\"M9.5 33.5c5-3 24-3 29 0-5 3-24 3-29 0Z\"/><path d=\"M21.3 36.6h5.4v7.4h-5.4Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M21.3 36.6h5.4v7.4h-5.4Z\"/><path d=\"M21.3 40.3h5.4\"/><circle cx=\"24\" cy=\"45.6\" r=\"2.4\" fill=\"rgba(230,213,167,.55)\"/><circle cx=\"24\" cy=\"45.6\" r=\"2.4\"/></svg>",
  "i-weapon-h": "<svg viewBox=\"0 0 48 48\"><path d=\"M19.5 7c-7 3.5-10.5 9-10.5 15.5S12.5 34.5 19.5 38Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M28.5 7c7 3.5 10.5 9 10.5 15.5S35.5 34.5 28.5 38Z\" fill=\"rgba(120,92,38,.45)\"/><path d=\"M19.5 7c-7 3.5-10.5 9-10.5 15.5S12.5 34.5 19.5 38Z\"/><path d=\"M28.5 7c7 3.5 10.5 9 10.5 15.5S35.5 34.5 28.5 38Z\"/><path d=\"M21.4 4.5h5.2v39h-5.2Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M21.4 4.5h5.2v39h-5.2Z\"/><path d=\"M21.4 12h5.2M21.4 18h5.2M21.4 24h5.2M21.4 30h5.2\"/><circle cx=\"24\" cy=\"45\" r=\"2.6\" fill=\"rgba(230,213,167,.55)\"/><circle cx=\"24\" cy=\"45\" r=\"2.6\"/></svg>",
  "i-weapon-x": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 3.5 34 12v17L24 37.5 14 29V12Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M24 3.5 34 12v17L24 37.5Z\" fill=\"rgba(120,92,38,.45)\"/><path d=\"M24 16.5 29 20v7l-5 3.5-5-3.5v-7Z\" fill=\"rgba(230,213,167,.55)\"/><path d=\"M24 3.5 34 12v17L24 37.5 14 29V12Z\"/><path d=\"M14 12l10 8 10-8M24 20v17.5\"/><path d=\"M24 16.5 29 20v7l-5 3.5-5-3.5v-7Z\"/><path d=\"M24 37.5V44\"/></svg>",
  "i-weapon-r": "<svg viewBox=\"0 0 48 48\"><path d=\"M13.5 5c11 6.5 11 27.5 0 34-.8-11.3-.8-22.7 0-34Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M13.5 5c11 6.5 11 27.5 0 34-.8-11.3-.8-22.7 0-34Z\"/><path d=\"M13.5 5v34\"/><path d=\"M13.5 22h19\"/><path d=\"M33 17.5l7.5 4.5-7.5 4.5Z\" fill=\"rgba(230,213,167,.55)\"/><path d=\"M33 17.5l7.5 4.5-7.5 4.5Z\"/><path d=\"M28 22h5\"/></svg>",
  "i-material-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M19 8l10 3 4 9-4 9-10 3-9-4-2-8 3-9Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M29 11l4 9-4 9-10 3 5-10Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M19 8l10 3 4 9-4 9-10 3-9-4-2-8 3-9Z\"/><path d=\"M10 12l9 5 10-6M19 17v15\"/></svg>",
  "i-herb-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 21C18 20 13 15 13 9c6-.6 11 5 11 12Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M24 30c6-1.4 11-7 11-13-6-.6-11 5-11 13Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M24 41V15\"/><path d=\"M24 21C18 20 13 15 13 9c6-.6 11 5 11 12Z\"/><path d=\"M24 30c6-1.4 11-7 11-13-6-.6-11 5-11 13Z\"/><path d=\"M11 41h26\"/></svg>",
  "i-silk-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M7 13c7-6 14 6 21 0s10 2 13 5v8c-3-3-6-9-13-3s-14-6-21 0Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M7 26c7-6 14 6 21 0s10 2 13 5v8c-3-3-6-9-13-3s-14-6-21 0Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M7 13c7-6 14 6 21 0s10 2 13 5\"/><path d=\"M7 26c7-6 14 6 21 0s10 2 13 5\"/><path d=\"M7 34c7-6 14 6 21 0s10 2 13 5\"/></svg>",
  "i-ore-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M15 16l9-7 9 7v12l-9 8-9-8Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M24 9l9 7v12l-9 8Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M15 16l9-7 9 7v12l-9 8-9-8Z\"/><path d=\"M15 16h18M24 21v15\"/></svg>",
  "i-coal-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M13 28l5-15 11 3 6 13-11 10Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M29 16l6 13-11 10Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M13 28l5-15 11 3 6 13-11 10Z\"/><path d=\"M18 13l-3 13 11 10\"/></svg>",
  "i-fang-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M20 5l5 9-2 28-4-28Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M25 14l7 5-5 4Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M20 5l5 9-2 28-4-28Z\"/><path d=\"M25 14l7 5-5 4Z\"/></svg>",
  "i-ring-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 12a12 12 0 1 0 0 24 12 12 0 0 0 0-24Zm0 6.5a5.5 5.5 0 1 1 0 11 5.5 5.5 0 0 1 0-11Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M18 6l6 4-3 5-4-4Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M24 12a12 12 0 1 0 0 24 12 12 0 0 0 0-24Zm0 6.5a5.5 5.5 0 1 1 0 11 5.5 5.5 0 0 1 0-11Z\"/><path d=\"M18 6l6 4-3 5-4-4Z\"/></svg>",
  "i-talis-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M18 12c-2 12 1 22 6 31 5-9 8-19 6-31Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M24 12c3 0 5 1 6 31-3 0-4-1-6-31Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M18 12c-2 12 1 22 6 31 5-9 8-19 6-31Z\"/><path d=\"M17 5h14v7H17Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M17 5h14v7H17Z\"/><path d=\"M20 22h8M21 30h6\"/></svg>",
  "i-pill-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 6l13 10v16L24 42 11 32V16Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M24 6l13 10v16L24 42Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M24 6l13 10v16L24 42 11 32V16Z\"/><path d=\"M11 16l13 10 13-10M24 26v16\"/></svg>",
  "i-elixir-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M21 12c-8 5-10 12-6 18 3 5 7 7 9 7s6-2 9-7c4-6 2-13-6-18Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M24 12c8 5 10 12 6 18-3 5-7 7-9 7Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M21 12c-8 5-10 12-6 18 3 5 7 7 9 7s6-2 9-7c4-6 2-13-6-18Z\"/><path d=\"M19 5h10v8H19Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M19 5h10v8H19Z\"/><path d=\"M15 26h18\"/></svg>",
  "i-scroll-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M16 9h16v30H16Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M32 9h3v30h-3Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M16 9h16v30H16Z\"/><path d=\"M13 6h22v5H13Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M13 6h22v5H13Z\"/><path d=\"M13 37h22v5H13Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M13 37h22v5H13Z\"/><path d=\"M20 18h8M20 24h8M20 30h5\"/></svg>",
  "i-consumable-d": "<svg viewBox=\"0 0 48 48\"><path d=\"M16 16h16v19c0 4-16 4-16 0Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M32 16v19c0 4-16 4-16 0Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M16 16h16v19c0 4-16 4-16 0Z\"/><path d=\"M20 7h8v9h-8Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M20 7h8v9h-8Z\"/><path d=\"M20 23h8M20 28h8\"/></svg>",
  "i-material": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 8l12 8v16l-12 8-12-8V16Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M24 8l12 8v16l-12 8V16Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M24 8l12 8v16l-12 8-12-8V16Z\"/><path d=\"M24 24l12-8M24 24l-12-8M24 24v16\"/></svg>",
  "i-consumable": "<svg viewBox=\"0 0 48 48\"><path d=\"M16 16h16v20c0 4-16 4-16 0Z\" fill=\"rgba(158,203,214,.32)\"/><path d=\"M16 16h16v20c0 4-16 4-16 0Z\"/><path d=\"M20 8h8v8h-8Z\" fill=\"rgba(51,96,108,.45)\"/><path d=\"M20 8h8v8h-8Z\"/></svg>",
  "i-core": "<svg viewBox=\"0 0 48 48\"><path d=\"M24 6l16 18-16 18L8 24Z\" fill=\"rgba(230,213,167,.30)\"/><path d=\"M24 6l16 18-16 18L8 24Z\"/><circle cx=\"24\" cy=\"24\" r=\"6\" fill=\"rgba(230,213,167,.55)\"/><circle cx=\"24\" cy=\"24\" r=\"6\"/></svg>",
  "i-acc": "<svg viewBox=\"0 0 48 48\"><circle cx=\"24\" cy=\"24\" r=\"14\" stroke-dasharray=\"4 4\" fill=\"rgba(150,164,144,.35)\"/><circle cx=\"24\" cy=\"24\" r=\"14\" stroke-dasharray=\"4 4\"/><circle cx=\"24\" cy=\"24\" r=\"6\"/><path d=\"M24 4v6\"/></svg>"
};
function bagTypeLine(item) {
  if (item.real) return item.type;
  let text = item.type;
  if (item.cat === 'equip') {
    const names = bagSlotsFor(item).map(id => SLOT_BY_ID[id].name).join(' / ');
    text += ` · 可安放：${names || '—'}`;
  } else if (item.cat === 'usable') text += ' · 随身可用';
  else text += ' · 锻造炼药之材';
  return text;
}
function bagRowName(item) {
  return `${item.name} · ${item.type} · ${RARITY[item.rarity].label}`;
}
function bagFindByUid(uid) {
  for (const slotId of Object.keys(bag.gear)) {
    if (bag.gear[slotId]?.uid === uid) return { item: bag.gear[slotId], from: 'gear', slot: slotId };
  }
  const items = bagCellItems();
  for (let i = 0; i < items.length; i++) {
    if (items[i]?.uid === uid) return { item: items[i], from: 'bag', idx: i };
  }
  return null;
}
function bagSelected() { return bag.sel ? bagFindByUid(bag.sel) : null; }
function bagGearMeta() { return `${GEAR_SLOTS.filter(slot => bag.gear[slot.id]).length} / ${GEAR_SLOTS.length}`; }function bagItemWeight(item) {
  return (RARITY[item.rarity].weight * 0.5 + (item.cat === 'equip' ? 2.2 : 0.3)) * item.qty;
}
function bagFoot() {
  const items = bagCellItems();
  const used = items.filter(Boolean).length;
  const weight = Object.values(bag.gear).reduce((sum, item) => sum + (item ? bagItemWeight(item) : 0), 0)
    + items.reduce((sum, item) => sum + (item ? bagItemWeight(item) : 0), 0);
  return {
    capText: `${used} / ${items.length}`,
    capWidth: `${Math.min(100, Math.max(0, (used / items.length) * 100)).toFixed(1)}%`,
    weightText: `${weight.toFixed(1)} / ${BAG_WEIGHT_MAX}`,
    weightWidth: `${Math.min(100, Math.max(0, (weight / BAG_WEIGHT_MAX) * 100)).toFixed(1)}%`,
    over: weight > BAG_WEIGHT_MAX,
  };
}
function bagSelectSlot(index) {
  const item = bagCellItems()[index];
  bag.sel = item ? item.uid : null;
  selectedSlot.value = index;
}
function bagSelectGear(slotId) {
  const item = bag.gear[slotId];
  bag.sel = item ? item.uid : null;
}
function bagToast(text) {
  if (!text) return;
  bag.toast = text;
  clearTimeout(bagToastTimer);
  bagToastTimer = setTimeout(() => { bag.toast = ''; }, 1400);
}
/* ------------------ 提示语 / 浮签 ------------------ */
function bagShowTip(item, event) {
  if (!item) return;
  bag.tipItem = item;
  bag.tipOn = true;
  bagMoveTip(event);
}
function bagMoveTip(event) {
  if (!bag.tipOn) return;
  const width = bagTip.value?.offsetWidth || 260;
  const height = bagTip.value?.offsetHeight || 160;
  let left = event.clientX + 18;
  let top = event.clientY + 16;
  if (left + width > window.innerWidth - 8) left = event.clientX - width - 18;
  if (top + height > window.innerHeight - 8) top = Math.max(8, event.clientY - height - 16);
  bag.tipStyle = { left: `${Math.max(8, left)}px`, top: `${Math.max(8, top)}px` };
}
function bagHideTip() { bag.tipOn = false; }
/* ------------------ 披掛 / 卸下 / 服用 / 弃置（仅界面状态） ------------------ */
function bagEquipFromSlot(index) {
  const item = bagCellItems()[index];
  if (!item || item.real) { bagToast('此物随身，不可披掛'); return; }
  if (item.cat !== 'equip') { bagToast('此物无法披掛'); return; }
  // 器物自带唯一披挂位：胸甲只进胸甲位，不存在“哪格都能装”。
  const [slotId] = bagSlotsFor(item);
  if (!slotId) { bagToast('无处安放'); return; }
  const previous = bag.gear[slotId] || null;
  bag.gear[slotId] = item;
  bag.cells[index] = previous;            // 旧物回落到同一格，格阵不塌陷
  bagToast(`披掛 · ${SLOT_BY_ID[slotId].name} · ${item.name}`);
  bag.sel = item.uid;
  bagSave();
}
function bagUnequip(slotId) {
  const item = bag.gear[slotId];
  if (!item) return;
  const index = bag.cells.findIndex(cell => !cell);
  if (index < 0) { bagToast('囊中已满'); return; }
  bag.cells[index] = item;
  bag.gear[slotId] = null;
  bagToast(`卸下 · ${item.name}`);
  bag.sel = item.uid;
  bagSave();
}
function bagUseFromSlot(index) {
  const entry = bag.cells[index];
  if (bagCellItems()[index]?.real) { bagToast('此物不可服用'); return; }
  if (!entry || entry.cat !== 'usable') return;
  if (entry.qty > 1) {
    entry.qty -= 1;
    bagToast(`服用 · ${entry.name}（余 ${entry.qty}）`);
  } else {
    bagToast(`服用 · ${entry.name}（已尽）`);
    bag.cells[index] = null;
    if (bag.sel === entry.uid) bag.sel = null;
  }
  bagSave();
}
function bagDropItem(uid) {
  const found = bagFindByUid(uid);
  if (!found || found.item.real) return;
  if (found.from === 'gear') bag.gear[found.slot] = null;
  else bag.cells[found.idx] = null;
  bag.sel = null;
  bagToast(`${found.item.cat === 'equip' ? '熔去' : '弃之'} · ${found.item.name}`);
  bagSave();
}
function bagDetailAct(uid, action) {
  const found = bagFindByUid(uid);
  if (!found || found.item.real) return;
  if (action === 'equip') {
    if (found.from === 'gear') bagUnequip(found.slot);
    else bagEquipFromSlot(found.idx);
  } else if (action === 'use' && found.from === 'bag') bagUseFromSlot(found.idx);
  else if (action === 'drop') bagDropItem(uid);
}
function bagDoubleAct(payload) {
  if (payload.slot) { bagUnequip(payload.slot); return; }
  const item = bagCellItems()[payload.index];
  if (!item) return;
  if (item.cat === 'equip') bagEquipFromSlot(payload.index);
  else if (item.cat === 'usable') bagUseFromSlot(payload.index);
  else bagToast('此物只堪锻造');
}
/* ------------------ 世界图象 ------------------ */
function normalizeArt(art) {
  return {
    s: Math.min(Math.max(Math.floor(Number(art?.s)) || 0, 0), WS_SHAPES.length - 1),
    m: Math.min(Math.max(Math.floor(Number(art?.m)) || 0, 0), WS_MOONS.length - 1),
  };
}
function worldArtHTML(art) {
  const a = normalizeArt(art);
  const shape = WS_SHAPES[a.s];
  const moon = WS_MOONS[a.m];
  const moonSVG = moon.r
    ? `<g class="ws-moon"><circle cx="${moon.x}" cy="${moon.y}" r="${moon.r}"/>`
      + (moon.cr ? `<circle class="ws-moon__cut" cx="${moon.cx}" cy="${moon.cy}" r="${moon.cr}"/>` : '')
      + '</g>'
    : '';
  return '<svg class="world__sigil" viewBox="0 0 100 100" aria-hidden="true">'
    + '<circle class="ws-ring" cx="50" cy="50" r="43"/>'
    + '<circle class="ws-ring ws-ring--2" cx="50" cy="50" r="37"/>'
    + '<path class="ws-tick" d="M50 3v5M50 92v5M3 50h5M92 50h5"/>'
    + '<path class="ws-tick ws-tick--d" d="M17 17l3.4 3.4M79.6 79.6L83 83M83 17l-3.4 3.4M20.4 79.6L17 83"/>'
    + '</svg>'
    + `<svg class="world__scene s${a.s}" viewBox="0 0 160 112" preserveAspectRatio="xMidYMid meet" aria-hidden="true">`
    + moonSVG
    + '<g class="ws-cloud"><path d="M12 62c9-5 17 3 26-1s13 2 21-2"/><path d="M46 74c8-4.4 15 2.6 23-1"/></g>'
    + `<g class="ws-far"><path d="${shape.far.join('"/><path d="')}"/></g>`
    + `<g class="ws-snow"><path d="${shape.snow.join('"/><path d="')}"/></g>`
    + `<g class="ws-near"><path d="${shape.near}"/></g>`
    + '<g class="ws-fall"><path d="M77 42v61"/><path d="M71 47c3 2 3 6 0 8"/><path d="M83 52c-3 2-3 6 0 8"/></g>'
    + `<g class="ws-pine"><path d="${shape.pine}"/><path d="M30 79l-13 9h26z"/><path d="M30 68l-10 8h20z"/><path d="M30 58l-7 7h14z"/></g>`
    + '<g class="ws-water"><path d="M6 108c10-4 18 4 28 0s18 4 28 0 18 4 28 0 18 4 28 0"/><path d="M18 103c8-3 14 3 22 0s14 3 22 0"/></g>'
    + '</svg>';
}
function worldThumbHTML(art) {
  const a = normalizeArt(art);
  const shape = WS_SHAPES[a.s];
  const moon = WS_MOONS[a.m];
  return '<svg viewBox="0 0 160 112" preserveAspectRatio="xMidYMid meet" aria-hidden="true">'
    + (moon.r ? `<g class="ws-moon"><circle cx="${moon.x}" cy="${moon.y}" r="${moon.r}"/>`
      + (moon.cr ? `<circle class="ws-moon__cut" cx="${moon.cx}" cy="${moon.cy}" r="${moon.cr}"/>` : '') + '</g>' : '')
    + `<g class="ws-far"><path d="${shape.far[1]}"/></g>`
    + `<g class="ws-near"><path d="${shape.near}"/></g>`
    + '</svg>';
}
function bagWorldList() { return bagVisibleWorlds(); }
function bagCurIndex() {
  const list = bagVisibleWorlds();
  return Math.min(Math.max(bag.worldCur, 0), Math.max(0, list.length - 1));
}
function bagCurWorld() { return bagVisibleWorlds()[bagCurIndex()] || null; }
/* 当前所在的世界角色：主世界 = role 'main'，小世界 = role 'child'。 */
function bagCurrentRole() {
  return inSubworld.value ? 'child' : 'main';
}
/* 这一格是不是「现在站着的世界」。 */
function bagWorldIsCurrent(index) {
  return bagVisibleWorlds()[index]?.role === bagCurrentRole();
}
/* 点这一格会往哪个方向走：进小世界 = enter，回主世界 = exit。 */
function bagTravelDirection(index) {
  const role = bagVisibleWorlds()[index]?.role;
  if (!role || role === bagCurrentRole()) return null;
  return role === 'child' ? 'enter' : 'exit';
}
function bagTravelLabel(index) {
  const world = bagVisibleWorlds()[index];
  const name = world?.name || '';
  if (bagWorldIsCurrent(index)) return `${name}（当前所在）`;
  if (bagEntryLocked(index)) return `${name}（未辟 · 击败 Boss 并拾取世界碎片后开启）`;
  return world?.role === 'child' ? `${name}（前往小世界）` : `${name}（返回主世界）`;
}
/* 点主世界回主世界场景，点小世界进小世界场景；都走既有的换场景流程。 */
async function bagTravel(index) {
  const world = bagVisibleWorlds()[index];
  if (!world) return;
  bag.worldCur = index;  if (bagWorldIsCurrent(index)) { bagToast(`已是 · ${world.name}`); bagSave(); return; }
  if (bagEntryLocked(index)) { bagToast('击败 Boss 并拾取世界碎片后开启'); return; }
  if (navigationPending.value || navigation.busy) { bagToast('正在切换世界…'); return; }
  bagToast(bagTravelDirection(index) === 'enter' ? `入界 · ${world.name}` : `返回 · ${world.name}`);
  bagSave();
  // 换场景流程要求行囊仍开着；点击本身就是收起行囊
  const travel = navigateWorld();
  camera.resetInput();
  inventoryOpen.value = false;
  await travel;
}
function initBag() {
  bagInit();
  bag.tab = bag.tab || 'all';
  selectedSlot.value = 0;
}
const bagFootInfo = computed(bagFoot);
const bagGearMetaText = computed(() => bagGearMeta());
/* bag2.html 顶栏的「荒野代币」与四项属性总览。本项目没有货币与战斗属性系统，
   因此这里只做**现有数据的诚实聚合**，不伪造数值来源：
   - 代币：持件数与品阶权重派生的计数，四舍五入到百位，避免看起来像精确经济数值；
   - 属性：取已披挂器物的「三围」标签求和（凡品权重 1 / 良品 2 / 珍品 3 / 秘宝 4），
     再按标签归入强度·防御·共鸣·耐久四类。 */
function bagStatNumbers(item) {
  const text = (item?.stats || []).map(pair => String(pair[1])).join(' ');
  return (text.match(/\d+(?:\.\d+)?/g) || []).map(Number);
}
const bagPurseText = computed(() => {
  const items = [ ...Object.values(bag.gear).filter(Boolean), ...bagCellItems().filter(Boolean) ];
  const value = items.reduce((sum, item) => sum + RARITY[item.rarity].weight * 1370 * (item.qty || 1), 0);
  return (Math.round(value / 100) * 100).toLocaleString('en-US');
});
const bagHeroStats = computed(() => {
  const total = { atk: 0, def: 0, crit: 0, hp: 0 };
  for (const item of Object.values(bag.gear)) {
    if (!item) continue;
    const weight = RARITY[item.rarity].weight;
    total.atk += 24 * weight;
    total.def += 18 * weight;
    total.crit += 2 * weight;
    total.hp += 60 * weight;
    for (const value of bagStatNumbers(item)) {
      const scaled = Math.round(value * weight);
      // 「护体/负重/筋骨」偏防御，「锋芒/力道/术法」偏强度，「定神/回气/身法」共鸣，
      // 「破甲/疾行」等百分比条目并入强度，避免出现无归属的读数。
      const label = (item.stats || []).find(pair => String(pair[1]).includes(String(value)))?.[0] || '';
      if (/护体|负重|筋骨|抗蚀/.test(label)) total.def += scaled;
      else if (/定神|回气|身法/.test(label)) total.crit += Math.round(scaled / 4);
      else total.atk += scaled;
    }
  }
  return total;
});
const bagWorld = computed(bagCurWorld);
const bagWorldScene = computed(() => (bagWorld.value ? worldArtHTML(bagWorld.value.art) : ''));
const bagTabList = computed(() => BAG_TABS.map(tab => ({ ...tab })));
/* 格阵：所有页签都**顶格**排列 —— 先把有器物的格位按原次序排到最前，
   空格位一律补在后面，因此「全部」和各个分类里都不会有夹在中间的空洞。
   总数仍是 BAG_TOTAL 格，所以方向键游走不会塌陷。
   每格仍带自己**底层**的格位号 index：装备/服用/双击都按它取物，
   因此重排只改变显示次序，不会改变操作落到哪一件器物。 */
const bagCellList = computed(() => {
  const source = bagCellItems();
  const show = source.map(item => bagPassFilter(item));
  const hit = [];
  const miss = [];
  show.forEach((pass, index) => { (pass ? hit : miss).push(index); });
  return [ ...hit, ...miss ].map((index, pos) => {
    const item = source[index];
    const shown = show[index] ? item : null;
    return {
      index,          // 底层格位，操作按它取物，不能动
      pos,            // 显示次序
      item: shown,    // 只有可见时才带器物：显示、提示、双击都跟着走
      occupied: Boolean(item),
      filled: Boolean(shown),
      hidden: Boolean(item) && !shown,
      img: bagIconHTML(shown),
      label: shown ? bagRowName(shown) : `空格 ${pos + 1}`,
    };
  });
});
const bagVisibleCount = computed(() => bagCellItems().filter(item => bagPassFilter(item)).length);
const bagDetailInfo = computed(() => {
  const found = bagSelected();
  if (!found) return null;
  const item = found.item;
  const actions = [];
  if (item.cat === 'equip') {
    actions.push({ act: 'equip', label: found.from === 'gear' ? '卸下' : '披掛' });
  } else if (item.cat === 'usable') {
    actions.push({ act: 'use', label: '服用' });
  }
  actions.push({ act: 'drop', label: item.cat === 'equip' ? '熔去' : '弃之', ghost: true });
  return {
    item,
    uid: item.uid,
    preview: bagIconHTML(item),
    rarity: RARITY[item.rarity].label,
    typeLine: bagTypeLine(item),
    stats: item.stats || [],
    flavor: item.flavor,
    actions,
  };
});
function bagTipFoot(item) {
  if (item.real) return '来自界隙，随身所负';
  return item.cat === 'equip' ? '双击披掛 · 亦可拖至左侧'
    : item.cat === 'usable' ? '双击服用' : '锻造 / 炼药之材';
}

/* ============================================================
   小世界：世界态派生、视角切换、先知交互、物质陈列
   ============================================================ */
// 切换世界会重建本组件，因此只有切换进行中才需要保持中性，否则指示器会先主后子跳变。
const transitioning = computed(() => navigation.busy || navigationPending.value);
const inSubworld = computed(() => !transitioning.value && roleRef.value === 'child');
const worldLabel = computed(() => transitioning.value ? '切换中…'
  : inSubworld.value ? '小世界' : '主世界');
// 小世界没有 Boss、没有怒气消耗、没有攻击目标，整条战斗 HUD 隐藏而不是显示一排禁用控件。
const showCombatHud = computed(() => !inSubworld.value);
function viewLabel() {
  void viewTick.value;
  const mode = camera.viewMode?.();
  return (mode && VIEW_LABELS?.[mode]) || '第三人称';
}
// 操作提示按世界过滤：先知的 F 与碎片的 F 分属两个世界，天然互斥。
// F 这一行只由 fKeyAction 决定（同一个裁决也驱动 F 键），因此提示与实际动作不会分歧。
const controlHints = computed(() => [
  ['WASD', '移动'],
  ['鼠标', '转向 · 滚轮缩放'],
  ['Space', '跳远'],
  ['Shift', '长按跑步 · 点按闪避'],
  ...(showCombatHud.value ? [['左键', '攻击'], ['E / R', '技能']] : []),
  ...(fKeyAction.value.prompt ? [['F', fKeyAction.value.prompt]] : []),
  ['V', `视角 · ${viewLabel()}`],
  ['Tab', '背包'],
  ['Esc', inSubworld.value ? '保存离开' : '保存退出'],
]);
const hotbarSlots = computed(() => {
  void viewTick.value;
  return hotbarSlotsFor(gameplayState.value?.state?.inventory, placements.value);
});
// 物品栏固定 8 格：可摆放的物质占前几格，其余是空格子。空格也能像 Minecraft 一样被选中，
// 只是按下 B 什么都不会放下。物品栏只在「小世界 + 没有任何面板」时存在；滚轮换格与模板
// v-if 共用这一条，避免两处漂移。
const hotbarSelected = computed(() => hotbarSlots.value[hotbarIndex.value] || null);
const hotbarVisible = computed(() => inSubworld.value
  && !inventoryOpen.value && !dialogueOpen.value && !adjustMode.value);
// 先知是引擎侧 actor，包围盒每次实时算：缓存会按“曾经站的位置”继续响应。
const prophetBounds = () => worldBounds(prophetActor);

function cycleView() {
  if (!cameraReady || navigation.busy || savingPlayer || initializing.value) return;
  const label = viewLabel();
  camera.cycleViewMode?.();
  viewTick.value++;
  showToast(`视角：${label} → ${viewLabel()}（V 键切换）`);
}
function openDialogue() {
  if (currentInteraction().kind !== 'prophet' || dialogueOpen.value) return;
  camera.resetInput();
  dialogueLine.value = 0;
  dialogueOpen.value = true;
  void nextTick(() => dialoguePanel?.focus());
}
function closeDialogue() {
  if (!dialogueOpen.value) return;
  // 关掉整个面板时不许只退一级：面板再打开时必须回到主选项，而不是停在子屏上。
  adjustFragmentId.value = null;
  prophetScreen.value = null;
  actionFeedback.value = '';
  exitAdjustMode({ reopenDialogue: false });
  dialogueOpen.value = false;
  dialogueLine.value = 0;
  // 立刻重新计算邻近提示；此处玩家没有移动，否则下一次 F 会被判为“不在范围内”。
  updateProximity();
  void nextTick(() => surface.value?.focus());
}
// Enter/Space 推进剧本；最后一句直接结束，避免玩家被困在面板里。
function advanceDialogue() {
  if (!dialogueOpen.value) return;
  if (dialogueLine.value + 1 < dialogue.lines.length) dialogueLine.value++;
  else closeDialogue();
}
// F 的含义由 interactionFor() 唯一决定，与上面那行操作提示同源（提示读缓存、按键现算，
// 但两者是同一个函数）。两者都不在范围内时 F 什么都不做——面板打开时提示已消失，
// 同样落在这里。
function interact() {
  const { kind } = currentInteraction();
  if (kind === 'prophet') openDialogue();
  else if (kind === 'pickup') void runAction(gameplay.pickup);
  else if (kind === 'block') void retrievePlacedBlock();
}

// ---- 「先知」旁的物体与世界规则（李淳珺任务）-----------------------------------
// The cube is a game-owned prop the prophet lets the player scale or move; the rule
// engine animates the small world once a fragment is installed. Both live in their own
// modules, so this page only wires them up: it renders the menu and applies what the
// pure functions return.
const installedRules = ref([]);
const actionFeedback = ref('');
// The saved records only say which fragments are installed; the semantics come from the
// catalogue, so a rewrite of a rule never invalidates an existing world. The numbers are the
// exception:「调整碎片」writes them into the record, and the running rule has to read them back
// from there — otherwise the saved change would never reach the frame loop.
const resolvedRules = computed(() => installedRules.value
  .map(record => {
    const rule = findFragment(record?.fragmentId)?.rule;
    if (!rule) return null;
    return { ...rule,
      amplitude: typeof record.amplitude === 'number' ? record.amplitude : rule.amplitude,
      periodMs: typeof record.periodMs === 'number' ? record.periodMs : rule.periodMs };
  }).filter(Boolean));
// The panel has two main options. «碎片» is a screen inside the panel; «调整物体» leaves the
// panel for the overhead editing mode, and leaving that mode reopens the panel.
// «调整碎片» is a sub-screen of «碎片»: it edits one installed fragment's numbers.
const prophetScreen = ref(null);
const adjustMode = ref(false);
const previousViewMode = ref(null);
const adjustSelection = ref(null);
// Which fragment the «调整碎片» sub-screen is editing; null whenever that screen is closed.
const adjustFragmentId = ref(null);
const fragmentList = computed(() => fragmentPanel(installedRules.value));
const fragmentAdjust = computed(() => fragmentAdjustPanel(installedRules.value,
  adjustFragmentId.value));
const adjustHint = computed(() => (adjustSelection.value
  ? `已选中：${adjustSelection.value.name}`
  : '俯瞰视角：点击物体选中它，再用下面的按钮调整'));
function enterAdjustMode() {
  actionFeedback.value = '';
  prophetScreen.value = null;
  adjustFragmentId.value = null;
  adjustSelection.value = null;
  // The overhead view is what makes the arrangement readable; remember where to return.
  previousViewMode.value = camera.viewMode?.() || null;
  camera.setViewMode?.('top');
  adjustMode.value = true;
  dialogueOpen.value = false;
  camera.resetInput();
  void nextTick(() => surface.value?.focus());
  void loadSelectableObjects();
}
// The list is the reliable way to choose an object: clicking a name cannot miss, while
// clicking a pixel depends on the engine's readback settling.
const selectableList = ref([]);
const isSelected = entry => Boolean(adjustSelection.value
  && adjustSelection.value.guid === entry?.guid);
async function loadSelectableObjects() {
  try {
    selectableList.value = selectableObjects(await editorApi.scene.getSnapshot(sceneId));
  } catch (error) {
    selectableList.value = [];
    actionFeedback.value = `读取物体列表失败：${error.message}`;
  }
}
// 选择只由名字列表驱动：视口里不做拾取，因此这里只用列表项自身携带的字段。
function selectObjectFromList(entry) {
  adjustSelection.value = { guid: entry.guid, name: entry.name,
    actor: { actor_guid: entry.guid, name: entry.name, geometry: entry.transform },
    transform: entry.transform };
  actionFeedback.value = `已选中：${entry.name}`;
}
function exitAdjustMode({ reopenDialogue = true } = {}) {
  if (!adjustMode.value) return;
  if (previousViewMode.value) camera.setViewMode?.(previousViewMode.value);
  previousViewMode.value = null;
  adjustMode.value = false;
  adjustSelection.value = null;
  actionFeedback.value = '';
  camera.resetInput();
  if (reopenDialogue) {
    // Back to the conversation, positioned on the last line so the script is not replayed.
    dialogueOpen.value = true;
    dialogueLine.value = Math.max(0, dialogue.lines.length - 1);
    void nextTick(() => dialoguePanel?.focus());
  }
}
function openProphetScreen(id) {
  if (id === 'adjust') { enterAdjustMode(); return; }
  actionFeedback.value = '';
  adjustFragmentId.value = null;
  prophetScreen.value = id;
}
// 「调整」在碎片行上：进子屏，记下改的是哪一块。没装填的碎片也进得去，子屏会说明要先装填，
// 这样按 1-4 得到的反馈与点击一致，而不是一个没有反应的按钮。
function openFragmentAdjust(entry) {
  adjustFragmentId.value = entry?.id || null;
  prophetScreen.value = 'fragment-adjust';
  actionFeedback.value = entry?.installed ? '' : '先装填该碎片再调整';
}
// Escape 与「返回」都只退一级：调整碎片 → 碎片 → 主选项。
function leaveProphetScreen() {
  if (prophetScreen.value === 'fragment-adjust') {
    adjustFragmentId.value = null;
    actionFeedback.value = '';
    prophetScreen.value = 'fragments';
    return;
  }
  adjustFragmentId.value = null;
  prophetScreen.value = null;
  actionFeedback.value = '';
}
const worldRuleRunner = createWorldRuleRunner({
  bridge: () => (typeof window === 'undefined' ? null : window.coronaBridge),
  onError: error => showFeedback(`世界规则已停止：${error.message}`),
});
function rememberCube(actor) {
  cubeActor = actor || null;
  cubeTransform = cubeTransformOf(cubeActor);
  return cubeTransform;
}
// Rebuild the animated set from a fresh snapshot: reading the scene is an asynchronous
// CEF round trip, so the frame loop only replays the handles captured here. Passing no
// snapshot stops the rule, which is what the main world wants.
function applyWorldRules(snapshot) {
  worldRuleRunner.stop();
  if (!snapshot || !inSubworld.value || !resolvedRules.value.length) return false;
  // One shared target list for every installed rule: a world may have sway and spin at the
  // same time, and the runner composes them into a single write per actor per frame.
  worldRuleRunner.setTargets(collectSwayTargets(snapshot));
  worldRuleRunner.setRules(resolvedRules.value);
  return worldRuleRunner.start();
}
/** Read one actor's transform back from the engine, to confirm that a write was kept. */
async function readBackTransform(guid) {
  const actors = sceneSnapshot(await editorApi.scene.getSnapshot(sceneId))?.actors;
  const actor = (Array.isArray(actors) ? actors : []).find(entry => entry.actor_guid === guid);
  return actor ? cubeTransformOf(actor) : null;
}
async function runProphetAction(chosen) {
  // Adjusting deliberately leaves the dialogue, so the guard has to accept either context.
  // Gating only on `dialogueOpen` silently dropped every adjustment action.
  if ((!dialogueOpen.value && !adjustMode.value) || !chosen) return;
  if (chosen.type === 'open-screen') { openProphetScreen(chosen.screen); return; }
  actionFeedback.value = '';
  if (chosen.type === 'toggle-fragment') {
    await runAction(async () => {
      const result = toggleFragment(installedRules.value, chosen.fragmentId);
      const saved = await gameplay.saveWorldRules(result.rules);
      installedRules.value = Array.isArray(saved?.rules) ? saved.rules : result.rules;
      actionFeedback.value = result.feedback;
      // A snapshot is what teaches the rule which objects to animate.
      applyWorldRules(await editorApi.scene.getSnapshot(sceneId));
    });
    return;
  }
  // 「调整碎片」：改的是已装填记录里的数字，因此与装填/拆下走完全同一条路径——先落盘，
  // 再用新快照重启规则引擎，改完立刻在场景里看得到。
  if (chosen.type === 'adjust-fragment') {
    const fragmentId = chosen.fragmentId || adjustFragmentId.value;
    // 纯函数先判一次：未装填、已到边界、未知动作都只改提示、一个字节也不写盘。放在
    // runAction 之外，是为了让这句话不受「必须有焦点」的限制，玩家一定看得到。
    const preview = adjustFragmentRule(installedRules.value, fragmentId, chosen.id);
    if (!preview.changed) {
      actionFeedback.value = preview.feedback;
      return;
    }
    await runAction(async () => {
      const result = adjustFragmentRule(installedRules.value, fragmentId, chosen.id);
      if (!result.changed) { actionFeedback.value = result.feedback; return; }
      const saved = await gameplay.saveWorldRules(result.rules);
      installedRules.value = Array.isArray(saved?.rules) ? saved.rules : result.rules;
      actionFeedback.value = result.feedback;
      applyWorldRules(await editorApi.scene.getSnapshot(sceneId));
    });
    return;
  }
  // An adjustment acts on whatever the player picked from the list; until then, on the cube.
  const target = adjustSelection.value?.actor || cubeActor;
  const current = cubeTransformOf(target)
    || (adjustSelection.value ? adjustSelection.value.transform : cubeTransform);
  if (!target || !current || !prophetActor || !playerActor) {
    actionFeedback.value = '物体尚未就绪，请先从列表中选择一个物体';
    return;
  }
  await runAction(async () => {
    const next = nextObjectTransform(chosen.id, { current, reference: prophetActor,
      facingYaw: playerFacingYaw(playerActor) });
    if (!next) return;
    // The light path: the setter does not re-import the resource, so it cannot add mesh work
    // to the geometry thread while the renderer is submitting. `persist: true` is explicit
    // because a runtime transform that is never persisted is exactly what made an earlier
    // adjustment look like it "did nothing"; the engine's own default is not relied on.
    const data = unwrap(await editorApi.scene.setActorTransform(sceneId, target.actor_guid,
      { ...next, persist: true }));
    if (!data || data.ok === false || !['success', 'loaded'].includes(data.status)) {
      throw new Error(data?.message || data?.diagnostics?.[0]?.message || '引擎未确认调整');
    }
    const actor = data.actor || { ...target, geometry: next };
    if (target === cubeActor) rememberCube(actor);
    if (adjustSelection.value) adjustSelection.value = { ...adjustSelection.value, actor, transform: next };
    // Re-anchor the sway, otherwise the next frame would undo the player's own move.
    worldRuleRunner.refreshBase(target.actor_guid, next.position);
    // Confirm against the engine instead of our own optimism. Reading the actor back is the
    // only way to separate "the engine refused the write" from "the engine took it but the
    // change is not on screen" — and those two need completely different fixes.
    const kept = (await readBackTransform(target.actor_guid))?.scale?.[0];
    const summary = adjustFeedback(chosen.id, next, prophetActor);
    actionFeedback.value = Number.isFinite(kept) && Math.abs(kept - next.scale[0]) < 1e-3
      ? `${summary}（引擎已接受）`
      : `${summary}｜引擎未保留，实际缩放 ${Number.isFinite(kept) ? kept.toFixed(2) : '读不到'}`;
  });
}
// 布局按序号记录，序号取最小空位，删一件不会让其它件重编号。
const placementSignature = list => JSON.stringify(sanitizePlacements(list)
  .map(entry => [placementGuid(entry.index), entry.propId, ...entry.position, ...entry.rotation, entry.scale]));
async function placementStep() {
  await gameplay.savePlacements(placements.value);
  if (!current()) return false;
  // 成员变化是 reconcile() 的确认输入，场景工作交给它统一处理。
  await gameplay.flush();
  return true;
}
/** 物品栏选格子：点击与数字键共用；8 格全在可选范围内（空格也可选中），越界一律忽略。 */
function selectHotbarSlot(index) {
  if (!Number.isInteger(index) || index < 0 || index >= hotbarSlots.value.length) return false;
  hotbarIndex.value = index;
  return true;
}
/** 滚轮在小世界里换物品栏格子（含空格）；换格规则由 storyProps.cycleSlot 决定（双向环绕）。 */
function cycleHotbar(delta) {
  const next = cycleSlot(hotbarIndex.value, hotbarSlots.value.length, delta);
  if (next === hotbarIndex.value) return false;
  hotbarIndex.value = next;
  return true;
}
/**
 * 视口滚轮只有一个入口：物品栏可见时换格且不缩放相机，其余情况（主世界、任何面板打开）
 * 仍交给相机缩放。preventDefault 已由模板上的 @wheel.prevent 完成，这里不再重复。
 */
function onSurfaceWheel(event) {
  if (!hotbarVisible.value) return camera.wheel(event);
  if (focused && !document.hidden) cycleHotbar(event.deltaY);
  return true;
}
/**
 * 直接摆放：把选中的一件物质放在玩家前方的地面上，不依赖任何固定陈列点。
 * 成功路径必须经过 sanitizePlacements + nextPlacementIndex + placementStep，
 * 场景里的 actor 由 reconcile() 的既有 watcher 负责创建。
 */
async function placeSelectedItem() {
  if (!inSubworld.value) { showToast('只能在小世界里摆放方块'); return; }
  if (!current() || navigation.busy || navigationPending.value || savingPlayer || actionBusy.value
    || gameplay.busy || gameplay.needsSave) {
    showToast('当前无法放置，请稍候');
    return;
  }
  const selected = hotbarSelected.value;
  if (!selected) { showToast('请先在物品栏中选择一件物品'); return; }
  // 选中的是空格子：与 Minecraft 一样什么也不放下，只说明原因。
  if (selected.empty || !selected.id) { showToast('这一格是空的'); return; }
  if (selected.available <= 0) {
    showToast(selected.id === 'block' ? '方块已用完' : `${selected.name}已用完`);
    return;
  }
  if (nextPlacementIndex(placements.value) >= MAX_PLACEMENTS) {
    showToast(`小世界已放满 ${MAX_PLACEMENTS} 件，无法继续摆放`);
    return;
  }
  // facingYaw 是逻辑朝向，与先知摆放方块用的是同一个约定。
  const player = camera.snapshotPlayer();
  let spawn;
  try {
    spawn = blockSpawnTransform(player, player?.facingYaw, PLACEMENT_DISTANCE, groundYAt);
  } catch (error) {
    showToast(`无法确定摆放位置：${error.message}`);
    return;
  }
  // 布局的改动放在 runAction 里：它还有自己的输入闸门，被拦下时不能留下没保存的幽灵摆放。
  await runAction(async () => {
    placements.value = sanitizePlacements([...placements.value, {
      propId: selected.id, index: nextPlacementIndex(placements.value), siteId: spawn.siteId,
      position: spawn.position, rotation: spawn.rotation, scale: spawn.scale,
    }]);
    try {
      await placementStep();
      showToast(`已摆放 ${selected.name} · 前方 ${spawn.position[0].toFixed(1)}, ${spawn.position[2].toFixed(1)}`);
    } catch (error) {
      // 后端是权威：失败时重新读取，而不是相信本地的错误猜测。
      placements.value = sanitizePlacements(gameplay.placements?.placements);
      throw error;
    }
  });
}
/**
 * 取回：摆放的逆操作，只能在小世界里做。裁决（interactionFor）已经判定 kind === 'block'，
 * 这里按同一条规则再算出最近的那一件——判定与动作读的是同一个 nearestPlacedBlock 与同一个
 * PLACED_BLOCK_REACH，因此不可能出现「提示说取回、F 却去拿另一件」。
 *
 * 成功路径必须经过 removePlacement + placementStep：布局是权威来源，删掉一项之后
 * reconcile() 里既有的陈列 watcher 会走 syncPlacementActors，把对应 guid 的 actor 从场景里移除；
 * 物品栏的 available 也随之回到摆放前的数字。这里不另开一条撤下路径。
 */
async function retrievePlacedBlock() {
  if (!inSubworld.value) { showToast('只能在小世界里取回方块'); return; }
  if (!current() || navigation.busy || navigationPending.value || savingPlayer || actionBusy.value
    || gameplay.busy || gameplay.needsSave) {
    showToast('当前无法取回，请稍候');
    return;
  }
  const target = nearestPlacedBlock(camera.snapshotPlayer(), placements.value, PLACED_BLOCK_REACH);
  if (!target) return;
  const name = findProp(target.propId)?.name || '方块';
  await runAction(async () => {
    placements.value = removePlacement(placements.value, target.index);
    try {
      await placementStep();
      showToast(`已取回 ${name} · 第 ${target.index + 1} 件`);
    } catch (error) {
      placements.value = sanitizePlacements(gameplay.placements?.placements);
      throw error;
    }
  });
}

const saveRegistration = registerWorldSessionSave(async () => {
  camera.resetInput();
  // Keep input locked until the actual acknowledgement, even after a UI timeout.
  // A retry must not release the scene with movement newer than the pending save.
  savingPlayer = true;
  try {
    // Initialization owns its error UI; finish any already-submitted native call.
    await initialization.catch(() => {});
    await gameplay.flush();
    await playerSave?.save();
    if (current()) gameplayError.value = '';
  } catch (error) {
    if (current()) gameplayError.value = error.message || '保存失败，请重试';
    throw error;
  } finally { savingPlayer = false; }
});
async function saveBeforeLeave() {
  if (exitPending) return exitPending;
  leaving = true;
  camera.resetInput();
  exitPending = (async () => {
    try {
      await saveRegistration.flush();
      saveRegistration.release();
      return true;
    } catch (error) {
      leaving = false;
      if (current()) gameplayError.value = error.message || '保存失败，请重试';
      if (current() && gameplay.data) {
        gameplayState.value = gameplay.data;
        updateProximity();
      }
      notifyWorldError(error, '保存剧情进度失败');
      return false;
    } finally { exitPending = null; }
  })();
  return exitPending;
}
onBeforeRouteLeave(saveBeforeLeave);
async function exitStory() {
  navigation.interrupt();
  if (!await saveBeforeLeave()) return;
  navigation.cancel();
  camera.dispose();
  await router.replace('/StartScreen');
}
const navigation = createStoryNavigationController({
  projectPath,
  isReady: () => cameraReady && focused && !document.hidden && !savingPlayer,
  isSourceCurrent: current,
  getSelectionVersion: getProjectSelectionVersion,
  readSession: () => worldModeState,
  resetInput: camera.resetInput,
  flushCamera: async () => {
    await saveRegistration.flush();
    const pose = camera.snapshotPose();
    if (!pose?.cameraName) throw new Error('当前相机尚未绑定，无法保存视角');
    const result = unwrap(await editorApi.viewport.setCameraPose(pose.sceneId, pose.cameraName, pose.camera));
    if (result?.status !== 'success') throw new Error(result?.message || '保存相机视角失败');
  },
  prepare: direction => editorApi.scratch.sendKeyEvent(NAVIGATION_KEY, '', JSON.stringify({ projectPath, direction })),
  trackPreparation: trackWorldSessionWork,
  openProject: path => projectLauncherService.openProject(path),
  cancelProjectOpen: cancelPendingProjectOpen,
  leave: () => router.replace('/StartScreen'),
  notify: error => { if (current()) gameplayError.value = error.message; notifyWorldError(error); },
});
function onKeyDown(event) {
  if (!current()) return;
  // The adjustment mode owns the keyboard outright: only the adjustment steps and the way out
  // get through, so a stray key cannot cast a skill or swing while the player is arranging.
  if (adjustMode.value) {
    event.preventDefault();
    event.stopPropagation();
    if (event.code === 'Escape' || event.key === 'Escape') {
      if (!event.repeat) exitAdjustMode();
      return;
    }
    if (!event.repeat) {
      const chosen = prophetKeyAction(event.key, 'adjust');
      if (chosen) void runProphetAction(chosen);
    }
    return;
  }
  // 对话面板打开时独占键盘：Esc 不再退出世界，F/Space 也不会漏给相机或其它交互。
  if (dialogueOpen.value) {
    event.preventDefault();
    event.stopPropagation();
    if (event.code === 'Escape' || event.key === 'Escape') {
      // Escape walks back one level before it closes the panel.
      if (!event.repeat) {
        if (prophetScreen.value) leaveProphetScreen();
        else closeDialogue();
      }
      return;
    }
    // 「碎片」屏里 Enter/Space 不再推进剧本，数字键才是选择键。「调整碎片」子屏同理。
    if (!prophetScreen.value && !event.repeat
      && ['Enter', 'Space', 'NumpadEnter'].includes(event.code)) advanceDialogue();
    if (!event.repeat) {
      const chosen = prophetKeyAction(event.key, prophetScreen.value, adjustFragmentId.value);
      if (chosen) void runProphetAction(chosen);
    }
    return;
  }
  // 物品栏：调整模式与对话（上面两个分支）已经吃掉数字键，因此这里只在
  // 小世界、面板关闭时接管数字键与 B。主世界里这套键完全不生效。
  if (inSubworld.value && !inventoryOpen.value && !event.ctrlKey && !event.altKey
    && !event.metaKey && !event.isComposing) {
    if (/^[1-9]$/.test(event.key || '')) {
      event.preventDefault(); event.stopPropagation();
      if (!event.repeat) selectHotbarSlot(Number(event.key) - 1);
      return;
    }
    if (event.code === 'KeyB' || event.key?.toLowerCase() === 'b') {
      event.preventDefault(); event.stopPropagation();
      if (!event.repeat) void placeSelectedItem();
      return;
    }
  }
  if (event.code === 'Escape' || event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    if (navigation.busy) { if (!event.repeat) void exitStory(); }
    else if (inventoryOpen.value) toggleInventory();
    else if (!event.repeat) void exitStory();
    return;
  }
  if (event.ctrlKey || event.altKey || event.metaKey || event.isComposing) return;
  if (event.code === 'Tab' || event.key === 'Tab') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused && !document.hidden && cameraReady && !navigation.busy && !savingPlayer) toggleInventory();
    return;
  }
  if (inventoryOpen.value) {
    if (!['Enter', 'Space'].includes(event.code)) event.preventDefault();
    event.stopPropagation(); return;
  }
  if (event.code === 'KeyV' || event.key?.toLowerCase() === 'v') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat) cycleView();
    return;
  }
  if (event.code === 'KeyF' || event.key?.toLowerCase() === 'f') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused) interact();
    return;
  }
  if ((event.code === 'Space' || event.key === ' ' || event.key === 'Spacebar')
    && (actionBusy.value || gameplay.busy || gameplay.needsSave)) {
    event.preventDefault(); event.stopPropagation(); return;
  }
  if (event.code === 'KeyE' || event.code === 'KeyR' || ['e', 'r'].includes(event.key?.toLowerCase())) {
    event.preventDefault(); event.stopPropagation();
    const skill = event.code === 'KeyE' || event.key?.toLowerCase() === 'e' ? 'heavy' : 'sweep';
    if (!event.repeat && focused) void runAction(() => gameplay.castSkill(skill));
    return;
  }
  if (focused && camera.keyDown(event)) event.stopPropagation?.();
}
function onBlur() { focused = false; camera.resetInput(); }
function onFocus() { focused = !document.hidden; }
function onVisibilityChange() {
  if (document.hidden) onBlur();
  else focused = document.hasFocus();
}
function watchDpi() {
  dpiQuery?.removeEventListener('change', onDpiChanged);
  dpiQuery = window.matchMedia?.(`(resolution: ${window.devicePixelRatio}dppx)`);
  dpiQuery?.addEventListener('change', onDpiChanged);
}
function onDpiChanged() { camera.syncViewport(); watchDpi(); }
function onPointerDown(event) {
  if (inventoryOpen.value) return;
  surface.value?.focus();
  // While adjusting, the click that focuses the viewport must not be swallowed by the gate.
  if (!focused && !adjustMode.value) return;
  if (event.button !== 0) { camera.pointerDown(event); return; }
  event.preventDefault();
  // Adjusting is label-driven: the left button neither attacks nor picks an object, it only
  // focuses the viewport so dragging and the wheel still let the player look at the object.
  if (adjustMode.value) return;
  void runAction(gameplay.attack);
}

onMounted(async () => {
  // Escape must work even while the native scene/snapshot request is pending.
  document.addEventListener('keydown', onKeyDown);
  document.addEventListener('focusin', onInventoryFocus);
  cooldownTimer = setInterval(() => { cooldownTick.value++; }, 100);
  initBag();
  window.addEventListener('blur', onBlur);
  window.addEventListener('focus', onFocus);
  document.addEventListener('visibilitychange', onVisibilityChange);
  initialization = trackWorldSessionWork((async () => {
    const init = unwrap(await editorApi.main.onInit());
    if (!current()) return;
    const scenes = init?.scenes || [];
    const index = Math.min(Math.max(Number(init?.active_index) || 0, 0), Math.max(0, scenes.length - 1));
    sceneId = scenes[index]?.path || init?.path;
    if (!sceneId) throw new Error('当前世界没有可用场景');
    // 地图高度网格是这张地图真正的地面线，而角色的脚 y 由它给出：必须在
    // ensureStoryCharacters 之前读入，否则角色先按固定地板落地，之后才补网格也来不及。
    // 读不到只报告、不阻断进入世界（`heightAt` 返回 null，摆位与控制器都退回固定地板）。
    try {
      await loadStoryMapHeight(mapHeightUrl(window.location.href));
    } catch (error) {
      actionFeedback.value = `地图高度网格未能读取：${error.message}`;
    }
    if (!current()) return;
    const loadedGameplay = await gameplay.load();
    if (!current()) return;
    lastHitHp = loadedGameplay.state.boss.hp;
    lastHitRage = loadedGameplay.state.rage;
    lastHitRevision = loadedGameplay.state.revision;
    // 主世界读到的是空布局，因此这里在两个世界都是安全的；
    // 必须在 ensureStoryCharacters 之前取回，陈列物才能在同一次渲染中落地。
    placements.value = sanitizePlacements(gameplay.placements?.placements);
    const { snapshot, player, targetOffset } = await ensureStoryCharacters({
      api: editorApi, sceneId, frontendUrl: window.location.href, gameplay: loadedGameplay, assertSource, projectPath,
      isCurrent: () => current() && worldModeState.projectPath === projectPath,
    });
    if (!current() || worldModeState.projectPath !== projectPath) return;
    // 地图是这条故事线的地面：与角色、方块一样在进入世界时由前端确认在位。
    // 不按角色分叉——主世界要有它，小世界是主场景的副本因此自动继承同一份；
    // 没有它角色只会站在空气里，所以这里不设 role 门槛。
    // 与方块同样的处理方式：失败只报告，不阻断进入世界。
    try {
      await ensureStoryMap({
        api: editorApi, sceneId, frontendUrl: window.location.href, resolveAsset: resolveStoryAssetPath,
      });
    } catch (error) {
      actionFeedback.value = `地图未能就位：${error.message}`;
    }
    if (!current() || worldModeState.projectPath !== projectPath) return;
    // Look the boss up by role: array position is not an identity, and inserting a
    // character would otherwise silently point this at whoever slid into slot 1.
    bossActor = snapshot.actors?.find(actor =>
      actor.actor_guid === STORY_CHARACTERS.find(character => character.role === 'boss').guid) || null;
    bossBounds = worldBounds(bossActor);
    // 先知只在小世界按需生成，主世界里缺失是正常的：交互保持不可用即可。
    prophetActor = snapshot.actors?.find(actor => actor.actor_guid === PROPHET_GUID) || null;
    playerActor = player;
    // 先知旁的方块，以及这个小世界已经装填的世界规则（李淳珺任务）：
    // 主世界没有先知，方块只隐藏不创建。方块只是摆设：创建失败必须仍能进世界并照常对话，
    // 因此这里只报告、不抛出。
    try {
      rememberCube(prophetActor && playerActor ? await ensureStoryCube({
        api: editorApi, sceneId, frontendUrl: window.location.href,
        role: loadedGameplay?.role, prophetActor, playerActor, resolveAsset: resolveStoryAssetPath,
        // 方块与角色同一个地面来源：地图的烘好高度网格（没有网格时退回原来的高度）。
        groundYAt,
      }) : null);
    } catch (error) {
      rememberCube(null);
      actionFeedback.value = `物体未能创建：${error.message}`;
    }
    installedRules.value = Array.isArray(gameplay.worldRules?.rules) ? gameplay.worldRules.rules : [];
    // 浮动需要重新读一次场景，才能把刚创建的方块也纳入；规则起不来同样不致命。
    try {
      applyWorldRules(installedRules.value.length && inSubworld.value
        ? await editorApi.scene.getSnapshot(sceneId) : null);
    } catch (error) {
      worldRuleRunner.stop();
      actionFeedback.value = `世界规则未能启动：${error.message}`;
    }
    visualSignature = signature(loadedGameplay);
    visualPlacements = placementSignature(placements.value);
    cameraReady = camera.bind(snapshot, sceneId);
    camera.bindPlayer(player, targetOffset);
    updateProximity();
    playerSave = createPlayerSave({ api: editorApi, sceneId,
      readPlayer: camera.snapshotPlayer, stopInput: camera.resetInput,
      assertSource, onSaved: camera.acknowledgePlayerSave });
    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(camera.syncViewport);
      resizeObserver.observe(surface.value);
    }
    window.addEventListener('resize', camera.syncViewport);
    watchDpi();
    window.addEventListener('pointermove', camera.pointerMove);
    window.addEventListener('pointerup', camera.pointerUp);
    window.addEventListener('pointercancel', camera.resetInput);
    document.addEventListener('keyup', camera.keyUp);
    initializing.value = false;
    if (focused) surface.value?.focus();
  })());
  try { await initialization; } catch (error) {
    if (!current()) return;
    await router.replace('/StartScreen');
    window.alert(error.message || '剧情世界加载失败');
  }
});
onUnmounted(() => {
  disposed = true;
  clearTimeout(feedbackTimer);
  clearTimeout(toastTimer);
  clearTimeout(bagToastTimer);
  clearInterval(cooldownTimer);
  document.removeEventListener('focusin', onInventoryFocus);
  cameraReady = false;
  navigation.dispose();
  // 离开世界不能把俯瞰视角留下来：否则存进场景的就是这个非常规机位。
  exitAdjustMode({ reopenDialogue: false });
  worldRuleRunner.dispose();
  camera.dispose();
  saveRegistration.retire();
  resizeObserver?.disconnect();
  window.removeEventListener('resize', camera.syncViewport);
  window.removeEventListener('blur', onBlur);
  window.removeEventListener('focus', onFocus);
  dpiQuery?.removeEventListener('change', onDpiChanged);
  window.removeEventListener('pointermove', camera.pointerMove);
  window.removeEventListener('pointerup', camera.pointerUp);
  window.removeEventListener('pointercancel', camera.resetInput);
  document.removeEventListener('keydown', onKeyDown);
  document.removeEventListener('keyup', camera.keyUp);
  document.removeEventListener('visibilitychange', onVisibilityChange);
});
</script>

<template>
  <div ref="surface" class="story-world-viewport" tabindex="0" data-story-viewport
    @pointerdown="onPointerDown" @pointerleave="camera.resetInput" @wheel.prevent="onSurfaceWheel" @contextmenu.prevent>
    <svg class="hud-symbols" aria-hidden="true" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <g id="i-heal"><polygon points="24,4 40,24 24,44 8,24" /><circle cx="24" cy="24" r="6" /></g>
        <g id="i-burst"><circle cx="24" cy="24" r="18" stroke-dasharray="6 6"/><polygon points="24,10 32,28 16,28"/></g>
        <g id="i-stasis"><circle cx="24" cy="24" r="16" /><line x1="18" y1="14" x2="18" y2="34"/><line x1="30" y1="14" x2="30" y2="34"/></g>
        <g id="i-shield"><polygon points="24,6 40,16 34,36 24,44 14,36 8,16" /><line x1="24" y1="16" x2="24" y2="44"/></g>
        <g id="i-mirage"><rect x="14" y="14" width="20" height="20" rx="2"/><rect x="8" y="8" width="20" height="20" rx="2" opacity="0.4"/><rect x="20" y="20" width="20" height="20" rx="2" opacity="0.4"/></g>
      </defs>
    </svg>
    <div v-if="initializing" class="story-loading" role="status">{{ gameplayError || '剧情世界加载中…（Esc 取消）' }}</div>
    <div v-if="gameplayState && !initializing" class="story-hud" :class="{ 'is-dim': inventoryOpen }"
      :aria-label="inSubworld ? '小世界界面' : '战斗界面'">
      <!-- 顶部：首领血条 -->
      <section v-if="bossNearby && !inventoryOpen && showCombatHud" class="boss-wrap" aria-label="Boss 血条">
        <div class="boss-header">
          <div class="boss-title">
            <span class="boss-badge">DANGER</span>
            <h2 class="boss-name">荒野巨兽 · 远古聚合体</h2>
          </div>
          <div class="boss-val"><b>{{ gameplayState.state.boss.hp }}</b> / <span>{{ gameplayState.config.bossHp }}</span></div>
        </div>
        <div class="boss-bar-box" role="progressbar" aria-label="Boss 生命"
          :aria-valuenow="gameplayState.state.boss.hp" :aria-valuemax="gameplayState.config.bossHp" aria-valuemin="0">
          <div class="boss-bar-lag" :style="{ width: `${bossPercent()}%` }"></div>
          <div class="boss-bar-fill" :style="{ width: `${bossPercent()}%` }"></div>
        </div>
      </section>

      <!-- 左上角：自然写实风格复合结构 -->
      <div v-if="!inventoryOpen && showCombatHud" class="vitals-container">
        <div class="soul-frame-group">
          <!-- 原型是「荒野修复剂」，引擎侧对应怒气：满怒即可释放重斩 -->
          <div class="gourd-hub" title="怒气槽 · 满怒可释放重斩">
            <span class="gourd-key">{{ heavySkill?.key || 'E' }}</span>
            <svg class="gourd-icon" viewBox="0 0 48 48">
              <polygon points="24,4 40,24 24,44 8,24" fill="rgba(230,213,167,.28)" />
              <polygon points="24,4 40,24 24,44 8,24" />
              <circle cx="24" cy="24" r="6" fill="rgba(230,213,167,.5)" />
              <circle cx="24" cy="24" r="6" />
            </svg>
            <span class="gourd-count">{{ gameplayState.state.rage }}</span>
          </div>
          <div class="sub-slot-mini" aria-hidden="true">
            <svg viewBox="0 0 24 24"><path d="M4 19.5v-15A2.5 2.5 0 0 1 6.5 2H20v20H6.5a2.5 2.5 0 0 1-2.5-2.5Z"/></svg>
          </div>
        </div>

        <div class="vitals-bars">
          <div class="meter-row">
            <div class="meter-track meter-hp" role="progressbar" aria-label="生命"
              :aria-valuenow="gameplayState.config.playerHp" :aria-valuemax="gameplayState.config.playerHp" aria-valuemin="0">
              <div class="meter-lag" style="width:100%"></div>
              <div class="meter-fill" style="width:100%"></div>
            </div>
            <span class="meter-label">HP</span>
            <span class="meter-val">{{ gameplayState.config.playerHp }}</span>
          </div>
          <div class="meter-row">
            <div class="meter-track meter-sp" role="progressbar" aria-label="怒气"
              :aria-valuenow="gameplayState.state.rage" :aria-valuemax="gameplayState.config.rageMax" aria-valuemin="0">
              <div class="meter-lag" :style="{ width: `${ragePercent()}%` }"></div>
              <div class="meter-fill" :style="{ width: `${ragePercent()}%` }"></div>
            </div>
            <span class="meter-label">SP</span>
            <span class="meter-val">{{ gameplayState.state.rage }}</span>
          </div>
          <!-- 原型第三条是 EP；引擎暂无该资源，沿用耐力槽的读数以免伪造数值 -->
          <div class="meter-row">
            <div class="meter-track meter-mp" role="progressbar" aria-label="耐力"
              :aria-valuenow="gameplayState.state.rage" :aria-valuemax="gameplayState.config.rageMax" aria-valuemin="0">
              <div class="meter-lag" :style="{ width: `${ragePercent()}%` }"></div>
              <div class="meter-fill" :style="{ width: `${ragePercent()}%` }"></div>
            </div>
            <span class="meter-label">EP</span>
            <span class="meter-val">{{ gameplayState.state.rage }}</span>
          </div>
        </div>
      </div>

      <!-- 左下：操作提示 -->
      <aside v-if="!inventoryOpen" class="action-tips" aria-label="操作提示">
        <p>
          <template v-for="([key, text], index) in controlHints" :key="key">
            <template v-if="index"> · </template>{{ text }} <kbd>{{ key }}</kbd>
          </template>
        </p>
      </aside>

      <!-- 右下：气刃槽 + 战技群 -->
      <div v-if="!inventoryOpen && showCombatHud" class="action-cluster">
        <div class="focus-system">
          <!-- 原型是三层气刃充能；引擎侧用怒气比例点亮 -->
          <div class="focus-blade" aria-hidden="true">
            <div class="focus-orb" :class="{ 'is-lit': ragePercent() >= 33 }"></div>
            <div class="focus-orb" :class="{ 'is-lit': ragePercent() >= 66 }"></div>
            <div class="focus-orb" :class="{ 'is-lit': ragePercent() >= 100 }"></div>
          </div>
          <button v-if="heavySkill" type="button" class="focus-heavy-btn hint-anchor"
            :class="{ 'is-active': skillStatus(HEAVY_SKILL_ID).available }"
            :disabled="!skillStatus(HEAVY_SKILL_ID).available"
            :aria-label="`${heavySkill.key} ${heavySkill.name}，消耗 ${heavySkill.rageCost} 怒气`"
            @click.stop="runAction(() => gameplay.castSkill(HEAVY_SKILL_ID))">
            <svg viewBox="0 0 48 48">
              <circle cx="24" cy="24" r="18" stroke-dasharray="6 6" fill="rgba(230,213,167,.18)" />
              <circle cx="24" cy="24" r="18" stroke-dasharray="6 6" />
              <polygon points="24,10 32,28 16,28" fill="rgba(230,213,167,.4)" />
              <polygon points="24,10 32,28 16,28" />
            </svg>
            <span class="key-tag">{{ heavySkill.key }}</span>
            <span class="ui-tooltip" role="tooltip">{{ heavySkill.name }} · {{ heavySkill.rageCost }} 怒气</span>
          </button>
        </div>

        <div class="spells-group">
          <div v-for="(skill, id) in gameplayState.config.skills" :key="id" class="spell-item">
            <button type="button" class="spell-btn hint-anchor" :data-key="skill.key"
              :class="{ 'is-cd': skillStatus(id).remainingMs > 0, unavailable: !skillStatus(id).available }"
              :style="{ '--cd-deg': `${Math.round(360 * skillStatus(id).remainingMs / (skill.cdMs || 1))}deg` }"
              :aria-label="`${skill.key} ${skill.name}，消耗 ${skill.rageCost} 怒气`"
              @click.stop="runAction(() => gameplay.castSkill(id))">
              <span class="spell-cost">{{ skill.rageCost }}</span>
              <!-- 技能图标：自包含行内 SVG（与器物图标同一套做法，不用 <use>） -->
              <svg class="spell-icon" viewBox="0 0 48 48" v-html="skillIconInner(id)" />
              <div class="spell-cd-overlay"></div>
              <span class="spell-cd-num">{{ cooldownSeconds(id) }}</span>
              <span class="spell-key">{{ skill.key }}</span>
              <span class="ui-tooltip" role="tooltip">{{ skill.name }} · {{ skill.rageCost }} 怒气</span>
            </button>
            <span class="spell-name">{{ skill.name }}</span>
          </div>
        </div>
      </div>

      <!-- 世界指示与状态层（原型无此项，沿用同一套面板语言） -->
      <div class="world-indicator" role="status" aria-live="polite">{{ worldLabel }}</div>
      <!-- 底部中央由这一个 flex 列独占：物品栏与状态条是它的子项，靠布局错开，
           而不是两条各自手算的 bottom 偏移（此前两条的锚点都是 bottom-center，
           行囊一恢复就在某些视口上叠在一起）。顶部中央归小世界状态条。 -->
      <div v-if="!inventoryOpen" class="story-bottom-column">
        <div class="story-bottom-stack">
          <div v-if="gameplayError" class="gameplay-error" role="alert" @pointerdown.stop @wheel.stop @pointermove.stop="camera.resetInput">
            <span>{{ gameplayError }}</span><button :disabled="actionBusy" @click.stop="runAction(saveRegistration.flush)">重试保存</button>
          </div>
          <div v-if="feedback" class="story-feedback" role="status" aria-live="polite">{{ feedback }}</div>
        </div>
        <section v-if="hotbarVisible" class="hotbar" aria-label="物品栏"
          @pointerdown.stop @pointermove.stop>
          <div class="hotbar__slots" role="listbox" aria-label="物品栏格子">
            <button v-for="entry in hotbarSlots" :key="entry.slot" type="button" class="hotbar__slot"
              :class="{ 'is-active': entry.slot === hotbarIndex, 'is-empty': entry.empty }"
              role="option" :aria-selected="entry.slot === hotbarIndex ? 'true' : 'false'"
              :aria-label="entry.empty ? `${entry.slot + 1} 空格` : `${entry.slot + 1} ${entry.name}，可用 ${entry.available}`"
              @click="selectHotbarSlot(entry.slot)">
              <span class="hotbar__key" aria-hidden="true">{{ entry.slot + 1 }}</span>
              <span v-if="!entry.empty" class="hotbar__name">{{ entry.name }}</span>
              <span v-if="!entry.empty" class="hotbar__count" aria-hidden="true">{{ entry.available }}</span>
            </button>
          </div>
          <p class="hotbar__hint">
            <span v-if="hotbarSelected && !hotbarSelected.empty">已选：{{ hotbarSelected.name }}</span>
            <span v-else>第 {{ (hotbarSelected?.slot ?? 0) + 1 }} 格是空的</span>
            <span class="hotbar__tip">滚轮换格 · 数字键 1 - {{ hotbarSlots.length }} · B 在身前放置 · F 取回身边的方块 · 共 {{ placements.length }} / {{ MAX_PLACEMENTS }}</span>
          </p>
        </section>
      </div>
      <!-- 小世界状态：顶部中央（顶部中央只在小世界空闲，主世界归 Boss 血条） -->
      <section v-if="!inventoryOpen && inSubworld" class="subworld-notice" aria-label="小世界状态">
        <strong>{{ worldLabel }}</strong>
        <span>此处为沙盒空间 · 无战斗目标</span>
      </section>
    </div>
    <!-- ============ 先知对话 + 调整物体：(F 交谈，Enter 继续，Esc 离开) ============ -->
    <div v-if="dialogueOpen || adjustMode" class="dialogue-overlay"
      :class="{ 'dialogue-overlay-passive': adjustMode }"
      @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent>
      <section v-if="dialogueOpen" ref="dialoguePanel" class="dialogue-panel" role="dialog" aria-modal="true"
        aria-labelledby="dialogue-title" tabindex="-1">
        <header class="dialogue-header">
          <strong id="dialogue-title">{{ dialogue.name }}</strong>
          <span v-if="dialogue.title">{{ dialogue.title }}</span>
        </header>
        <p class="dialogue-line">{{ dialogue.lines[dialogueLine] }}</p>
        <p v-if="dialogueLine + 1 >= dialogue.lines.length && dialogue.closing" class="dialogue-closing">
          {{ dialogue.closing }}
        </p>
        <!-- 先知的两个主选项：1 调整物体（离开对话面板）／2 碎片（面板内的另一屏） -->
        <div class="prophet-actions" aria-label="先知的两个主选项">
          <template v-if="!prophetScreen">
            <button v-for="screen in PROPHET_SCREENS" :key="screen.id" class="prophet-action"
              :data-prophet-screen="screen.id" @click="openProphetScreen(screen.id)">
              <span class="prophet-action-key">{{ screen.key }}</span>
              <span class="prophet-action-label">{{ screen.label }}</span>
              <span class="prophet-action-detail">{{ screen.detail }}</span>
            </button>
          </template>

          <!-- 「碎片」屏：点整行装填/拆下，行尾的「调整」进子屏改数字 -->
          <template v-else-if="prophetScreen === 'fragments'">
            <div v-for="(entry, index) in fragmentList" :key="entry.id" class="prophet-row"
              :data-prophet-row="entry.id">
              <button class="prophet-action prophet-row-main"
                :data-prophet-fragment="entry.id" :disabled="!entry.owned"
                @click="runProphetAction({ type: 'toggle-fragment', fragmentId: entry.id })">
                <span class="prophet-action-key">{{ index + 1 }}</span>
                <span class="prophet-action-label">{{ entry.name }}</span>
                <span class="prophet-action-state" :data-state="entry.state">{{ entry.stateLabel }}</span>
                <span class="prophet-action-detail">{{ entry.description }} · {{ entry.details }}</span>
              </button>
              <button class="prophet-action prophet-row-adjust"
                :data-prophet-adjust="entry.id" @click="openFragmentAdjust(entry)">
                <span class="prophet-action-label">调整</span>
              </button>
            </div>
          </template>

          <!-- 「调整碎片」子屏：只改选中碎片的幅度与周期，改完即落盘并重启规则 -->
          <template v-else>
            <p class="prophet-action-detail" data-fragment-adjust-title>
              {{ fragmentAdjust.known ? `${fragmentAdjust.name} · ${fragmentAdjust.details}` : fragmentAdjust.name }}
            </p>
            <p class="prophet-action-detail" data-fragment-adjust-values>
              当前：{{ fragmentAdjust.amplitudeLabel }} · {{ fragmentAdjust.periodLabel }}
            </p>
            <p v-if="fragmentAdjust.hint" class="prophet-action-detail" data-fragment-adjust-hint>
              {{ fragmentAdjust.hint }}
            </p>
            <button v-for="action in FRAGMENT_ADJUST_ACTIONS" :key="action.id" class="prophet-action"
              :data-fragment-action="action.id"
              @click="runProphetAction({ type: 'adjust-fragment', id: action.id, fragmentId: fragmentAdjust.id })">
              <span class="prophet-action-key">{{ action.key }}</span>
              <span class="prophet-action-group">{{ action.group }}</span>
              <span class="prophet-action-label">{{ action.label }}</span>
            </button>
          </template>

          <button v-if="prophetScreen" class="prophet-action prophet-action-back"
            :data-prophet-back="prophetScreen" @click="leaveProphetScreen">
            <span class="prophet-action-label">返回</span>
          </button>
          <p v-if="actionFeedback" class="prophet-action-feedback" role="status">{{ actionFeedback }}</p>
        </div>
        <footer class="dialogue-footer">
          <span class="dialogue-progress">{{ dialogueLine + 1 }} / {{ dialogue.lines.length }}</span>
          <span class="dialogue-hint">{{ dialogue.hint }}</span>
          <button class="dialogue-next" type="button" @click="advanceDialogue">
            {{ dialogueLine + 1 >= dialogue.lines.length ? '结束交谈' : '继续' }}
          </button>
        </footer>
      </section>
      <!-- 调整物体：只用名字列表选择，视口里不做拾取；左键此时只用来聚焦视口 -->
      <section v-if="adjustMode" class="dialogue-panel" data-adjust-panel role="group"
        aria-label="调整物体" tabindex="-1">
        <strong>调整物体 · 俯瞰视角</strong>
        <p class="dialogue-line">{{ adjustHint }}</p>
        <div class="prophet-actions" style="flex-basis: 100%; max-height: 150px; overflow-y: auto;">
          <button v-for="entry in selectableList" :key="entry.guid" class="prophet-action"
            :data-adjust-object="entry.guid"
            :style="isSelected(entry) ? { borderColor: '#a08750', background: '#2a2f22' } : null"
            @click="selectObjectFromList(entry)">
            <span class="prophet-action-label">{{ objectLabel(entry) }}</span>
          </button>
          <button class="prophet-action" data-adjust-refresh @click="loadSelectableObjects">
            <span class="prophet-action-label">刷新列表</span>
          </button>
          <p v-if="!selectableList.length" class="prophet-action-detail">
            场景里没有读出可调整的物体，点「刷新列表」重试
          </p>
        </div>
        <div class="prophet-actions">
          <button v-for="action in ADJUST_ACTIONS" :key="action.id" class="prophet-action"
            :data-prophet-action="action.id" @click="runProphetAction({ type: 'adjust', id: action.id })">
            <span class="prophet-action-key">{{ action.key }}</span>
            <span class="prophet-action-group">{{ action.group }}</span>
            <span class="prophet-action-label">{{ action.label }}</span>
          </button>
          <button class="prophet-action prophet-action-back" data-adjust-exit @click="exitAdjustMode()">
            <span class="prophet-action-label">退出调整，回到对话</span>
          </button>
          <p v-if="actionFeedback" class="prophet-action-feedback" role="status">{{ actionFeedback }}</p>
        </div>
      </section>
    </div>
    <!-- ============ 行囊（Tab 背包）：复刻 111/bag.html ============ -->
    <!-- ============ 行囊（Tab 背包）：复刻 bag2.html《自然荒野 · 战术编成与收纳库》 ============ -->
    <div v-if="inventoryOpen && gameplayState" ref="inventoryPanel" class="story-bag"
      aria-label="行囊" @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent @keydown="onInventoryKey">

      <div class="bagStage">
        <section class="bag symphony-panel">
          <div class="bag__body">

            <!-- 顶栏 -->
            <header class="bag__head">
              <div class="bag__seal">
                <svg viewBox="0 0 24 24"><rect x="4" y="4" width="16" height="16" rx="2"/><circle cx="12" cy="12" r="4"/></svg>
              </div>
              <div class="bag__title-wrap">
                <h1 class="bag__title">战术编成 // 收纳库</h1>
                <span class="bag__sub">TACTICAL LOADOUT &amp; SECURE INVENTORY</span>
              </div>
              <div class="bag__headline"></div>
              <div class="purse" title="荒野代币">
                <span class="purse__coin"></span>
                <span class="purse__num">{{ bagPurseText }}</span>
              </div>
              <button type="button" class="closeBtn" data-inventory-close title="关闭 (Esc)" aria-label="关闭行囊"
                :disabled="navigationPending" @click="toggleInventory">✕</button>
            </header>

            <!-- 优化后的主体网格布局 -->
            <div class="bag__cols">

              <!-- 左上：武装编成与人偶 -->
              <div class="colL">
                <div class="secHead">
                  <span class="secHead__dot"></span>
                  <span class="secHead__txt">现役武装编成</span>
                  <div class="secHead__line"></div>
                  <span class="secHead__meta">{{ bagGearMetaText }}</span>
                </div>

              <div class="dollWrap">
                <div class="doll">
                  <div class="figure" title="机体投影">
                    <img :src="HERO_FIGURE" alt="" aria-hidden="true" draggable="false" />
                  </div>

                  <button v-for="slot in GEAR_SLOTS" :key="slot.id" type="button" class="eslot hint-anchor"
                    :data-slot="slot.id" :title="slot.name"
                    :class="{ has: Boolean(bag.gear[slot.id]), 'is-sel': bag.gear[slot.id] && bag.gear[slot.id].uid === bag.sel, ['r-' + (bag.gear[slot.id]?.rarity || 'common')]: true }"
                    :aria-label="bag.gear[slot.id] ? `${slot.name}：${bag.gear[slot.id].name}` : `${slot.name}（空）`"
                    @click="bagSelectGear(slot.id)" @dblclick="bagUnequip(slot.id)"
                    @pointerenter="bag.gear[slot.id] && bagShowTip(bag.gear[slot.id], $event)"
                    @pointermove="bagMoveTip($event)" @pointerleave="bagHideTip">
                    <span class="eslot__ph">{{ slot.name }}</span>
                    <span v-if="bag.gear[slot.id]" class="icon" aria-hidden="true" v-html="bagIconHTML(bag.gear[slot.id])" />
                  </button>
                </div>

                <div class="hero-stats">
                  <div class="hstat"><span class="hstat-name">物理强度</span><span class="hstat-val">{{ bagHeroStats.atk }}</span></div>
                  <div class="hstat"><span class="hstat-name">护甲防御</span><span class="hstat-val">{{ bagHeroStats.def }}</span></div>
                  <div class="hstat"><span class="hstat-name">暴击共鸣</span><span class="hstat-val">{{ bagHeroStats.crit }}%</span></div>
                  <div class="hstat"><span class="hstat-name">耐久上限</span><span class="hstat-val">{{ bagHeroStats.hp }}</span></div>
                </div>
              </div>
              </div>

              <!-- 右上：物品分类与仓库阵列 -->
              <div class="colR">
                <div class="tabs" role="tablist" aria-label="分类">
                  <button v-for="tabItem in bagTabList" :key="tabItem.id" type="button" class="tab" role="tab"
                    :aria-selected="bag.tab === tabItem.id ? 'true' : 'false'" @click="bag.tab = tabItem.id">
                    {{ tabItem.name }}<span class="tab__count">{{ bagTabCount(tabItem.id) }}</span>
                  </button>
                </div>
                <div class="gridWrap">
                  <div class="grid" role="listbox" aria-label="背包格">
                    <button v-for="cell in bagCellList" :key="cell.index" type="button"
                      class="cell inventory-slot hint-anchor"
                      :class="{ has: cell.filled, 'is-flow': !cell.filled, 'is-sel': cell.filled && selectedSlot === cell.index,
                        ['r-' + (cell.item?.rarity || 'common')]: true }"
                      :disabled="!cell.filled" :tabindex="cell.filled && selectedSlot === cell.index ? 0 : -1"
                      :aria-label="cell.item ? `${cell.item.name}，数量 ${cell.item.qty}` : `空格 ${cell.index + 1}`"
                      @click="bagSelectSlot(cell.index)" @dblclick="bagDoubleAct({ index: cell.index })"
                      @focus="selectedSlot = cell.index"
                      @pointerenter="cell.item && bagShowTip(cell.item, $event)"
                      @pointermove="bagMoveTip($event)" @pointerleave="bagHideTip">
                      <span v-if="cell.item" class="icon" aria-hidden="true" v-html="bagIconHTML(cell.item)" />
                      <span v-if="cell.item && cell.item.qty > 1" class="qty">{{ cell.item.qty }}</span>
                    </button>
                  </div>
                </div>
              </div>

              <!-- 左下：世界坐标（点主世界回主世界场景，点小世界进小世界场景） -->
              <section class="sector-wrap">
                <div class="sector__body">
                  <header class="sector__head">
                    <span class="secHead__dot"></span>
                    <span class="sector__title">世界坐标 // WORLDS</span>
                  </header>
                  <div class="sector__row">
                    <div class="sector__art" :aria-label="bagWorld ? `${bagWorld.name}：${bagWorld.tier}` : '未有界'" v-html="bagWorldScene"></div>
                    <div class="sector__list" role="listbox" aria-label="世界一览">
                      <button v-for="(world, index) in bagWorldList()" :key="world.role" type="button"
                        class="snode hint-anchor"
                        :class="{ 'is-cur': bagWorldIsCurrent(index), 'is-dest': !bagWorldIsCurrent(index) && !bagEntryLocked(index) }"
                        :disabled="bagEntryLocked(index)"
                        :data-inventory-travel="index === bagEntryIndex() ? '' : undefined"
                        :data-inventory-travel-focus="index === bagEntryIndex() ? '' : undefined"
                        role="option" :aria-selected="bagWorldIsCurrent(index) ? 'true' : 'false'"
                        :aria-label="bagTravelLabel(index)" @click="bagTravel(index)">
                        <strong class="snode__name">{{ world.name }}</strong>
                        <span class="snode__meta">{{ bagEntryLocked(index) ? '未辟' : (bagWorldIsCurrent(index) ? '所在' : '前往') }}</span>
                      </button>
                    </div>
                  </div>
                </div>
              </section>

              <!-- 右下：装备详情与属性浮动对比 -->
              <div class="detailWrap">
                <div v-if="bagDetailInfo" class="detail">
                  <div class="detail__preview" :class="`r-${bagDetailInfo.item.rarity}`">
                    <span class="icon" aria-hidden="true" v-html="bagDetailInfo.preview" />
                  </div>
                  <div class="detail__info">
                    <div class="detail__title-row">
                      <h2 class="detail__name">{{ bagDetailInfo.item.name }}</h2>
                      <span class="detail__rarity" :class="`r-${bagDetailInfo.item.rarity}`">{{ bagDetailInfo.rarity }}</span>
                      <span class="detail__type">{{ bagDetailInfo.typeLine }}</span>
                    </div>
                    <div v-if="bagDetailInfo.stats.length" class="detail__stats">
                      <div v-for="pair in bagDetailInfo.stats" :key="pair[0]" class="dstat-item">
                        <span class="dstat-name">{{ pair[0] }}</span>
                        <div class="dstat-val-group"><span class="dstat-curr">{{ pair[1] }}</span></div>
                      </div>
                    </div>
                    <p class="detail__flavor">“{{ bagDetailInfo.flavor }}”</p>
                    <div class="detail__acts">
                      <button v-for="action in bagDetailInfo.actions" :key="action.act" type="button"
                        class="dbtn" :class="action.ghost ? 'dbtn--ghost' : 'dbtn--primary'"
                        @click="bagDetailAct(bagDetailInfo.uid, action.act)">{{ action.label }}</button>
                    </div>
                  </div>
                </div>
                <p v-else class="detail__empty">点击对应资源块以加载结构图谱</p>
              </div>

            </div>

            <!-- 底栏 -->
            <footer class="bag__foot">
              <div>
                <span>收纳空间利用率：</span>
                <b>{{ bagFootInfo.capText }}</b>
              </div>
              <div class="hints">
                <span>单击选择检视</span>
                <span><kbd>双击 / 装配</kbd> 极速换装</span>
                <span><kbd>Esc</kbd> 退出终端</span>
              </div>
            </footer>

          </div>
        </section>
      </div>

      <!-- 跟随光标的浮签 -->
      <div ref="bagTip" class="tip" role="tooltip" :class="{ 'is-on': bag.tipOn }"
        :aria-hidden="bag.tipOn ? 'false' : 'true'" :style="bag.tipStyle">
        <p v-if="bag.tipItem" class="tip__name">{{ bag.tipItem.name }}
          <span class="tip__rarity" :class="`r-${bag.tipItem.rarity}`">{{ RARITY[bag.tipItem.rarity].label }}</span>
          <span v-if="bag.tipItem.qty > 1" class="tip__rarity">× {{ bag.tipItem.qty }}</span>
        </p>
        <p v-if="bag.tipItem" class="tip__type">{{ bagTypeLine(bag.tipItem) }}</p>
        <div v-if="bag.tipItem" class="tip__stats">
          <div v-for="pair in (bag.tipItem.stats || [])" :key="pair[0]"><span>{{ pair[0] }}</span><b>{{ pair[1] }}</b></div>
        </div>
        <p v-if="bag.tipItem" class="tip__flavor">{{ bag.tipItem.flavor }}</p>
        <p v-if="bag.tipItem" class="tip__foot">{{ bagTipFoot(bag.tipItem) }}</p>
      </div>

      <!-- 提示语 -->
      <div class="toast" :class="{ 'is-on': Boolean(bag.toast) }" role="status" aria-live="polite">{{ bag.toast }}</div>
    </div>
    <div ref="fxLayer" class="fx-layer" aria-hidden="true"></div>
    <div class="toast" :class="{ 'is-on': Boolean(toastText) }" role="status" aria-live="polite">{{ toastText }}</div>
  </div>
</template>

<style scoped>
/* =========================================================
   设计令牌与共用面板
   HUD 的配色、字体与版式在 ./StoryWorld.hud.css（复刻 ui2.html）；
   行囊在 ./StoryWorld.bag.css（复刻 bag2.html）。
   这里保留视口令牌，以及对话面板 / 小世界提示仍在用的切角面板。
   ========================================================= */
.story-world-viewport {
  /* —— 颜色 —— */
  --c-paper: #e9e0cc;
  --c-paper-dim: #bdb298;
  --c-gold: #c8a659;
  --c-gold-dim: #8a7038;
  --c-gold-bright: #f0d68f;
  --c-cinnabar: #b3382a;
  --c-cinnabar-bright: #e0523b;
  --c-bronze: #5f8a72;
  --c-bronze-dim: #2c4a3c;

  /* —— 结构 —— */
  --gold-line: rgba(200, 166, 89, .42);
  --gold-line-soft: rgba(200, 166, 89, .22);
  --panel-a: .88;
  --pattern: rgba(233, 224, 204, .08);
  --haze: .12;
  --cut: 14px;
  --edge: 2px;
  --shape: polygon(var(--cut) 0, 100% 0, 100% calc(100% - var(--cut)), calc(100% - var(--cut)) 100%, 0 100%, 0 var(--cut));
  --font-serif: 'Noto Serif SC', 'Source Han Serif SC', 'Source Han Serif CN', 'Songti SC', 'STSong', 'SimSun', serif;
  --font-brush: 'LXGW WenKai', '霞鹜文楷', 'Kaiti SC', 'STKaiti', 'KaiTi', '楷体', var(--font-serif);

  position: fixed;
  inset: 0;
  overflow: hidden;
  outline: none;
  background: transparent;
  color: var(--c-paper);
  font-family: var(--font-serif);
  -webkit-font-smoothing: antialiased;
  text-rendering: optimizeLegibility;
}

/* =========================================================
   通用面板：切角金边「框 + 芯」，浅色花纹打在芯上
   ========================================================= */
.plate {
  position: relative;
  padding: var(--edge);
  clip-path: var(--shape);
  background-image:
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .18) 0 1px, transparent 1px 4px),
    linear-gradient(148deg,
      rgba(250, 232, 182, .95) 0%, rgba(200, 168, 98, .78) 16%,
      rgba(122, 98, 52, .46) 38%, rgba(96, 78, 42, .58) 58%,
      rgba(208, 176, 106, .82) 82%, rgba(250, 232, 182, .92) 100%);
  filter: drop-shadow(0 10px 24px rgba(0, 0, 0, .62));
}

.plate__body {
  position: relative;
  height: 100%;
  clip-path: var(--shape);
  background-color: rgba(26, 23, 18, .94);
  background-image:
    repeating-linear-gradient(90deg, var(--pattern) 0 1px, transparent 1px 9px),
    repeating-linear-gradient(90deg, rgba(233, 224, 204, .05) 0 2px, transparent 2px 16px),
    repeating-linear-gradient(58deg, rgba(233, 224, 204, .03) 0 1px, transparent 1px 13px),
    repeating-linear-gradient(-58deg, rgba(233, 224, 204, .022) 0 1px, transparent 1px 17px),
    repeating-linear-gradient(114deg, rgba(233, 224, 204, .035) 0 1px, transparent 1px 6px),
    radial-gradient(120% 92% at 8% 0%, rgba(233, 224, 204, .10), transparent 62%),
    radial-gradient(92% 120% at 100% 112%, rgba(179, 56, 42, .11), transparent 62%),
    linear-gradient(180deg, rgba(70, 61, 48, .70), rgba(25, 22, 18, .94));
  background-size: 100% 5px, 100% 3px, auto, auto, auto, auto, auto, auto;
  background-position: left top, left bottom, 0 0, 0 0, 0 0, 0 0, 0 0, 0 0;
  background-repeat: no-repeat, no-repeat, repeat, repeat, repeat, no-repeat, no-repeat, no-repeat;
  box-shadow: inset 0 0 0 1px rgba(0, 0, 0, .62), inset 0 0 30px rgba(0, 0, 0, .5);
}
.plate__body > * {
  position: relative;
  z-index: 1;
}
/* 朱砂小菱：点睛（贴在未切角的右上） */
.plate__body::before {
  content: "";
  position: absolute;
  right: 9px;
  top: 9px;
  width: 5px;
  height: 5px;
  z-index: 2;
  background: linear-gradient(135deg, var(--c-cinnabar-bright), var(--c-cinnabar));
  transform: rotate(45deg);
  opacity: .9;
  box-shadow: 0 0 9px rgba(179, 56, 42, .5);
}
/* 局部墨迹晕染 */
.haze {
  position: absolute !important;
  z-index: 0 !important;
  pointer-events: none;
  left: -16%;
  top: -45%;
  width: 64%;
  height: 190%;
  background: radial-gradient(closest-side, rgba(233, 224, 204, var(--haze)), rgba(233, 224, 204, 0) 72%);
  filter: blur(10px);
}
.haze--r {
  left: auto;
  right: -20%;
  top: -30%;
  width: 56%;
  height: 170%;
  background: radial-gradient(closest-side, rgba(179, 56, 42, calc(var(--haze) * .9)), rgba(179, 56, 42, 0) 72%);
}

/* =========================================================
   行囊（Tab 背包）
   样式见 ./StoryWorld.bag.css（复刻 bag2.html）；命中测试由该文件自己声明。
   ========================================================= */
.hint-anchor { position: relative; }

/* =========================================================
   关键帧
   ========================================================= */
@keyframes breathe {
  0%, 100% { filter: brightness(.93) saturate(.95); }
  50% { filter: brightness(1.14) saturate(1.06); }
}

@keyframes breathe-soft {
  0%, 100% { opacity: .62; }
  50% { opacity: 1; }
}

@keyframes dmg-float {
  0% { opacity: 0; transform: translate(-50%, -50%) scale(.62) rotate(var(--rot, 0deg)); }
  20% { opacity: 1; transform: translate(-50%, -58%) scale(1.12) rotate(var(--rot, 0deg)); }
  60% { opacity: .95; }
  100% { opacity: 0; transform: translate(calc(-50% + var(--dx, 0px)), calc(-50% - var(--rise, 60px))) scale(.98) rotate(var(--rot, 0deg)); }
}

@media (prefers-reduced-motion: reduce) {
  .bar--hp .bar__fill, .bar--rage .bar__fill, .tip { animation: none; }
  .bar__lag, .bar__fill { transition: none; }
  .dmg { animation-duration: 1.15s; }
}


/* ============================================================
   小世界：位置指示器 / 小世界状态 / 先知对话 / 物质陈列
   配色沿用行囊的 --c-* 令牌与切角
   ============================================================ */
.world-indicator {
  position: absolute; top: 26px; right: 24px; z-index: 3;
  padding: 7px 14px; font-size: 12px; letter-spacing: 2px;
  color: var(--c-gold-bright); background: rgba(16, 14, 12, .82);
  border: 1px solid var(--gold-line);
  clip-path: polygon(8px 0, 100% 0, 100% calc(100% - 8px), calc(100% - 8px) 100%, 0 100%, 0 8px);
  text-shadow: 0 1px 3px #000;
}
/* 小世界状态：顶部中央。放到这里的唯一原因是物品栏要占用底部中央（Minecraft 位置），
   而顶部中央在小世界是空的（图例左上、世界铭牌右上、小世界没有首领血条）。 */
.subworld-notice {
  position: absolute; top: 18px; left: 50%; transform: translateX(-50%);
  display: grid; gap: 5px; justify-items: center; padding: 4px 12px; text-align: center; z-index: 3;
  background: rgba(16, 14, 12, .82); border: 1px solid var(--gold-line);
  clip-path: var(--shape); box-shadow: 0 8px 22px rgba(0, 0, 0, .55);
}
.subworld-notice strong { font-size: 12px; font-weight: 500; letter-spacing: 3px; color: var(--c-gold-bright); }
.subworld-notice span { font-size: 10px; color: var(--c-paper-dim); }

.dialogue-overlay {
  pointer-events: auto; position: absolute; inset: 0; z-index: 6;
  display: grid; align-items: end; justify-items: center;
  padding: 0 20px 92px; background: rgba(5, 8, 7, .42);
}
/* 调整物体时叠加层不能挡住视口：点击必须落到场景表面上的对准提示。
   只有面板本身保持可交互。 */
.dialogue-overlay-passive { pointer-events: none; background: none; padding: 0; }
.dialogue-overlay-passive > * { pointer-events: auto; }
.dialogue-panel {
  width: min(560px, 100%); box-sizing: border-box; padding: 18px 22px; outline: none;
  background: linear-gradient(145deg, rgba(32, 35, 28, .93), rgba(15, 20, 17, .98));
  border: 1px solid var(--c-gold-dim); clip-path: var(--shape);
  box-shadow: 0 18px 60px rgba(0, 0, 0, .6);
}
.dialogue-header { display: flex; align-items: baseline; gap: 12px; padding-bottom: 12px; border-bottom: 1px solid var(--c-stone); }
.dialogue-header strong { font-size: 15px; font-weight: 500; letter-spacing: 3px; color: var(--c-gold-bright); }
.dialogue-header span { font-size: 11px; color: var(--c-paper-dim); }
.dialogue-line { margin: 14px 0 0; min-height: 2.6em; font-size: 14px; line-height: 1.9; color: var(--c-paper); }
.dialogue-closing { margin: 8px 0 0; font-size: 12px; color: var(--c-gold); }
.dialogue-footer { display: flex; align-items: center; gap: 12px; padding-top: 16px; }
.dialogue-progress { font-size: 11px; color: var(--c-gold-dim); font-variant-numeric: tabular-nums; }
.dialogue-hint { flex: 1; font-size: 11px; color: var(--c-gold-dim); }
.dialogue-next {
  flex-shrink: 0; padding: 7px 16px; font-size: 12px; cursor: pointer;
  color: var(--c-gold-bright); background: #343820; border: 1px solid var(--c-gold);
}
/* 先知的选项：两个主选项／调整步骤／碎片条目 */
.prophet-actions { display: flex; flex-wrap: wrap; gap: 6px; margin: 12px 0 0; }
.prophet-action {
  display: inline-flex; align-items: center; gap: 6px; padding: 5px 9px; cursor: pointer;
  border: 1px solid var(--c-stone); background: rgba(23, 26, 21, .9);
  color: var(--c-paper); font-size: 12px; font-family: inherit;
}
.prophet-action:hover:not(:disabled) { border-color: var(--c-gold); background: #22261d; }
.prophet-action:disabled { cursor: not-allowed; opacity: .45; }
.prophet-action-key {
  min-width: 14px; padding: 0 3px; border: 1px solid var(--c-stone);
  text-align: center; font-size: 11px; color: var(--c-gold);
}
.prophet-action-group { color: var(--c-paper-dim); font-size: 11px; }
.prophet-action-feedback { flex-basis: 100%; margin: 4px 0 0; font-size: 11px; color: var(--c-gold-bright); }
.prophet-action-detail { flex-basis: 100%; font-size: 11px; color: var(--c-paper-dim); }
.prophet-action-state { padding: 0 4px; border: 1px solid var(--c-stone); font-size: 11px; color: var(--c-gold); }
.prophet-action-state[data-state="installed"] { border-color: #7f9a5c; color: #cbe0a6; }
.prophet-action-state[data-state="locked"] { opacity: .5; }
/* 碎片条目：整行点一下装填/拆下，右边单独的「调整」进子屏改数字。
   两个按钮必须平级——按钮里再嵌按钮是非法的 HTML。 */
.prophet-row { display: flex; flex-basis: 100%; align-items: stretch; gap: 6px; }
.prophet-row-main { flex: 1 1 auto; }
.prophet-row-adjust { flex: 0 0 auto; align-self: flex-start; }
.prophet-action-back { margin-left: auto; }

/* 物品栏：小世界底部中央的快捷栏（Minecraft 位置）：滚轮/数字键选格，B 键在身前落物。
   固定 8 个方形格子（见 storyProps.HOTBAR_SLOTS）：有物的格子显示名字与数量，空格子只留
   编号并压暗，但仍可被选中。选中的格子边框明显更亮，数量压在格子右下角。
   它不是自己贴底定位的：位置由 .story-bottom-column 这个 flex 列决定，
   与状态条按布局上下排开，因此不可能再和它叠在一起。 */
.hotbar {
  position: static; display: grid; gap: 4px; justify-items: center;
}
.hotbar__slots { display: flex; gap: 6px; }
.hotbar__slot {
  position: relative; display: grid; place-items: center; gap: 1px;
  width: clamp(44px, 4.4vw, 54px); height: clamp(44px, 4.4vw, 54px);
  padding: 3px; box-sizing: border-box; cursor: pointer;
  background: rgba(11, 17, 15, .78); border: 1px solid var(--c-stone);
  color: var(--c-paper);
}
/* 选中格：边框更亮更粗，并加一层内描边，避免外描边被相邻格子挤掉。 */
.hotbar__slot.is-active {
  border-color: var(--c-gold-bright);
  background: #343820;
  box-shadow: inset 0 0 0 1px rgba(240, 214, 143, .55), 0 0 10px rgba(200, 166, 89, .35);
}
.hotbar__slot.is-empty { opacity: .55; }
.hotbar__key {
  position: absolute; top: 1px; left: 3px; font-size: 10px; line-height: 1;
  color: var(--c-gold-dim); font-variant-numeric: tabular-nums;
}
.hotbar__name { font-size: 11px; font-weight: 500; line-height: 1.2; text-align: center; color: var(--c-paper); }
.hotbar__count {
  position: absolute; right: 3px; bottom: 1px; font-size: 11px; line-height: 1;
  color: var(--c-gold-bright); text-shadow: 0 1px 2px #000;
  font-variant-numeric: tabular-nums;
}
.hotbar__hint {
  display: flex; gap: 10px; margin: 0; padding: 2px 8px; font-size: 10.5px; line-height: 1.5;
  color: var(--c-paper-dim); background: rgba(11, 17, 15, .6); border: 1px solid var(--c-stone);
}
.hotbar__tip { color: var(--c-gold-dim); }

@media (max-width: 800px) {
  .world-indicator { top: 18px; right: 12px; font-size: 11px; letter-spacing: 1px; }
}
</style>
