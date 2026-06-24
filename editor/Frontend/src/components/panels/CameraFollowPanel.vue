<template>
  <div v-if="visible" class="camera-follow-root">
    <!-- 小圆点切换按钮 -->
    <div
      ref="dotRef"
      class="camera-follow-dot"
      :class="{ active: following }"
      :style="dotStyle"
      title="摄像机跟随 - 按住拖拽移动，点击展开"
      @mousedown="onDotMouseDown"
      @mouseup="onDotMouseUp"
    >
      ●
    </div>

    <!-- 设置面板 -->
    <div v-if="panelOpen" ref="panelRef" class="camera-follow-panel" :style="panelStyle">
      <div class="camera-follow-title">相机跟随</div>

      <label class="camera-follow-row">
        <span>启用</span>
        <input v-model="enabled" type="checkbox" @change="applyLock" />
      </label>

      <div class="camera-follow-offsets">
        <span>偏移</span>
        <label>X<input v-model.number="ox" type="number" step="0.1" @change="updateOffset" /></label>
        <label>Y<input v-model.number="oy" type="number" step="0.1" @change="updateOffset" /></label>
        <label>Z<input v-model.number="oz" type="number" step="0.1" @change="updateOffset" /></label>
      </div>

      <button class="camera-follow-apply" @click="applyLock">应用</button>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onUnmounted } from 'vue';
import { Bridge } from '@/utils/bridge.js';
import { shouldHandleCameraFollowKey } from '@/utils/editorInputFocusGate.js';

const visible = ref(false);
const panelOpen = ref(false);
const following = ref(false);
const enabled = ref(false);
const ox = ref(0);
const oy = ref(0);
const oz = ref(2);

const dotRef = ref(null);
const panelRef = ref(null);

const dotStyle = ref({});
const panelStyle = ref({});

// ── 拖拽 ──
let dragging = false;
let dragStartX = 0;
let dragStartY = 0;
let dragStartLeft = 0;
let dragStartTop = 0;
let wasDragged = false;

function onDotMouseDown(e) {
  if (e.button !== 0) return;
  dragging = true;
  wasDragged = false;
  dragStartX = e.clientX;
  dragStartY = e.clientY;
  const rect = dotRef.value.getBoundingClientRect();
  dragStartLeft = rect.left;
  dragStartTop = rect.top;
  dotStyle.value = {
    left: dragStartLeft + 'px',
    top: dragStartTop + 'px',
    right: 'auto',
  };
  e.preventDefault();
}

function onDotMouseUp() {
  if (!dragging) return;
  dragging = false;
  if (!wasDragged) {
    panelOpen.value = !panelOpen.value;
    if (panelOpen.value) updatePanelPosition();
  }
}

function onMouseMove(e) {
  if (!dragging) return;
  wasDragged = true;
  dotStyle.value = {
    left: dragStartLeft + e.clientX - dragStartX + 'px',
    top: dragStartTop + e.clientY - dragStartY + 'px',
  };
}

function onMouseUpGlobal() {
  dragging = false;
}

function updatePanelPosition() {
  const dotRect = dotRef.value?.getBoundingClientRect();
  if (dotRect) {
    panelStyle.value = {
      left: Math.max(0, dotRect.left - 220 + 24) + 'px',
      top: dotRect.top + 30 + 'px',
    };
  }
}

// ── API 调用 ──
async function applyLock() {
  try {
    const resp = await Bridge.callCEF('CoronaEditor', 'camera_lock_set', [
      enabled.value,
      ox.value,
      oy.value,
      oz.value,
      0,
      0,
      0,
    ]);
    if (resp?.data?.ok) {
      following.value = enabled.value;
      if (enabled.value && resp.data.offset) {
        ox.value = Number(resp.data.offset[0].toFixed?.(1) ?? resp.data.offset[0]);
        oy.value = Number(resp.data.offset[1].toFixed?.(1) ?? resp.data.offset[1]);
        oz.value = Number(resp.data.offset[2].toFixed?.(1) ?? resp.data.offset[2]);
      }
    } else {
      enabled.value = false;
      following.value = false;
    }
  } catch {
    enabled.value = false;
    following.value = false;
  }
}

function updateOffset() {
  if (!following.value) return;
  Bridge.callCEF('CoronaEditor', 'camera_lock_set', [true, ox.value, oy.value, oz.value, 0, 0, 0]);
}

function isGamePreviewInputLocked() {
  return Boolean(window.__coronaGamePreviewInputLocked);
}

// ── WASD ──
function onKeyDown(e) {
  if (!shouldHandleCameraFollowKey(e, {
    following: following.value,
    previewLocked: isGamePreviewInputLocked(),
  })) return;
  const k = e.key.toLowerCase();
  if (k === 'escape' && following.value) {
    e.preventDefault();
    e.stopImmediatePropagation();
    enabled.value = false;
    applyLock();
    return;
  }
  if (['w', 'a', 's', 'd'].includes(k) && following.value) {
    e.preventDefault();
    e.stopImmediatePropagation();
    Bridge.callCEF('CoronaEditor', 'object_key_down', [k]);
  }
}

function onKeyUp(e) {
  if (!shouldHandleCameraFollowKey(e, {
    following: following.value,
    previewLocked: isGamePreviewInputLocked(),
  })) return;
  const k = e.key.toLowerCase();
  if (['w', 'a', 's', 'd'].includes(k) && following.value) {
    e.preventDefault();
    e.stopImmediatePropagation();
    Bridge.callCEF('CoronaEditor', 'object_key_up', [k]);
  }
}

onMounted(() => {
  visible.value = true;
  window.addEventListener('mousemove', onMouseMove);
  window.addEventListener('mouseup', onMouseUpGlobal);
  document.addEventListener('keydown', onKeyDown, true);
  document.addEventListener('keyup', onKeyUp, true);
});

onUnmounted(() => {
  window.removeEventListener('mousemove', onMouseMove);
  window.removeEventListener('mouseup', onMouseUpGlobal);
  document.removeEventListener('keydown', onKeyDown, true);
  document.removeEventListener('keyup', onKeyUp, true);
});
</script>

<style scoped>
.camera-follow-root {
  position: fixed;
  z-index: 100000;
}
.camera-follow-dot {
  position: fixed;
  top: 12px;
  right: 12px;
  width: 24px;
  height: 24px;
  border-radius: 50%;
  background: #ec4899;
  cursor: grab;
  opacity: 0.85;
  border: 2px solid #fff;
  font-size: 14px;
  line-height: 24px;
  text-align: center;
  color: #fff;
  user-select: none;
  z-index: 100000;
  box-shadow: 0 0 6px #ec4899;
  animation: camPulse 1.5s ease-in-out infinite;
}
.camera-follow-dot.active {
  background: #4caf50;
  box-shadow: 0 0 6px #4caf50;
  animation: none;
}
@keyframes camPulse {
  0%, 100% { box-shadow: 0 0 6px #ec4899; }
  50% { box-shadow: 0 0 18px #ec4899, 0 0 28px #ec4899; }
}
.camera-follow-panel {
  position: fixed;
  background: #2d2d2d;
  border: 2px solid #ec4899;
  border-radius: 8px;
  padding: 12px;
  color: #e0e0e0;
  font-size: 12px;
  min-width: 220px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.6);
  z-index: 99999;
}
.camera-follow-title {
  font-weight: bold;
  margin-bottom: 8px;
  color: #ec4899;
}
.camera-follow-row {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 6px;
}
.camera-follow-row input[type='checkbox'] {
  accent-color: #ec4899;
}
.camera-follow-offsets {
  margin-top: 4px;
  font-size: 10px;
  color: #909090;
}
.camera-follow-offsets label {
  margin: 0 2px;
}
.camera-follow-offsets input {
  width: 50px;
  background: #1a1a1a;
  color: #e0e0e0;
  border: 1px solid #3c3c3c;
  border-radius: 3px;
  margin: 0 2px;
  padding: 1px 3px;
}
.camera-follow-apply {
  margin-top: 8px;
  width: 100%;
  padding: 4px;
  background: #ec4899;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
  font-size: 11px;
}
</style>
