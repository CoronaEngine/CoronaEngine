<!-- 剧情模式背包 UI：只负责展示物品和触发使用事件。 -->
<template>
  <section class="panel" @pointerdown.stop>
    <header>
      <h2>背包</h2>
      <button type="button" @click="$emit('close')">×</button>
    </header>

    <div class="items">
      <button
        v-for="item in items"
        :key="item.id"
        type="button"
        class="item"
        :class="{ active: selected?.id === item.id }"
        @click="selected = item"
      >
        <span>{{ item.name }}</span>
        <b>×{{ item.quantity }}</b>
      </button>
    </div>

    <article v-if="selected">
      <h3>{{ selected.name }}</h3>
      <p>{{ selected.description }}</p>
      <button
        v-if="selected.id === 'world-orb-demo'"
        type="button"
        @click="$emit('use-orb', selected)"
      >
        进入空白世界
      </button>
    </article>
  </section>
</template>

<script setup>
import { ref } from 'vue';

defineProps({
  items: {
    type: Array,
    default: () => [],
  },
});

defineEmits(['close', 'use-orb']);
const selected = ref(null);
</script>

<style scoped>
.panel { position: absolute; z-index: 5; top: 50%; left: 50%; width: min(560px, 90vw); padding: 22px; background: #111d2aec; border: 1px solid #6f8496; border-radius: 12px; color: #fff; transform: translate(-50%, -50%); }
header { display: flex; align-items: center; justify-content: space-between; }
button { padding: 7px 10px; border: 1px solid #61788e; border-radius: 6px; background: #243448; color: inherit; cursor: pointer; }
.items { display: grid; grid-template-columns: repeat(2, 1fr); gap: 8px; margin: 16px 0; }
.item { display: flex; justify-content: space-between; width: 100%; text-align: left; }
.active { border-color: #d8b86c; background: #51452a; }
article { padding-top: 12px; border-top: 1px solid #456; }
</style>
