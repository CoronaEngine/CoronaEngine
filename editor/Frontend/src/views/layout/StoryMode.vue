<!-- 剧情模式根组件：只负责游戏视口生命周期、HUD 绑定和模式退出。 -->
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

    <StoryHud :hint="interactionHint" :debug="debugState" :debug-visible="store.debugVisible" />

    <button
      class="menu-button"
      type="button"
      aria-label="退出剧情模式"
      @pointerdown.stop
      @click.stop="exit"
    >
      <span class="menu-icon">↩</span>
      <span>退出剧情模式</span>
    </button>

    <InventoryPanel
      v-if="store.inventoryOpen"
      :items="store.items"
      @close="closeOverlay"
      @use-orb="useWorldOrb"
    />

    <MapPanel v-if="store.mapOpen" :player="mapPlayer" @close="closeOverlay" />
  </main>
</template>

<script setup>
import { computed, onMounted, onUnmounted, reactive, ref } from 'vue';
import { useRouter } from 'vue-router';
import * as THREE from 'three';
import StoryHud from '@/story/components/StoryHud.vue';
import InventoryPanel from '@/story/components/InventoryPanel.vue';
import MapPanel from '@/story/components/MapPanel.vue';
import { createFallbackScene } from '@/story/adapters/fallbackSceneAdapter.js';
import { createStoryCameraController } from '@/story/storyCameraController.js';
import { createStoryCombatSystem } from '@/story/storyCombatSystem.js';
import { createStoryInputManager } from '@/story/storyInputManager.js';
import { createStoryInteractionSystem } from '@/story/storyInteractionSystem.js';
import { createStoryPhysicsSystem } from '@/story/storyPhysicsSystem.js';
import { createStoryPlayer } from '@/story/storyPlayer.js';
import { createStoryRuntime } from '@/story/storyRuntime.js';
import { inventorySystem } from '@/story/inventorySystem.js';
import { createUgcWorldSession } from '@/story/ugc/ugcWorldSession.js';
import { storyModeStore as store, toggleInventory, toggleMap } from '@/story/storyModeStore.js';

const root = ref(null);
const canvas = ref(null);
const router = useRouter();
const interactionHint = ref('');
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

let renderer;
let sceneBundle;
let camera;
let input;
let runtime;
let interactionSystem;
let frame;
let resize;
let ugcSession;

const { player, resetToSpawn } = createStoryPlayer();
const cameraController = createStoryCameraController();

function showHint(message, timeout = 2500) {
  interactionHint.value = message;
  if (timeout > 0) {
    window.setTimeout(() => {
      if (interactionHint.value === message) interactionHint.value = '';
    }, timeout);
  }
}

function lockPointer(event) {
  const target = event?.target;
  if (store.inventoryOpen || store.mapOpen || target?.closest?.('.menu-button, .overlay, button'))
    return;

  input?.setMouseActive?.(true);
  store.mouseActive = true;
  root.value?.focus?.({ preventScroll: true });
  canvas.value?.requestPointerLock?.();
}

function closeOverlay() {
  store.inventoryOpen = false;
  store.mapOpen = false;
  input?.clearTransient?.();
  input?.setMouseActive?.(false);
  store.mouseActive = false;
}

function exit() {
  document.exitPointerLock?.();
  router.push('/StartScreen');
}

function useWorldOrb() {
  if (!inventorySystem.hasItem('world-orb-demo')) {
    showHint('你没有世界小球。');
    return;
  }

  const fragments = store.items.filter((item) => item.category === 'ugc');
  ugcSession?.loadResources(
    store.items.filter((item) => item.category === 'material'),
    fragments
  );
  ugcSession?.enter({
    player: store.player,
    items: store.items,
    worldType: store.worldType,
  });
  store.worldType = 'ugc';
  showHint('已进入 UGC 空白世界会话，资源注入接口已完成。', 3500);
  closeOverlay();
}

function handleInteraction(result) {
  if (!result) return;
  if (result.type === 'picked-item') {
    inventorySystem.addItem(result.item);
    result.object.visible = false;
    result.object.userData.disabled = true;
    showHint(`已获得：${result.item.name}`);
  } else if (result.type === 'boss-status') {
    showHint(`灰盒 Boss：${Math.max(0, store.bossHealth)} / 100`);
  }
}

function handleAttack(result) {
  const target = result?.target;
  if (!target) {
    showHint('攻击未命中目标', 900);
    return;
  }

  store.bossHealth = target.userData.health;
  showHint(`命中灰盒 Boss，剩余生命：${target.userData.health}`, 900);
  if (target.userData.health > 0) return;

  target.visible = false;
  target.userData.disabled = true;
  const fragment = sceneBundle.storyObjects.fragment;
  fragment.position.copy(target.position).add(new THREE.Vector3(0, -1, 0));
  fragment.visible = true;
  fragment.userData.disabled = false;
  showHint('Boss 已被击败，世界碎片已掉落！', 3500);
}

onMounted(async () => {
  sceneBundle = await createFallbackScene();
  camera = new THREE.PerspectiveCamera(70, 1, 0.1, 1000);
  input = createStoryInputManager(document);
  ugcSession = createUgcWorldSession();

  const boss = sceneBundle.storyObjects.boss;
  const fragment = sceneBundle.storyObjects.fragment;
  fragment.visible = false;

  boss.userData.interact = () => ({ type: 'boss-status' });
  sceneBundle.storyObjects.orb.userData.interact = () => ({
    type: 'picked-item',
    object: sceneBundle.storyObjects.orb,
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
    item: {
      id: 'world-fragment-demo',
      name: '世界碎片',
      category: 'ugc',
      quantity: 1,
      description: '承载受控游戏逻辑，可用于制作 Demo。',
    },
  });

  const physics = createStoryPhysicsSystem({
    player,
    onRespawn: () => showHint('你离开了灰盒区域，已回到出生点。', 1800),
  });
  const combatSystem = createStoryCombatSystem({
    camera,
    scene: sceneBundle.scene,
    onHit: (target, damage) => {
      target.userData.health = Math.max(0, target.userData.health - damage);
    },
  });
  interactionSystem = createStoryInteractionSystem({
    camera,
    scene: sceneBundle.scene,
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
    const delta = Math.min(0.05, (time - (tick.last || time)) / 1000);
    tick.last = time;

    if (input.consumePressed('inventory')) toggleInventory();
    if (input.consumePressed('map')) toggleMap();

    const blocked = store.inventoryOpen || store.mapOpen;
    if (blocked) {
      if (document.pointerLockElement) document.exitPointerLock();
      input.setMouseActive(false);
      input.clearTransient();
      store.mouseActive = false;
    } else {
      runtime.update(delta);
    }

    const target = interactionSystem.getFocusedTarget();
    const prompt = interactionSystem.getPrompt();
    if (!blocked && prompt) showHint(prompt, 0);
    if (!target && interactionHint.value?.startsWith('按 F')) interactionHint.value = '';

    store.pointerLocked = input.isPointerLocked();
    store.mouseActive = input.isMouseActive();
    store.player.x = player.position.x;
    store.player.y = player.position.y;
    store.player.z = player.position.z;

    debugState.x = player.position.x;
    debugState.y = player.position.y;
    debugState.z = player.position.z;
    debugState.yaw = player.yaw;
    debugState.pitch = player.pitch;
    debugState.grounded = player.grounded;
    debugState.pointerLocked = input.isPointerLocked();
    debugState.mouseActive = input.isMouseActive();
    debugState.move = !blocked && Boolean(input.getMoveAxis().x || input.getMoveAxis().z);
    debugState.worldType = store.worldType;
    debugState.bossHealth = store.bossHealth;
    debugState.target = store.interactionTarget;

    renderer.render(sceneBundle.scene, camera);
    frame = requestAnimationFrame(tick);
  };

  frame = requestAnimationFrame(tick);
});

onUnmounted(() => {
  cancelAnimationFrame(frame);
  input?.dispose();
  renderer?.dispose();
  sceneBundle?.dispose?.();
  ugcSession?.dispose?.();
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
  --game-bg: #07131f;
  --game-panel: #101d2a;
  --game-panel-deep: #0b1723;
  --game-border: #304656;
  --game-border-strong: #456173;
  --game-text: #e5ebee;
  --game-muted: #8f9da6;
  --game-cyan: #75cdbd;
  --game-gold: #c6a15b;
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
  transition: 160ms ease;
}

.menu-button:hover,
.menu-button:focus-visible {
  border-color: var(--game-cyan);
  background: #18323c;
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
</style>
