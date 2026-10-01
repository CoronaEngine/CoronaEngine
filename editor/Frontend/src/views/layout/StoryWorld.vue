<script setup>
import { onMounted, onUnmounted, ref, nextTick } from 'vue';
import { useRouter, onBeforeRouteLeave } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState, normalizeProjectPath } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';
import { projectLauncherService, cancelPendingProjectOpen, getProjectSelectionVersion } from '@/services/projectLauncherService.js';
import { trackWorldSessionWork, notifyWorldError, registerWorldSessionSave } from '@/services/worldSessionLifecycle.js';
import { createStoryNavigationController, NAVIGATION_KEY } from '../../../../../game/frontend/storyNavigation.mjs';

import { ensureStoryCharacters } from '../../../../../game/frontend/storyActors.mjs';
import { createPlayerController } from '../../../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../../../game/frontend/playerSave.mjs';
import { createStoryGameplay, worldBounds, distanceToBounds, pickupDistance } from '../../../../../game/frontend/storyGameplay.mjs';
import { beginWorldLoad, worldLoadingState } from '@/services/worldLoadingService.js';
import { waitForProjectResources, hasPendingLoadTimeout, withLoadTimeout } from '../../../../../game/frontend/worldLoading.mjs';
import { STORY_CHARACTERS } from '../../../../../game/frontend/storyCharacters.mjs';

const router = useRouter();
const surface = ref(null);
const inventoryOpen = ref(false);
const inventoryPanel = ref(null);
const selectedSlot = ref(0);
const navigationPending = ref(false);
const cooldownTick = ref(0);
let cooldownTimer = null;
const gameplayState = ref(null);
const initializing = ref(true);
const bossNearby = ref(false);
const canPickup = ref(false);
const feedback = ref('');
const gameplayError = ref('');
const actionBusy = ref(false);
let feedbackTimer = null;
let bossActor = null;
let bossBounds = null;
let sceneId = null;
let visualSignature = null;
const revision = worldModeState.revision;
const projectPath = worldModeState.projectPath;
let disposed = false;
let cameraReady = false;
let playerSave = null;
let savingPlayer = false;
let initialization = Promise.resolve();
let navigationLoad = null;
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
  isInputLocked: () => initializing.value || worldLoadingState.busy || worldLoadingState.error || hasPendingLoadTimeout() || !focused || document.hidden || navigation.busy || savingPlayer || inventoryOpen.value,
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
}
function showFeedback(message) {
  if (!current()) return;
  feedback.value = message;
  clearTimeout(feedbackTimer);
  feedbackTimer = setTimeout(() => { feedback.value = ''; }, 2200);
}
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
  onState: data => { if (current()) { gameplayState.value = data; updateProximity(); } },
  onFeedback: showFeedback,
  reconcile: async data => {
    const next = signature(data);
    if (next === visualSignature) return;
    await ensureStoryCharacters({ api: editorApi, sceneId, frontendUrl: window.location.href,
      gameplay: data, combatOnly: true, assertSource, trackWork: trackWorldSessionWork });
    visualSignature = next;
    if (current()) updateProximity();
  },
});
async function runAction(action) {
  if (!current() || initializing.value || worldLoadingState.busy || worldLoadingState.error || hasPendingLoadTimeout() || !focused || document.hidden || inventoryOpen.value
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
  if (initializing.value || worldLoadingState.busy || worldLoadingState.error || hasPendingLoadTimeout() || navigation.busy || navigationPending.value || savingPlayer) return;
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
async function navigateWorld() {
  const data = gameplayState.value;
  if (!current() || !inventoryOpen.value || !data || navigationPending.value || navigation.busy
    || (data.role === 'main' && data.state.boss.hp > 0)) return;
  navigationPending.value = true;
  gameplayError.value = '';
  navigationLoad = beginWorldLoad({ source: projectPath, target: data.role === 'main' ? `${projectPath}/.game/subworld` : '主世界', isCurrent: current });
  try { await navigation.navigate(data.role === 'main' ? 'enter' : 'exit'); }
  finally {
    navigationPending.value = false;
    if (!worldLoadingState.error) navigationLoad?.finish();
  }
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
  runPhase: (label, task) => navigationLoad ? navigationLoad.phase(label, task) : task(),
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
  prepare: direction => withLoadTimeout(trackWorldSessionWork(editorApi.scratch.sendKeyEvent(
    NAVIGATION_KEY, '', JSON.stringify({ projectPath, direction }))), '准备世界切换'),
  trackPreparation: trackWorldSessionWork,
  openProject: path => projectLauncherService.openProject(path),
  cancelProjectOpen: cancelPendingProjectOpen,
  leave: () => router.replace('/StartScreen'),
  notify: error => { navigationLoad?.fail(error); if (current()) gameplayError.value = error.message; notifyWorldError(error); },
});
function onKeyDown(event) {
  if (!current()) return;
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
  if (event.code === 'KeyF' || event.key?.toLowerCase() === 'f') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused) void runAction(gameplay.pickup);
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
  if (inventoryOpen.value || initializing.value || worldLoadingState.busy || worldLoadingState.error || hasPendingLoadTimeout()) return;
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
  window.addEventListener('blur', onBlur);
  window.addEventListener('focus', onFocus);
  document.addEventListener('visibilitychange', onVisibilityChange);
  const loading = beginWorldLoad({ source: projectPath, target: projectPath,
    operationId: worldLoadingState.operationId || undefined, isCurrent: current });
  initialization = trackWorldSessionWork((async () => {
    await loading.phase('等待场景资源', () => waitForProjectResources({
      getStatus: () => editorApi.project.getProjectLoadStatus(), projectPath,
    }));
    const init = unwrap(await loading.phase('读取场景', () => editorApi.main.onInit()));
    if (!current()) return;
    const scenes = init?.scenes || [];
    const index = Math.min(Math.max(Number(init?.active_index) || 0, 0), Math.max(0, scenes.length - 1));
    sceneId = scenes[index]?.path || init?.path;
    if (!sceneId) throw new Error('当前世界没有可用场景');
    const loadedGameplay = await loading.phase('读取剧情进度', gameplay.load);
    if (!current()) return;
    const { snapshot, player, targetOffset } = await ensureStoryCharacters({
      api: editorApi, sceneId, frontendUrl: window.location.href, gameplay: loadedGameplay, assertSource, operation: loading,
      isCurrent: () => current() && worldModeState.projectPath === projectPath,
    });
    if (!current() || worldModeState.projectPath !== projectPath) return;
    bossActor = snapshot.actors?.find(actor => actor.actor_guid === STORY_CHARACTERS[1].guid) || null;
    bossBounds = worldBounds(bossActor);
    visualSignature = signature(loadedGameplay);
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
    loading.finish();
    if (focused) surface.value?.focus();
  })());
  try { await initialization; } catch (error) {
    loading.fail(error);
    if (!current()) return;
    gameplayError.value = error.message || '剧情世界加载失败';
    // Do not unmount/recover while an uncancelled native request is outstanding.
    if (error.nativePending) return;
    await router.replace('/StartScreen');
    notifyWorldError(error, '剧情世界加载失败');
  }
});
onUnmounted(() => {
  disposed = true;
  clearTimeout(feedbackTimer);
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
        <symbol id="story-shard" viewBox="0 0 48 48"><path d="m24 3 14 18-14 24L10 21Z"/><path d="m24 3-4 18 4 24 5-24ZM10 21h28"/></symbol>
        <symbol id="story-heavy" viewBox="0 0 48 48"><path d="M36 5 17 24l7 7L43 12V5ZM13 22l13 13M19 29 8 40M5 37l6 6"/></symbol>
        <symbol id="story-sweep" viewBox="0 0 48 48"><path d="M7 30C-1 8 28-2 41 15M6 34C17 45 40 38 42 25M35 12l7 4 1-9M12 30l20-17-13 24Z"/></symbol>
      </defs>
    </svg>
    <div v-if="gameplayState && !initializing" class="story-hud" aria-label="战斗界面">
      <section v-if="bossNearby && !inventoryOpen" class="boss-status" aria-label="Boss 血条">
        <div class="boss-name">巨龙</div>
        <div class="boss-bar" role="progressbar" aria-label="Boss 生命" :aria-valuenow="gameplayState.state.boss.hp"
          :aria-valuemax="gameplayState.config.bossHp" aria-valuemin="0">
          <span :style="{ width: `${100 * gameplayState.state.boss.hp / gameplayState.config.bossHp}%` }" />
        </div>
        <div class="boss-number">{{ gameplayState.state.boss.hp }} / {{ gameplayState.config.bossHp }}</div>
      </section>
      <aside v-if="!inventoryOpen" class="story-controls" aria-label="操作提示">
        <span><kbd>WASD</kbd> 移动</span>
        <span><kbd>鼠标</kbd> 转向 · 滚轮缩放</span>
        <span><kbd>Space</kbd> 跳远</span>
        <span><kbd>Shift</kbd> 长按跑步 · 点按闪避</span>
        <span><kbd>左键</kbd> 攻击</span>
        <span><kbd>E / R</kbd> 技能</span>
        <span v-if="canPickup" class="pickup-control" role="status"><kbd>F</kbd> 拾取 世界碎片</span>
        <span><kbd>Tab</kbd> 背包</span>
        <span><kbd>Esc</kbd> 保存退出</span>
      </aside>
      <div v-if="!inventoryOpen" class="story-bottom-stack">
        <div v-if="gameplayError" class="gameplay-error" role="alert" @pointerdown.stop @wheel.stop @pointermove.stop="camera.resetInput">
          <span>{{ gameplayError }}</span><button :disabled="actionBusy" @click.stop="runAction(saveRegistration.flush)">重试保存</button>
        </div>
        <div v-if="feedback" class="story-feedback" role="status" aria-live="polite">{{ feedback }}</div>
        <section class="player-vitals" aria-label="人物生命与怒气">
          <div class="vitals-content">
            <div class="vital-label"><span>生命</span><span>{{ gameplayState.config.playerHp }} <em>/ {{ gameplayState.config.playerHp }}</em></span></div>
            <div class="vital-bar health" role="progressbar" aria-label="生命" :aria-valuenow="gameplayState.config.playerHp" :aria-valuemax="gameplayState.config.playerHp" aria-valuemin="0"><span /></div>
            <div class="vital-label"><span>怒气</span><span>{{ gameplayState.state.rage }} <em>/ {{ gameplayState.config.rageMax }}</em></span></div>
            <div class="vital-bar rage" role="progressbar" aria-label="怒气" :aria-valuenow="gameplayState.state.rage" :aria-valuemax="gameplayState.config.rageMax" aria-valuemin="0">
              <span :style="{ width: `${100 * gameplayState.state.rage / gameplayState.config.rageMax}%` }" />
            </div>
          </div>
          <div class="skill-strip" aria-label="怒气技能" @pointerdown.stop @pointermove.stop="camera.resetInput">
            <div v-for="(skill, id) in gameplayState.config.skills" :key="id" class="skill-tile hint-anchor" tabindex="0"
              :class="{ unavailable: !skillStatus(id).available }" :aria-label="`${skill.key} ${skill.name}，消耗 ${skill.rageCost} 怒气`">
              <svg aria-hidden="true"><use :href="`#story-${id}`" /></svg><kbd>{{ skill.key }}</kbd>
              <span v-if="skillStatus(id).remainingMs > 0" class="skill-cooldown">{{ (skillStatus(id).remainingMs / 1000).toFixed(1) }}</span>
              <span class="ui-tooltip" role="tooltip">{{ skill.name }} · {{ skill.rageCost }} 怒气</span>
            </div>
          </div>
        </section>
      </div>
    </div>
    <div v-if="inventoryOpen && gameplayState" class="inventory-overlay" @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent>
      <section ref="inventoryPanel" class="inventory-panel" role="dialog" aria-modal="true" aria-labelledby="inventory-title" @keydown="onInventoryKey">
        <header class="inventory-header">
          <h1 id="inventory-title">背包</h1>
          <button class="close-inventory" data-inventory-close aria-label="关闭背包" :disabled="navigationPending" @click="toggleInventory">×</button>
        </header>
        <div class="inventory-grid" aria-label="物品格子">
          <button v-for="slot in 20" :key="slot" class="inventory-slot hint-anchor"
            :class="{ occupied: slot === 1 && gameplayState.state.inventory.worldFragment }"
            :tabindex="selectedSlot === slot - 1 ? 0 : -1" :disabled="navigationPending"
            :aria-label="slot === 1 && gameplayState.state.inventory.worldFragment ? `世界碎片，数量 ${gameplayState.state.inventory.worldFragment}` : `空格 ${slot}`"
            @focus="selectedSlot = slot - 1" @click="selectedSlot = slot - 1">
            <template v-if="slot === 1 && gameplayState.state.inventory.worldFragment">
              <svg class="fragment-icon" aria-hidden="true"><use href="#story-shard" /></svg>
              <span class="slot-count">{{ gameplayState.state.inventory.worldFragment }}</span>
              <span class="ui-tooltip" role="tooltip">世界碎片</span>
            </template>
          </button>
        </div>
        <div v-if="gameplayError" class="gameplay-error" role="alert">{{ gameplayError }}</div>
        <footer class="inventory-footer">
          <span data-inventory-travel-focus class="world-button-hint hint-anchor" :tabindex="gameplayState.role === 'main' && gameplayState.state.boss.hp > 0 ? 0 : -1"
            :aria-label="gameplayState.role === 'main' && gameplayState.state.boss.hp > 0 ? '击败 Boss 后开启小世界' : undefined">
            <button data-inventory-travel class="world-travel-button" :disabled="navigationPending || (gameplayState.role === 'main' && gameplayState.state.boss.hp > 0)" @click="navigateWorld">
              {{ gameplayState.role === 'main' ? '小世界' : '返回主世界' }}
            </button>
            <span v-if="gameplayState.role === 'main' && gameplayState.state.boss.hp > 0" class="ui-tooltip" role="tooltip">击败 Boss 后开启</span>
          </span>
        </footer>
      </section>
    </div>
  </div>
</template>

<style scoped>
.story-world-viewport { position: fixed; inset: 0; overflow: hidden; outline: none; background: transparent; color: #eee3ca; font-family: 'Microsoft YaHei', sans-serif; }
.hud-symbols { position: absolute; width: 0; height: 0; overflow: hidden; }
.story-hud { position: absolute; inset: 0; pointer-events: none; }
.boss-status { position: absolute; top: 26px; left: 50%; transform: translateX(-50%); width: min(440px, 45vw); text-align: center; text-shadow: 0 2px 4px #000; }
.boss-name { font-size: 17px; letter-spacing: 5px; margin-bottom: 9px; }
.boss-number { margin-top: 5px; font-size: 12px; font-variant-numeric: tabular-nums; }
.boss-bar, .vital-bar { padding: 2px; border: 1px solid #9b8049; background: #14120fe8; height: 11px; }
.boss-bar span, .vital-bar span { display: block; width: 100%; height: 100%; background: linear-gradient(90deg, #661e26, #be4b45); transition: width 150ms ease-out; }
.story-controls { position: absolute; left: 24px; top: 50%; transform: translateY(-50%); display: grid; gap: 9px; padding: 14px 16px; border-left: 1px solid #806b3d; background: linear-gradient(90deg, #13130dd4, #13130d00); font-size: 12px; text-shadow: 0 1px 3px #000; }
.story-controls span { display: flex; align-items: center; gap: 10px; }
kbd { font-family: inherit; font-size: 11px; color: #dfcb94; }
.story-controls kbd { min-width: 42px; padding: 2px 3px; border: 1px solid #706441; text-align: center; background: #171813b8; }
.pickup-control { color: #ffdf8d; }
.pickup-control kbd { border-color: #d9b45c; }
.story-bottom-stack { position: absolute; bottom: 22px; left: 50%; transform: translateX(-50%); width: min(490px, calc(100vw - 32px)); display: grid; gap: 9px; }
.player-vitals { display: flex; align-items: center; gap: 22px; padding: 12px 16px; border: 1px solid #8f7846; background: linear-gradient(120deg, #151711f5, #090c09ec); box-shadow: 0 5px 24px #0007; }
.vitals-content { flex: 1; min-width: 0; }
.vital-label { display: flex; justify-content: space-between; margin: 0 0 5px; font-size: 12px; font-variant-numeric: tabular-nums; }
.vital-label em { font-style: normal; color: #a89d85; }
.vital-bar { box-sizing: border-box; height: 9px; padding: 1px; }
.vital-bar.health { margin-bottom: 10px; }
.vital-bar.rage span { background: linear-gradient(90deg, #aa501e, #f2b750); }
.skill-strip { display: flex; gap: 10px; pointer-events: auto; }
.skill-tile { position: relative; width: 46px; height: 49px; border: 1px solid #b99a55; background: #27251b; }
.skill-tile > svg { width: 33px; height: 33px; margin: 4px 6px; fill: none; stroke: #f0d08d; stroke-width: 2; }
.skill-tile > kbd { position: absolute; bottom: -5px; left: -4px; min-width: 15px; padding: 0 3px; border: 1px solid #8e7950; background: #141510; text-align: center; }
.skill-tile.unavailable > svg { opacity: .35; filter: grayscale(1); }
.skill-tile.unavailable { border-color: #68624e; background: #171914; }
.skill-cooldown { position: absolute; inset: 0; display: grid; place-items: center; background: #111b; color: #f8e6b2; font-size: 17px; font-variant-numeric: tabular-nums; }
.story-feedback { justify-self: center; padding: 6px 12px; background: #10150be6; color: #e6cc8e; font-size: 13px; }
.gameplay-error { pointer-events: auto; display: flex; align-items: center; gap: 10px; padding: 10px; color: #ffcac0; background: #341713f2; font-size: 13px; overflow-wrap: anywhere; }
.gameplay-error button { flex-shrink: 0; padding: 5px 8px; border: 1px solid #b78b66; }
.inventory-overlay { pointer-events: auto; position: absolute; inset: 0; display: grid; place-items: center; padding: 20px; background: #050807b5; }
.inventory-panel { pointer-events: auto; width: min(454px, 100%); box-sizing: border-box; max-height: calc(100vh - 40px); overflow-y: auto; padding: 22px 24px; background: linear-gradient(145deg, #20231cec, #0f1411fa); border: 1px solid #a08750; box-shadow: 0 18px 80px #0009; }
.inventory-header { display: flex; justify-content: space-between; align-items: center; padding-bottom: 20px; }
.inventory-header h1 { margin: 0; font-size: 20px; font-weight: 500; letter-spacing: 3px; }
.close-inventory { width: 30px; height: 30px; font-size: 24px; color: #cabb9b; background: transparent; border: 1px solid #746744; cursor: pointer; line-height: 1; }
.inventory-grid { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 8px; }
.inventory-slot { position: relative; aspect-ratio: 1; border: 1px solid #4e513d; background: #0b110f99; color: #e4d5b2; cursor: pointer; min-width: 0; }
.inventory-slot.occupied { border-color: #b39252; background: radial-gradient(ellipse, #51432680, #111812); }
.fragment-icon { width: 60%; height: 60%; margin: auto; fill: #7d976653; stroke: #d7ddb0; stroke-width: 1.5; }
.slot-count { position: absolute; bottom: 4px; right: 6px; font-size: 12px; }
.inventory-footer { display: flex; justify-content: center; padding-top: 22px; }
.world-travel-button { min-width: 158px; height: 38px; border: 1px solid #bfa167; color: #f0d69a; background: #343820; font-size: 14px; cursor: pointer; }
button:disabled { color: #929384; border-color: #4e5148; background: #232820; cursor: not-allowed; }
button:focus-visible, .hint-anchor:focus-visible { outline: 2px solid #f3cf79; outline-offset: 3px; }
.hint-anchor { position: relative; }
.ui-tooltip { display: none; position: absolute; bottom: calc(100% + 9px); left: 50%; transform: translateX(-50%); padding: 7px 10px; border: 1px solid #8f7846; background: #0c120ff5; color: #efe0bc; font-size: 12px; font-weight: 400; white-space: nowrap; z-index: 3; pointer-events: none; }
.hint-anchor:hover > .ui-tooltip, .hint-anchor:focus > .ui-tooltip, .hint-anchor:focus-within > .ui-tooltip { display: block; }
@media (max-height: 760px) { .story-bottom-stack { bottom: 14px; } .story-controls { gap: 7px; } .boss-status { top: 18px; } }
@media (max-width: 800px) { .story-controls { left: 12px; padding: 10px; font-size: 11px; } .player-vitals { gap: 14px; padding: 10px 12px; } }
@media (prefers-reduced-motion: reduce) { .vital-bar span, .boss-bar span { transition: none; } }
</style>
