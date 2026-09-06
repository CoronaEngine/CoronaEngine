<!-- 剧情模式 HUD：负责呈现准星、交互提示、玩家状态、资源数量和调试信息。 -->
<template>
  <div class="hud">
    <div class="crosshair" :class="{ active: Boolean(hint) }" aria-hidden="true">
      <span class="crosshair-line crosshair-line-top"></span>
      <span class="crosshair-line crosshair-line-right"></span>
      <span class="crosshair-line crosshair-line-bottom"></span>
      <span class="crosshair-line crosshair-line-left"></span>
      <span class="crosshair-dot"></span>
    </div>

    <div v-if="hint" class="interaction-hint" role="status">
      <span class="interaction-key">F</span>
      <span>{{ hint }}</span>
    </div>

    <div class="hud-bottom">
      <section class="player-status" aria-label="玩家状态">
        <div class="avatar-mark" aria-hidden="true">P</div>
        <div class="status-content">
          <div class="status-name">
            <strong>探索者</strong>
          </div>
          <div class="meter-row">
            <span class="meter-label">生命</span>
            <div class="meter health-meter" aria-label="生命值 100"><i></i></div>
            <span class="meter-value">100</span>
          </div>
          <div class="meter-row">
            <span class="meter-label">体力</span>
            <div class="meter stamina-meter" aria-label="体力值 100"><i></i></div>
            <span class="meter-value">100</span>
          </div>
        </div>
      </section>

      <section class="resource-strip" aria-label="资源数量">
        <div class="resource-item">
          <span class="resource-icon material-icon" aria-hidden="true">◆</span>
          <span>
            <small>木材</small>
            <strong>12</strong>
          </span>
        </div>
        <div class="resource-item">
          <span class="resource-icon fragment-icon" aria-hidden="true">✦</span>
          <span>
            <small>世界碎片</small>
            <strong>1</strong>
          </span>
        </div>
      </section>
    </div>

    <div v-if="debugVisible" class="debug-panel">
      <div class="debug-title">灰盒调试 · 运行状态</div>
      <div>位置：{{ format(debug.x) }}, {{ format(debug.y) }}, {{ format(debug.z) }}</div>
      <div>视角：yaw {{ format(debug.yaw) }} · pitch {{ format(debug.pitch) }}</div>
      <div>
        移动：{{ debug.move ? 'WASD 输入中' : '静止' }} · 地面：{{ debug.grounded ? '是' : '否' }}
      </div>
      <div>
        鼠标：{{ debug.pointerLocked ? '已锁定' : debug.mouseActive ? '普通模式' : '未激活' }}
      </div>
      <div>世界：{{ debug.worldType }} · Boss：{{ debug.bossHealth }}</div>
      <div>目标：{{ debug.target || '无' }}</div>
    </div>
  </div>
</template>

<script setup>
defineProps({
  hint: { type: String, default: '' },
  debugVisible: { type: Boolean, default: false },
  debug: {
    type: Object,
    default: () => ({
      x: 0,
      y: 1.7,
      z: 0,
      yaw: 0,
      pitch: 0,
      grounded: true,
      pointerLocked: false,
      mouseActive: false,
      move: false,
      worldType: 'main',
      bossHealth: 100,
      target: '',
    }),
  },
});

function format(value) {
  return Number(value || 0).toFixed(2);
}
</script>

<style scoped>
.hud {
  position: absolute;
  z-index: 2;
  inset: 0;
  pointer-events: none;
  color: var(--game-text, #e5ebee);
  font-family: var(--game-font, 'Segoe UI', 'Microsoft YaHei', sans-serif);
}

.hud-bottom {
  position: absolute;
  right: 28px;
  bottom: 24px;
  left: 28px;
  display: grid;
  grid-template-columns: 1fr auto 1fr;
  align-items: end;
  gap: 18px;
}

.crosshair {
  position: absolute;
  top: 50%;
  left: 50%;
  width: 22px;
  height: 22px;
  transform: translate(-50%, -50%);
  transition: transform 160ms ease;
}

.crosshair.active {
  transform: translate(-50%, -50%) scale(1.15);
}

.crosshair-line,
.crosshair-dot {
  position: absolute;
  display: block;
}

.crosshair-line {
  width: 2px;
  height: 7px;
  border-radius: 2px;
  background: #e5ebee;
}

.crosshair-line-top {
  top: 0;
  left: 10px;
}

.crosshair-line-right {
  top: 8px;
  right: 0;
  width: 7px;
  height: 2px;
}

.crosshair-line-bottom {
  bottom: 0;
  left: 10px;
}

.crosshair-line-left {
  top: 8px;
  left: 0;
  width: 7px;
  height: 2px;
}

.crosshair-dot {
  top: 9px;
  left: 9px;
  width: 4px;
  height: 4px;
  border-radius: 50%;
  background: var(--game-cyan, #75cdbd);
}

.interaction-hint {
  position: absolute;
  top: calc(50% + 34px);
  left: 50%;
  display: flex;
  gap: 10px;
  align-items: center;
  padding: 9px 13px 9px 9px;
  border: 1px solid var(--game-gold, #c6a15b);
  border-radius: 6px;
  background: var(--game-panel-deep, #0b1723);
  color: #fff2ca;
  box-shadow: 0 8px 18px rgb(0 0 0 / 22%);
  transform: translateX(-50%);
  animation: hint-in 180ms ease-out;
}

.interaction-key {
  display: inline-flex;
  width: 24px;
  height: 24px;
  align-items: center;
  justify-content: center;
  border: 1px solid var(--game-gold, #c6a15b);
  border-radius: 4px;
  background: #263443;
  color: var(--game-gold, #c6a15b);
  font-size: 10px;
  font-weight: 800;
  letter-spacing: 0.08em;
}

.player-status,
.resource-strip,
.debug-panel {
  border: 1px solid var(--game-border, #304656);
  background: var(--game-panel, #101d2a);
  box-shadow: 0 10px 24px rgb(0 0 0 / 24%);
}

.player-status {
  grid-column: 1;
  display: flex;
  min-width: 244px;
  align-items: center;
  justify-self: start;
  gap: 11px;
  padding: 11px 14px;
  border-radius: 8px;
}

.avatar-mark {
  display: grid;
  width: 38px;
  height: 38px;
  flex: 0 0 auto;
  place-items: center;
  border: 1px solid var(--game-cyan, #75cdbd);
  border-radius: 50%;
  background: #1a3038;
  color: var(--game-cyan, #75cdbd);
  font-size: 16px;
  font-weight: 800;
}

.status-content {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 6px;
}

.status-name strong {
  font-size: 12px;
}

.meter-row {
  display: flex;
  align-items: center;
  gap: 6px;
}

.meter-label {
  width: 26px;
  color: var(--game-muted, #8f9da6);
  font-size: 10px;
}

.meter {
  height: 5px;
  flex: 1;
  overflow: hidden;
  border-radius: 99px;
  background: #26343f;
}

.meter i {
  display: block;
  width: 100%;
  height: 100%;
  border-radius: inherit;
}

.health-meter i {
  background: #d56f7c;
}

.stamina-meter i {
  width: 82%;
  background: var(--game-cyan, #75cdbd);
}

.meter-value {
  width: 24px;
  color: var(--game-muted, #8f9da6);
  font-size: 10px;
  text-align: right;
}

.resource-strip {
  grid-column: 2;
  display: flex;
  gap: 8px;
  padding: 8px;
  border-radius: 8px;
}

.resource-item {
  display: flex;
  gap: 8px;
  align-items: center;
  min-width: 100px;
  padding: 5px 8px;
  border-radius: 5px;
  background: #182a38;
}

.resource-item > span:last-child {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.resource-item small {
  color: var(--game-muted, #8f9da6);
  font-size: 10px;
}

.resource-item strong {
  color: var(--game-text, #e5ebee);
  font-size: 12px;
}

.resource-icon {
  display: grid;
  width: 26px;
  height: 26px;
  place-items: center;
  border-radius: 5px;
  font-size: 14px;
}

.material-icon {
  background: #3d3327;
  color: #d1a56d;
}

.fragment-icon {
  background: #3a3428;
  color: var(--game-gold, #c6a15b);
}

.debug-panel {
  position: absolute;
  right: 28px;
  bottom: 98px;
  min-width: 240px;
  padding: 11px 13px;
  border-radius: 6px;
  color: #b8c8cf;
  font:
    11px/1.65 ui-monospace,
    SFMono-Regular,
    Consolas,
    monospace;
}

.debug-title {
  margin-bottom: 4px;
  color: var(--game-gold, #c6a15b);
  font-size: 10px;
  letter-spacing: 0.1em;
}

@keyframes hint-in {
  from {
    opacity: 0;
    transform: translate(-50%, 8px);
  }
  to {
    opacity: 1;
    transform: translate(-50%, 0);
  }
}

@media (max-width: 620px) {
  .hud-bottom {
    right: 14px;
    bottom: 14px;
    left: 14px;
    grid-template-columns: 1fr auto;
  }

  .player-status {
    min-width: 0;
  }

  .resource-strip {
    grid-column: 2;
  }
}
</style>
