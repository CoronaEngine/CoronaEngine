<template>
  <button type="button" class="story-mini-map" title="打开地图 (M)" @click="$emit('open')">
    <div class="story-mini-map__header">
      <span>区域地图</span>
      <kbd>M</kbd>
    </div>
    <div class="story-mini-map__canvas">
      <StoryMapCanvas
        v-if="bounds"
        :bounds="bounds"
        :markers="markers"
        :player-position="playerState.position"
        :player-forward="playerState.forward"
        compact
        aria-label="玩家周边小地图"
      />
      <div v-else class="story-mini-map__unavailable">地图同步中</div>
    </div>
    <div class="story-mini-map__coordinates">
      X {{ coordinate(playerState.position?.[0]) }} · Z {{ coordinate(playerState.position?.[2]) }}
    </div>
  </button>
</template>

<script setup>
import { computed } from 'vue';

import StoryMapCanvas from '@/components/story/StoryMapCanvas.vue';
import { createStoryLocalMapBounds } from '@/utils/storyMap.js';

const props = defineProps({
  markers: { type: Array, default: () => [] },
  playerState: { type: Object, required: true },
});

defineEmits(['open']);

const bounds = computed(() => createStoryLocalMapBounds(props.playerState.position, 40));
const coordinate = (value) => (Number.isFinite(Number(value)) ? Number(value).toFixed(1) : '0.0');
</script>

<style scoped>
.story-mini-map {
  width: 224px;
  padding: 9px;
  border: 1px solid rgba(216, 184, 108, 0.34);
  border-radius: 12px;
  background: linear-gradient(145deg, rgba(21, 22, 18, 0.88), rgba(5, 7, 6, 0.9));
  box-shadow: 0 12px 34px rgba(0, 0, 0, 0.42);
  color: #e8ddc2;
  cursor: pointer;
  backdrop-filter: blur(5px);
  transition: border-color 150ms ease, transform 150ms ease;
}

.story-mini-map:hover {
  border-color: rgba(216, 184, 108, 0.72);
  transform: translateY(-1px);
}

.story-mini-map:focus-visible {
  outline: 2px solid #ead08e;
  outline-offset: 3px;
}

.story-mini-map__header,
.story-mini-map__coordinates {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.story-mini-map__header {
  padding: 0 2px 7px;
  color: #d8ccb0;
  font-size: 12px;
  letter-spacing: 0.08em;
}

.story-mini-map kbd {
  min-width: 22px;
  padding: 2px 6px;
  border: 1px solid rgba(216, 184, 108, 0.35);
  border-radius: 5px;
  background: rgba(0, 0, 0, 0.28);
  color: #d8b86c;
  font: inherit;
  text-align: center;
}

.story-mini-map__canvas {
  height: 182px;
  overflow: hidden;
  border-radius: 7px;
}

.story-mini-map__unavailable {
  display: grid;
  height: 100%;
  place-items: center;
  border: 1px solid rgba(216, 184, 108, 0.18);
  border-radius: 7px;
  background: rgba(4, 6, 5, 0.85);
  color: #938a77;
  font-size: 12px;
}

.story-mini-map__coordinates {
  padding: 7px 2px 0;
  color: #8e887b;
  font-size: 10px;
  font-variant-numeric: tabular-nums;
}
</style>
