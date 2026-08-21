<template>
  <section
    class="story-map-overlay"
    aria-label="剧情模式地图"
    @pointerdown.stop
    @mousedown.stop
    @click.stop
    @wheel.stop
  >
    <div class="story-map-panel" role="dialog" aria-modal="true" aria-labelledby="story-map-title">
      <header class="story-map-panel__header">
        <div>
          <p>WORLD NAVIGATION</p>
          <h1 id="story-map-title">{{ sceneName || '区域地图' }}</h1>
        </div>
        <div class="story-map-panel__status">
          <span :class="{ 'story-map-panel__status-dot--warning': !boundsReady }"></span>
          {{ boundsReady ? '引擎场景边界' : '临时地图边界' }}
        </div>
        <button
          ref="closeButtonRef"
          type="button"
          class="story-map-panel__close"
          aria-label="关闭地图"
          @click="$emit('close')"
        >
          ×
        </button>
      </header>

      <div class="story-map-panel__body">
        <div class="story-map-panel__map">
          <StoryMapCanvas
            v-if="bounds"
            :bounds="bounds"
            :markers="markers"
            :player-position="playerState.position"
            :player-forward="playerState.forward"
            aria-label="完整场景地图"
          />
          <div v-else class="story-map-panel__unavailable">
            <div v-if="loading" class="story-map-panel__spinner" aria-hidden="true"></div>
            <h2>{{ loading ? '正在同步地图' : '地图暂不可用' }}</h2>
            <p>{{ errorMessage || '当前场景还没有可用的边界或对象坐标。' }}</p>
            <button type="button" @click="$emit('refresh')">重新同步</button>
          </div>
          <div v-if="playerOutside" class="story-map-panel__outside">玩家已离开场景边界</div>
        </div>

        <aside class="story-map-panel__legend">
          <div class="story-map-panel__player-position">
            <span>玩家坐标</span>
            <strong>X {{ coordinate(playerState.position?.[0]) }}</strong>
            <strong>Y {{ coordinate(playerState.position?.[1]) }}</strong>
            <strong>Z {{ coordinate(playerState.position?.[2]) }}</strong>
          </div>
          <div class="story-map-panel__legend-list">
            <h2>地图图例</h2>
            <div><i class="story-map-panel__legend-dot story-map-panel__legend-dot--player"></i>玩家</div>
            <div><i class="story-map-panel__legend-dot story-map-panel__legend-dot--quest"></i>任务目标</div>
            <div><i class="story-map-panel__legend-dot story-map-panel__legend-dot--item"></i>可收集对象</div>
            <div><i class="story-map-panel__legend-dot story-map-panel__legend-dot--danger"></i>危险目标</div>
            <div><i class="story-map-panel__legend-dot"></i>场景对象</div>
          </div>
          <div class="story-map-panel__summary">
            <span>已标记对象</span>
            <strong>{{ markers.length }}</strong>
          </div>
          <button type="button" class="story-map-panel__refresh" :disabled="loading" @click="$emit('refresh')">
            {{ loading ? '同步中…' : '刷新地图' }}
          </button>
          <p class="story-map-panel__hint">按 M 或 Esc 返回游戏</p>
        </aside>
      </div>
    </div>
  </section>
</template>

<script setup>
import { computed, nextTick, onMounted, ref } from 'vue';

import StoryMapCanvas from '@/components/story/StoryMapCanvas.vue';
import { projectStoryWorldToMap } from '@/utils/storyMap.js';

const props = defineProps({
  sceneName: { type: String, default: '' },
  bounds: { type: Object, default: null },
  boundsReady: { type: Boolean, default: false },
  markers: { type: Array, default: () => [] },
  playerState: { type: Object, required: true },
  loading: { type: Boolean, default: false },
  errorMessage: { type: String, default: '' },
});

defineEmits(['close', 'refresh']);

const closeButtonRef = ref(null);
const playerOutside = computed(
  () => projectStoryWorldToMap(props.playerState.position, props.bounds)?.outOfBounds ?? false
);
const coordinate = (value) => (Number.isFinite(Number(value)) ? Number(value).toFixed(2) : '0.00');

onMounted(async () => {
  await nextTick();
  closeButtonRef.value?.focus?.();
});
</script>

<style scoped>
.story-map-overlay {
  position: absolute;
  inset: 0;
  z-index: 20;
  display: grid;
  place-items: center;
  padding: 28px;
  background: rgba(1, 3, 2, 0.76);
  backdrop-filter: blur(8px);
  pointer-events: auto;
}

.story-map-panel {
  display: flex;
  flex-direction: column;
  width: min(1180px, calc(100vw - 56px));
  height: min(820px, calc(100vh - 56px));
  border: 1px solid rgba(216, 184, 108, 0.4);
  border-radius: 16px;
  overflow: hidden;
  background: linear-gradient(145deg, rgba(22, 25, 21, 0.98), rgba(7, 9, 8, 0.99));
  box-shadow: 0 30px 96px rgba(0, 0, 0, 0.72);
}

.story-map-panel__header {
  display: flex;
  align-items: center;
  gap: 20px;
  padding: 19px 24px;
  border-bottom: 1px solid rgba(216, 184, 108, 0.16);
}

.story-map-panel__header > div:first-child {
  flex: 1;
}

.story-map-panel__header p {
  margin: 0 0 3px;
  color: #887a5b;
  font-size: 10px;
  letter-spacing: 0.25em;
}

.story-map-panel__header h1 {
  margin: 0;
  color: #f1e6ca;
  font-size: 24px;
  letter-spacing: 0.06em;
}

.story-map-panel__status {
  display: flex;
  align-items: center;
  gap: 7px;
  color: #999180;
  font-size: 11px;
}

.story-map-panel__status span {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: #72bd91;
  box-shadow: 0 0 8px currentColor;
}

.story-map-panel__status .story-map-panel__status-dot--warning {
  background: #d3a25f;
}

.story-map-panel__close {
  width: 38px;
  height: 38px;
  border: 1px solid rgba(216, 184, 108, 0.24);
  border-radius: 9px;
  background: rgba(255, 255, 255, 0.035);
  color: #cabfaa;
  font-size: 24px;
  cursor: pointer;
}

.story-map-panel__close:hover,
.story-map-panel__close:focus-visible {
  border-color: rgba(216, 184, 108, 0.65);
  color: #fff0ca;
  outline: none;
}

.story-map-panel__body {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 250px;
  min-height: 0;
  flex: 1;
}

.story-map-panel__map {
  position: relative;
  min-width: 0;
  min-height: 0;
  padding: 24px;
  background: radial-gradient(circle at center, rgba(216, 184, 108, 0.06), transparent 54%);
}

.story-map-panel__unavailable {
  display: grid;
  height: 100%;
  place-content: center;
  justify-items: center;
  border: 1px solid rgba(216, 184, 108, 0.18);
  border-radius: 12px;
  background: rgba(3, 5, 4, 0.7);
  color: #8f8879;
  text-align: center;
}

.story-map-panel__unavailable h2 {
  margin: 12px 0 5px;
  color: #d8ccb1;
}

.story-map-panel__unavailable p {
  margin: 0 0 20px;
}

.story-map-panel__unavailable button,
.story-map-panel__refresh {
  min-height: 38px;
  border: 1px solid rgba(216, 184, 108, 0.3);
  border-radius: 8px;
  background: rgba(216, 184, 108, 0.08);
  color: #d9c9a6;
  cursor: pointer;
}

.story-map-panel__spinner {
  width: 32px;
  height: 32px;
  border: 3px solid rgba(216, 184, 108, 0.15);
  border-top-color: #d8b86c;
  border-radius: 50%;
  animation: map-spin 0.9s linear infinite;
}

.story-map-panel__outside {
  position: absolute;
  left: 50%;
  bottom: 36px;
  transform: translateX(-50%);
  padding: 7px 12px;
  border: 1px solid rgba(224, 109, 97, 0.45);
  border-radius: 999px;
  background: rgba(55, 13, 10, 0.84);
  color: #f0a59b;
  font-size: 11px;
}

.story-map-panel__legend {
  display: flex;
  flex-direction: column;
  gap: 22px;
  padding: 24px;
  border-left: 1px solid rgba(216, 184, 108, 0.14);
  background: rgba(0, 0, 0, 0.17);
}

.story-map-panel__player-position {
  display: grid;
  gap: 5px;
  padding: 16px;
  border: 1px solid rgba(216, 184, 108, 0.14);
  border-radius: 10px;
  background: rgba(216, 184, 108, 0.045);
}

.story-map-panel__player-position span,
.story-map-panel__summary span {
  color: #8e8676;
  font-size: 10px;
  letter-spacing: 0.1em;
}

.story-map-panel__player-position strong {
  color: #d9cfb9;
  font-size: 12px;
  font-variant-numeric: tabular-nums;
}

.story-map-panel__legend-list h2 {
  margin: 0 0 12px;
  color: #d8ccb1;
  font-size: 14px;
}

.story-map-panel__legend-list div {
  display: flex;
  align-items: center;
  gap: 9px;
  margin: 9px 0;
  color: #999181;
  font-size: 11px;
}

.story-map-panel__legend-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #a9a59b;
}

.story-map-panel__legend-dot--player { background: #f5dd9f; }
.story-map-panel__legend-dot--quest { background: #e6c765; }
.story-map-panel__legend-dot--item { background: #6fc59b; }
.story-map-panel__legend-dot--danger { background: #e06d61; }

.story-map-panel__summary {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  padding-top: 16px;
  border-top: 1px solid rgba(216, 184, 108, 0.12);
}

.story-map-panel__summary strong {
  color: #d8b86c;
  font-size: 24px;
}

.story-map-panel__refresh {
  width: 100%;
  margin-top: auto;
}

.story-map-panel__refresh:disabled {
  cursor: wait;
  opacity: 0.55;
}

.story-map-panel__hint {
  margin: -10px 0 0;
  color: #716d64;
  font-size: 10px;
  text-align: center;
}

@keyframes map-spin {
  to { transform: rotate(360deg); }
}

@media (max-width: 760px) {
  .story-map-overlay { padding: 14px; }
  .story-map-panel {
    width: calc(100vw - 28px);
    height: calc(100vh - 28px);
  }
  .story-map-panel__body { grid-template-columns: 1fr; }
  .story-map-panel__legend { display: none; }
}
</style>
