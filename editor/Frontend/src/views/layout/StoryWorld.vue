<script setup>
import { onMounted, onUnmounted, ref } from 'vue';
import { useRouter, onBeforeRouteLeave } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';
import { projectLauncherService, cancelPendingProjectOpen, getProjectSelectionVersion } from '@/services/projectLauncherService.js';
import { trackWorldSessionWork, notifyWorldError, registerWorldSessionSave } from '@/services/worldSessionLifecycle.js';
import { createStoryNavigationController } from '../../../../../game/frontend/storyNavigation.mjs';

import { ensureStoryCharacters } from '../../../../../game/frontend/storyActors.mjs';
import { createPlayerController } from '../../../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../../../game/frontend/playerSave.mjs';

const router = useRouter();
const surface = ref(null);
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
  isInputLocked: () => !focused || document.hidden || navigation.busy || savingPlayer,
  getRect: () => surface.value?.getBoundingClientRect(),
  getPixelRatio: () => window.devicePixelRatio,
});
const unwrap = (response) => response?.data ?? response;
const saveRegistration = registerWorldSessionSave(async () => {
  camera.resetInput();
  // Keep input locked until the actual acknowledgement, even after a UI timeout.
  // A retry must not release the scene with movement newer than the pending save.
  savingPlayer = true;
  try {
    // Initialization owns its error UI; finish any already-submitted native call.
    await initialization.catch(() => {});
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
      notifyWorldError(error, '保存玩家位置失败');
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
  isReady: () => cameraReady && focused && !document.hidden,
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
    void exitStory();
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
  surface.value?.focus();
  if (focused) camera.pointerDown(event);
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
    const sceneId = scenes[index]?.path || init?.path;
    if (!sceneId) throw new Error('当前世界没有可用场景');
    const { snapshot, player, targetOffset } = await ensureStoryCharacters({
      api: editorApi, sceneId, frontendUrl: window.location.href,
      isCurrent: () => current() && worldModeState.projectPath === projectPath,
    });
    if (!current() || worldModeState.projectPath !== projectPath) return;
    cameraReady = camera.bind(snapshot, sceneId);
    camera.bindPlayer(player, targetOffset);
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
    @pointerdown="onPointerDown" @wheel.prevent="camera.wheel" @contextmenu.prevent
  />
</template>

<style scoped>
.story-world-viewport {
  position: fixed;
  inset: 0;
  background: transparent;
  outline: none;
  overflow: hidden;
  touch-action: none;
}
</style>
