<!-- 剧情模式 HUD：只展示游戏提示、准星和可选灰盒调试信息。 -->
<template>
  <div class="hud">
    <div class="status">
      <span class="status-dot"></span>
      <strong>主世界 · 生存探索</strong>
      <small>灰盒测试场景</small>
    </div>

    <div class="objective">
      <span>当前目标</span>
      <strong>探索遗迹，寻找世界碎片</strong>
      <small>击败 Boss 后开启 UGC Demo 制作</small>
    </div>

    <div class="crosshair">＋</div>
    <div class="tips">WASD 移动 · 鼠标视角 · 空格跳跃 · 左键攻击 · F 交互 · B 背包 · M 地图</div>
    <div v-if="hint" class="hint">{{ hint }}</div>

    <div v-if="debugVisible" class="debug">
      <div>位置：{{ format(debug.x) }}, {{ format(debug.y) }}, {{ format(debug.z) }}</div>
      <div>视角：yaw {{ format(debug.yaw) }} · pitch {{ format(debug.pitch) }}</div>
      <div>移动：{{ debug.move ? 'WASD 输入中' : '静止' }} · 地面：{{ debug.grounded ? '是' : '否' }}</div>
      <div>鼠标：{{ debug.pointerLocked ? '已锁定' : '未锁定' }}</div>
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
      y: 0,
      z: 0,
      yaw: 0,
      pitch: 0,
      grounded: true,
      pointerLocked: false,
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
  inset: 0;
  pointer-events: none;
  color: #fff;
  text-shadow: 0 2px 5px #000;
}

.status {
  position: absolute;
  top: 20px;
  left: 24px;
  display: flex;
  align-items: center;
  gap: 9px;
  padding: 10px 14px;
  background: #10202dcc;
  border: 1px solid #7690a5;
  border-radius: 8px;
}

.status strong { font-size: 15px; }
.status small { color: #b7c6d0; font-size: 11px; }
.status-dot { width: 9px; height: 9px; border-radius: 50%; background: #65d8ad; box-shadow: 0 0 12px #65d8ad; }

.objective {
  position: absolute;
  top: 78px;
  left: 24px;
  display: flex;
  flex-direction: column;
  gap: 3px;
}

.objective span { color: #d8b86c; font-size: 11px; letter-spacing: .16em; }
.objective strong { font-size: 16px; }
.objective small { color: #d5dde4; font-size: 12px; }

.crosshair {
  position: absolute;
  top: 50%;
  left: 50%;
  color: #f5ead0;
  font-size: 24px;
  transform: translate(-50%, -50%);
}

.tips {
  position: absolute;
  bottom: 16px;
  left: 50%;
  padding: 8px 14px;
  background: #101820aa;
  border-radius: 999px;
  font-size: 13px;
  opacity: .9;
  transform: translateX(-50%);
}

.hint {
  position: absolute;
  top: 59%;
  left: 50%;
  padding: 9px 16px;
  background: #09131ddd;
  border: 1px solid #d8b86c;
  border-radius: 6px;
  color: #f5d887;
  transform: translateX(-50%);
}

.debug {
  position: absolute;
  right: 20px;
  bottom: 18px;
  padding: 9px 12px;
  background: #101820c9;
  border: 1px solid #71869b;
  border-radius: 6px;
  color: #c8d6df;
  font: 12px/1.6 monospace;
}
</style>
