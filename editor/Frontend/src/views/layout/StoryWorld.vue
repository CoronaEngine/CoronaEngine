<script setup>
import { onMounted, onUnmounted, ref } from 'vue';
import { useRouter, onBeforeRouteLeave } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState, normalizeProjectPath } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';
import { projectLauncherService, cancelPendingProjectOpen, getProjectSelectionVersion } from '@/services/projectLauncherService.js';
import { trackWorldSessionWork, notifyWorldError, registerWorldSessionSave } from '@/services/worldSessionLifecycle.js';
import { createStoryNavigationController } from '../../../../../game/frontend/storyNavigation.mjs';

import { ensureStoryCharacters } from '../../../../../game/frontend/storyActors.mjs';
import { createPlayerController } from '../../../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../../../game/frontend/playerSave.mjs';
import { createStoryGameplay, worldBounds, distanceToBounds, pickupDistance } from '../../../../../game/frontend/storyGameplay.mjs';
import { STORY_CHARACTERS } from '../../../../../game/frontend/storyCharacters.mjs';

const router = useRouter();
const surface = ref(null);
const inventoryOpen = ref(false);
const gameplayState = ref(null);
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
let exitPending = null;
let resizeObserver = null;
let leaving = false;
let dpiQuery = null;
let focused = document.hasFocus();
const current = () => !disposed && !leaving && worldModeState.status === 'ready'
  && worldModeState.mode === 'story' && worldModeState.revision === revision;
const camera = createStoryCameraController({
  createControls: createPlayerController,
  onError: notifyWorldError,
  getBridge: () => current() ? window.coronaBridge : null,
  isCurrent: current,
  isInputLocked: () => !focused || document.hidden || navigation.busy || savingPlayer || inventoryOpen.value,
  onPlayerChanged: updateProximity,
  getRect: () => surface.value?.getBoundingClientRect(),
  getPixelRatio: () => window.devicePixelRatio,
});
const unwrap = (response) => response?.data ?? response;
function updateProximity() {
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
      gameplay: data, combatOnly: true, assertSource });
    visualSignature = next;
    if (current()) updateProximity();
  },
});
async function runAction(action) {
  if (!current() || inventoryOpen.value || navigation.busy || savingPlayer || actionBusy.value || !cameraReady) return;
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
  camera.resetInput();
  inventoryOpen.value = !inventoryOpen.value;
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
  isReady: () => cameraReady && focused && !document.hidden && !inventoryOpen.value,
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
  prepare: key => editorApi.scratch.sendKeyEvent(key, '', key.slice(3)),
  trackPreparation: trackWorldSessionWork,
  openProject: path => projectLauncherService.openProject(path),
  cancelProjectOpen: cancelPendingProjectOpen,
  leave: () => router.replace('/StartScreen'),
  notify: notifyWorldError,
});
function onKeyDown(event) {
  if (!current()) return;
  if (event.code === 'Escape' || event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    if (inventoryOpen.value) toggleInventory();
    else if (!event.repeat) void exitStory();
    return;
  }
  if (event.ctrlKey || event.altKey || event.metaKey || event.isComposing) return;
  if (event.code === 'Tab' || event.key === 'Tab') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused && !document.hidden && cameraReady && !navigation.busy && !savingPlayer) toggleInventory();
    return;
  }
  if (inventoryOpen.value) { event.preventDefault(); event.stopPropagation(); return; }
  if (event.code === 'KeyF' || event.key?.toLowerCase() === 'f') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused) void runAction(gameplay.pickup);
    return;
  }
  if (navigation.keyDown(event)) return;
  if (focused) camera.keyDown(event);
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
    const { snapshot, player, targetOffset } = await ensureStoryCharacters({
      api: editorApi, sceneId, frontendUrl: window.location.href, gameplay: loadedGameplay, assertSource,
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
      readPlayer: camera.snapshotPlayer, stopInput: camera.resetInput });
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
  <div
    ref="surface" class="story-world-viewport" tabindex="0" data-story-viewport
    @pointerdown="onPointerDown" @pointerleave="camera.resetInput" @wheel.prevent="camera.wheel" @contextmenu.prevent
  >
    <div v-if="gameplayState" class="story-hud" aria-label="战斗界面">
      <section class="player-vitals" aria-label="人物生命与法力">
        <div class="hud-eyebrow">{{ gameplayState.role === 'main' ? '主世界' : '子世界' }} · 旅者</div>
        <div class="vital-label"><span>生命</span><span>{{ gameplayState.config.playerHp }} / {{ gameplayState.config.playerHp }}</span></div>
        <div class="vital-bar health" role="progressbar" aria-label="生命" :aria-valuenow="gameplayState.config.playerHp" :aria-valuemax="gameplayState.config.playerHp" aria-valuemin="0"><span /></div>
        <div class="vital-label"><span>法力</span><span>{{ gameplayState.config.playerMp }} / {{ gameplayState.config.playerMp }}</span></div>
        <div class="vital-bar mana" role="progressbar" aria-label="法力" :aria-valuenow="gameplayState.config.playerMp" :aria-valuemax="gameplayState.config.playerMp" aria-valuemin="0"><span /></div>
      </section>
      <section v-if="bossNearby && !inventoryOpen" class="boss-status" aria-label="Boss 血条">
        <div class="boss-name">BOSS · 巨龙</div>
        <div class="boss-bar" role="progressbar" aria-label="Boss 生命" :aria-valuenow="gameplayState.state.boss.hp" :aria-valuemax="gameplayState.config.bossHp" aria-valuemin="0">
          <span :style="{ width: `${100 * gameplayState.state.boss.hp / gameplayState.config.bossHp}%` }" />
        </div>
        <div class="boss-number">{{ gameplayState.state.boss.hp }} / {{ gameplayState.config.bossHp }}</div>
      </section>
      <div class="story-feedback" role="status" aria-live="polite">
        <div v-if="feedback" class="feedback-text">{{ feedback }}</div>
        <div v-if="canPickup && !inventoryOpen" class="pickup-hint"><kbd>F</kbd> 拾取 世界碎片</div>
      </div>
      <div v-if="gameplayError" class="gameplay-error" role="alert" @pointerdown.stop @wheel.stop @pointermove.stop="camera.resetInput">
        <span>{{ gameplayError }}</span>
        <button :disabled="actionBusy || inventoryOpen" @click.stop="runAction(gameplay.flush)">重试保存</button>
      </div>
      <div class="story-controls">
        <span><kbd>W A S D</kbd> 移动</span><span>鼠标 转视角 · 边缘持续转向</span>
        <span><kbd>左键</kbd> 攻击</span><span><kbd>F</kbd> 拾取</span><span><kbd>Tab</kbd> 背包</span>
        <span><kbd>O / P</kbd> 进入 / 返回</span><span><kbd>Esc</kbd> {{ inventoryOpen ? '关闭背包' : '保存退出' }}</span>
      </div>
    </div>
    <div v-if="inventoryOpen && gameplayState" class="inventory-overlay" @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent>
      <section class="inventory-panel" role="dialog" aria-modal="true" aria-labelledby="inventory-title">
        <header><div><div class="hud-eyebrow">旅者行囊 · 主子世界共享</div><h1 id="inventory-title">背包</h1></div><button aria-label="关闭背包" @click="toggleInventory">关闭 <kbd>Tab</kbd></button></header>
        <div v-if="gameplayState.state.inventory.worldFragment" class="inventory-item">
          <div class="fragment-icon" aria-hidden="true">◆</div>
          <div><h2>世界碎片</h2><p>击败巨龙后拾取的世界残片。</p></div>
          <strong>× {{ gameplayState.state.inventory.worldFragment }}</strong>
        </div>
        <div v-else class="inventory-empty"><span aria-hidden="true">◇</span><p>背包还是空的</p><small>在主世界击败 Boss，靠近掉落物按 F 拾取。</small></div>
        <footer>按 <kbd>Tab</kbd> 或 <kbd>Esc</kbd> 返回世界</footer>
      </section>
    </div>
  </div>
</template>

<style scoped>
.story-world-viewport { position: fixed; inset: 0; background: transparent; outline: none; overflow: hidden; touch-action: none; color: #f2ede6; font-family: "Microsoft YaHei", sans-serif; }
.story-hud { position: absolute; inset: 0; pointer-events: none; }
.player-vitals { position: absolute; top: 28px; left: 28px; width: min(240px, 27vw); padding: 18px 20px; border: 1px solid #ffffff20; background: #10151ddb; border-radius: 8px; }
.hud-eyebrow { color: #c9b58d; font-size: 11px; letter-spacing: 2px; margin-bottom: 14px; }
.vital-label { display: flex; justify-content: space-between; font-size: 12px; margin: 9px 0 5px; }
.vital-bar, .boss-bar { overflow: hidden; background: #05070cba; border: 1px solid #ffffff24; border-radius: 3px; }
.vital-bar { height: 9px; }
.vital-bar span, .boss-bar span { display: block; height: 100%; width: 100%; }
.health span { background: linear-gradient(90deg, #914347, #df756c); }
.mana span { background: linear-gradient(90deg, #365f9d, #75b8e3); }
.boss-status { position: absolute; top: 30px; left: 50%; transform: translateX(-50%); width: min(430px, 38vw); text-align: center; text-shadow: 0 2px 5px #000; }
.boss-name { font-size: 17px; letter-spacing: 4px; margin-bottom: 10px; }
.boss-bar { height: 12px; }
.boss-bar span { background: linear-gradient(90deg, #8e292e, #e36256); transition: width .15s; }
.boss-number { font-size: 12px; margin-top: 5px; }
.story-feedback { position: absolute; bottom: 105px; width: 100%; text-align: center; }
.feedback-text { font-size: 18px; color: #f7dba9; text-shadow: 0 2px 5px #000; margin-bottom: 16px; }
.pickup-hint { display: inline-flex; gap: 12px; align-items: center; padding: 12px 22px; border: 1px solid #cdb88b88; border-radius: 6px; background: #10151de8; }
kbd { display: inline-block; font: inherit; font-size: 11px; color: #f6dfb5; padding: 2px 6px; border: 1px solid #b9a78977; border-radius: 3px; white-space: nowrap; }
.story-controls { position: absolute; bottom: 18px; left: 50%; transform: translateX(-50%); width: max-content; max-width: calc(100% - 40px); display: flex; flex-wrap: wrap; justify-content: center; gap: 10px 17px; font-size: 11px; padding: 11px 16px; border-radius: 6px; background: #10151ddd; }
.story-controls span { white-space: nowrap; }
.gameplay-error { pointer-events: auto; position: absolute; bottom: 180px; left: 50%; transform: translateX(-50%); width: min(600px, 85vw); background: #431e22f0; padding: 16px; border: 1px solid #e89a89; display: flex; align-items: center; gap: 18px; font-size: 13px; }
button { color: #eddfc5; border: 1px solid #b5a27f66; background: #ffffff09; padding: 8px 12px; border-radius: 4px; cursor: pointer; white-space: nowrap; }
button:hover { background: #ffffff18; } button:disabled { opacity: .5; cursor: wait; }
.inventory-overlay, .inventory-panel { pointer-events: auto; }
.inventory-overlay { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; background: #040810b8; }
.inventory-panel { width: min(620px, 86vw); background: #141b24f5; border: 1px solid #ad93675c; border-radius: 12px; box-shadow: 0 24px 100px #0008; padding: 28px; }
.inventory-panel header { display: flex; align-items: center; justify-content: space-between; border-bottom: 1px solid #ffffff14; padding-bottom: 20px; }
.inventory-panel h1 { margin: 0; font-size: 26px; letter-spacing: 4px; } .inventory-panel .hud-eyebrow { margin-bottom: 8px; }
.inventory-item { display: flex; gap: 18px; align-items: center; margin: 26px 0; padding: 20px; border: 1px solid #b4a07d40; background: #ffffff04; border-radius: 6px; }
.fragment-icon { font-size: 42px; color: #e2726c; width: 64px; text-align: center; }
.inventory-item h2 { font-size: 17px; margin: 0 0 8px; } .inventory-item p { font-size: 12px; color: #b6b7bb; margin: 0; }
.inventory-item strong { margin-left: auto; color: #f1d9b3; white-space: nowrap; }
.inventory-empty { text-align: center; padding: 50px 0; color: #bfc1c7; } .inventory-empty > span { font-size: 42px; color: #b6a17b; } .inventory-empty small { color: #9198a4; }
.inventory-panel footer { border-top: 1px solid #ffffff14; padding-top: 16px; font-size: 12px; color: #a7aab1; text-align: right; }
@media (max-width: 720px) { .player-vitals { left: 12px; top: 12px; padding: 12px; } .boss-status { left: auto; right: 12px; transform: none; top: 20px; width: 45vw; } .story-controls { font-size: 10px; } }
</style>
