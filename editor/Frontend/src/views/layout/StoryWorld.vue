<script setup>
import { onMounted, onUnmounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';

const router = useRouter();
const surface = ref(null);
const revision = worldModeState.revision;
const projectPath = worldModeState.projectPath;
let disposed = false;
let resizeObserver = null;
let leaving = false;
let dpiQuery = null;
let focused = document.hasFocus();
const current = () => !disposed && !leaving && worldModeState.status === 'ready'
  && worldModeState.mode === 'story' && worldModeState.revision === revision;
const camera = createStoryCameraController({
  getBridge: () => current() ? window.coronaBridge : null,
  isCurrent: current,
  isInputLocked: () => !focused || document.hidden,
  getRect: () => surface.value?.getBoundingClientRect(),
  getPixelRatio: () => window.devicePixelRatio,
});
const unwrap = (response) => response?.data ?? response;
function onKeyDown(event) {
  if (!current()) return;
  if (event.code === 'Escape' || event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    camera.resetInput();
    if (!leaving) { leaving = true; camera.dispose(); void router.replace('/StartScreen'); }
    return;
  }
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
  try {
    const init = unwrap(await editorApi.main.onInit());
    if (!current()) return;
    const scenes = init?.scenes || [];
    const index = Math.min(Math.max(Number(init?.active_index) || 0, 0), Math.max(0, scenes.length - 1));
    const sceneId = scenes[index]?.path || init?.path;
    if (!sceneId) throw new Error('当前世界没有可用场景');
    const snapshot = await editorApi.scene.getSnapshot(sceneId);
    if (!current() || worldModeState.projectPath !== projectPath) return;
    camera.bind(snapshot, sceneId);
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
  } catch (error) {
    if (!current()) return;
    await router.replace('/StartScreen');
    window.alert(error.message || '剧情世界加载失败');
  }
});
onUnmounted(() => {
  disposed = true;
  camera.dispose();
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
