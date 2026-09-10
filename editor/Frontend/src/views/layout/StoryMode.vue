<!-- 剧情模式根组件：只负责 Three.js 生命周期、HUD 数据绑定和主世界/小世界切换。 -->
<template>
  <main
    ref="root"
    class="story-mode"
    :class="{
      looking: store.pointerLocked || store.mouseActive,
      'overlay-open': store.inventoryOpen || store.mapOpen,
    }"
    tabindex="0"
    @pointerdown="lockPointer"
  >
    <canvas ref="canvas" class="viewport"></canvas>
    <div class="screen-vignette" aria-hidden="true"></div>

    <StoryHud
      :health="store.vitals.health"
      :max-health="store.vitals.maxHealth"
      :armor="armor"
      :hint="interactionHint"
      :debug="debugState"
      :debug-visible="store.debugVisible"
    />

    <!-- 死亡界面采用模态显示，玩家主动复活之前暂停战斗。 -->
    <section v-if="store.vitals.dead" class="death-overlay" @pointerdown.stop>
      <div role="dialog" aria-modal="true" aria-label="玩家死亡">
        <h2>你倒下了</h2>
        <p>背包和装备已保留。</p>
        <button @click="respawnPlayer">返回出生点</button>
      </div>
    </section>
    <div v-if="store.worldType === 'ugc' && ugcViewState.mode === 'build'" class="build-controls">
      右键看向 · WASD 移动 · Q 上升 / E 下降 · 方向键旋转 · 滚轮前后移动 · Shift＋滚轮调速
    </div>
    <StoryHotbar
      :slots="hotbarSlots"
      :selected-index="selectedHotbarIndex"
      @select="selectHotbarSlot"
    />

    <button
      v-if="store.worldType === 'main'"
      class="menu-button"
      type="button"
      aria-label="退出剧情模式"
      @pointerdown.stop
      @click.stop="exitStoryMode"
    >
      <span class="menu-icon">↩</span>
      <span>退出剧情模式</span>
    </button>

    <InventoryPanel
      v-if="store.inventoryOpen"
      :items="store.items"
      :equipment="store.equipment"
      :transfer-error="equipmentError"
      :hotbar-slots="hotbarSlots"
      :selected-hotbar-index="selectedHotbarIndex"
      @equip="equipItem"
      @unequip="unequipItem"
      @close="closeOverlay"
      @select-hotbar="selectHotbarSlot"
      @use-orb="useWorldOrb"
    />

    <MapPanel v-if="store.mapOpen" :player="mapPlayer" @close="closeOverlay" />

    <UgcWorldPicker
      v-if="ugcPickerOpen"
      class="ugc-world-picker"
      :worlds="ugcWorlds"
      :loading="ugcPickerLoading"
      :error="ugcPickerError"
      @cancel="cancelUgcPicker"
      @create="createNewUgcWorld"
      @open="openExistingUgcWorld"
    />

    <UgcWorldOverlay
      v-if="store.worldType === 'ugc'"
      :view-state="ugcViewState"
      :exit-confirm="ugcExitConfirm"
      :error="ugcError"
      @set-mode="setUgcMode"
      @save="saveUgcWorld"
      @exit="requestExitUgcWorld"
      @save-exit="saveAndExitUgcWorld"
      @discard-exit="discardAndExitUgcWorld"
      @cancel-exit="cancelExitUgc"
      @place="placeUgcObject"
      @delete="deleteUgcObject"
      @bind="bindUgcFragment"
    />
  </main>
</template>

<script setup>
import { computed, onMounted, onUnmounted, reactive, ref, watch } from 'vue';
import { useRouter } from 'vue-router';
import * as THREE from 'three';
import StoryHud from '@/story/components/StoryHud.vue';
import StoryHotbar from '@/story/components/StoryHotbar.vue';
import InventoryPanel from '@/story/components/InventoryPanel.vue';
import MapPanel from '@/story/components/MapPanel.vue';
import UgcWorldOverlay from '@/story/components/UgcWorldOverlay.vue';
import UgcWorldPicker from '@/story/components/UgcWorldPicker.vue';
import { createFallbackScene } from '@/story/adapters/fallbackSceneAdapter.js';
import { createStoryCameraController } from '@/story/storyCameraController.js';
import { createStoryCombatSystem } from '@/story/storyCombatSystem.js';
import { createStoryInputManager } from '@/story/storyInputManager.js';
import { createStoryInteractionSystem } from '@/story/storyInteractionSystem.js';
import { createStoryPhysicsSystem } from '@/story/storyPhysicsSystem.js';
import { createStoryPlayer } from '@/story/storyPlayer.js';
import { createStoryRuntime } from '@/story/storyRuntime.js';
import { hotbarSystem } from '@/story/hotbarSystem.js';
import { inventorySystem } from '@/story/inventorySystem.js';
import { createUgcWorldController } from '@/story/ugc/ugcWorldController.js';
import { createUgcId } from '@/story/ugc/ugcWorldState.js';
import { createDemoWorldFragment } from '@/story/ugc/worldFragment.js';
import { storyModeStore as store, toggleInventory, toggleMap } from '@/story/storyModeStore.js';

import { createEquipmentSystem, armorValue, attackProfile } from '@/story/equipmentSystem.js';
import { createPlayerVitals } from '@/story/playerVitals.js';
import { createMonsterSystem } from '@/story/monsterSystem.js';
import { createMonsterSceneAdapter } from '@/story/adapters/monsterSceneAdapter.js';
import { createBuildCameraController } from '@/story/buildCameraController.js';
const equipmentSystem = createEquipmentSystem(store);
equipmentSystem.seed();
const armor = computed(() => armorValue(store.equipment));
const vitals = createPlayerVitals(store.vitals, () => armor.value);
const windowFocused = ref(true);
const equipmentError = ref('');
let monsters = null,
  monsterScene = null,
  buildCamera = null;
function equipItem({ id, slot }) {
  const result = equipmentSystem.equip(id, slot);
  equipmentError.value = result.ok ? '' : result.error;
  showHint(result.ok ? '装备已穿戴' : result.error);
}
function unequipItem(slot) {
  const result = equipmentSystem.unequip(slot);
  equipmentError.value = result.ok ? '' : result.error;
  showHint(result.ok ? '装备已放回背包' : result.error);
}
function respawnPlayer() {
  vitals.respawn();
  resetToSpawn();
  monsters?.resetAggro();
  pauseGameInput();
}
function onFocus() {
  windowFocused.value = true;
}
function onBlur() {
  windowFocused.value = false;
  pauseGameInput();
  buildCamera?.clear();
}
function onVisibility() {
  if (document.hidden) onBlur();
  else onFocus();
}
const root = ref(null);
const canvas = ref(null);
const router = useRouter();
const interactionHint = ref('');
const ugcViewState = ref({
  world: null,
  resources: { materials: [], fragments: [] },
  mode: 'build',
  dirty: false,
  saving: false,
  active: false,
  error: '',
});
const ugcExitConfirm = ref(false);
const ugcError = ref('');
const ugcPickerOpen = ref(false);
const ugcPickerLoading = ref(false);
const ugcPickerError = ref('');
const ugcWorlds = ref([]);
const ugcEntering = ref(false);
let ugcPickerRequestId = 0;
const debugState = reactive({
  x: 0,
  y: 1.7,
  z: 4,
  yaw: 0,
  pitch: 0,
  grounded: true,
  pointerLocked: false,
  mouseActive: false,
  move: false,
  worldType: 'main',
  bossHealth: 100,
  target: '',
});
const mapPlayer = computed(() => ({
  x: Math.max(5, Math.min(95, 50 + store.player.x * 2)),
  z: Math.max(5, Math.min(95, 50 + store.player.z * 2)),
}));
const hotbarSlots = computed(() => hotbarSystem.getSlots());
const selectedHotbarIndex = computed(() => hotbarSystem.getSelectedIndex());

let renderer = null;
let mainSceneBundle = null;
let activeSceneBundle = null;
let camera = null;
let input = null;
let runtime = null;
let interactionSystem = null;
let combatSystem = null;
let animationFrame = 0;
let resize = null;
let ugcController = null;
let ugcPickerController = null;
let disposed = false;
let hintTimeout = 0;

const { player, resetToSpawn } = createStoryPlayer();
const cameraController = createStoryCameraController();

watch(
  () => store.items,
  (items) => hotbarSystem.syncFromInventory(items),
  { immediate: true, deep: true }
);

function showHint(message, timeout = 2500) {
  if (disposed) return;
  window.clearTimeout(hintTimeout);
  interactionHint.value = message;
  if (timeout > 0) {
    hintTimeout = window.setTimeout(() => {
      if (interactionHint.value === message) interactionHint.value = '';
    }, timeout);
  }
}

function updateUgcView(viewState) {
  ugcViewState.value = viewState || ugcViewState.value;
  ugcError.value = viewState?.error || '';
}

function isGameInputBlocked() {
  return (
    store.inventoryOpen ||
    store.mapOpen ||
    ugcPickerOpen.value ||
    ugcPickerLoading.value ||
    ugcEntering.value ||
    ugcExitConfirm.value ||
    ugcViewState.value.saving ||
    store.vitals.dead ||
    !windowFocused.value ||
    document.hidden
  );
}

function lockPointer(event) {
  const target = event?.target;
  if (
    isGameInputBlocked() ||
    (store.worldType === 'ugc' && ugcViewState.value.mode === 'build') ||
    target?.closest?.('.menu-button, .ugc-overlay, .ugc-world-picker, .overlay, .hotbar, button')
  ) {
    return;
  }

  input?.setMouseActive?.(true);
  store.mouseActive = true;
  root.value?.focus?.({ preventScroll: true });
  const request = canvas.value?.requestPointerLock?.();
  if (request?.catch) request.catch(() => input?.setMouseActive?.(true));
}

function pauseGameInput() {
  buildCamera?.clear();
  if (document.pointerLockElement) document.exitPointerLock();
  input?.setMouseActive?.(false);
  input?.clearAll?.();
  store.mouseActive = false;
}

function resumeGameInput() {
  input?.clearAll?.();
  store.mouseActive = false;
  store.pointerLocked = false;
}

function closeOverlay() {
  store.inventoryOpen = false;
  store.mapOpen = false;
  pauseGameInput();
}

function selectHotbarSlot(index) {
  hotbarSystem.select(index);
}

function applyPlayerSpawn(spawn = {}) {
  const position = spawn.position || [0, 1.7, 4];
  player.spawn.set(position[0], position[1], position[2]);
  player.position.copy(player.spawn);
  player.velocityY = 0;
  player.grounded = true;
  player.yaw = spawn.yaw || 0;
  player.pitch = spawn.pitch || 0;
  cameraController.applyToCamera(camera, player);
}

function syncMainStore() {
  if (store.worldType === 'main') {
    store.player.x = player.position.x;
    store.player.y = player.position.y;
    store.player.z = player.position.z;
  }
  store.pointerLocked = input?.isPointerLocked?.() || false;
  store.mouseActive = input?.isMouseActive?.() || false;
  debugState.x = player.position.x;
  debugState.y = player.position.y;
  debugState.z = player.position.z;
  debugState.yaw = player.yaw;
  debugState.pitch = player.pitch;
  debugState.grounded = player.grounded;
  debugState.pointerLocked = store.pointerLocked;
  debugState.mouseActive = store.mouseActive;
  debugState.worldType = store.worldType;
  debugState.bossHealth = store.bossHealth;
  debugState.target = store.interactionTarget;
}

function restoreMainWorld(snapshot) {
  if (!snapshot) return;

  activeSceneBundle = mainSceneBundle;
  interactionSystem.setScene(mainSceneBundle.scene);
  combatSystem.setScene(mainSceneBundle.scene);

  if (snapshot.vitals) Object.assign(store.vitals, snapshot.vitals);
  if (snapshot.equipment) Object.assign(store.equipment, snapshot.equipment);
  if (snapshot.items) store.items.splice(0, store.items.length, ...snapshot.items);
  if (snapshot.bossHealth !== undefined) store.bossHealth = snapshot.bossHealth;
  if (snapshot.storyState?.worldType) store.worldType = snapshot.storyState.worldType;

  const savedBoss = snapshot.boss;
  if (savedBoss && mainSceneBundle.storyObjects.boss) {
    const boss = mainSceneBundle.storyObjects.boss;
    boss.userData.health = savedBoss.health;
    boss.visible = savedBoss.visible;
    boss.userData.disabled = savedBoss.disabled;
  }

  if (snapshot.player) {
    const savedSpawn = snapshot.player.spawn || [0, 1.7, 4];
    player.spawn.fromArray(savedSpawn);
    player.position.set(snapshot.player.x, snapshot.player.y, snapshot.player.z);
    player.velocityY = snapshot.player.velocityY || 0;
    player.grounded = snapshot.player.grounded !== false;
    player.yaw = snapshot.camera?.yaw ?? snapshot.player.yaw ?? 0;
    player.pitch = snapshot.camera?.pitch ?? snapshot.player.pitch ?? 0;
  }

  cameraController.applyToCamera(camera, player);
  store.worldType = 'main';
  ugcViewState.value = { ...ugcViewState.value, active: false };
  resumeGameInput();
  syncMainStore();
}

function handleInteraction(result) {
  if (!result) return;
  if (result.type === 'picked-item') {
    inventorySystem.addItem(result.item);
    result.object.visible = false;
    result.object.userData.disabled = true;
    showHint(`已获得：${result.item.name}`);
    return;
  }
  if (result.type === 'boss-status') {
    showHint(`灰盒 Boss：${Math.max(0, store.bossHealth)} / 100`);
    return;
  }
  if (result.errors?.length) showHint(result.errors[0], 2500);
}

function handleAttack(result) {
  const target = result?.target;
  if (!target) {
    if (result?.accepted) showHint('攻击未命中目标', 900);
    return;
  }

  if (store.worldType === 'ugc') {
    const objectId = target.userData.objectId;
    if (objectId) {
      ugcController?.recordAttack(objectId, target.userData.health);
      if (target.userData.health <= 0) {
        target.visible = false;
        target.userData.disabled = true;
      }
      showHint(`目标生命：${Math.max(0, target.userData.health)}`, 900);
    }
    return;
  }

  if (target.userData.monsterId) {
    showHint(`测试怪物生命：${target.userData.health}`, 900);
    return;
  }
  if (target !== mainSceneBundle.storyObjects.boss) return;
  store.bossHealth = target.userData.health;
  showHint(`命中灰盒 Boss，剩余生命：${target.userData.health}`, 900);
  if (target.userData.health > 0) return;

  target.visible = false;
  target.userData.disabled = true;
  const fragment = mainSceneBundle.storyObjects.fragment;
  fragment.position.copy(target.position).add(new THREE.Vector3(0, -1, 0));
  fragment.visible = true;
  fragment.userData.disabled = false;
  showHint('Boss 已被击败，世界碎片已掉落！', 3500);
}

function setActiveScene(bundle) {
  activeSceneBundle = bundle;
  interactionSystem.setScene(bundle.scene);
  combatSystem.setScene(bundle.scene);
}

async function openUgcWorldPicker() {
  const requestId = ++ugcPickerRequestId;
  ugcPickerOpen.value = true;
  ugcPickerLoading.value = true;
  ugcPickerError.value = '';
  ugcWorlds.value = [];
  pauseGameInput();

  ugcPickerController?.dispose?.();
  ugcPickerController = createUgcWorldController({ mainItems: store.items });
  try {
    ugcWorlds.value = await ugcPickerController.listWorlds();
  } catch (error) {
    // 浏览器预览没有 C++ bridge 时仍允许创建，但保存会明确提示接口不可用。
    ugcPickerError.value = error.message || '无法读取当前项目的小世界列表。';
  } finally {
    if (requestId === ugcPickerRequestId) {
      ugcPickerLoading.value = false;
      ugcPickerController?.dispose?.();
      ugcPickerController = null;
    }
  }
}

function cancelUgcPicker() {
  ugcPickerRequestId += 1;
  ugcPickerOpen.value = false;
  ugcPickerLoading.value = false;
  ugcPickerError.value = '';
  resumeGameInput();
}

async function enterUgcWorld({ worldId, loadExisting = false } = {}) {
  ugcPickerRequestId += 1;
  ugcPickerOpen.value = false;
  ugcPickerLoading.value = false;
  ugcEntering.value = true;
  pauseGameInput();

  ugcController?.dispose?.();
  // 资源提交使用独立背包副本，临时装备交换不会污染保存结果。
  const committedInventory = JSON.parse(JSON.stringify(store.items));
  ugcController = createUgcWorldController({
    mainItems: committedInventory,
    onChanged: updateUgcView,
    onMessage: ({ type, message }) => {
      if (type === 'message') showHint(message, 2800);
    },
  });

  const snapshot = {
    player: {
      x: player.position.x,
      y: player.position.y,
      z: player.position.z,
      velocityY: player.velocityY,
      grounded: player.grounded,
      yaw: player.yaw,
      pitch: player.pitch,
      spawn: player.spawn.toArray(),
    },
    camera: { yaw: player.yaw, pitch: player.pitch },
    items: store.items,
    vitals: { ...store.vitals },
    equipment: JSON.parse(JSON.stringify(store.equipment)),
    bossHealth: store.bossHealth,
    boss: {
      health: mainSceneBundle.storyObjects.boss.userData.health,
      visible: mainSceneBundle.storyObjects.boss.visible,
      disabled: Boolean(mainSceneBundle.storyObjects.boss.userData.disabled),
    },
    storyState: { worldType: store.worldType },
    activeScene: mainSceneBundle,
  };

  try {
    const viewState = await ugcController.enter({
      worldId: worldId || createUgcId('ugc-world'),
      mainItems: committedInventory,
      mainWorldSnapshot: snapshot,
      loadExisting,
    });
    if (disposed) return;
    updateUgcView(viewState);
    setActiveScene(ugcController.getSceneBundle());
    applyPlayerSpawn(viewState.world.spawn);
    // 装备实例与主世界快照、材料提交背包相互隔离。
    Object.assign(store.equipment, JSON.parse(JSON.stringify(store.equipment)));
    buildCamera?.reset();
    store.worldType = 'ugc';
    closeOverlay();
    showHint('已进入小世界，当前为建造模式。', 3200);
  } catch (error) {
    ugcController?.dispose();
    ugcController = null;
    ugcError.value = error.message || '进入小世界失败。';
    showHint(ugcError.value, 3200);
  } finally {
    ugcEntering.value = false;
  }
}

function createNewUgcWorld() {
  enterUgcWorld({ worldId: createUgcId('ugc-world') });
}

function openExistingUgcWorld(worldId) {
  if (!worldId) return;
  enterUgcWorld({ worldId, loadExisting: true });
}

async function useWorldOrb() {
  if (store.worldType !== 'main') {
    showHint('请先返回主世界。');
    return;
  }
  if (!inventorySystem.hasItem('world-orb-demo')) {
    showHint('你没有世界小球。');
    return;
  }
  await openUgcWorldPicker();
}

function setUgcMode(mode) {
  if (!ugcController?.setMode(mode)) return;
  updateUgcView(ugcController.getViewState());
  pauseGameInput();
  if (mode === 'play') {
    const spawn = ugcController.getWorld()?.spawn;
    applyPlayerSpawn(spawn);
    showHint('试玩模式已开启。', 1800);
  } else {
    showHint('建造模式已开启，已恢复自由视角。', 1800);
  }
}

function placeUgcObject(options) {
  const result = ugcController?.placeObject(options);
  if (!result?.ok) showHint(result?.error || '放置失败。', 2200);
  else showHint('对象已放置。', 1200);
}

function deleteUgcObject(id) {
  const result = ugcController?.removeObject(id);
  if (!result?.ok) showHint(result?.error || '删除失败。', 2200);
}

function bindUgcFragment({ objectId, fragmentId }) {
  const result = ugcController?.bindFragment(objectId, fragmentId);
  if (!result?.ok) showHint(result?.error || '绑定失败。', 2200);
}

async function saveUgcWorld() {
  const result = await ugcController?.save();
  if (!result?.ok) showHint(result?.error || '保存失败。', 3200);
  else showHint('小世界已保存。', 1800);
}

function requestExitUgcWorld() {
  if (!ugcController) return;
  if (ugcController.isDirty()) ugcExitConfirm.value = true;
  else discardAndExitUgcWorld();
}

async function saveAndExitUgcWorld() {
  ugcExitConfirm.value = false;
  const result = await ugcController?.exit({ save: true });
  if (!result?.ok) {
    showHint(result?.error || '保存并退出失败。', 3200);
    return;
  }
  finishUgcExit(result.snapshot);
}

function discardAndExitUgcWorld() {
  ugcExitConfirm.value = false;
  ugcController?.exit({ discard: true }).then((result) => {
    if (result?.ok) finishUgcExit(result.snapshot);
    else showHint(result?.error || '退出小世界失败。', 2600);
  });
}

function cancelExitUgc() {
  ugcExitConfirm.value = false;
}

async function finishUgcExit(snapshot) {
  ugcController?.dispose();
  ugcController = null;
  restoreMainWorld(snapshot);
  showHint('已返回主世界。', 1800);
}

async function exitStoryMode() {
  if (store.worldType === 'ugc') {
    requestExitUgcWorld();
    return;
  }
  document.exitPointerLock?.();
  router.push('/StartScreen');
}

onMounted(async () => {
  try {
    mainSceneBundle = await createFallbackScene();
    if (disposed) {
      mainSceneBundle.dispose();
      return;
    }
    window.addEventListener('focus', onFocus);
    window.addEventListener('blur', onBlur);
    document.addEventListener('visibilitychange', onVisibility);
    activeSceneBundle = mainSceneBundle;
    camera = new THREE.PerspectiveCamera(70, 1, 0.1, 1000);
    input = createStoryInputManager(document, {
      gameElement: canvas.value,
    });

    buildCamera = createBuildCameraController(camera, canvas.value);
    monsters = createMonsterSystem({
      visible: (a, b) => monsterScene.visible(a, b),
      canMove: (a, b) => monsterScene.canMove(a, b),
      onDamage: (damage) => {
        vitals.damage(damage);
        if (store.vitals.dead) pauseGameInput();
      },
    });
    monsterScene = createMonsterSceneAdapter(mainSceneBundle.scene, monsters.monsters);
    monsterScene.sync();
    const boss = mainSceneBundle.storyObjects.boss;
    const fragment = mainSceneBundle.storyObjects.fragment;
    fragment.visible = false;
    boss.userData.interact = () => ({ type: 'boss-status' });
    mainSceneBundle.storyObjects.orb.userData.interact = () => ({
      type: 'picked-item',
      object: mainSceneBundle.storyObjects.orb,
      item: {
        id: 'world-orb-demo',
        name: '世界小球',
        category: 'ugc',
        quantity: 1,
        description: '进入空白 UGC 世界的入口。',
      },
    });
    fragment.userData.interact = () => ({
      type: 'picked-item',
      object: fragment,
      item: createDemoWorldFragment(),
    });

    const physics = createStoryPhysicsSystem({
      player,
      onRespawn: () => showHint('你离开了场景区域，已回到出生点。', 1800),
    });
    combatSystem = createStoryCombatSystem({
      camera,
      scene: mainSceneBundle.scene,
      getAttackProfile: () => attackProfile(store.equipment),
      onHit: (target, damage) => {
        if (target.userData.monsterId) {
          monsters.hit(target.userData.monsterId, damage);
          monsterScene.sync();
        } else target.userData.health = Math.max(0, target.userData.health - damage);
      },
    });
    interactionSystem = createStoryInteractionSystem({
      camera,
      scene: mainSceneBundle.scene,
      onTargetChanged: (target) => {
        store.interactionTarget = target?.userData?.name || '';
      },
    });
    runtime = createStoryRuntime({
      input,
      camera,
      player,
      physics,
      cameraController,
      interactionSystem,
      combatSystem,
      onInteraction: handleInteraction,
      onAttack: handleAttack,
    });

    cameraController.applyToCamera(camera, player);
    store.running = true;
    renderer = new THREE.WebGLRenderer({ canvas: canvas.value, antialias: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));

    resize = () => {
      const width = root.value?.clientWidth || 1;
      const height = root.value?.clientHeight || 1;
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
      renderer.setSize(width, height, false);
    };
    resize();
    window.addEventListener('resize', resize);
    runtime.start();

    const tick = (time) => {
      if (disposed) return;
      const delta = Math.min(0.05, (time - (tick.last || time)) / 1000);
      tick.last = time;

      if (input.consumePressed('inventory')) {
        toggleInventory();
        if (store.inventoryOpen) pauseGameInput();
      }
      if (input.consumePressed('map')) {
        toggleMap();
        if (store.mapOpen) pauseGameInput();
      }
      for (let index = 0; index < 7; index += 1) {
        if (input.consumePressed(`hotbar${index + 1}`)) {
          hotbarSystem.select(index);
          break;
        }
      }

      const building = store.worldType === 'ugc' && ugcViewState.value.mode === 'build';
      const blocked = isGameInputBlocked();
      buildCamera.setActive(building && !blocked);
      if (building && !blocked) {
        runtime.pause();
        input.clearAll();
        buildCamera.update(delta);
      } else if (blocked) {
        runtime.pause();
        pauseGameInput();
      } else {
        runtime.resume();
        runtime.update(delta);
        if (store.worldType === 'ugc' && ugcViewState.value.mode === 'play') {
          ugcController?.updatePlayerPosition(player.position);
        }
      }

      if (store.worldType === 'main') {
        monsters.update(
          delta,
          {
            position: player.position,
            height: player.height,
            get dead() {
              return store.vitals.dead;
            },
          },
          blocked
        );
        monsterScene.sync();
      }
      const target = interactionSystem.getFocusedTarget();
      const prompt = interactionSystem.getPrompt();
      if (!isGameInputBlocked() && prompt) interactionHint.value = prompt;
      if (!target && interactionHint.value?.startsWith('按 F')) interactionHint.value = '';
      syncMainStore();
      renderer.render(activeSceneBundle.scene, camera);
      animationFrame = requestAnimationFrame(tick);
    };
    animationFrame = requestAnimationFrame(tick);
  } catch (error) {
    showHint(error.message || '剧情模式初始化失败。', 0);
  }
});

onUnmounted(() => {
  disposed = true;
  cancelAnimationFrame(animationFrame);
  window.clearTimeout(hintTimeout);
  input?.dispose();
  buildCamera?.dispose();
  monsterScene?.dispose();
  window.removeEventListener('focus', onFocus);
  window.removeEventListener('blur', onBlur);
  document.removeEventListener('visibilitychange', onVisibility);
  renderer?.dispose();
  mainSceneBundle?.dispose?.();
  ugcController?.dispose?.();
  ugcPickerController?.dispose?.();
  if (resize) window.removeEventListener('resize', resize);
  document.exitPointerLock?.();
  store.running = false;
  store.pointerLocked = false;
  store.mouseActive = false;
  store.inventoryOpen = false;
  store.mapOpen = false;
  store.worldType = 'main';
  resetToSpawn();
});
</script>

<style scoped>
.story-mode {
  --game-bg: var(--ce-black-0);
  --game-panel: var(--ce-black-1);
  --game-panel-deep: var(--ce-black-0);
  --game-border: var(--ce-gold-border);
  --game-border-strong: var(--ce-gold-muted);
  --game-text: var(--ce-text-primary);
  --game-muted: var(--ce-text-secondary);
  --game-cyan: var(--ce-gold-bright);
  --game-gold: var(--ce-gold-primary);
  --game-font: 'Segoe UI', 'Microsoft YaHei', sans-serif;
  position: fixed;
  inset: 0;
  overflow: hidden;
  background: var(--game-bg);
  color: var(--game-text);
  cursor: default;
  outline: none;
}

.story-mode.looking {
  cursor: none;
}

.story-mode.overlay-open .viewport {
  filter: brightness(0.68) saturate(0.82);
}

.viewport {
  position: relative;
  z-index: 0;
  display: block;
  width: 100%;
  height: 100%;
  transition: filter 180ms ease;
}

.screen-vignette {
  position: absolute;
  z-index: 1;
  inset: 0;
  pointer-events: none;
  background: rgb(4 12 21 / 14%);
}

.menu-button {
  position: absolute;
  z-index: 4;
  top: 24px;
  right: 28px;
  display: inline-flex;
  min-height: 34px;
  align-items: center;
  gap: 8px;
  padding: 0 12px;
  border: 1px solid var(--game-border-strong);
  border-radius: 7px;
  background: var(--game-panel);
  color: var(--game-muted);
  cursor: pointer;
  font: 11px var(--game-font);
  letter-spacing: 0.04em;
  box-shadow: 0 8px 20px rgb(0 0 0 / 24%);
}

.menu-button:hover,
.menu-button:focus-visible {
  border-color: var(--game-cyan);
  background: var(--ce-black-3);
  color: var(--game-text);
  outline: none;
}

.menu-icon {
  color: var(--game-gold);
  font-size: 16px;
}

@media (max-width: 620px) {
  .menu-button {
    top: 14px;
    right: 14px;
    padding: 0 9px;
  }

  .menu-button span:last-child {
    display: none;
  }
}
/* 建造操作提示和死亡弹窗共用创造模式的主题变量。 */
.build-controls {
  position: absolute;
  bottom: 105px;
  left: 50%;
  transform: translateX(-50%);
  max-width: 70%;
  padding: 8px 12px;
  color: var(--ce-text-secondary);
  background: var(--ce-black-1);
  border: 1px solid var(--ce-gold-border);
  font-size: 11px;
  pointer-events: none;
}
.death-overlay {
  position: absolute;
  inset: 0;
  z-index: 30;
  display: grid;
  place-items: center;
  background: #080806bb;
}
.death-overlay > div {
  padding: 32px 48px;
  text-align: center;
  border: 1px solid var(--ce-gold-primary);
  background: var(--ce-black-1);
  color: var(--ce-text-primary);
}
.death-overlay button {
  margin-top: 18px;
  padding: 10px 20px;
  background: var(--ce-black-3);
  border: 1px solid var(--ce-gold-primary);
  cursor: pointer;
}
</style>
