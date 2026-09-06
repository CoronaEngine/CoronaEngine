<!-- 剧情模式地图 UI：负责呈现灰盒地图、出生点、玩家位置和地点图例。 -->
<template>
  <section class="overlay" @pointerdown.stop @click.stop>
    <div class="panel" role="dialog" aria-modal="true" aria-labelledby="map-title">
      <header class="panel-header">
        <div class="title-group">
          <h2 id="map-title">地图</h2>
          <p>主世界</p>
        </div>
        <div class="header-actions">
          <span class="coordinate-readout">
            X {{ coordinate(player.x) }} · Z {{ coordinate(player.z) }}
          </span>
          <button class="icon-button" type="button" aria-label="关闭地图" @click="closePanel">
            ×
          </button>
        </div>
      </header>

      <div class="map-layout">
        <section class="map-section" aria-label="灰盒地图">
          <div class="map">
            <div class="map-grid" aria-hidden="true"></div>
            <div class="map-contour contour-one" aria-hidden="true"></div>
            <div class="map-contour contour-two" aria-hidden="true"></div>
            <div class="map-road road-one" aria-hidden="true"></div>
            <div class="map-road road-two" aria-hidden="true"></div>
            <span class="location location-gate">
              <i></i>
              <b>遗迹入口</b>
            </span>
            <span class="location location-camp">
              <i></i>
              <b>营地</b>
            </span>
            <span class="location location-boss">
              <i></i>
              <b>Boss 区域</b>
            </span>
            <span class="marker spawn" title="出生点">
              <i></i>
              <b>出生点</b>
            </span>
            <span class="marker player" :style="playerStyle" title="玩家当前位置">
              <i></i>
              <b>玩家</b>
            </span>
            <div class="compass" aria-hidden="true">
              <span>N</span>
              <span>E</span>
              <span>S</span>
              <span>W</span>
            </div>
          </div>
        </section>

        <aside class="map-info" aria-label="地图信息">
          <div class="info-card current-card">
            <span class="info-title">当前位置</span>
            <strong>主世界</strong>
            <small>灰盒探索区域</small>
          </div>

          <div class="legend-card">
            <span class="info-title">地图图例</span>
            <div class="legend-row">
              <span class="legend-marker legend-player"></span>
              <span>玩家当前位置</span>
            </div>
            <div class="legend-row">
              <span class="legend-marker legend-spawn"></span>
              <span>出生点</span>
            </div>
            <div class="legend-row">
              <span class="legend-marker legend-location"></span>
              <span>已发现地点</span>
            </div>
          </div>

          <div class="discover-card">
            <div class="discover-heading">
              <span class="info-title">发现进度</span>
              <strong>03 / 08</strong>
            </div>
            <div class="progress-track"><i></i></div>
          </div>
        </aside>
      </div>
    </div>
  </section>
</template>

<script setup>
import { computed } from 'vue';

const props = defineProps({
  player: {
    type: Object,
    default: () => ({ x: 50, z: 50 }),
  },
});

const emit = defineEmits(['close']);

const playerStyle = computed(() => ({
  left: `${clamp(props.player.x)}%`,
  top: `${clamp(props.player.z)}%`,
}));

function clamp(value) {
  return Math.max(5, Math.min(95, Number(value) || 50));
}

function coordinate(value) {
  return ((Number(value) || 50) - 50).toFixed(1);
}

function closePanel() {
  emit('close');
}
</script>

<style scoped>
.overlay {
  position: absolute;
  z-index: 10;
  inset: 0;
  display: grid;
  place-items: center;
  padding: 28px;
  background: rgb(3 9 17 / 68%);
  animation: overlay-in 180ms ease-out;
}

.panel {
  width: min(1020px, 100%);
  max-height: min(760px, calc(100vh - 56px));
  overflow: hidden;
  border: 1px solid var(--game-border-strong, #456173);
  border-radius: 14px;
  background: var(--game-panel, #101d2a);
  color: var(--game-text, #e5ebee);
  box-shadow: 0 18px 42px rgb(0 0 0 / 34%);
  animation: panel-in 180ms ease-out;
}

.panel-header,
.header-actions,
.map-info,
.legend-row,
.discover-heading {
  display: flex;
  align-items: center;
}

.panel-header,
.discover-heading {
  justify-content: space-between;
}

.panel-header {
  padding: 22px 26px;
  border-bottom: 1px solid var(--game-border, #304656);
}

.title-group {
  display: flex;
  flex-direction: column;
  gap: 5px;
}

.title-group h2 {
  margin: 0;
  font-size: 26px;
  letter-spacing: 0.04em;
}

.title-group p,
.coordinate-readout,
.map-info small {
  margin: 0;
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
}

.header-actions {
  gap: 16px;
}

.icon-button {
  display: grid;
  width: 36px;
  height: 36px;
  place-items: center;
  border: 1px solid var(--game-border-strong, #456173);
  border-radius: 7px;
  background: #162735;
  color: var(--game-text, #e5ebee);
  cursor: pointer;
  font-size: 22px;
  line-height: 1;
  transition: 160ms ease;
}

.icon-button:hover,
.icon-button:focus-visible {
  border-color: var(--game-cyan, #75cdbd);
  background: #1b3440;
  outline: none;
}

.map-layout {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 230px;
  gap: 18px;
  padding: 22px 26px 26px;
}

.map-section {
  min-width: 0;
}

.map {
  position: relative;
  height: 470px;
  overflow: hidden;
  border: 1px solid var(--game-border-strong, #456173);
  border-radius: 8px;
  background: #0d202c;
}

.map-grid {
  position: absolute;
  inset: 0;
  opacity: 0.55;
  background-image:
    linear-gradient(rgb(117 205 189 / 10%) 1px, transparent 1px),
    linear-gradient(90deg, rgb(117 205 189 / 10%) 1px, transparent 1px);
  background-size: 36px 36px;
}

.map-contour,
.map-road {
  position: absolute;
  pointer-events: none;
}

.map-contour {
  width: 62%;
  height: 54%;
  border: 1px solid rgb(117 205 189 / 18%);
  border-radius: 48% 52% 42% 58%;
  transform: rotate(-14deg);
}

.contour-one {
  top: 13%;
  left: 14%;
}

.contour-two {
  right: -4%;
  bottom: 10%;
  transform: rotate(18deg) scale(0.8);
}

.map-road {
  height: 1px;
  background: rgb(198 161 91 / 55%);
  transform-origin: left center;
}

.road-one {
  top: 58%;
  left: 8%;
  width: 72%;
  transform: rotate(-19deg);
}

.road-two {
  top: 28%;
  left: 38%;
  width: 47%;
  transform: rotate(34deg);
}

.location,
.marker {
  position: absolute;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 5px;
  transform: translate(-50%, -50%);
  white-space: nowrap;
}

.location {
  color: var(--game-muted, #8f9da6);
  font-size: 10px;
}

.location i,
.marker i {
  display: block;
  width: 9px;
  height: 9px;
  border-radius: 50%;
}

.location i {
  border: 1px solid rgb(154 190 206 / 55%);
  background: rgb(154 190 206 / 35%);
}

.location-gate {
  top: 27%;
  left: 22%;
}

.location-camp {
  top: 72%;
  left: 26%;
}

.location-boss {
  top: 30%;
  left: 76%;
}

.location-boss i {
  border: 1px solid #c15d70;
  background: #8e3f50;
}

.marker {
  z-index: 2;
  font-size: 10px;
  font-weight: 700;
}

.marker b {
  padding: 3px 6px;
  border-radius: 4px;
  background: #122735;
}

.spawn {
  top: 50%;
  left: 50%;
  color: #f0d99f;
}

.spawn i {
  width: 12px;
  height: 12px;
  border: 2px solid #f0d99f;
  background: var(--game-gold, #c6a15b);
}

.player {
  color: var(--game-cyan, #75cdbd);
  transition:
    left 120ms linear,
    top 120ms linear;
}

.player i {
  width: 14px;
  height: 14px;
  border: 2px solid #d8fff7;
  background: var(--game-cyan, #75cdbd);
}

.compass {
  position: absolute;
  top: 14px;
  right: 16px;
  left: 16px;
  display: flex;
  justify-content: space-between;
  color: rgb(242 246 248 / 60%);
  font-size: 10px;
  font-weight: 800;
  pointer-events: none;
}

.compass span:first-child {
  color: var(--game-gold, #c6a15b);
}

.map-info {
  flex-direction: column;
  align-items: stretch;
  gap: 12px;
}

.info-card,
.legend-card,
.discover-card {
  padding: 14px;
  border: 1px solid var(--game-border, #304656);
  border-radius: 8px;
  background: #142735;
}

.current-card {
  display: flex;
  flex-direction: column;
  gap: 8px;
  border-color: #3c756e;
  background: #17333b;
}

.current-card strong {
  font-size: 14px;
  line-height: 1.4;
}

.info-title {
  color: var(--game-text, #e5ebee);
  font-size: 12px;
  font-weight: 600;
}

.legend-card,
.discover-card {
  display: flex;
  flex-direction: column;
  gap: 11px;
}

.legend-row {
  gap: 9px;
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
}

.legend-marker {
  display: inline-block;
  width: 10px;
  height: 10px;
  flex: 0 0 auto;
  border-radius: 50%;
}

.legend-player {
  background: var(--game-cyan, #75cdbd);
}

.legend-spawn {
  background: var(--game-gold, #c6a15b);
}

.legend-location {
  border: 1px solid rgb(154 190 206 / 60%);
  background: rgb(154 190 206 / 30%);
}

.discover-heading strong {
  color: var(--game-cyan, #75cdbd);
  font-size: 12px;
}

.progress-track {
  height: 5px;
  overflow: hidden;
  border-radius: 99px;
  background: #263843;
}

.progress-track i {
  display: block;
  width: 38%;
  height: 100%;
  border-radius: inherit;
  background: var(--game-cyan, #75cdbd);
}

@keyframes overlay-in {
  from {
    opacity: 0;
  }

  to {
    opacity: 1;
  }
}

@keyframes panel-in {
  from {
    opacity: 0;
    transform: translateY(10px);
  }

  to {
    opacity: 1;
    transform: translateY(0);
  }
}

@media (max-width: 820px) {
  .map-layout {
    grid-template-columns: 1fr;
  }

  .map-info {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
  }
}

@media (max-width: 620px) {
  .overlay {
    padding: 12px;
  }

  .panel {
    max-height: calc(100vh - 24px);
    border-radius: 12px;
  }

  .panel-header {
    padding: 18px;
  }

  .coordinate-readout {
    display: none;
  }

  .map-layout {
    display: block;
    max-height: calc(100vh - 110px);
    overflow-y: auto;
    padding: 14px;
  }

  .map {
    height: 320px;
  }

  .map-info {
    display: grid;
    grid-template-columns: 1fr;
    margin-top: 14px;
  }
}
</style>
