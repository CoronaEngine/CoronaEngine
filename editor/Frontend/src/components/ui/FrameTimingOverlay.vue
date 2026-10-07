<template>
  <div v-if="enabled && cameraId" class="frame-timing-overlay" aria-label="当前相机每帧渲染耗时与总帧间隔">
    <span>渲染</span> <b>{{ formatFrameTiming(sample) }}</b>
    <span aria-label="总帧间隔，包含等待和限帧">总帧</span>
    <b>{{ formatFrameTiming(sample, 'frame_ms') }}</b>
  </div>
</template>

<script setup>
import { ref, watch } from 'vue';
import { editorApi } from '@/api/editorApi.js';
import { formatFrameTiming, startFrameTimingPolling } from '@/utils/frameTiming.js';

const props = defineProps({
  sceneId: { type: String, default: '' },
  cameraId: { type: String, default: '' },
  enabled: { type: Boolean, default: false },
  renderMode: { type: String, default: '' },
});
const sample = ref(null);
watch(() => [props.sceneId, props.cameraId, props.enabled, props.renderMode],
  ([sceneId, cameraId, enabled], _, onCleanup) => {
    sample.value = null;
    if (!enabled || !cameraId) return;
    const stop = startFrameTimingPolling({
      read: () => editorApi.sceneTools.getFrameTiming(sceneId, cameraId),
      onSample: value => { sample.value = value; },
    });
    onCleanup(stop);
  }, { immediate: true });
</script>

<style scoped>
.frame-timing-overlay {
  position: absolute;
  right: 12px;
  bottom: 12px;
  z-index: 3;
  display: grid;
  grid-template-columns: auto auto;
  gap: 2px 8px;
  align-items: baseline;
  padding: 5px 9px;
  border: 1px solid rgba(255, 255, 255, 0.12);
  border-radius: 5px;
  background: rgba(15, 18, 23, 0.72);
  color: #e7edf4;
  font-size: 12px;
  line-height: 18px;
  font-variant-numeric: tabular-nums;
  pointer-events: none;
  user-select: none;
}
.frame-timing-overlay span { color: #b8c3d0; }
.frame-timing-overlay b { font-family: ui-monospace, Consolas, monospace; font-weight: 500; text-align: right; }
</style>
