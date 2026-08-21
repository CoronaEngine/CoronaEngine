<template>
  <svg class="story-map-canvas" viewBox="0 0 100 100" role="img" :aria-label="ariaLabel">
    <defs>
      <pattern :id="gridPatternId" width="10" height="10" patternUnits="userSpaceOnUse">
        <path d="M 10 0 L 0 0 0 10" fill="none" stroke="currentColor" stroke-width="0.25" />
      </pattern>
      <radialGradient :id="glowGradientId">
        <stop offset="0%" stop-color="#d8b86c" stop-opacity="0.22" />
        <stop offset="100%" stop-color="#d8b86c" stop-opacity="0" />
      </radialGradient>
    </defs>

    <rect width="100" height="100" rx="3" class="story-map-canvas__background" />
    <rect
      width="100"
      height="100"
      rx="3"
      :fill="`url(#${gridPatternId})`"
      class="story-map-canvas__grid"
    />
    <circle
      v-if="playerPoint"
      :cx="playerPoint.x"
      :cy="playerPoint.y"
      r="10"
      :fill="`url(#${glowGradientId})`"
    />

    <g class="story-map-canvas__markers">
      <circle
        v-for="marker in projectedMarkers"
        :key="marker.id"
        :cx="marker.x"
        :cy="marker.y"
        :r="compact ? 1.25 : 1.05"
        :class="`story-map-canvas__marker story-map-canvas__marker--${marker.kind}`"
      >
        <title>{{ marker.name }} · {{ marker.type }}</title>
      </circle>
    </g>

    <g
      v-if="playerPoint"
      class="story-map-canvas__player"
      :class="{ 'story-map-canvas__player--outside': playerPoint.outOfBounds }"
      :transform="`translate(${playerPoint.x} ${playerPoint.y}) rotate(${heading})`"
    >
      <path d="M 0 -5 L -3.6 4 L 0 2.6 L 3.6 4 Z" />
    </g>

    <text v-if="showNorth" x="50" y="7" text-anchor="middle" class="story-map-canvas__north">
      N
    </text>
  </svg>
</template>

<script setup>
import { computed, useId } from 'vue';

import { projectStoryWorldToMap, storyPlayerHeadingDegrees } from '@/utils/storyMap.js';

const props = defineProps({
  bounds: { type: Object, default: null },
  markers: { type: Array, default: () => [] },
  playerPosition: { type: Array, default: () => [0, 0, 0] },
  playerForward: { type: Array, default: () => [0, 0, 1] },
  compact: { type: Boolean, default: false },
  clampPlayer: { type: Boolean, default: true },
  showNorth: { type: Boolean, default: true },
  ariaLabel: { type: String, default: '剧情地图' },
});

const id = useId().replace(/:/g, '');
const gridPatternId = `story-map-grid-${id}`;
const glowGradientId = `story-map-glow-${id}`;

const projectedMarkers = computed(() =>
  props.markers
    .map((marker) => {
      const point = projectStoryWorldToMap(marker.position, props.bounds);
      if (!point || point.outOfBounds) return null;
      return { ...marker, ...point };
    })
    .filter(Boolean)
);

const playerPoint = computed(() =>
  projectStoryWorldToMap(props.playerPosition, props.bounds, { clamp: props.clampPlayer })
);
const heading = computed(() => storyPlayerHeadingDegrees(props.playerForward));
</script>

<style scoped>
.story-map-canvas {
  display: block;
  width: 100%;
  height: 100%;
  color: rgba(216, 184, 108, 0.2);
}

.story-map-canvas__background {
  fill: rgba(7, 10, 9, 0.92);
  stroke: rgba(216, 184, 108, 0.35);
  stroke-width: 0.8;
}

.story-map-canvas__grid {
  opacity: 0.72;
}

.story-map-canvas__marker {
  fill: #a9a59b;
  stroke: rgba(0, 0, 0, 0.65);
  stroke-width: 0.45;
}

.story-map-canvas__marker--danger {
  fill: #e06d61;
}

.story-map-canvas__marker--quest {
  fill: #e6c765;
}

.story-map-canvas__marker--item {
  fill: #6fc59b;
}

.story-map-canvas__marker--light {
  fill: #eee3aa;
}

.story-map-canvas__marker--water {
  fill: #4aa2ad;
}
.story-map-canvas__marker--building {
  fill: #d8c39b;
}
.story-map-canvas__marker--landmark {
  fill: #d98b4d;
}
.story-map-canvas__marker--vegetation {
  fill: #5f9c5b;
}
.story-map-canvas__marker--terrain {
  fill: #7b765d;
  opacity: 0.72;
}

.story-map-canvas__player path {
  fill: #f5dd9f;
  stroke: #5b4414;
  stroke-width: 0.75;
  filter: drop-shadow(0 0 2px rgba(216, 184, 108, 0.95));
}

.story-map-canvas__player--outside path {
  fill: #e67967;
  stroke: #6b1710;
}

.story-map-canvas__north {
  fill: #d8b86c;
  font-size: 5px;
  font-weight: 700;
  letter-spacing: 0.08em;
}
</style>
