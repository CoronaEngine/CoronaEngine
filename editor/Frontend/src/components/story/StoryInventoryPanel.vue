<template>
  <section
    class="story-panel-overlay"
    aria-label="剧情模式背包"
    @pointerdown.stop
    @mousedown.stop
    @click.stop
    @wheel.stop
  >
    <div class="story-inventory" role="dialog" aria-modal="true" aria-labelledby="inventory-title">
      <header class="story-inventory__header">
        <div>
          <p>PLAYER INVENTORY</p>
          <h1 id="inventory-title">背包</h1>
        </div>
        <div class="story-inventory__capacity">
          <strong>{{ inventory.occupiedSlots }}</strong>/{{ inventory.slots.length }} 格
        </div>
        <button ref="closeButtonRef" type="button" class="story-icon-button" @click="$emit('close')">
          ×
        </button>
      </header>

      <div class="story-inventory__body">
        <div class="story-inventory__grid" aria-label="背包槽位">
          <button
            v-for="(slot, index) in inventory.slots"
            :key="index"
            type="button"
            class="story-inventory__slot"
            :class="{
              'story-inventory__slot--selected': inventory.selectedIndex === index,
              'story-inventory__slot--filled': slot,
            }"
            :aria-label="slotLabel(slot, index)"
            @click="inventory.selectSlot(index)"
          >
            <span class="story-inventory__slot-number">{{ index + 1 }}</span>
            <template v-if="slot">
              <span
                class="story-inventory__symbol"
                :style="{ color: itemForSlot(slot).color }"
                aria-hidden="true"
              >
                {{ itemForSlot(slot).symbol }}
              </span>
              <span v-if="slot.quantity > 1" class="story-inventory__quantity">×{{ slot.quantity }}</span>
            </template>
          </button>
        </div>

        <aside class="story-inventory__details">
          <template v-if="inventory.selectedSlot && inventory.selectedItem">
            <div
              class="story-inventory__detail-symbol"
              :style="{ color: inventory.selectedItem.color }"
              aria-hidden="true"
            >
              {{ inventory.selectedItem.symbol }}
            </div>
            <p class="story-inventory__category">{{ inventory.selectedItem.category }}</p>
            <h2>{{ inventory.selectedItem.name }}</h2>
            <div class="story-inventory__count">持有数量：{{ inventory.selectedSlot.quantity }}</div>
            <p class="story-inventory__description">{{ inventory.selectedItem.description }}</p>
            <p v-if="inventory.selectedItem.unknown" class="story-inventory__unknown">
              未知道具已保留，但当前不能使用。
            </p>
          </template>
          <div v-else class="story-inventory__empty-detail">
            <span aria-hidden="true">◇</span>
            <p>选择一个道具查看详情</p>
          </div>
        </aside>
      </div>

      <footer class="story-inventory__footer">
        <p>{{ inventory.totalItems }} 件道具 · B 关闭背包 · Esc 返回游戏</p>
        <div class="story-inventory__actions">
          <button
            type="button"
            class="story-action-button story-action-button--secondary"
            :disabled="!inventory.selectedSlot"
            @click="inventory.dropSelectedItem()"
          >
            丢弃 1 个
          </button>
          <button
            type="button"
            class="story-action-button story-action-button--primary"
            :disabled="!inventory.selectedSlot || !inventory.selectedItem?.usable"
            @click="inventory.useSelectedItem()"
          >
            使用
          </button>
        </div>
      </footer>
    </div>
  </section>
</template>

<script setup>
import { nextTick, onMounted, ref } from 'vue';

import { useStoryInventoryStore } from '@/stores/storyInventory.js';
import { getStoryItemDefinition } from '@/utils/storyInventory.js';

const inventory = useStoryInventoryStore();
const closeButtonRef = ref(null);

defineEmits(['close']);

const itemForSlot = (slot) => getStoryItemDefinition(slot?.itemId);
const slotLabel = (slot, index) =>
  slot ? `${index + 1}号槽位，${itemForSlot(slot).name}，数量 ${slot.quantity}` : `${index + 1}号空槽位`;

onMounted(async () => {
  await nextTick();
  closeButtonRef.value?.focus?.();
});
</script>

<style scoped>
.story-panel-overlay {
  position: absolute;
  inset: 0;
  z-index: 20;
  display: grid;
  place-items: center;
  padding: 34px;
  background: rgba(2, 3, 2, 0.72);
  backdrop-filter: blur(7px);
  pointer-events: auto;
}

.story-inventory {
  width: min(1000px, calc(100vw - 68px));
  max-height: calc(100vh - 68px);
  overflow: hidden;
  border: 1px solid rgba(216, 184, 108, 0.42);
  border-radius: 16px;
  background: linear-gradient(145deg, rgba(27, 27, 22, 0.98), rgba(8, 10, 8, 0.98));
  box-shadow: 0 28px 90px rgba(0, 0, 0, 0.7);
}

.story-inventory__header,
.story-inventory__footer {
  display: flex;
  align-items: center;
  gap: 20px;
  padding: 22px 26px;
}

.story-inventory__header {
  border-bottom: 1px solid rgba(216, 184, 108, 0.16);
}

.story-inventory__header > div:first-child {
  flex: 1;
}

.story-inventory__header p {
  margin: 0 0 3px;
  color: #897b5c;
  font-size: 10px;
  letter-spacing: 0.25em;
}

.story-inventory__header h1 {
  margin: 0;
  color: #f2e6c9;
  font-size: 26px;
  letter-spacing: 0.08em;
}

.story-inventory__capacity {
  color: #99917e;
  font-size: 13px;
}

.story-inventory__capacity strong {
  color: #d8b86c;
  font-size: 18px;
}

.story-icon-button {
  width: 38px;
  height: 38px;
  border: 1px solid rgba(216, 184, 108, 0.24);
  border-radius: 9px;
  background: rgba(255, 255, 255, 0.035);
  color: #cabfaa;
  font-size: 24px;
  line-height: 1;
  cursor: pointer;
}

.story-icon-button:hover,
.story-icon-button:focus-visible {
  border-color: rgba(216, 184, 108, 0.65);
  color: #fff0ca;
  outline: none;
}

.story-inventory__body {
  display: grid;
  grid-template-columns: minmax(0, 2fr) minmax(250px, 0.9fr);
  gap: 28px;
  padding: 28px;
  overflow: auto;
}

.story-inventory__grid {
  display: grid;
  grid-template-columns: repeat(6, minmax(64px, 1fr));
  gap: 10px;
  align-content: start;
}

.story-inventory__slot {
  position: relative;
  aspect-ratio: 1;
  min-width: 0;
  border: 1px solid rgba(216, 184, 108, 0.14);
  border-radius: 10px;
  background: linear-gradient(145deg, rgba(255, 255, 255, 0.035), rgba(0, 0, 0, 0.18));
  color: #a9a18f;
  cursor: pointer;
  transition: border-color 130ms ease, background 130ms ease, transform 130ms ease;
}

.story-inventory__slot:hover {
  border-color: rgba(216, 184, 108, 0.42);
  transform: translateY(-1px);
}

.story-inventory__slot--filled {
  background: linear-gradient(145deg, rgba(216, 184, 108, 0.08), rgba(0, 0, 0, 0.23));
}

.story-inventory__slot--selected {
  border-color: #d8b86c;
  box-shadow: inset 0 0 0 1px rgba(216, 184, 108, 0.32), 0 0 16px rgba(216, 184, 108, 0.12);
}

.story-inventory__slot:focus-visible {
  outline: 2px solid #ead08e;
  outline-offset: 2px;
}

.story-inventory__slot-number {
  position: absolute;
  top: 5px;
  left: 7px;
  color: #625f58;
  font-size: 9px;
}

.story-inventory__symbol {
  display: block;
  font-size: clamp(24px, 3vw, 38px);
  line-height: 1;
  text-shadow: 0 0 14px currentColor;
}

.story-inventory__quantity {
  position: absolute;
  right: 7px;
  bottom: 5px;
  color: #f3ead5;
  font-size: 12px;
  font-weight: 700;
  text-shadow: 0 1px 4px #000;
}

.story-inventory__details {
  min-height: 310px;
  padding: 28px;
  border: 1px solid rgba(216, 184, 108, 0.16);
  border-radius: 13px;
  background: radial-gradient(circle at 50% 15%, rgba(216, 184, 108, 0.09), transparent 36%), rgba(0, 0, 0, 0.19);
}

.story-inventory__detail-symbol {
  min-height: 82px;
  font-size: 66px;
  line-height: 1;
  text-align: center;
  text-shadow: 0 0 22px currentColor;
}

.story-inventory__category {
  margin: 22px 0 4px;
  color: #9f8b5b;
  font-size: 11px;
  letter-spacing: 0.12em;
}

.story-inventory__details h2 {
  margin: 0;
  color: #f1e5c8;
  font-size: 24px;
}

.story-inventory__count {
  margin-top: 8px;
  color: #a9a08d;
  font-size: 12px;
}

.story-inventory__description,
.story-inventory__unknown {
  color: #aaa18e;
  font-size: 13px;
  line-height: 1.75;
}

.story-inventory__unknown {
  color: #c69b70;
}

.story-inventory__empty-detail {
  display: grid;
  height: 100%;
  place-content: center;
  color: #777166;
  text-align: center;
}

.story-inventory__empty-detail span {
  font-size: 50px;
}

.story-inventory__footer {
  justify-content: space-between;
  border-top: 1px solid rgba(216, 184, 108, 0.14);
  background: rgba(0, 0, 0, 0.14);
}

.story-inventory__footer p {
  margin: 0;
  color: #817b6f;
  font-size: 11px;
}

.story-inventory__actions {
  display: flex;
  gap: 10px;
}

.story-action-button {
  min-width: 112px;
  min-height: 40px;
  padding: 8px 18px;
  border: 1px solid transparent;
  border-radius: 8px;
  font-weight: 600;
  cursor: pointer;
}

.story-action-button:disabled {
  cursor: not-allowed;
  opacity: 0.4;
}

.story-action-button--primary {
  background: #d8b86c;
  color: #17130a;
}

.story-action-button--secondary {
  border-color: rgba(216, 184, 108, 0.28);
  background: rgba(255, 255, 255, 0.04);
  color: #d6cbb4;
}

@media (max-width: 780px) {
  .story-panel-overlay {
    padding: 16px;
  }

  .story-inventory {
    width: calc(100vw - 32px);
    max-height: calc(100vh - 32px);
  }

  .story-inventory__body {
    grid-template-columns: 1fr;
  }

  .story-inventory__grid {
    grid-template-columns: repeat(6, minmax(44px, 1fr));
  }

  .story-inventory__details {
    min-height: 210px;
  }
}
</style>
