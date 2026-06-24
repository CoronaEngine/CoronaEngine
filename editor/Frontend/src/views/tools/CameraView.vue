<template>
  <div class="camera-overlay" :class="{ borderless: borderlessFullscreen }" @contextmenu.prevent>
    <header v-if="!borderlessFullscreen" ref="toolbarRef" class="toolbar camera-drag-region">
      <div class="drag-handle" aria-label="Move camera window">::</div>
      <input
        v-model="cameraName"
        class="name-input no-drag"
        aria-label="Camera name"
        @keydown.enter="renameCamera"
        @blur="renameCamera"
      />
      <div class="dropdown no-drag">
        <button
          class="control dropdown-trigger"
          aria-label="Render backend"
          @click.stop="backendMenuOpen = !backendMenuOpen"
        >
          {{ backend === 'vision' ? 'Vision' : 'Native' }}
        </button>
        <div v-if="backendMenuOpen" class="dropdown-menu">
          <button @click="selectBackend('native')">Native</button>
          <button :disabled="!visionAvailable" @click="selectBackend('vision')">Vision</button>
        </div>
      </div>
      <div v-if="backend === 'vision'" class="dropdown no-drag">
        <button
          class="control dropdown-trigger vision-mode-trigger"
          aria-label="Vision render mode"
          @click.stop="visionModeMenuOpen = !visionModeMenuOpen"
        >
          {{ visionRenderModes.find((mode) => mode.value === visionRenderMode)?.label || 'Vision Path Tracing' }}
        </button>
        <div v-if="visionModeMenuOpen" class="dropdown-menu vision-mode-menu">
          <button
            v-for="mode in visionRenderModes"
            :key="mode.value"
            @click="selectVisionRenderMode(mode.value)"
          >
            {{ mode.label }}
          </button>
        </div>
      </div>
      <div class="dropdown no-drag">
        <button
          class="control dropdown-trigger"
          aria-label="Output channel"
          :disabled="backend === 'vision'"
          @click.stop="outputMenuOpen = !outputMenuOpen"
        >
          {{ outputModes.find((mode) => mode.value === outputMode)?.label || 'Final' }}
        </button>
        <div v-if="outputMenuOpen" class="dropdown-menu output-menu">
          <button
            v-for="mode in outputModes"
            :key="mode.value"
            @click="selectOutput(mode.value)"
          >
            {{ mode.label }}
          </button>
        </div>
      </div>
      <label class="speed no-drag">
        Speed
        <input v-model.number="moveSpeed" type="number" min="0.01" step="0.1" @change="saveSettings" />
      </label>
      <label class="resolution no-drag">
        <input v-model.number="renderWidth" type="number" min="64" step="16" @change="saveSettings" />
        x
        <input v-model.number="renderHeight" type="number" min="64" step="16" @change="saveSettings" />
      </label>
      <button class="button no-drag" @click="takeScreenshot">Screenshot</button>
      <button class="window-action maximize no-drag" aria-label="Toggle camera window fullscreen" @click="cycleWindowMode">[]</button>
      <button class="window-action close no-drag" aria-label="Close camera view" @click="closeView">x</button>
    </header>
    <div class="ui-mode-switch no-drag" @mousedown.stop @pointerdown.stop>
      <button
        v-for="item in viewportUiModeItems"
        :key="item.mode"
        type="button"
        :class="{ active: viewportUiMode === item.mode }"
        :title="item.title"
        @click="selectViewportUiMode(item.mode)"
      >
        {{ item.label }}
      </button>
    </div>
    <div
      ref="inputLayerRef"
      class="input-layer"
      :class="{ 'viewport-cursor-hidden': nativeViewportCursorEnabled && viewportUiMode === 'stereo3d' }"
      :style="nativeViewportCursorEnabled && viewportUiMode === 'stereo3d' ? { cursor: 'none' } : null"
      @pointermove="handleViewportPointer"
      @pointerdown="handleViewportPointerDown"
      @pointerup="handleViewportPointer"
      @pointerleave="handleViewportPointerLeave"
      @mousedown="beginLook"
      @mousemove="updateLook"
      @mouseup="endLook"
    />
    <div v-if="errorText" class="error">{{ errorText }}</div>
  </div>
</template>

<script setup>
import { nextTick, onBeforeUnmount, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { appService, projectService, sceneService } from '@/utils/bridge.js';
import { buildDragRegions, dragRegionsSignature } from '@/utils/cameraDragRegions.js';
import { coronaEventBus } from '@/utils/eventBus.js';
import {
  createViewportUiCalibrationStore,
  createViewportUiModeStore,
  createViewportUiPointerController,
  isNativeViewportCursorEnabled,
} from '@/utils/viewportUiMode.js';

const route = useRoute();
const sceneId = String(route.query.scene || '');
const cameraId = String(route.query.camera || '');

const camera = ref(null);
const cameraName = ref('Camera');
const backend = ref('native');
const visionRenderMode = ref('path_tracing');
const outputMode = ref('final_color');
const moveSpeed = ref(1);
const renderWidth = ref(960);
const renderHeight = ref(540);
const viewportUiMode = ref('flat2d');
const visionAvailable = ref(false);
const errorText = ref('');
const toolbarRef = ref(null);
const inputLayerRef = ref(null);
const backendMenuOpen = ref(false);
const visionModeMenuOpen = ref(false);
const outputMenuOpen = ref(false);
const borderlessFullscreen = ref(false);
let borderlessTogglePending = false;

const outputModes = [
  { value: 'final_color', label: 'Final' },
  { value: 'base_color', label: 'Base Color' },
  { value: 'normal', label: 'Normal' },
  { value: 'position', label: 'Position' },
  { value: 'object_id', label: 'Object ID' },
  { value: 'visibility_buffer', label: 'Visibility' },
];
const visionRenderModes = [
  { value: 'path_tracing', label: 'Vision Path Tracing' },
  { value: 'svgf', label: 'Vision SVGF' },
  { value: 'ssat', label: 'Vision SSAT' },
];

const viewportUiModeStore = createViewportUiModeStore();
const viewportUiCalibrationStore = createViewportUiCalibrationStore();
const viewportUiCalibrationDescriptor = {};
const nativeViewportCursorEnabled = isNativeViewportCursorEnabled();
const viewportUiModeItems = [
  { mode: 'flat2d', label: '2D UI', title: '普通屏幕 UI' },
  { mode: 'stereo3d', label: '3D UI', title: '光场屏立体 UI' },
];

const unwrap = (result) => result?.data ?? result;

const loadCamera = async () => {
  const [listResult, visionResult] = await Promise.all([
    sceneService.listCameraViews(sceneId),
    sceneService.isVisionAvailable(),
  ]);
  const payload = unwrap(listResult);
  camera.value = payload?.cameras?.find(
    (item) => String(item.camera_id || item.id) === cameraId,
  );
  if (!camera.value) throw new Error(`Camera ${cameraId} was not found`);
  visionAvailable.value = !!unwrap(visionResult)?.available;
  cameraName.value = camera.value.name;
  backend.value = camera.value.render_backend || 'native';
  visionRenderMode.value = camera.value.vision_render_mode || 'path_tracing';
  outputMode.value = backend.value === 'vision'
    ? 'final_color'
    : camera.value.output_mode || 'final_color';
  moveSpeed.value = camera.value.move_speed || 1;
  renderWidth.value = camera.value.width || 960;
  renderHeight.value = camera.value.height || 540;
};

const handleVisionSceneImported = async (payload = {}) => {
  if (payload?.sceneId && String(payload.sceneId) !== sceneId) return;
  const importedCameraId = payload?.cameraId ? String(payload.cameraId) : '';
  const importedCameraName = payload?.cameraName ? String(payload.cameraName) : '';
  if (importedCameraId && importedCameraId !== cameraId) return;
  if (!importedCameraId && importedCameraName && importedCameraName !== cameraName.value) return;
  if (payload?.visionRenderMode) {
    backend.value = 'vision';
    visionRenderMode.value = payload.visionRenderMode;
    if (camera.value) {
      camera.value.render_backend = 'vision';
      camera.value.vision_render_mode = payload.visionRenderMode;
    }
  }
  try {
    await loadCamera();
  } catch (error) {
    errorText.value = error.message;
  }
};

const renameCamera = async () => {
  if (!camera.value || !cameraName.value.trim() || cameraName.value === camera.value.name) return;
  try {
    const result = unwrap(await sceneService.renameCameraView(sceneId, cameraId, cameraName.value.trim()));
    camera.value = result.camera;
    cameraName.value = result.camera.name;
  } catch (error) {
    errorText.value = error.message;
    cameraName.value = camera.value.name;
  }
};

const changeBackend = async () => {
  try {
    const result = unwrap(await sceneService.setRenderBackend(backend.value, sceneId, cameraId));
    backend.value = result.mode;
    if (backend.value === 'vision') {
      outputMode.value = 'final_color';
      await sceneService.setOutputMode(sceneId, cameraId, 'final_color');
      await sceneService.setVisionRenderMode(sceneId, cameraId, visionRenderMode.value);
    }
  } catch (error) {
    errorText.value = error.message;
  }
};

const selectBackend = async (mode) => {
  backendMenuOpen.value = false;
  visionModeMenuOpen.value = false;
  if (backend.value === mode) return;
  backend.value = mode;
  await changeBackend();
};

const selectVisionRenderMode = async (mode) => {
  visionModeMenuOpen.value = false;
  if (visionRenderMode.value === mode && backend.value === 'vision') return;
  try {
    const result = unwrap(await sceneService.setVisionRenderMode(sceneId, cameraId, mode));
    visionRenderMode.value = result.mode || mode;
    if (camera.value) {
      camera.value.vision_render_mode = visionRenderMode.value;
    }
    if (backend.value !== 'vision') {
      const backendResult = unwrap(await sceneService.setRenderBackend('vision', sceneId, cameraId));
      backend.value = backendResult.mode || 'vision';
      if (backend.value === 'vision') {
        outputMode.value = 'final_color';
        await sceneService.setOutputMode(sceneId, cameraId, 'final_color');
      }
    }
  } catch (error) {
    errorText.value = error.message;
  }
};

const changeOutput = async () => {
  if (backend.value === 'vision') return;
  try {
    await sceneService.setOutputMode(sceneId, cameraId, outputMode.value);
  } catch (error) {
    errorText.value = error.message;
  }
};

const selectOutput = async (mode) => {
  outputMenuOpen.value = false;
  if (outputMode.value === mode) return;
  outputMode.value = mode;
  await changeOutput();
};

const viewportUiDescriptor = () => ({
  scope: 'camera',
  sceneId,
  cameraId,
  cameraHandle: camera.value?.handle || '',
});

const getCameraViewHitRect = () => inputLayerRef.value?.getBoundingClientRect?.() ?? null;

const getCameraRenderRect = () => {
  const width = Math.max(Number(window.innerWidth || renderWidth.value || 0), 0);
  const height = Math.max(Number(window.innerHeight || renderHeight.value || 0), 0);
  return {
    left: 0,
    top: 0,
    width,
    height,
    renderWidth: Math.max(Number(renderWidth.value || width), 0),
    renderHeight: Math.max(Number(renderHeight.value || height), 0),
  };
};

const viewportUiPointerController = createViewportUiPointerController({
  getBridge: () => window.coronaBridge,
  getCameraHandle: () => camera.value?.handle,
  getEnabled: () => viewportUiMode.value === 'stereo3d',
  getNativeCursorEnabled: () => nativeViewportCursorEnabled,
  getHitRect: getCameraViewHitRect,
  getRenderRect: getCameraRenderRect,
});

const applyViewportUiCalibration = (calibration) => {
  viewportUiCalibrationStore.applyToBridge({
    bridge: window.coronaBridge,
    cameraHandle: camera.value?.handle,
    calibration: calibration ?? viewportUiCalibrationStore.get(viewportUiCalibrationDescriptor),
  });
};

const syncViewportUiCalibration = () => {
  if (viewportUiMode.value === 'stereo3d') {
    applyViewportUiCalibration();
  }
};

const syncViewportUiMode = () => {
  viewportUiMode.value = viewportUiModeStore.get(viewportUiDescriptor());
  viewportUiModeStore.applyToBridge({
    bridge: window.coronaBridge,
    cameraHandle: camera.value?.handle,
    mode: viewportUiMode.value,
  });
  if (viewportUiMode.value !== 'stereo3d') {
    viewportUiPointerController.hide();
  }
  syncViewportUiCalibration();
};

const selectViewportUiMode = (mode) => {
  viewportUiMode.value = viewportUiModeStore.set(viewportUiDescriptor(), mode);
  viewportUiModeStore.applyToBridge({
    bridge: window.coronaBridge,
    cameraHandle: camera.value?.handle,
    mode: viewportUiMode.value,
  });
  if (viewportUiMode.value !== 'stereo3d') {
    viewportUiPointerController.hide();
  }
  syncViewportUiCalibration();
};

const handleViewportUiCalibrationChanged = (calibration) => {
  if (viewportUiMode.value === 'stereo3d') {
    applyViewportUiCalibration(calibration);
  }
};

const handleViewportUiCalibrationStorage = (event) => {
  if (event.key === viewportUiCalibrationStore.keyFor(viewportUiCalibrationDescriptor)) {
    syncViewportUiCalibration();
  }
};

const saveSettings = async () => {
  try {
    const width = Math.max(Number(renderWidth.value) || 960, 64);
    const height = Math.max(Number(renderHeight.value) || 540, 64);
    const result = unwrap(await sceneService.updateCameraView(sceneId, cameraId, {
      view_open: true,
      move_speed: Math.max(Number(moveSpeed.value) || 1, 0.01),
      width,
      height,
      view_width: width,
      view_height: height,
    }));
    camera.value = result.camera;
    renderWidth.value = result.camera.width || width;
    renderHeight.value = result.camera.height || height;
    await appService.resizeThisCameraView(
      renderWidth.value,
      renderHeight.value,
      sceneId,
      cameraId,
    ).catch(() => {});
  } catch (error) {
    errorText.value = error.message;
  }
};

let resizeTimer = 0;
let lastSyncedWidth = 0;
let lastSyncedHeight = 0;
let dragRegionFrame = 0;
let lastDragRegionSignature = '';

const syncWindowSize = async (force = false) => {
  if (!camera.value) return;
  const width = Math.max(Math.round(window.innerWidth || renderWidth.value), 64);
  const height = Math.max(Math.round(window.innerHeight || renderHeight.value), 64);
  if (!force && width === lastSyncedWidth && height === lastSyncedHeight) return;
  lastSyncedWidth = width;
  lastSyncedHeight = height;
  renderWidth.value = width;
  renderHeight.value = height;
  try {
    const result = unwrap(await sceneService.updateCameraView(sceneId, cameraId, {
      view_open: true,
      width,
      height,
      view_width: width,
      view_height: height,
      move_speed: Math.max(Number(moveSpeed.value) || 1, 0.01),
    }));
    camera.value = result.camera;
  } catch (error) {
    errorText.value = error.message;
  }
};

const scheduleWindowSizeSync = () => {
  window.clearTimeout(resizeTimer);
  resizeTimer = window.setTimeout(() => syncWindowSize(false), 120);
};

const takeScreenshot = async () => {
  try {
    const selected = unwrap(await sceneService.selectScreenshotPath(sceneId, cameraId));
    if (selected?.status === 'canceled' || !selected?.path) return;
    await sceneService.saveScreenshot(sceneId, selected.path, cameraId);
  } catch (error) {
    errorText.value = error.message;
  }
};

const toggleMaximize = async () => {
  await appService.toggleMaximizeThisCameraView(sceneId, cameraId).catch((error) => {
    errorText.value = error.message;
  });
};

const cycleWindowMode = async () => {
  await appService.cycleThisCameraViewWindowMode(sceneId, cameraId).catch(async (error) => {
    if (appService.toggleMaximizeThisCameraView) {
      await toggleMaximize();
      return;
    }
    errorText.value = error.message;
  });
};

const toggleBorderlessFullscreen = async () => {
  if (borderlessTogglePending) return;
  borderlessTogglePending = true;
  try {
    await appService.toggleBorderlessThisCameraView(sceneId, cameraId);
    borderlessFullscreen.value = !borderlessFullscreen.value;
    await nextTick();
    await syncDragRegions({ force: true });
  } catch (error) {
    errorText.value = error.message;
  } finally {
    borderlessTogglePending = false;
  }
};

const closeView = async () => {
  try {
    await sceneService.closeCameraView(sceneId, cameraId);
  } finally {
    await appService.closeThisTab(`camera:${cameraId}`).catch(() => {});
  }
};

const keys = new Set();
let animationFrame = 0;
let previousTime = 0;
let looking = false;
let lastMouseX = 0;
let lastMouseY = 0;

const normalize = (value) => {
  const length = Math.hypot(value[0], value[1], value[2]) || 1;
  return value.map((item) => item / length);
};
const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1],
  a[2] * b[0] - a[0] * b[2],
  a[0] * b[1] - a[1] * b[0],
];

const publishPose = () => {
  const item = camera.value;
  const bridge = window.coronaBridge;
  if (!item || !bridge || typeof bridge.cameraMove !== 'function') return;
  bridge.cameraMove(
    item.handle,
    Array.from(item.position),
    Array.from(item.forward),
    Array.from(item.world_up),
    item.fov,
  );
};

const moveCamera = (dt) => {
  const item = camera.value;
  if (!item || !keys.size || dt <= 0) return;
  const forward = normalize(item.forward);
  const up = normalize(item.world_up);
  const right = normalize(cross(up, forward));
  const direction = [0, 0, 0];
  const add = (axis, scale) => axis.forEach((value, index) => { direction[index] += value * scale; });
  if (keys.has('KeyW')) add(forward, 1);
  if (keys.has('KeyS')) add(forward, -1);
  if (keys.has('KeyD')) add(right, 1);
  if (keys.has('KeyA')) add(right, -1);
  if (keys.has('KeyQ')) add(up, 1);
  if (keys.has('KeyE')) add(up, -1);
  if (Math.hypot(...direction) === 0) return;
  const unit = normalize(direction);
  const distance = Math.max(Number(moveSpeed.value) || 1, 0.01) * dt;
  item.position = item.position.map((value, index) => value + unit[index] * distance);
  publishPose();
};

const rotateCamera = (dt) => {
  if (!camera.value || dt <= 0) return;
  const horizontal = (keys.has('ArrowRight') ? 1 : 0) - (keys.has('ArrowLeft') ? 1 : 0);
  const vertical = (keys.has('ArrowUp') ? 1 : 0) - (keys.has('ArrowDown') ? 1 : 0);
  if (horizontal === 0 && vertical === 0) return;

  const forward = normalize(camera.value.forward);
  let yaw = Math.atan2(forward[0], forward[2]);
  let pitch = Math.asin(Math.max(-0.999, Math.min(0.999, forward[1])));
  const angle = (2 * Math.PI / 180) * 60 * dt;
  yaw += horizontal * angle;
  pitch = Math.max(-1.4, Math.min(1.4, pitch + vertical * angle));
  camera.value.forward = [
    Math.sin(yaw) * Math.cos(pitch),
    Math.sin(pitch),
    Math.cos(yaw) * Math.cos(pitch),
  ];
  publishPose();
};

const movementFrame = (time) => {
  const dt = previousTime ? Math.min((time - previousTime) / 1000, 0.05) : 0;
  previousTime = time;
  moveCamera(dt);
  rotateCamera(dt);
  animationFrame = requestAnimationFrame(movementFrame);
};

const movementCode = (event) => {
  if (event.code && event.code !== 'Unidentified') return event.code;
  const key = event.key?.toLowerCase();
  return {
    w: 'KeyW',
    a: 'KeyA',
    s: 'KeyS',
    d: 'KeyD',
    q: 'KeyQ',
    e: 'KeyE',
    arrowup: 'ArrowUp',
    arrowdown: 'ArrowDown',
    arrowleft: 'ArrowLeft',
    arrowright: 'ArrowRight',
  }[key] || '';
};

const onKeyDown = (event) => {
  if (event.key === 'F11' || event.code === 'F11') {
    event.preventDefault();
    if (!event.repeat) {
      toggleBorderlessFullscreen();
    }
    return;
  }
  const code = movementCode(event);
  if ([
    'KeyW', 'KeyA', 'KeyS', 'KeyD', 'KeyQ', 'KeyE',
    'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight',
  ].includes(code)) {
    const wasDown = keys.has(code);
    keys.add(code);
    if (!wasDown) {
      moveCamera(1 / 60);
      rotateCamera(1 / 60);
    }
    event.preventDefault();
  }
};
const onKeyUp = (event) => keys.delete(movementCode(event));
const beginLook = (event) => {
  if (event.button !== 2) return;
  looking = true;
  lastMouseX = event.clientX;
  lastMouseY = event.clientY;
};
const endLook = () => { looking = false; };

const viewportCursorShape = () => (looking ? 'grabbing' : 'arrow');

const handleViewportPointer = (event) => {
  viewportUiPointerController.send(event, event.type, viewportCursorShape());
};

const handleViewportPointerDown = (event) => {
  viewportUiPointerController.send(
    event,
    event.type,
    event.button === 2 ? 'grabbing' : viewportCursorShape(),
  );
};

const handleViewportPointerLeave = () => {
  viewportUiPointerController.hide();
};

const updateLook = (event) => {
  if (!looking || !camera.value) return;
  const dx = event.clientX - lastMouseX;
  const dy = event.clientY - lastMouseY;
  lastMouseX = event.clientX;
  lastMouseY = event.clientY;
  const forward = normalize(camera.value.forward);
  let yaw = Math.atan2(forward[0], forward[2]);
  let pitch = Math.asin(Math.max(-0.999, Math.min(0.999, forward[1])));
  yaw += dx * 0.003;
  pitch = Math.max(-1.4, Math.min(1.4, pitch - dy * 0.003));
  camera.value.forward = [
    Math.sin(yaw) * Math.cos(pitch),
    Math.sin(pitch),
    Math.cos(yaw) * Math.cos(pitch),
  ];
  publishPose();
};

const pushDragRegions = async (regions, force = false) => {
  const signature = dragRegionsSignature(regions);
  if (!force && signature === lastDragRegionSignature) return;
  lastDragRegionSignature = signature;
  await projectService.setCurrentTabDragRegions(regions).catch(() => {});
};

const syncDragRegions = async ({ force = false } = {}) => {
  if (borderlessFullscreen.value) {
    await pushDragRegions([{ x: 0, y: 0, w: 0, h: 0 }], force);
    return;
  }
  await nextTick();
  const toolbar = toolbarRef.value;
  const toolbarRect = toolbar?.getBoundingClientRect?.();
  if (!toolbarRect) return;
  const noDragRects = Array.from(toolbar.querySelectorAll('.no-drag'))
    .map((element) => element.getBoundingClientRect());
  const regions = buildDragRegions({ toolbarRect, noDragRects, padding: 2 });
  await pushDragRegions(regions, force);
};

const scheduleDragRegionSync = () => {
  if (dragRegionFrame) return;
  dragRegionFrame = window.requestAnimationFrame(() => {
    dragRegionFrame = 0;
    syncDragRegions().catch(() => {});
  });
};

onMounted(async () => {
  document.documentElement.style.background = 'transparent';
  document.body.style.background = 'transparent';
  await syncDragRegions({ force: true });
  try {
    await loadCamera();
    syncViewportUiMode();
    await syncWindowSize(true);
  } catch (error) {
    errorText.value = error.message;
  }
  window.addEventListener('resize', scheduleWindowSizeSync);
  window.addEventListener('resize', scheduleDragRegionSync);
  window.addEventListener('keydown', onKeyDown);
  window.addEventListener('keyup', onKeyUp);
  window.addEventListener('storage', handleViewportUiCalibrationStorage);
  coronaEventBus.on('viewport-ui-calibration-changed', handleViewportUiCalibrationChanged);
  coronaEventBus.on('vision-scene-imported', handleVisionSceneImported);
  animationFrame = requestAnimationFrame(movementFrame);
});

onBeforeUnmount(() => {
  cancelAnimationFrame(animationFrame);
  if (dragRegionFrame) window.cancelAnimationFrame(dragRegionFrame);
  window.clearTimeout(resizeTimer);
  window.removeEventListener('resize', scheduleWindowSizeSync);
  window.removeEventListener('resize', scheduleDragRegionSync);
  window.removeEventListener('keydown', onKeyDown);
  window.removeEventListener('keyup', onKeyUp);
  window.removeEventListener('storage', handleViewportUiCalibrationStorage);
  coronaEventBus.off('viewport-ui-calibration-changed', handleViewportUiCalibrationChanged);
  viewportUiPointerController.dispose();
  coronaEventBus.off('vision-scene-imported', handleVisionSceneImported);
});
</script>

<style scoped>
.camera-overlay {
  position: fixed;
  inset: 0;
  color: #f4f4f4;
  background: transparent;
  overflow: hidden;
  user-select: none;
}
.toolbar {
  position: absolute;
  z-index: 2;
  top: 0;
  left: 0;
  right: 0;
  height: 34px;
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 6px;
  background: rgba(22, 22, 22, 0.86);
  border-bottom: 1px solid rgba(255, 255, 255, 0.14);
}
.input-layer { position: absolute; inset: 34px 0 0; z-index: 1; }
.input-layer.viewport-cursor-hidden,
.input-layer.viewport-cursor-hidden * {
  cursor: none !important;
}
.camera-overlay.borderless .input-layer { inset: 0; }
.ui-mode-switch {
  position: absolute;
  z-index: 4;
  top: 42px;
  left: 10px;
  display: flex;
  gap: 2px;
  padding: 3px;
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 4px;
  background: rgba(18, 22, 27, 0.82);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.35);
  pointer-events: auto;
}
.camera-overlay.borderless .ui-mode-switch { top: 10px; }
.ui-mode-switch button {
  height: 24px;
  min-width: 42px;
  padding: 0 8px;
  border: 0;
  border-radius: 3px;
  background: transparent;
  color: #cbd5e1;
  font-size: 11px;
  font-weight: 600;
  cursor: pointer;
}
.ui-mode-switch button:hover { background: rgba(255, 255, 255, 0.1); }
.ui-mode-switch button.active {
  background: #4b5563;
  color: #fff;
}
.drag-handle {
  width: 22px;
  height: 24px;
  display: grid;
  place-items: center;
  flex: 0 0 22px;
  color: #aaa;
  cursor: move;
}
.name-input, .control, .speed input, .resolution input, .button, .window-action {
  height: 24px;
  border: 1px solid #555;
  border-radius: 4px;
  background: rgba(35, 35, 35, 0.9);
  color: #eee;
  font-size: 11px;
}
.name-input { width: 150px; padding: 0 6px; }
.control { padding: 0 5px; }
.dropdown { position: relative; }
.dropdown-trigger { min-width: 66px; cursor: pointer; }
.vision-mode-trigger {
  max-width: 146px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.dropdown-menu {
  position: absolute;
  z-index: 5;
  top: 27px;
  left: 0;
  min-width: 100%;
  padding: 3px;
  display: grid;
  gap: 2px;
  border: 1px solid #555;
  border-radius: 4px;
  background: rgba(28, 28, 28, 0.98);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.45);
}
.dropdown-menu button {
  height: 24px;
  padding: 0 8px;
  border: 0;
  border-radius: 3px;
  background: transparent;
  color: #eee;
  text-align: left;
  white-space: nowrap;
  cursor: pointer;
}
.dropdown-menu button:hover { background: rgba(255, 255, 255, 0.12); }
.dropdown-menu button:disabled { color: #777; cursor: default; }
.output-menu { min-width: 92px; }
.vision-mode-menu { min-width: 148px; }
.speed, .resolution { display: flex; align-items: center; gap: 3px; font-size: 10px; }
.speed input { width: 54px; padding: 0 4px; }
.resolution input { width: 58px; padding: 0 4px; }
.button { padding: 0 8px; cursor: pointer; }
.window-action { width: 24px; cursor: pointer; }
.maximize { margin-left: auto; }
.close { color: #ffb4b4; }
.error {
  position: absolute;
  z-index: 3;
  left: 8px;
  bottom: 8px;
  max-width: 70%;
  padding: 5px 8px;
  border-radius: 4px;
  background: rgba(140, 20, 20, 0.88);
  font-size: 11px;
}
</style>
