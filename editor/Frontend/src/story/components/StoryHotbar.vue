<!-- 剧情模式快捷栏：固定显示 7 个槽位，不负责键盘事件监听。 -->
<template>
  <section class="hotbar" aria-label="快捷栏" @pointerdown.stop>
    <button
      v-for="(slot, index) in normalizedSlots"
      :key="index"
      class="hotbar-slot"
      :class="{ selected: selectedIndex === index }"
      type="button"
      :aria-label="slot ? `${slot.name}，第 ${index + 1} 格` : `空槽位，第 ${index + 1} 格`"
      :aria-pressed="selectedIndex === index"
      @click="selectSlot(index)"
    >
      <span class="slot-number">{{ index + 1 }}</span>
      <span v-if="slot" class="slot-icon" aria-hidden="true">{{ itemIcon(slot) }}</span>
      <span v-if="slot" class="slot-quantity">{{ slot.quantity }}</span>
      <span v-if="slot" class="slot-name">{{ slot.name }}</span>
    </button>
  </section>
</template>

<script setup>
import { computed } from 'vue';

const props = defineProps({
  slots: {
    type: Array,
    default: () => [],
  },
  selectedIndex: {
    type: Number,
    default: 0,
  },
});

const emit = defineEmits(['select']);

const normalizedSlots = computed(() =>
  Array.from({ length: 7 }, (_, index) => props.slots[index] || null)
);

function itemIcon(item) {
  if (item.category === 'material') return '◆';
  if (item.id === 'world-orb-demo') return '●';
  if (item.id === 'world-fragment-demo') return '✦';
  return '◇';
}

function selectSlot(index) {
  emit('select', index);
}
</script>

<style scoped>
.hotbar {
  position: absolute;
  z-index: 3;
  bottom: 24px;
  left: 50%;
  display: grid;
  grid-template-columns: repeat(7, 64px);
  gap: 4px;
  padding: 5px;
  border: 1px solid var(--game-border, #304656);
  background: #0b1723;
  box-shadow: 0 10px 24px rgb(0 0 0 / 24%);
  transform: translateX(-50%);
}

.hotbar-slot {
  position: relative;
  display: grid;
  width: 64px;
  height: 64px;
  place-items: center;
  padding: 0;
  border: 1px solid #3a5060;
  background: #172936;
  color: var(--game-text, #e5ebee);
  cursor: pointer;
  font: inherit;
}

.hotbar-slot:hover,
.hotbar-slot:focus-visible {
  border-color: var(--game-border-strong, #456173);
  background: #1c3441;
  outline: none;
}

.hotbar-slot.selected {
  border-color: var(--game-gold, #c6a15b);
  background: #263422;
}

.slot-number {
  position: absolute;
  top: 3px;
  left: 5px;
  color: var(--game-muted, #8f9da6);
  font-size: 10px;
  line-height: 1;
}

.slot-icon {
  color: var(--game-gold, #c6a15b);
  font-size: 25px;
  line-height: 1;
}

.slot-quantity {
  position: absolute;
  right: 5px;
  bottom: 4px;
  color: var(--game-text, #e5ebee);
  font-size: 12px;
  font-weight: 700;
  line-height: 1;
  text-shadow: 1px 1px 0 #07131f;
}

.slot-name {
  position: absolute;
  right: 24px;
  bottom: 4px;
  left: 3px;
  overflow: hidden;
  color: var(--game-muted, #8f9da6);
  font-size: 8px;
  line-height: 1;
  text-overflow: ellipsis;
  white-space: nowrap;
}

@media (max-width: 620px) {
  .hotbar {
    bottom: 14px;
    grid-template-columns: repeat(7, minmax(36px, 1fr));
    width: calc(100% - 28px);
    gap: 3px;
  }

  .hotbar-slot {
    width: auto;
    height: 52px;
  }

  .slot-name {
    display: none;
  }
}
</style>
