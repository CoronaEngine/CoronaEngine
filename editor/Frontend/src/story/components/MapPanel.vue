<!-- 剧情模式地图 UI：使用灰盒网格展示世界、出生点和玩家位置。 -->
<template>
  <section class="panel" @pointerdown.stop>
    <header>
      <h2>地图</h2>
      <button type="button" @click="$emit('close')">×</button>
    </header>

    <div class="map">
      <span class="spawn" title="出生点"></span>
      <span
        class="player"
        :style="{ left: `${player.x}%`, top: `${player.z}%` }"
        title="玩家"
      ></span>
      <i>出生点</i>
      <b>玩家</b>
    </div>

    <p>当前世界：主世界 · 灰盒地图</p>
  </section>
</template>

<script setup>
defineProps({
  player: {
    type: Object,
    default: () => ({ x: 50, z: 50 }),
  },
});

defineEmits(['close']);
</script>

<style scoped>
.panel { position: absolute; z-index: 5; top: 50%; left: 50%; width: min(650px, 90vw); padding: 22px; background: #111d2aec; border: 1px solid #6f8496; border-radius: 12px; color: #fff; transform: translate(-50%, -50%); }
.panel header { display: flex; justify-content: space-between; }
.panel button { padding: 7px 10px; border: 1px solid #61788e; border-radius: 6px; background: #243448; color: inherit; cursor: pointer; }
.map { position: relative; height: 330px; margin-top: 16px; overflow: hidden; background-color: #253746; background-image: linear-gradient(#ffffff15 1px, transparent 1px), linear-gradient(90deg, #ffffff15 1px, transparent 1px); background-size: 33px 33px; }
.spawn, .player { position: absolute; width: 14px; height: 14px; border-radius: 50%; transform: translate(-50%, -50%); }
.spawn { top: 50%; left: 50%; background: #f5c85d; }
.player { background: #62d8ff; }
i, b { position: absolute; font-size: 12px; font-style: normal; }
.map i { top: calc(50% + 8px); left: calc(50% + 10px); }
.map b { top: 10px; right: 10px; }
</style>
