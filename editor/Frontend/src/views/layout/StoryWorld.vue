<script setup>
import { onMounted, onUnmounted, ref, nextTick, watch, reactive, computed } from 'vue';
import { useRouter, onBeforeRouteLeave } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState, normalizeProjectPath } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';
import { projectLauncherService, cancelPendingProjectOpen, getProjectSelectionVersion } from '@/services/projectLauncherService.js';
import { trackWorldSessionWork, notifyWorldError, registerWorldSessionSave } from '@/services/worldSessionLifecycle.js';
import { createStoryNavigationController, NAVIGATION_KEY } from '../../../../../game/frontend/storyNavigation.mjs';
// 行囊（Tab 背包）样式：复刻 111/bag.html，收在 .story-bag 之下
import './StoryWorld.bag.css';

import { ensureStoryCharacters, syncPlacementActors } from '../../../../../game/frontend/storyActors.mjs';
import { createPlayerController, VIEW_LABELS } from '../../../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../../../game/frontend/playerSave.mjs';
import { createStoryGameplay, worldBounds, distanceToBounds, pickupDistance } from '../../../../../game/frontend/storyGameplay.mjs';
import { STORY_CHARACTERS, PROPHET_GUID } from '../../../../../game/frontend/storyCharacters.mjs';
import { PLACEMENT_SITES, exhibitItems as exhibitItemsFor, nextPlacementIndex,
  placementGuid, sanitizePlacements } from '../../../../../game/frontend/storyProps.mjs';
import { PROPHET_INTERACTION, canTalkToProphet, normalizeDialogue } from '../../../../../game/frontend/prophetDialogue.mjs';

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
const prophetNearby = ref(false);
const dialogue = normalizeDialogue(null);
let prophetActor = null;
let dialoguePanel = null;
// 小世界陈列
const placements = ref([]);
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
const camera = createStoryCameraController({
  createControls: createPlayerController,
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
  canPickup.value = Boolean(data?.role === 'main' && data.state.drop && !data.state.drop.collected
    && pickupDistance(player, data.state.drop) <= data.config.pickupRange);
  // 先知只存在于小世界，且只有真正靠近时才应答。
  prophetNearby.value = canTalkToProphet({
    role: data?.role, player, bounds: prophetBounds(), range: PROPHET_INTERACTION.range,
  }) && !dialogueOpen.value;
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
const WORLD_MAX = 6;

const RARITY = {
  common: { label: '凡品', weight: 1 },
  fine: { label: '良品', weight: 2 },
  rare: { label: '珍品', weight: 3 },
  epic: { label: '秘宝', weight: 4 },
};
const GEAR_SLOTS = [
  { id: 'helm', name: '头盔', types: ['重甲', '轻甲'] },
  { id: 'amulet', name: '护符', types: ['护符'] },
  { id: 'main', name: '主兵', types: ['长兵', '刀剑', '弓弩', '法器'] },
  { id: 'armor', name: '战衣', types: ['重甲', '轻甲'] },
  { id: 'off', name: '副兵', types: ['刀剑', '法器', '弓弩', '长兵'] },
  { id: 'belt', name: '腰系', types: ['腰系'] },
  { id: 'boots', name: '足履', types: ['靴履'] },
  { id: 'ring', name: '指环', types: ['佩环'] },
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
  { u: 'armor-1', name: '影纹鳞甲', type: '重甲', cat: 'equip', slot: 'armor', rarity: 'epic', glyph: 'g-armor',
    stats: [['护体', '+186'], ['筋骨', '+24'], ['抗蚀', '+18%']],
    flavor: '鳞片取自深潭老蛟，贴着火光看，纹路会自己游动。' },
  { u: 'main-1', name: '赤霄长兵', type: '长兵', cat: 'equip', slot: 'main', rarity: 'epic', glyph: 'g-sword',
    stats: [['锋芒', '86 - 124'], ['力道', '+31'], ['破甲', '+12%']],
    flavor: '刃上缠着一线不散的血气，主人不问，它也不说。' },
  { u: 'helm-1', name: '兜鍪 · 静水', type: '重甲', cat: 'equip', slot: 'helm', rarity: 'rare', glyph: 'g-helmet',
    stats: [['护体', '+92'], ['定神', '+14']],
    flavor: '面甲内侧刻着半句残咒，据说是铸者留给自己的。' },
  { u: 'amulet-1', name: '青囊护符', type: '护符', cat: 'equip', slot: 'amulet', rarity: 'rare', glyph: 'g-talis',
    stats: [['定神', '+22'], ['回气', '+8/s']],
    flavor: '符纸换了七次，绳结始终没舍得拆。' },
  { u: 'boots-1', name: '踏雾履', type: '靴履', cat: 'equip', slot: 'boots', rarity: 'rare', glyph: 'g-boots',
    stats: [['身法', '+19'], ['疾行', '+9%']],
    flavor: '踩在松针上，声音比风还迟一步。' },
  { u: 'belt-1', name: '缠云腰带', type: '腰系', cat: 'equip', slot: 'belt', rarity: 'fine', glyph: 'g-belt',
    stats: [['护体', '+38'], ['负重', '+12']],
    flavor: '三层素绢，一层一线都是山里人自己纺的。' },
  { u: 'ring-1', name: '残玉指环', type: '佩环', cat: 'equip', slot: 'ring', rarity: 'fine', glyph: 'g-ring',
    stats: [['锋芒', '+9'], ['回气', '+3/s']],
    flavor: '玉上有一道旧裂，握紧时微微发烫。' },
  { u: 'off-1', name: '断水短刃', type: '刀剑', cat: 'equip', slot: 'off', rarity: 'fine', glyph: 'g-sword',
    stats: [['锋芒', '42 - 58'], ['身法', '+11']],
    flavor: '短得只能贴身用，也近得让人来不及退。' },
  { u: 'main-2', name: '乌木猎弓', type: '弓弩', cat: 'equip', slot: 'main', rarity: 'fine', glyph: 'g-bow',
    stats: [['锋芒', '56 - 74'], ['力道', '+14']],
    flavor: '弦是旧弦，换过三次弓臂，准头反而更稳。' },
  { u: 'main-3', name: '镇魂法铃', type: '法器', cat: 'equip', slot: 'main', rarity: 'rare', glyph: 'g-staff',
    stats: [['术法', '+27'], ['定神', '+16']],
    flavor: '摇一下，活人听见的是铃，别的东西听见的是雷。' },
  { u: 'off-2', name: '开山短斧', type: '刀剑', cat: 'equip', slot: 'off', rarity: 'common', glyph: 'g-axe',
    stats: [['锋芒', '33 - 45'], ['力道', '+8']],
    flavor: '斧背豁了几处口，砍柴砍人都不耽误。' },
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
const BAG_LOADOUT = { main: 'main-1', off: 'off-1', armor: 'armor-1', helm: 'helm-1',
  amulet: 'amulet-1', belt: 'belt-1', boots: 'boots-1', ring: 'ring-1' };
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
/* 小世界一览：第一条是主世界；拾到世界碎片后才多出第二条（界隙所辟），
   点它即走原有的换场景流程。 */
const WORLDS_DEF = [
  { name: '無常劫', tier: '凡界', art: { s: 0, m: 0 } },
];
const WORLDS_FRAGMENT = { name: '界隙', tier: '新界', art: { s: 1, m: 1 } };

let bagUidSeq = 0;
const bag = reactive({
  tab: 'all',
  sel: null,
  gear: {},
  cells: [],
  worlds: [],
  worldCur: 0,
  worldView: 0,
  worldNameDraft: '',
  manageOpen: false,
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
function bagSlotsFor(item) {
  if (!item || item.cat !== 'equip') return [];
  return GEAR_SLOTS.filter(slot => slot.types.includes(item.type)).map(slot => slot.id);
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
function bagVisibleWorlds() {
  const owned = bagFragmentOwned();
  return WORLDS_DEF.map((world, index) => ({
    ...world,
    art: { ...(bag.worlds[index]?.art || world.art) },
    name: bag.worlds[index]?.name || world.name,
    tier: bag.worlds[index]?.tier || world.tier,
  })).concat(owned ? [{ ...WORLDS_FRAGMENT, art: { ...(bag.worlds[1]?.art || WORLDS_FRAGMENT.art) },
    name: bag.worlds[1]?.name || WORLDS_FRAGMENT.name, tier: bag.worlds[1]?.tier || WORLDS_FRAGMENT.tier }] : []);
}
/* 可点入的界：拾得碎片后的那一界（在子世界中同样可用它返回主世界） */
function bagEntryIndex() {
  return bagVisibleWorlds().length > 1 ? 1 : -1;
}
/* 碎片尚未到手：那一届不可入 */
function bagEntryLocked(index) {
  return index === bagEntryIndex() && !bagFragmentOwned();
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
      worlds: bag.worlds.map(world => ({ name: world.name, tier: world.tier, art: { ...world.art } })),
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
    bag.cells = Array.from({ length: BAG_TOTAL }, (_, i) => {
      const entry = saved.cells?.[i];
      const def = entry && bagDef(entry.u);
      if (!def) return null;
      const item = bagMakeItem(def);
      if (entry.qty > 1) item.qty = entry.qty;
      return item;
    });
    if (Array.isArray(saved.worlds) && saved.worlds.length) {
      bag.worlds = saved.worlds.slice(0, WORLD_MAX).map(world => ({ name: String(world.name || '小世界'),
        tier: String(world.tier || '新界'), art: { s: Number(world.art?.s) || 0, m: Number(world.art?.m) || 0 } }));
      bag.worldCur = clampWorldIndex(saved.worldCur);
    }
  }
  if (!bag.worlds.length) {
    bag.worlds = WORLDS_DEF.map(world => ({ name: world.name, tier: world.tier, art: { ...world.art } }));
    bag.worldCur = 0;
  }
  // 第二界的外观数据位：拾得碎片后它才会出现在一览里
  while (bag.worlds.length < 2) {
    bag.worlds.push({ name: WORLDS_FRAGMENT.name, tier: WORLDS_FRAGMENT.tier, art: { ...WORLDS_FRAGMENT.art } });
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
function bagIconHTML(item) {
  return item ? `<span class="icon" aria-hidden="true"><svg viewBox="0 0 48 48"><use href="#${item.glyph}"/></svg></span>` : '';
}
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
function bagGearMeta() { return `${GEAR_SLOTS.filter(slot => bag.gear[slot.id]).length} / ${GEAR_SLOTS.length}`; }
function bagItemWeight(item) {
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
function bagManageArt() {
  const world = bagManageWorld.value;
  return world ? normalizeArt(world.art).s : -1;
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
function bagEquipFromSlot(index, preferredSlot) {
  const item = bagCellItems()[index];
  if (!item || item.real) { bagToast('此物随身，不可披掛'); return; }
  if (item.cat !== 'equip') { bagToast('此物无法披掛'); return; }
  let slotId = preferredSlot;
  if (!slotId || !bagSlotsFor(item).includes(slotId)) [slotId] = bagSlotsFor(item);
  if (!slotId) { bagToast('无处安放'); return; }
  const previous = bag.gear[slotId] || null;
  bag.gear[slotId] = item;
  bag.cells[index] = previous;            // 旧物回落到同一格，格阵不塌陷
  bagToast(`披掛 · ${item.name}`);
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
function bagWorldCount() { return bagVisibleWorlds().length; }
function bagCurIndex() {
  const list = bagVisibleWorlds();
  return Math.min(Math.max(bag.worldCur, 0), Math.max(0, list.length - 1));
}
function bagCurWorld() { return bagVisibleWorlds()[bagCurIndex()] || null; }
function bagWorldStatus() {
  const world = bagCurWorld();
  const count = bagWorldCount();
  return world ? `${world.name} · ${world.tier} · ${bagCurIndex() + 1} / ${count}` : '未有界 · 0 / 0';
}
function bagTravelLabel(index) {
  const world = bagVisibleWorlds()[index];
  if (index !== bagEntryIndex()) return `小世界：${world?.name || ''}（当前）`;
  return bagEntryLocked(index)
    ? `小世界：${world?.name || ''}（未辟 · 击败 Boss 并拾取世界碎片后开启）`
    : `小世界：${world?.name || ''}（进入／返回）`;
}
/* 一览里可点入的那一格＝原有的「小世界」按钮：走既有换场景流程 */
async function bagTravel(index) {
  const list = bagVisibleWorlds();
  const world = list[index];
  if (!world) return;
  bag.worldCur = index;
  bag.worldView = index;
  bag.worldNameDraft = world.name;
  if (index !== bagEntryIndex()) { bagToast(`已是 · ${world.name}`); bagSave(); return; }
  if (bagEntryLocked(index) || navigationPending.value) { bagToast('击败 Boss 并拾取世界碎片后开启'); return; }
  bagToast(`入界 · ${world.name}`);
  bagSave();
  // 换场景流程要求行囊仍开着（原来就是这样）；点击本身就是收起行囊
  const travel = navigateWorld();
  camera.resetInput();
  inventoryOpen.value = false;
  await travel;
}
function bagTapWorld() {
  bagToast(bagCurWorld() ? `${bagCurWorld().name} · 图象已转` : '先开一界');
}
function bagOpenManage() {
  bag.manageOpen = true;
  bag.worldView = bagCurIndex();
}
function bagCloseManage() { bag.manageOpen = false; }
function bagToggleManage() { bag.manageOpen ? bagCloseManage() : bagOpenManage(); }
function bagValidateName(raw, ignoreIndex) {
  const name = String(raw ?? '').trim();
  if (!name) { bagToast('界名不可空'); return null; }
  if (name.length > 8) { bagToast('界名至多八字'); return null; }
  if (bagVisibleWorlds().some((world, i) => world.name === name && i !== ignoreIndex)) { bagToast('已有同名之界'); return null; }
  return name;
}
/* 世界一览由玩法决定（拾得碎片才多一界），这里只改它的名字与图象 */
function bagRenameWorld() {
  const list = bagVisibleWorlds();
  const world = list[bag.worldView];
  if (!world) { bagToast('先择一界'); return; }
  const name = bagValidateName(bag.worldNameDraft, bag.worldView);
  if (!name) return;
  if (name === world.name) { bagToast('界名未改'); return; }
  if (!bag.worlds[bag.worldView]) bag.worlds[bag.worldView] = { ...world, art: { ...world.art } };
  bag.worlds[bag.worldView].name = name;
  bag.worldNameDraft = name;
  bagToast(`更名 · ${name}`);
  bagSave();
}
function bagDeleteWorld() {
  const list = bagVisibleWorlds();
  const world = list[bag.worldView];
  if (!world) { bagToast('先择一界'); return; }
  if (bag.worldView === 0) { bagToast('主世界不可碎'); return; }
  // 「碎界」＝舍去这一枚碎片所辟之界
  const index = bag.cells.findIndex(cell => !cell);
  if (index >= 0) bag.cells[index] = { ...FRAGMENT_DEF, uid: FRAGMENT_DEF.u, qty: 1, real: false };
  bag.worldCur = 0;
  bag.worldView = 0;
  bag.worldNameDraft = list[0]?.name || '';
  bagToast(`碎界 · ${world.name}（碎片已归囊中）`);
  bagSave();
}
function bagPickArt(styleIndex) {
  const world = bagVisibleWorlds()[bag.worldView];
  if (!world) return;
  if (!bag.worlds[bag.worldView]) bag.worlds[bag.worldView] = { ...world, art: { ...world.art } };
  bag.worlds[bag.worldView].art = { ...bag.worlds[bag.worldView].art, s: Number(styleIndex) };
  bagToast(`图象已改 · ${Number(styleIndex) + 1} 式`);
  bagSave();
}
function initBag() {
  bagInit();
  bag.worldNameDraft = bagCurWorld()?.name || '';
  bag.tab = bag.tab || 'all';
  selectedSlot.value = 0;
}
const bagFootInfo = computed(bagFoot);
const bagGearMetaText = computed(() => bagGearMeta());
const bagWorld = computed(bagCurWorld);
const bagManageWorld = computed(() => bagVisibleWorlds()[bag.worldView] || null);
const bagWorldScene = computed(() => (bagWorld.value ? worldArtHTML(bagWorld.value.art) : ''));
const bagTabList = computed(() => BAG_TABS.map(tab => ({ ...tab })));
const bagCellList = computed(() => bagCellItems().map((item, index) => ({
  index,
  item,
  filled: Boolean(item),
  img: bagIconHTML(item),
  label: item ? bagRowName(item) : `空格 ${index + 1}`,
})));
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
  : inSubworld.value ? `小世界${bagWorld.value?.name ? ` · ${bagWorld.value.name}` : ''}` : '主世界');
// 小世界没有 Boss、没有怒气消耗、没有攻击目标，整条战斗 HUD 隐藏而不是显示一排禁用控件。
const showCombatHud = computed(() => !inSubworld.value);
function viewLabel() {
  void viewTick.value;
  const mode = camera.viewMode?.();
  return (mode && VIEW_LABELS?.[mode]) || '第三人称';
}
// 操作提示按世界过滤：先知的 F 与碎片的 F 分属两个世界，天然互斥。
const controlHints = computed(() => [
  ['WASD', '移动'],
  ['鼠标', '转向 · 滚轮缩放'],
  ['Space', '跳远'],
  ['Shift', '长按跑步 · 点按闪避'],
  ...(showCombatHud.value ? [['左键', '攻击'], ['E / R', '技能']] : []),
  ...(prophetNearby.value ? [['F', `与${dialogue.name}交谈`]] : []),
  ...(canPickup.value ? [['F', '拾取 世界碎片']] : []),
  ['V', `视角 · ${viewLabel()}`],
  ['Tab', '背包'],
  ['Esc', inSubworld.value ? '保存离开' : '保存退出'],
]);
const exhibitItems = computed(() => {
  void viewTick.value;
  return exhibitItemsFor(gameplayState.value?.state?.inventory, placements.value);
});
const siteNames = PLACEMENT_SITES.map(site => site.name).join(' → ');
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
  if (!prophetNearby.value || dialogueOpen.value) return;
  camera.resetInput();
  dialogueLine.value = 0;
  dialogueOpen.value = true;
  void nextTick(() => dialoguePanel?.focus());
}
function closeDialogue() {
  if (!dialogueOpen.value) return;
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
// F 优先与先知交谈，否则保持原有行为：拾取世界碎片。
function interact() {
  if (prophetNearby.value) openDialogue();
  else void runAction(gameplay.pickup);
}
// 陈列物按固定点位顺序布置，序号取最小空位，删一件不会让其它件重编号。
function nextSite() {
  return PLACEMENT_SITES[placements.value.length % PLACEMENT_SITES.length];
}
const placementSignature = list => JSON.stringify(sanitizePlacements(list)
  .map(entry => [placementGuid(entry.index), entry.propId, ...entry.position, ...entry.rotation, entry.scale]));
async function placementStep() {
  await gameplay.savePlacements(placements.value);
  if (!current()) return false;
  // 成员变化是 reconcile() 的确认输入，场景工作交给它统一处理。
  await gameplay.flush();
  return true;
}
async function placeExhibit(propId) {
  if (!inSubworld.value || !current() || navigation.busy || savingPlayer || actionBusy.value) return;
  const item = exhibitItems.value.find(entry => entry.id === propId);
  if (!item?.available) return;
  const site = nextSite();
  placements.value = sanitizePlacements([...placements.value, {
    propId, index: nextPlacementIndex(placements.value), siteId: site.id,
    position: [...site.position], rotation: [0, 0, 0], scale: 1,
  }]);
  await runAction(async () => {
    try {
      await placementStep();
      showToast(`已陈列 ${item.name} · ${site.name}`);
    } catch (error) {
      // 后端是权威：失败时重新读取，而不是相信本地的错误猜测。
      placements.value = sanitizePlacements(gameplay.placements?.placements);
      throw error;
    }
  });
}
async function withdrawExhibit() {
  if (!inSubworld.value || !current() || !placements.value.length
    || navigation.busy || savingPlayer || actionBusy.value) return;
  placements.value = placements.value.slice(0, -1);
  await runAction(async () => {
    try {
      await placementStep();
      showToast('已撤回最后一件陈列');
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
  // 对话面板打开时独占键盘：Esc 不再退出世界，F/Space 也不会漏给相机或其它交互。
  if (dialogueOpen.value) {
    event.preventDefault();
    event.stopPropagation();
    if (event.code === 'Escape' || event.key === 'Escape') { if (!event.repeat) closeDialogue(); return; }
    if (!event.repeat && ['Enter', 'Space', 'NumpadEnter'].includes(event.code)) advanceDialogue();
    return;
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
  if (!focused) return;
  if (event.button === 0) {
    event.preventDefault();
    void runAction(gameplay.attack);
  } else camera.pointerDown(event);
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
    bossActor = snapshot.actors?.find(actor => actor.actor_guid === STORY_CHARACTERS[1].guid) || null;
    bossBounds = worldBounds(bossActor);
    // 先知只在小世界按需生成，主世界里缺失是正常的：交互保持不可用即可。
    prophetActor = snapshot.actors?.find(actor => actor.actor_guid === PROPHET_GUID) || null;
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
    @pointerdown="onPointerDown" @pointerleave="camera.resetInput" @wheel.prevent="camera.wheel" @contextmenu.prevent>
    <svg class="hud-symbols" aria-hidden="true" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="story-brush" x="-25%" y="-45%" width="150%" height="190%">
          <feTurbulence type="fractalNoise" baseFrequency="0.055 0.09" numOctaves="3" seed="11" result="noise" />
          <feDisplacementMap in="SourceGraphic" in2="noise" scale="4.6" xChannelSelector="R" yChannelSelector="G" />
        </filter>
        <symbol id="story-shard" viewBox="0 0 48 48"><path d="m24 3 14 18-14 24L10 21Z"/><path d="m24 3-4 18 4 24 5-24ZM10 21h28"/></symbol>
        <symbol id="story-heavy" viewBox="0 0 48 48"><path d="M36 5 17 24l7 7L43 12V5ZM13 22l13 13M19 29 8 40M5 37l6 6"/></symbol>
        <symbol id="story-sweep" viewBox="0 0 48 48"><path d="M7 30C-1 8 28-2 41 15M6 34C17 45 40 38 42 25M35 12l7 4 1-9M12 30l20-17-13 24Z"/></symbol>
      </defs>
    </svg>
    <div v-if="initializing" class="story-loading" role="status">{{ gameplayError || '剧情世界加载中…（Esc 取消）' }}</div>
    <div v-if="gameplayState && !initializing" class="story-hud"
      :aria-label="inSubworld ? '小世界界面' : '战斗界面'">
      <!-- 位置指示器：右上是唯一空闲区域，切世界时可被读屏播报 -->
      <div class="world-indicator" role="status" aria-live="polite">{{ worldLabel }}</div>
      <section v-if="bossNearby && !inventoryOpen && showCombatHud" class="boss-status plate" aria-label="Boss 血条">
        <div class="plate__body">
          <span class="haze haze--r" aria-hidden="true"></span>
          <div class="boss-head">
            <span class="boss-seal" aria-hidden="true">
              <svg viewBox="0 0 24 24"><path d="M4 8h16M8 15h8"/></svg>
            </span>
            <span class="boss-name">巨龙</span>
            <span class="boss-number"><b>{{ gameplayState.state.boss.hp }}</b><i>/</i><span>{{ gameplayState.config.bossHp }}</span></span>
          </div>
          <div class="boss-bar bar bar--boss" role="progressbar" aria-label="Boss 生命"
            :aria-valuenow="gameplayState.state.boss.hp" :aria-valuemax="gameplayState.config.bossHp" aria-valuemin="0">
            <span class="bar__lag"></span>
            <span class="bar__fill" :style="{ width: `${bossPercent()}%` }"></span>
            <span class="bar__flash"></span>
            <span class="bar__tick" style="left:20%"></span>
            <span class="bar__tick" style="left:40%"></span>
            <span class="bar__tick" style="left:60%"></span>
            <span class="bar__tick" style="left:80%"></span>
          </div>
        </div>
      </section>
      <aside v-if="!inventoryOpen" class="story-controls tip" aria-label="操作提示">
        <svg class="tip__seal" viewBox="0 0 24 24" aria-hidden="true">
          <path d="M3 6.5h18M3 12h18M3 17.5h18"/>
        </svg>
        <span v-for="[key, text] in controlHints" :key="key"
          :class="{ 'pickup-control': key === 'F' }" :role="key === 'F' ? 'status' : undefined">
          <kbd>{{ key }}</kbd> {{ text }}
        </span>
      </aside>
      <div v-if="!inventoryOpen" class="story-bottom-stack">
        <div v-if="gameplayError" class="gameplay-error" role="alert" @pointerdown.stop @wheel.stop @pointermove.stop="camera.resetInput">
          <span>{{ gameplayError }}</span><button :disabled="actionBusy" @click.stop="runAction(saveRegistration.flush)">重试保存</button>
        </div>
        <div v-if="feedback" class="story-feedback" role="status" aria-live="polite">{{ feedback }}</div>
      </div>
      <section v-if="!inventoryOpen && showCombatHud" class="player-vitals vitals plate" aria-label="人物生命与怒气">
        <div class="plate__body">
          <span class="haze" aria-hidden="true"></span>
          <div class="vitals__row">
            <span class="vitals__seal" aria-hidden="true">
              <svg viewBox="0 0 24 24">
                <path d="M4 7.2c4.6.7 7.6-.4 11.4-.2" />
                <path d="M4.8 12c3.9.6 6.6-.3 9.6-.2" />
                <path d="M3.6 16.6c4.4.7 7.8-.4 11.8-.2" />
              </svg>
            </span>
            <div class="vitals__bars">
              <div class="stat">
                <span class="stat__name">生命</span>
                <div class="vital-bar bar bar--hp" role="progressbar" aria-label="生命"
                  :aria-valuenow="gameplayState.config.playerHp" :aria-valuemax="gameplayState.config.playerHp" aria-valuemin="0">
                  <!-- 生命值恒满：与原界面一致，不伪造掉落 -->
                  <span class="bar__lag" style="width:100%"></span>
                  <span class="bar__fill" style="width:100%"></span>
                  <span class="bar__flash"></span>
                </div>
                <span class="stat__num">{{ gameplayState.config.playerHp }}</span>
              </div>
              <div class="stat">
                <span class="stat__name">怒气</span>
                <div class="vital-bar bar bar--rage" role="progressbar" aria-label="怒气"
                  :aria-valuenow="gameplayState.state.rage" :aria-valuemax="gameplayState.config.rageMax" aria-valuemin="0">
                  <span class="bar__lag" :style="{ width: `${ragePercent()}%` }"></span>
                  <span class="bar__fill" :style="{ width: `${ragePercent()}%` }"></span>
                  <span class="bar__flash"></span>
                </div>
                <span class="stat__num">{{ gameplayState.state.rage }}<i>/{{ gameplayState.config.rageMax }}</i></span>
              </div>
            </div>
          </div>
        </div>
      </section>
      <section v-if="!inventoryOpen && showCombatHud" class="skill-strip skills" aria-label="怒气技能" @pointerdown.stop @pointermove.stop="camera.resetInput">
        <div v-for="(skill, id) in gameplayState.config.skills" :key="id" class="skill-wrap">
          <button type="button" class="skill-tile skill hint-anchor" tabindex="0"
            :class="{ unavailable: !skillStatus(id).available, 'is-cd': skillStatus(id).remainingMs > 0 }"
            :aria-label="`${skill.key} ${skill.name}，消耗 ${skill.rageCost} 怒气`">
            <span class="skill__ring" aria-hidden="true"></span>
            <svg class="skill__glyph" aria-hidden="true"><use :href="`#story-${id}`" /></svg>
            <span class="skill__cost">{{ skill.rageCost }}</span>
            <span class="skill__left">{{ cooldownSeconds(id) }}</span>
            <span class="skill__key" aria-hidden="true">{{ skill.key }}</span>
            <span class="ui-tooltip" role="tooltip">{{ skill.name }} · {{ skill.rageCost }} 怒气</span>
          </button>
          <span class="skill__name">{{ skill.name }}</span>
        </div>
      </section>
      <section v-if="!inventoryOpen && inSubworld" class="subworld-notice" aria-label="小世界状态">
        <strong>{{ worldLabel }}</strong>
        <span>此处为沙盒空间 · 无战斗目标</span>
      </section>
    </div>
    <!-- ============ 先知对话：F 交谈，Enter 继续，Esc 离开 ============ -->
    <div v-if="dialogueOpen" class="dialogue-overlay"
      @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent>
      <section ref="dialoguePanel" class="dialogue-panel" role="dialog" aria-modal="true"
        aria-labelledby="dialogue-title" tabindex="-1">
        <header class="dialogue-header">
          <strong id="dialogue-title">{{ dialogue.name }}</strong>
          <span v-if="dialogue.title">{{ dialogue.title }}</span>
        </header>
        <p class="dialogue-line">{{ dialogue.lines[dialogueLine] }}</p>
        <p v-if="dialogueLine + 1 >= dialogue.lines.length && dialogue.closing" class="dialogue-closing">
          {{ dialogue.closing }}
        </p>
        <footer class="dialogue-footer">
          <span class="dialogue-progress">{{ dialogueLine + 1 }} / {{ dialogue.lines.length }}</span>
          <span class="dialogue-hint">{{ dialogue.hint }}</span>
          <button class="dialogue-next" type="button" @click="advanceDialogue">
            {{ dialogueLine + 1 >= dialogue.lines.length ? '结束交谈' : '继续' }}
          </button>
        </footer>
      </section>
    </div>
    <!-- ============ 行囊（Tab 背包）：复刻 111/bag.html ============ -->
    <div v-if="inventoryOpen && gameplayState" ref="inventoryPanel" class="story-bag"
      aria-label="行囊" @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent @keydown="onInventoryKey">
      <svg class="svgdefs" aria-hidden="true" focusable="false">
        <defs>
          <filter id="sbFBrush" x="-25%" y="-45%" width="150%" height="190%">
            <feTurbulence type="fractalNoise" baseFrequency="0.055 0.09" numOctaves="3" seed="11" result="sbNoise" />
            <feDisplacementMap in="SourceGraphic" in2="sbNoise" scale="4.6" xChannelSelector="R" yChannelSelector="G" />
          </filter>
          <g id="g-sword"><path d="M24 42V9"/><path d="M24 9l-5 7h10z"/><path d="M6 42h36"/><path d="M24 42l-7-8"/></g>
          <g id="g-axe"><path d="M15 7v34"/><path d="M31 9C22.5 8.5 15.6 12.7 15.5 19.5 15.4 25 19.6 28.8 25 29"/><path d="M31 9c5 1.4 9.3 5 12 9-5.6 2.2-12 1.6-17-1.6"/></g>
          <g id="g-bow"><path d="M14 8C31 15 31 33 14 40"/><path d="M14 8L14 40"/><path d="M11 24h27"/><path d="M34 19l6 5-6 5"/></g>
          <g id="g-staff"><path d="M36 5C19 15 12 31 9 43"/><path d="M25 17l11-11"/><path d="M31 10l6 5"/><circle cx="33" cy="10" r="3"/></g>
          <g id="g-helmet"><path d="M10 33c0-10 6-18 14-18s14 8 14 18"/><path d="M8 33h32"/><path d="M24 15v18"/><path d="M17 24h14"/></g>
          <g id="g-armor"><path d="M24 6l12 4v11c0 9-5.5 16.5-12 21-6.5-4.5-12-12-12-21V10z"/><path d="M18 18h12"/><path d="M24 13v14"/></g>
          <g id="g-belt"><path d="M4 21h40v6H4z"/><path d="M31 17v14"/><path d="M17 17v14"/></g>
          <g id="g-boots"><path d="M17 6v19l-8 8v3h9V6z"/><path d="M31 6v19l8 8v-3"/><path d="M31 6h-6"/><path d="M6 36h36"/></g>
          <g id="g-ring"><circle cx="24" cy="24" r="10"/><path d="M24 14v-6"/><path d="M20 5h8"/><path d="M24 10l-3 4h6z"/></g>
          <g id="g-elixir"><path d="M19 5h10v6"/><path d="M21 11c-7 4.4-9 10.4-6.5 16.6C17 33.8 22.4 36 24 36s7-2.2 9.5-8.4C36 21.4 34 15.4 27 11"/><path d="M13 27h22"/></g>
          <g id="g-pill"><path d="M24 5l14 10.5v17L24 43 10 32.5v-17z"/><path d="M10 15.5l14 10 14-10"/><path d="M24 25.5V43"/></g>
          <g id="g-scroll"><path d="M13 8h22v30H13z"/><path d="M18 16h12"/><path d="M18 22h12"/><path d="M18 28h7"/></g>
          <g id="g-talis"><path d="M17 7h14v6H17z"/><path d="M18 13c-2 12 1 22 6 30 5-8 8-18 6-30"/><path d="M21 22h6"/></g>
          <g id="g-ore"><path d="M14 15l10-8 10 8-4 15-12 0z"/><path d="M14 15h20"/><path d="M24 7v23"/><path d="M24 30v13"/></g>
          <g id="g-herb"><path d="M24 42V13"/><path d="M24 19c-6-1-10-6-10-11 6-.6 10 4 10 11"/><path d="M24 27c6-1.4 10-6.6 10-12-6-.6-10 4.8-10 12"/><path d="M14 42h20"/></g>
          <g id="g-fang"><path d="M21 5l6 8-3 26"/><path d="M27 13l7 6-6 4"/><path d="M19 41l3-4"/></g>
          <g id="g-silk"><path d="M8 17c7-6.4 14 6.4 21 0s9 .4 11 3"/><path d="M8 28c7-6.4 14 6.4 21 0s9 .4 11 3"/></g>
          <g id="g-coal"><path d="M13 28l5-15 12 4 5 13-11 9z"/><path d="M18 13l-5 15"/><path d="M30 17l5 13"/></g>
        </defs>
      </svg>

      <div class="stage" aria-hidden="true"></div>

      <div class="bagStage" role="dialog" aria-modal="true" aria-label="行囊">
        <section class="bag plate" id="bag" aria-label="行囊">
          <div class="plate__body bag__body">
            <span class="haze" aria-hidden="true"></span>

            <!-- 标题栏（隐去标题，仅留右上角关闭钮） -->
            <header class="bag__head">
              <span class="bag__seal" aria-hidden="true">
                <svg viewBox="0 0 24 24">
                  <path d="M4 8.6c4.6.7 7.6-.4 11.4-.2" />
                  <path d="M4.8 13c3.9.6 6.6-.3 9.6-.2" />
                  <path d="M3.6 17.2c4.4.7 7.8-.4 11.8-.2" />
                </svg>
              </span>
              <div class="bag__titles">
                <h1 class="bag__title">行囊</h1>
                <span class="bag__sub">無常劫 · 隨身所負 · 結印以啟</span>
              </div>
              <svg class="bag__headline" viewBox="0 0 300 20" preserveAspectRatio="none" aria-hidden="true">
                <path d="M5 12C60 5 118 16 178 9S266 5 296 11" filter="url(#sbFBrush)" />
              </svg>
              <div class="purse" title="随身金铢">
                <span class="purse__coin" aria-hidden="true"></span>
                <span class="purse__num">12,480</span>
              </div>
              <button type="button" class="closeBtn" data-inventory-close aria-label="合上行囊" title="合上行囊（Esc）"
                :disabled="navigationPending" @click="toggleInventory"><span>✕</span></button>
            </header>

            <div class="bag__cols">
              <!-- 左：符纸人形 + 装备八格 -->
              <div class="colL">
                <div class="secHead">
                  <span class="secHead__dot" aria-hidden="true"></span>
                  <span class="secHead__txt">披掛</span>
                  <svg class="secHead__line" viewBox="0 0 200 12" preserveAspectRatio="none" aria-hidden="true">
                    <path d="M3 7C40 2 78 10 118 5s56-1 79 3" filter="url(#sbFBrush)" />
                  </svg>
                  <span class="secHead__meta">{{ bagGearMetaText }}</span>
                </div>

                <div class="dollWrap">
                  <div class="doll">
                    <button v-for="slot in GEAR_SLOTS" :key="slot.id" type="button" class="eslot hint-anchor"
                      :data-slot="slot.id" :class="{ has: Boolean(bag.gear[slot.id]), ['r-' + (bag.gear[slot.id]?.rarity || 'common')]: true }"
                      :aria-label="bag.gear[slot.id] ? `${slot.name}：${bag.gear[slot.id].name}` : `${slot.name}（空）`"
                      @click="bagSelectGear(slot.id)" @dblclick="bagUnequip(slot.id)"
                      @pointerenter="bag.gear[slot.id] && bagShowTip(bag.gear[slot.id], $event)"
                      @pointermove="bagMoveTip($event)" @pointerleave="bagHideTip">
                      <span class="eslot__ph">{{ slot.name }}</span>
                      <span v-if="bag.gear[slot.id]" class="icon" aria-hidden="true" v-html="bagIconHTML(bag.gear[slot.id])" />
                    </button>
                  </div>
                </div>
              </div>

              <!-- 右：分类 + 格阵 -->
              <div class="colR">
                <div class="tabs" role="tablist" aria-label="分类">
                  <button v-for="tabItem in bagTabList" :key="tabItem.id" type="button" class="tab" role="tab"
                    :aria-selected="bag.tab === tabItem.id ? 'true' : 'false'" @click="bag.tab = tabItem.id">{{ tabItem.name }}</button>
                </div>

                <div class="gridWrap">
                  <div class="grid" role="listbox" aria-label="背包格">
                    <button v-for="cell in bagCellList" :key="cell.index" type="button"
                      class="cell inventory-slot hint-anchor" :class="{ has: cell.filled, ['r-' + (cell.item?.rarity || 'common')]: true }"
                      :style="{ '--i': String(cell.index), width: 'var(--s)', height: 'var(--s)' }"
                      :disabled="!cell.filled" :tabindex="selectedSlot === cell.index ? 0 : -1"
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

              <!-- 左下：小世界栏 -->
              <section class="world plate" aria-label="小世界">
                <div class="plate__body world__body">
                  <span class="haze" aria-hidden="true"></span>

                  <header class="world__head">
                    <span class="world__seal" aria-hidden="true">
                      <svg viewBox="0 0 24 24">
                        <path d="M4 8.6c4.6.7 7.6-.4 11.4-.2" />
                        <path d="M4.8 13c3.9.6 6.6-.3 9.6-.2" />
                        <path d="M3.6 17.2c4.4.7 7.8-.4 11.8-.2" />
                      </svg>
                    </span>
                    <h2 class="world__title">小世界</h2>
                    <span class="world__status">{{ bagWorldStatus() }}</span>
                    <button type="button" class="world__manage" aria-label="管理小世界"
                      :aria-expanded="bag.manageOpen ? 'true' : 'false'" title="管理小世界"
                      @click="bagToggleManage">管</button>
                  </header>

                  <div class="world__row">
                    <div class="world__art" :aria-label="bagWorld ? `当前小世界：${bagWorld.name}` : '尚未开辟小世界'"
                      @click="bagTapWorld" v-html="bagWorldScene" />

                    <div class="world__list" role="listbox" aria-label="小世界一览">
                      <template v-if="bagWorldList().length">
                        <button v-for="(world, index) in bagWorldList()" :key="`${world.name}-${index}`" type="button"
                          class="wnode hint-anchor" :class="{ 'is-cur': index === bagCurIndex(), 'is-entry': index === bagEntryIndex() }"
                          :disabled="bagEntryLocked(index)"
                          :data-inventory-travel="index === bagEntryIndex() ? '' : undefined"
                          :data-inventory-travel-focus="index === bagEntryIndex() ? '' : undefined"
                          role="option" :aria-selected="index === bagCurIndex() ? 'true' : 'false'"
                          :aria-label="bagTravelLabel(index)" @click="bagTravel(index)">
                          <span class="wnode__art" aria-hidden="true" v-html="worldThumbHTML(world.art)" />
                          <span class="wnode__name">{{ world.name }}</span>
                          <span class="wnode__dot" aria-hidden="true" />
                          <span v-if="bagEntryLocked(index)" class="ui-tooltip" role="tooltip">击败 Boss 并拾取世界碎片后开启</span>
                        </button>
                      </template>
                      <p v-else class="world__empty">未有界 · 拾得世界碎片后自辟</p>
                    </div>
                  </div>

                  <!-- 陈列：只在小世界发生。主世界读到的布局恒为空，写入也被后端拒绝。 -->
                  <div v-if="inSubworld" class="exhibit">
                    <header class="exhibit__head">
                      <span class="secHead__dot" aria-hidden="true"></span>
                      <span class="secHead__txt">陈列主世界的物质</span>
                      <span class="exhibit__total">{{ placements.length }} 件在展</span>
                    </header>
                    <p v-if="!exhibitItems.some(item => item.owned)" class="exhibit__empty">
                      尚未从小世界之外带回物质。击败巨龙取得世界碎片后即可陈列。
                    </p>
                    <ul v-else class="exhibit__list">
                      <li v-for="item in exhibitItems" :key="item.id" :class="{ 'is-empty': !item.owned }">
                        <div class="exhibit__info">
                          <strong>{{ item.name }}</strong>
                          <span class="exhibit__count">持有 {{ item.owned }} · 已陈列 {{ item.placed }}</span>
                          <span class="exhibit__desc">{{ item.description }}</span>
                        </div>
                        <button type="button" class="exhibit__act"
                          :disabled="!item.available || actionBusy || navigationPending"
                          :aria-label="item.available ? `将${item.name}陈列到小世界` : `${item.name}已全部陈列`"
                          @click="placeExhibit(item.id)">
                          {{ item.available ? '陈列' : '已全部陈列' }}
                        </button>
                      </li>
                    </ul>
                    <p v-if="placements.length" class="exhibit__hint">陈列点按顺序循环：{{ siteNames }}</p>
                    <button v-if="placements.length" type="button" class="exhibit__back"
                      :disabled="actionBusy || navigationPending" @click="withdrawExhibit">撤回最后一件</button>
                  </div>
                </div>

                <!-- 管理模块：改其名与图象（小世界由玩法决定，不在此增删） -->
                <div class="world__manageWrap plate" :class="{ 'is-hidden': !bag.manageOpen }" role="dialog"
                  aria-label="管理小世界" :aria-hidden="bag.manageOpen ? 'false' : 'true'">
                  <div class="plate__body world__mBody">
                    <header class="world__mHead">
                      <span class="secHead__dot" aria-hidden="true"></span>
                      <span class="secHead__txt">掌界</span>
                      <span class="world__mHint">小世界 {{ bagWorldCount() }} / {{ WORLD_MAX }}</span>
                      <button type="button" class="world__mClose" aria-label="收起管理模块" title="收起"
                        @click="bagCloseManage">✕</button>
                    </header>

                    <div class="wrow">
                      <label class="wrow__name" for="bagWorldName">界名</label>
                      <input id="bagWorldName" v-model="bag.worldNameDraft" class="wrow__input" type="text" maxlength="8"
                        autocomplete="off" spellcheck="false" placeholder="至多八字"
                        @keydown.enter.prevent.stop="bagRenameWorld" @keydown.esc.prevent.stop="bagCloseManage" />
                    </div>

                    <div class="wrow">
                      <span class="wrow__name">图象</span>
                      <div class="wam" role="radiogroup" aria-label="世界图象">
                        <button v-for="(shape, styleIndex) in WS_SHAPES" :key="styleIndex" type="button" class="wam__opt"
                          :class="{ 'is-on': bagManageArt() === styleIndex }" role="radio"
                          :aria-checked="bagManageArt() === styleIndex ? 'true' : 'false'"
                          :aria-label="`世界图象 ${styleIndex + 1} 式`" @click="bagPickArt(styleIndex)"
                          v-html="worldThumbHTML({ s: styleIndex, m: 0 })" />
                      </div>
                    </div>

                    <div class="world__acts">
                      <button type="button" class="dbtn" :disabled="!bagManageWorld" @click="bagRenameWorld">更名</button>
                      <button type="button" class="dbtn dbtn--ghost"
                        :disabled="!bagManageWorld || bag.worldView === 0" @click="bagDeleteWorld">碎界</button>
                      <span class="world__mMeta">{{ bagManageWorld ? `正在改：${bagManageWorld.name}` : '先择一界' }}</span>
                    </div>
                  </div>
                </div>
              </section>

              <!-- 右下：详情面板 -->
              <div class="detailWrap">
                <div v-if="bagDetailInfo" class="detail">
                  <div class="detail__preview" :class="`r-${bagDetailInfo.item.rarity}`" v-html="bagDetailInfo.preview" />
                  <div class="detail__info">
                    <h2 class="detail__name">{{ bagDetailInfo.item.name }}
                      <span class="detail__rarity" :class="`r-${bagDetailInfo.item.rarity}`">{{ bagDetailInfo.rarity }}</span>
                      <span v-if="bagDetailInfo.item.qty > 1" class="detail__type">× {{ bagDetailInfo.item.qty }}</span>
                    </h2>
                    <p class="detail__type">{{ bagDetailInfo.typeLine }}</p>
                    <div class="detail__sep"></div>
                    <dl class="stats">
                      <div v-for="pair in bagDetailInfo.stats" :key="pair[0]" class="stat-row">
                        <dt>{{ pair[0] }}</dt><dd>{{ pair[1] }}</dd>
                      </div>
                    </dl>
                    <p class="flavor">{{ bagDetailInfo.flavor }}</p>
                    <div class="detail__acts">
                      <button v-for="action in bagDetailInfo.actions" :key="action.act" type="button"
                        class="dbtn" :class="{ 'dbtn--ghost': action.ghost }"
                        @click="bagDetailAct(bagDetailInfo.uid, action.act)">{{ action.label }}</button>
                    </div>
                  </div>
                </div>
                <p v-else class="detail__empty">未择一物 · 点选囊中器物以观其详</p>
              </div>
            </div>

            <!-- 底栏 -->
            <footer class="bag__foot">
              <div class="meter">
                <span class="meter__name">囊中</span>
                <span class="bar"><span class="bar__fill" :style="{ width: bagFootInfo.capWidth }" /></span>
                <span class="meter__num">{{ bagFootInfo.capText }}</span>
              </div>
              <div class="meter">
                <span class="meter__name">負重</span>
                <span class="bar bar--weight"><span class="bar__fill" :class="{ 'is-over': bagFootInfo.over }"
                  :style="{ width: bagFootInfo.weightWidth }" /></span>
                <span class="meter__num">{{ bagFootInfo.weightText }}</span>
              </div>
              <div class="hints">
                <span>单击拾取 · <b>双击</b> 披掛</span>
                <span>点小世界第一格出入小世界 · <b>Esc</b> 合上</span>
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
          <span v-if="bag.tipItem.qty > 1" class="tip__rarity" style="color:rgba(233,224,204,.55)">× {{ bag.tipItem.qty }}</span>
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
   设计令牌：东方暗黑神话色板 / 切角金边 / 墨色浓度
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
  --slot: clamp(50px, 6.2vw, 74px);
  --gap-edge: clamp(10px, 2.1vw, 30px);
  --cut: 14px;
  --edge: 2px;
  --shape: polygon(var(--cut) 0, 100% 0, 100% calc(100% - var(--cut)), calc(100% - var(--cut)) 100%, 0 100%, 0 var(--cut));
  --boss-top: clamp(10px, 1.7vh, 22px);
  --toast-top: calc(var(--boss-top) + 64px);
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
  box-sizing: border-box;
  clip-path: var(--shape);
  background-color: rgba(26, 23, 18, .94);
  background-image:
    repeating-linear-gradient(90deg, var(--pattern) 0 1px, transparent 1px 9px),
    repeating-linear-gradient(90deg, rgba(233, 224, 204, .05) 0 2px, transparent 2px 16px),
    repeating-linear-gradient(58deg, rgba(233, 224, 204, .03) 0 1px, transparent 1px 13px),
    repeating-linear-gradient(-58deg, rgba(233, 224, 204, .022) 0 1px, transparent 1px 17px),
    repeating-linear-gradient(114deg, rgba(233, 224, 204, .035) 0 1px, transparent 1px 6px),
    radial-gradient(120% 92% at 8% 0%, rgba(233, 224, 204, .11), transparent 62%),
    radial-gradient(92% 120% at 100% 112%, rgba(179, 56, 42, .11), transparent 62%),
    linear-gradient(180deg, rgba(70, 61, 48, var(--panel-a)), rgba(25, 22, 18, .94));
  background-size: 100% 5px, 100% 3px, auto, auto, auto, auto, auto, auto;
  background-position: left top, left bottom, 0 0, 0 0, 0 0, 0 0, 0 0, 0 0;
  background-repeat: no-repeat, no-repeat, repeat, repeat, repeat, no-repeat, no-repeat, no-repeat;
  box-shadow: inset 0 0 0 1px rgba(0, 0, 0, .62), inset 0 0 30px rgba(0, 0, 0, .5);
}

.plate__body > * { position: relative; z-index: 1; }

/* 朱砂小菱：贴在切角处，作为点睛 */
.plate__body::before {
  content: '';
  position: absolute;
  left: 8px;
  top: 8px;
  width: 5px;
  height: 5px;
  z-index: 2;
  background: linear-gradient(135deg, var(--c-cinnabar-bright), var(--c-cinnabar));
  transform: rotate(45deg);
  opacity: .9;
  box-shadow: 0 0 9px rgba(179, 56, 42, .5);
}

/* 局部墨迹晕染（只贴在控件边缘，不铺满屏幕） */
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
   HUD 总容器
   ========================================================= */
.story-loading {
  position: absolute;
  inset: 0;
  z-index: 30;
  display: grid;
  place-items: center;
  background: radial-gradient(circle at 50% 46%, rgba(24, 21, 17, .72), rgba(5, 5, 4, .94) 78%);
  color: var(--c-paper-dim);
  font-size: clamp(12px, 1.2vw, 15px);
  letter-spacing: .34em;
  text-indent: .34em;
}

.hud-symbols { position: absolute; width: 0; height: 0; overflow: hidden; }

.story-hud {
  position: absolute;
  inset: 0;
  pointer-events: none;
  z-index: 20;
}

/* ------------------ 顶部中央：首领血条 ------------------ */
.boss-status {
  position: absolute;
  left: 50%;
  transform: translateX(-50%);
  top: var(--boss-top);
  width: min(600px, 44vw);
}

.boss-status .plate__body { padding: clamp(7px, .9vh, 10px) clamp(10px, 1vw, 14px) clamp(8px, 1vh, 11px); }
.boss-status .plate__body::before { left: auto; right: 8px; top: 8px; }

.boss-head { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.boss-seal { flex: none; width: 16px; height: 16px; display: grid; place-items: center; opacity: .9; }
.boss-seal svg { width: 16px; height: 16px; }
.boss-seal path { fill: none; stroke: var(--c-cinnabar-bright); stroke-width: 3; stroke-linecap: round; }

.boss-name {
  font-family: var(--font-brush);
  font-size: clamp(11px, 1.15vw, 14px);
  letter-spacing: .2em;
  color: #e7d9b6;
  text-shadow: 0 1px 3px rgba(0, 0, 0, .95), 0 0 12px rgba(179, 56, 42, .25);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.boss-number {
  margin-left: auto;
  flex: none;
  font-size: clamp(9px, .95vw, 11px);
  font-variant-numeric: tabular-nums;
  color: var(--c-paper-dim);
  opacity: .85;
  text-shadow: 0 1px 2px #000;
}

.boss-number b { color: var(--c-gold-bright); font-weight: 600; }
.boss-number i { font-style: normal; opacity: .45; margin: 0 2px; }

/* ------------------ 通用状态条 ------------------ */
.bar {
  position: relative;
  flex: 1;
  min-width: 0;
  height: var(--bar-h, 13px);
  overflow: hidden;
  border-radius: 2px 12px 12px 2px;
  background: linear-gradient(180deg, #1c1813 0%, #110f0c 34%, #080706 72%, #050504 100%);
  border: var(--edge) solid var(--gold-line-soft);
  box-shadow: inset 0 2px 5px rgba(0, 0, 0, .85), inset 0 -1px 0 rgba(233, 224, 204, .07);
}

.bar__lag, .bar__fill { position: absolute; left: 0; top: 0; bottom: 0; width: 0; }

.bar__lag {
  width: 100%;
  background: linear-gradient(180deg,
    rgba(233, 224, 204, .42) 0%, rgba(233, 224, 204, .26) 38%,
    rgba(233, 224, 204, .12) 72%, rgba(233, 224, 204, .05) 100%);
  transition: width .62s cubic-bezier(.25, .8, .3, 1) .34s;
}

.bar__fill {
  transition: width .13s linear;
  border-right: 1px solid rgba(255, 240, 205, .5);
  box-shadow:
    inset 0 1px 0 rgba(255, 255, 255, .55),
    inset 0 5px 7px -4px rgba(255, 255, 255, .30),
    inset 0 -2px 3px rgba(0, 0, 0, .55),
    inset 0 -6px 8px -4px rgba(0, 0, 0, .75);
}

.bar__fill::after {
  content: '';
  position: absolute;
  left: 1px;
  right: 1px;
  top: 1px;
  height: 1px;
  background: linear-gradient(90deg, rgba(255, 255, 255, .6), rgba(255, 255, 255, .06));
  opacity: .5;
}

.bar--hp .bar__fill {
  background: linear-gradient(180deg, #e97a60 0%, #cb4a36 30%, #a3321f 58%, #71200f 82%, #420f07 100%);
  animation: breathe 3.6s ease-in-out infinite;
}

.bar--rage .bar__fill {
  background: linear-gradient(180deg, #f7e6b4 0%, #d8b569 30%, #b08c3f 58%, #7d6023 82%, #48370f 100%);
  animation: breathe 3s ease-in-out infinite .8s;
}

.bar--boss { --bar-h: 14px; border-color: rgba(200, 166, 89, .38); }

.bar--boss .bar__fill {
  background: linear-gradient(180deg, #f0805f 0%, #cf4d38 30%, #a3311f 58%, #701f0f 82%, #3f0e07 100%);
}

.bar--boss .bar__lag {
  transition: width .8s cubic-bezier(.3, .7, .2, 1) .38s;
  background: linear-gradient(180deg,
    rgba(233, 224, 204, .46) 0%, rgba(233, 224, 204, .28) 38%,
    rgba(233, 224, 204, .13) 72%, rgba(233, 224, 204, .05) 100%);
}

/* 人物状态条比首领血条窄一号 */
.vital-bar { --bar-h: 12px; }

.bar__flash {
  position: absolute;
  inset: 0;
  opacity: 0;
  pointer-events: none;
  background: linear-gradient(180deg, rgba(255, 255, 255, .96), rgba(255, 255, 255, .42));
}

/* 首领血条的刻度：斜切浅线 */
.bar__tick {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  transform: skewX(-18deg);
  background: linear-gradient(180deg, rgba(233, 224, 204, .42), rgba(233, 224, 204, .08));
  box-shadow: 0 0 5px rgba(0, 0, 0, .9);
}

/* ------------------ 左下：生命 / 怒气 ------------------ */
.player-vitals {
  position: absolute;
  left: var(--gap-edge);
  bottom: var(--gap-edge);
  width: clamp(196px, 25vw, 318px);
  pointer-events: auto;
}

.player-vitals .plate__body { padding: clamp(9px, 1.2vh, 13px) clamp(11px, 1.2vw, 16px); }
.vitals__row { display: flex; align-items: center; gap: clamp(8px, 1vw, 12px); }

/* 朱砂小印（抽象三笔） */
.vitals__seal {
  flex: none;
  width: 26px;
  height: 26px;
  display: grid;
  place-items: center;
  border: var(--edge) solid rgba(214, 96, 72, .8);
  background: linear-gradient(180deg, rgba(179, 56, 42, .28), rgba(179, 56, 42, .06));
  border-radius: 2px 8px 2px 2px;
  box-shadow: 0 0 12px rgba(179, 56, 42, .26), inset 0 0 8px rgba(0, 0, 0, .6);
}

.vitals__seal svg { width: 16px; height: 16px; }
.vitals__seal path { fill: none; stroke: var(--c-cinnabar-bright); stroke-width: 1.7; stroke-linecap: round; }
.vitals__bars { flex: 1; display: grid; gap: clamp(6px, .9vh, 10px); min-width: 0; }

.stat { display: flex; align-items: center; gap: clamp(6px, .8vw, 10px); min-width: 0; }

.stat__name {
  flex: none;
  min-width: 2.4em;
  white-space: nowrap;
  font-family: var(--font-brush);
  font-size: clamp(10px, 1.02vw, 13px);
  letter-spacing: .18em;
  color: var(--c-paper-dim);
  text-shadow: 0 1px 2px rgba(0, 0, 0, .9);
}

.stat__num {
  flex: none;
  min-width: 2.9em;
  text-align: right;
  white-space: nowrap;
  font-size: clamp(9px, .92vw, 12px);
  font-variant-numeric: tabular-nums;
  color: var(--c-gold);
  opacity: .88;
  text-shadow: 0 1px 2px rgba(0, 0, 0, .9);
}

.stat__num i { font-style: normal; opacity: .5; }

/* ------------------ 右下：技能槽 ------------------ */
.skills {
  position: absolute;
  right: var(--gap-edge);
  bottom: var(--gap-edge);
  display: flex;
  align-items: flex-end;
  gap: clamp(9px, 1.2vw, 18px);
  pointer-events: auto;
}

.skill-wrap { display: grid; justify-items: center; gap: 6px; }

.skill {
  position: relative;
  width: var(--slot);
  height: var(--slot);
  padding: 0;
  border-radius: 50%;
  cursor: pointer;
  color: #ded2b6;
  pointer-events: auto;
  border: var(--edge) solid rgba(200, 166, 89, .55);
  background:
    radial-gradient(circle at 34% 26%, rgba(233, 224, 204, .10), transparent 56%),
    radial-gradient(circle at 72% 80%, rgba(179, 56, 42, .14), transparent 56%),
    linear-gradient(180deg, rgba(26, 24, 20, .94), rgba(8, 8, 8, .96));
  box-shadow:
    inset 0 0 14px rgba(0, 0, 0, .9),
    inset 0 1px 0 rgba(233, 224, 204, .07),
    0 4px 16px rgba(0, 0, 0, .65),
    0 0 0 3px rgba(0, 0, 0, .35);
  transition: transform .16s ease, box-shadow .22s ease, border-color .22s ease;
  outline: none;
}

.skill__ring { position: absolute; inset: 4px; border-radius: 50%; border: 1px solid rgba(200, 166, 89, .24); }
.skill__ring::after { content: ''; position: absolute; inset: 4px; border-radius: 50%; border: 1px dashed rgba(233, 224, 204, .10); }

/* 岩石底纹（局部材质） */
.skill::after {
  content: '';
  position: absolute;
  inset: 0;
  border-radius: 50%;
  pointer-events: none;
  opacity: .5;
  mix-blend-mode: screen;
  background-image:
    repeating-linear-gradient(70deg, rgba(233, 224, 204, .05) 0 1px, transparent 1px 6px),
    radial-gradient(circle at 30% 70%, rgba(233, 224, 204, .05), transparent 45%);
}

.skill__glyph {
  position: absolute;
  inset: 0;
  display: grid;
  place-items: center;
  pointer-events: none;
  transition: opacity .25s ease, transform .25s ease;
}

/* 图标本体是按钮的直属子节点，这里统一笔触与颜色（居中由 .skill__glyph 负责） */
.skill-tile > svg {
  width: 52%;
  height: 52%;
  overflow: visible;
  color: #f0d08d;
  fill: none;
  stroke: currentColor;
  stroke-width: 2.1;
  stroke-linecap: round;
  stroke-linejoin: round;
}

.skill__cost {
  position: absolute;
  left: 12%;
  top: 6%;
  font-size: clamp(8px, .85vw, 10px);
  letter-spacing: .06em;
  color: rgba(143, 184, 160, .9);
  text-shadow: 0 1px 2px #000;
  pointer-events: none;
}

.skill__key {
  position: absolute;
  right: -5px;
  bottom: -5px;
  min-width: 21px;
  height: 21px;
  padding: 0 3px;
  display: grid;
  place-items: center;
  font-size: 11px;
  letter-spacing: .04em;
  color: #f2e6c8;
  background: linear-gradient(180deg, #7a3326, #2c110c);
  border: var(--edge) solid rgba(214, 182, 116, .75);
  box-shadow: 0 2px 9px rgba(0, 0, 0, .8), inset 0 1px 0 rgba(255, 255, 255, .14);
  pointer-events: none;
}

.skill__name {
  font-family: var(--font-brush);
  font-size: clamp(10px, 1vw, 12px);
  letter-spacing: .3em;
  text-indent: .3em;
  color: var(--c-paper-dim);
  opacity: .9;
  text-shadow: 0 1px 2px rgba(0, 0, 0, .9);
}

.skill__left {
  position: absolute;
  inset: 0;
  display: grid;
  place-items: center;
  font-size: clamp(14px, 1.7vw, 20px);
  font-variant-numeric: tabular-nums;
  color: #f2e8ce;
  text-shadow: 0 0 9px rgba(0, 0, 0, .95), 0 1px 0 rgba(0, 0, 0, .9);
  pointer-events: none;
}

/* 冷却中压暗符文，让倒计时数字清晰可读 */
.skill.is-cd .skill__glyph { opacity: .22; transform: scale(.9); }

.skill-tile.unavailable { border-color: #68624e; }
.skill-tile.unavailable .skill__glyph { opacity: .35; filter: grayscale(1); }
.skill-tile.unavailable .skill__glyph path { stroke: #cfc7ae; }

.skill:hover {
  transform: translateY(-2px);
  border-color: rgba(240, 214, 143, .6);
  box-shadow: inset 0 0 14px rgba(0, 0, 0, .9), 0 6px 18px rgba(0, 0, 0, .7), 0 0 18px rgba(200, 166, 89, .22), 0 0 0 3px rgba(0, 0, 0, .35);
}

/* ------------------ 左上：操作提示 ------------------ */
.tip {
  position: absolute;
  left: var(--gap-edge);
  top: clamp(12px, 1.8vh, 22px);
  display: grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: 6px 9px;
  max-width: min(300px, 30vw);
  padding: 9px 13px 9px 10px;
  font-size: clamp(10px, 1vw, 12.5px);
  line-height: 1.5;
  color: rgba(233, 224, 204, .72);
  letter-spacing: .06em;
  clip-path: polygon(8px 0, 100% 0, 100% calc(100% - 8px), calc(100% - 8px) 100%, 0 100%, 0 8px);
  border: var(--edge) solid transparent;
  background:
    linear-gradient(180deg, rgba(46, 40, 32, .80), rgba(22, 19, 16, .70)) padding-box,
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .18) 0 1px, transparent 1px 4px) border-box,
    linear-gradient(140deg, rgba(240, 214, 143, .85), rgba(122, 98, 52, .42) 52%, rgba(240, 214, 143, .75)) border-box;
  box-shadow: inset 0 0 22px rgba(0, 0, 0, .45);
  animation: breathe-soft 5.2s ease-in-out infinite;
}

.tip__seal { grid-row: span 9; flex: none; width: 15px; height: 15px; align-self: start; }
.tip__seal path { fill: none; stroke: var(--c-cinnabar-bright); stroke-width: 1.9; stroke-linecap: round; }
.tip span { display: flex; align-items: center; gap: 10px; white-space: nowrap; }

kbd {
  font-family: inherit;
  font-size: 11px;
  color: #dfcb94;
}

.tip kbd {
  min-width: 42px;
  padding: 2px 3px;
  border: 1px solid rgba(200, 166, 89, .45);
  background: rgba(200, 166, 89, .10);
  text-align: center;
  color: var(--c-gold-bright);
}

.pickup-control { color: #ffdf8d; }
.pickup-control kbd { border-color: #d9b45c; background: rgba(217, 180, 92, .18); }

/* ------------------ 底部中央：错误 / 反馈 ------------------ */
.story-bottom-stack {
  position: absolute;
  left: 50%;
  transform: translateX(-50%);
  bottom: calc(var(--gap-edge) + clamp(72px, 11vh, 122px));
  width: min(490px, calc(100vw - 32px));
  display: grid;
  gap: 9px;
  justify-items: center;
}

.gameplay-error {
  pointer-events: auto;
  display: flex;
  align-items: center;
  gap: 10px;
  width: 100%;
  padding: 10px 12px;
  font-size: 13px;
  color: #ffcac0;
  overflow-wrap: anywhere;
  clip-path: polygon(10px 0, 100% 0, 100% calc(100% - 10px), calc(100% - 10px) 100%, 0 100%, 0 10px);
  border: var(--edge) solid rgba(224, 82, 59, .6);
  background: linear-gradient(180deg, rgba(58, 22, 18, .92), rgba(26, 12, 10, .9));
  box-shadow: 0 8px 22px rgba(0, 0, 0, .55), inset 0 0 20px rgba(0, 0, 0, .5);
}

.gameplay-error button {
  flex-shrink: 0;
  padding: 5px 10px;
  font-family: inherit;
  color: #f3d9b4;
  background: rgba(179, 56, 42, .28);
  border: 1px solid rgba(214, 182, 116, .6);
  cursor: pointer;
}

.story-feedback {
  justify-self: center;
  padding: 5px 16px;
  font-family: var(--font-brush);
  font-size: clamp(12px, 1.25vw, 15px);
  letter-spacing: .24em;
  text-indent: .24em;
  color: #efdfba;
  white-space: nowrap;
  clip-path: polygon(9px 0, 100% 0, 100% calc(100% - 9px), calc(100% - 9px) 100%, 0 100%, 0 9px);
  border: var(--edge) solid transparent;
  background:
    linear-gradient(180deg, rgba(40, 34, 26, .88), rgba(18, 16, 13, .84)) padding-box,
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .18) 0 1px, transparent 1px 4px) border-box,
    linear-gradient(140deg, rgba(240, 214, 143, .85), rgba(122, 98, 52, .42) 50%, rgba(240, 214, 143, .7)) border-box;
  box-shadow: 0 8px 22px rgba(0, 0, 0, .55);
}

/* ------------------ 飘字（受击数字，落在 fx 层） ------------------ */
.fx-layer { position: absolute; inset: 0; overflow: hidden; pointer-events: none; z-index: 40; }

.dmg {
  position: absolute;
  transform: translate(-50%, -50%);
  font-family: var(--font-serif);
  font-weight: 700;
  font-size: clamp(18px, 2.1vw, 27px);
  color: var(--c-gold-bright);
  text-shadow: 0 0 12px rgba(200, 166, 89, .5), 0 2px 0 rgba(0, 0, 0, .85), 0 0 3px rgba(0, 0, 0, .9);
  animation: dmg-float .95s cubic-bezier(.2, .7, .3, 1) forwards;
  will-change: transform, opacity;
  pointer-events: none;
}

.dmg--miss { font-size: clamp(13px, 1.4vw, 17px); color: rgba(233, 224, 204, .68); }

/* =========================================================
   行囊（Tab 背包）
   样式见 ./StoryWorld.bag.css（复刻 111/bag.html）；
   这里只保留 HUD 与行囊共用的锚点与浮签，
   以及键盘游走用的 .inventory-slot 锚点。
   ========================================================= */
/* 行囊整块必须参与命中测试：它会盖住 3D 视口 */
.story-bag { pointer-events: auto; }

.hint-anchor { position: relative; }

.ui-tooltip {
  display: none;
  position: absolute;
  bottom: calc(100% + 9px);
  left: 50%;
  transform: translateX(-50%);
  padding: 7px 10px;
  border: var(--edge) solid rgba(143, 120, 70, .8);
  background: rgba(12, 18, 15, .96);
  color: #efe0bc;
  font-family: var(--font-serif);
  font-size: 12px;
  font-weight: 400;
  white-space: nowrap;
  z-index: 3;
  pointer-events: none;
}

.hint-anchor:hover > .ui-tooltip,
.hint-anchor:focus > .ui-tooltip,
.hint-anchor:focus-within > .ui-tooltip { display: block; }

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

/* =========================================================
   响应式
   ========================================================= */
@media (max-width: 870px) {
  .boss-status { width: min(600px, 88vw); }
  .tip { max-width: min(64vw, 420px); font-size: 11px; }
}

@media (max-width: 900px) {
  .story-world-viewport { --slot: clamp(46px, 7.6vw, 62px); }
  .player-vitals { width: clamp(180px, 36vw, 250px); }
  .boss-name { font-size: 12px; letter-spacing: .12em; }
}

@media (max-width: 660px) {
  .story-world-viewport { --slot: clamp(44px, 12vw, 58px); --gap-edge: 12px; }
  .boss-name { max-width: 46vw; }
  .tip { max-width: 88vw; font-size: 10.5px; line-height: 1.45; }
  .skill__name { display: none; }
  .player-vitals { width: min(58vw, 220px); }
}

@media (max-width: 400px) {
  .story-world-viewport { --slot: 38px; }
  .skills { gap: 6px; }
  .player-vitals { width: min(50vw, 170px); }
  .vitals__seal { display: none; }
  .stat__name { letter-spacing: .04em; min-width: 2em; }
  .skill__cost { display: none; }
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
.subworld-notice {
  position: absolute; bottom: 22px; left: 50%; transform: translateX(-50%);
  display: grid; gap: 5px; justify-items: center; padding: 14px 22px; text-align: center;
  background: rgba(16, 14, 12, .88); border: 1px solid var(--gold-line);
  clip-path: var(--shape); box-shadow: 0 8px 22px rgba(0, 0, 0, .55);
}
.subworld-notice strong { font-size: 14px; font-weight: 500; letter-spacing: 3px; color: var(--c-gold-bright); }
.subworld-notice span { font-size: 11px; color: var(--c-paper-dim); }

.dialogue-overlay {
  pointer-events: auto; position: absolute; inset: 0; z-index: 6;
  display: grid; align-items: end; justify-items: center;
  padding: 0 20px 92px; background: rgba(5, 8, 7, .42);
}
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

.exhibit { margin-top: 14px; padding-top: 12px; border-top: 1px solid var(--c-stone); }
.exhibit__head { display: flex; align-items: center; gap: 8px; }
.exhibit__total { margin-left: auto; font-size: 11px; color: var(--c-gold-dim); font-variant-numeric: tabular-nums; }
.exhibit__empty, .exhibit__hint { margin: 10px 0 0; font-size: 11px; line-height: 1.6; color: var(--c-paper-dim); }
.exhibit__list { list-style: none; margin: 10px 0 0; padding: 0; display: grid; gap: 8px; }
.exhibit__list li {
  display: flex; align-items: center; gap: 12px; padding: 9px 11px;
  background: rgba(11, 17, 15, .6); border: 1px solid var(--c-stone);
}
.exhibit__list li.is-empty { opacity: .5; }
.exhibit__info { flex: 1; min-width: 0; display: grid; gap: 3px; }
.exhibit__info strong { font-size: 13px; font-weight: 500; color: var(--c-paper); }
.exhibit__count { font-size: 11px; color: var(--c-gold); font-variant-numeric: tabular-nums; }
.exhibit__desc { font-size: 11px; line-height: 1.5; color: #8a8172; }
.exhibit__act, .exhibit__back {
  flex-shrink: 0; padding: 6px 12px; font-size: 12px; cursor: pointer;
  color: var(--c-gold-bright); background: #343820; border: 1px solid var(--c-gold);
}
.exhibit__back { margin-top: 10px; color: var(--c-paper-dim); background: transparent; border-color: var(--c-gold-dim); }

@media (max-width: 800px) {
  .world-indicator { top: 18px; right: 12px; font-size: 11px; letter-spacing: 1px; }
}
</style>
