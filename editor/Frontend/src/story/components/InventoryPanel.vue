<!-- 剧情模式完整背包：提供 Minecraft 风格物品网格、物品详情和世界小球入口。 -->
<template>
  <section class="overlay" @pointerdown.stop @click.stop>
    <div class="panel" role="dialog" aria-modal="true" aria-labelledby="inventory-title">
      <header class="panel-header">
        <h2 id="inventory-title">背包</h2>
        <div class="header-actions">
          <span class="item-count">{{ items.length }} 类物品</span>
          <button class="icon-button" type="button" aria-label="关闭背包" @click="closePanel">
            ×
          </button>
        </div>
      </header>

      <div class="panel-body">
        <section class="inventory-section" aria-label="物品栏">
          <div class="section-heading">
            <h3>物品栏</h3>
            <span>7 × 3</span>
          </div>

          <div class="inventory-grid">
            <button
              v-for="(item, index) in inventorySlots"
              :key="`inventory-${index}`"
              class="inventory-slot"
              :class="{ selected: selected?.id === item?.id }"
              type="button"
              :aria-label="item ? `${item.name}，数量 ${item.quantity}` : `空槽位 ${index + 1}`"
              @click="selectItem(item)"
            >
              <template v-if="item">
                <span class="slot-icon" aria-hidden="true">{{ itemIcon(item) }}</span>
                <span class="slot-quantity">{{ item.quantity }}</span>
              </template>
            </button>
          </div>

          <div class="hotbar-heading">
            <h3>快捷栏</h3>
            <span>1 - 7</span>
          </div>
          <div class="hotbar-preview" aria-label="快捷栏预览">
            <button
              v-for="(slot, index) in normalizedHotbarSlots"
              :key="`hotbar-${index}`"
              class="inventory-slot hotbar-slot"
              :class="{ selected: selectedHotbarIndex === index }"
              type="button"
              :aria-label="
                slot ? `${slot.name}，快捷栏第 ${index + 1} 格` : `空快捷栏 ${index + 1}`
              "
              @click="selectHotbar(index)"
            >
              <span class="slot-number">{{ index + 1 }}</span>
              <template v-if="slot">
                <span class="slot-icon" aria-hidden="true">{{ itemIcon(slot) }}</span>
                <span class="slot-quantity">{{ slot.quantity }}</span>
              </template>
            </button>
          </div>
        </section>

        <aside class="detail-panel" aria-label="物品详情">
          <template v-if="selected">
            <div class="detail-art" :class="`detail-art-${selected.category}`">
              <span aria-hidden="true">{{ itemIcon(selected) }}</span>
            </div>
            <div class="detail-copy">
              <h3>{{ selected.name }}</h3>
              <span>{{ categoryLabel(selected.category) }}</span>
              <p>{{ selected.description }}</p>
            </div>
            <div class="detail-meta">
              <div>
                <span>数量</span>
                <strong>×{{ selected.quantity }}</strong>
              </div>
              <div>
                <span>槽位</span>
                <strong>{{ slotLabel(selected) }}</strong>
              </div>
            </div>
            <button
              v-if="selected.id === 'world-orb-demo'"
              class="primary-button"
              type="button"
              @click="useSelectedOrb"
            >
              进入空白世界
            </button>
          </template>
          <div v-else class="detail-empty">
            <span class="empty-icon" aria-hidden="true">◇</span>
            <strong>选择一件物品</strong>
          </div>
        </aside>
      </div>
    </div>
  </section>
</template>

<script setup>
import { computed, ref, watch } from 'vue';

const INVENTORY_SLOT_COUNT = 21;

const props = defineProps({
  items: {
    type: Array,
    default: () => [],
  },
  hotbarSlots: {
    type: Array,
    default: () => [],
  },
  selectedHotbarIndex: {
    type: Number,
    default: 0,
  },
});

const emit = defineEmits(['close', 'use-orb', 'select-hotbar']);
const selected = ref(null);

const inventorySlots = computed(() =>
  Array.from({ length: INVENTORY_SLOT_COUNT }, (_, index) => props.items[index] || null)
);

const normalizedHotbarSlots = computed(() =>
  Array.from({ length: 7 }, (_, index) => props.hotbarSlots[index] || null)
);

watch(
  () => props.items,
  (items) => {
    if (
      !selected.value ||
      !items.some((item) => item.id === selected.value.id && item.quantity > 0)
    ) {
      selected.value = items.find((item) => item.quantity > 0) || null;
    }
  },
  { immediate: true, deep: true }
);

function itemIcon(item) {
  if (item.category === 'material') return '◆';
  if (item.id === 'world-orb-demo') return '●';
  if (item.id === 'world-fragment-demo') return '✦';
  return '◇';
}

function categoryLabel(category) {
  return (
    {
      material: '材料',
      ugc: 'UGC 道具',
    }[category] || '物品'
  );
}

function slotLabel(item) {
  const index = props.hotbarSlots.findIndex((slot) => slot?.id === item.id);
  return index >= 0 ? `${index + 1}` : '背包';
}

function selectItem(item) {
  if (item) selected.value = item;
}

function selectHotbar(index) {
  emit('select-hotbar', index);
  const item = props.hotbarSlots[index];
  if (item) selected.value = item;
}

function closePanel() {
  emit('close');
}

function useSelectedOrb() {
  emit('use-orb');
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
  width: min(900px, 100%);
  max-height: min(720px, calc(100vh - 56px));
  overflow: hidden;
  border: 1px solid var(--game-border-strong, #456173);
  border-radius: 10px;
  background: var(--game-panel, #101d2a);
  color: var(--game-text, #e5ebee);
  box-shadow: 0 18px 42px rgb(0 0 0 / 34%);
  animation: panel-in 180ms ease-out;
}

.panel-header,
.header-actions,
.section-heading,
.hotbar-heading {
  display: flex;
  align-items: center;
}

.panel-header,
.section-heading,
.hotbar-heading {
  justify-content: space-between;
}

.panel-header {
  padding: 18px 22px;
  border-bottom: 1px solid var(--game-border, #304656);
}

.panel-header h2,
.section-heading h3,
.hotbar-heading h3 {
  margin: 0;
}

.panel-header h2 {
  font-size: 24px;
  letter-spacing: 0.04em;
}

.header-actions {
  gap: 14px;
}

.item-count,
.section-heading span,
.hotbar-heading span {
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
}

.icon-button {
  display: grid;
  width: 30px;
  height: 30px;
  place-items: center;
  border: 1px solid var(--game-border, #304656);
  border-radius: 5px;
  background: #172936;
  color: var(--game-muted, #8f9da6);
  cursor: pointer;
  font-size: 20px;
  line-height: 1;
}

.icon-button:hover,
.icon-button:focus-visible {
  border-color: var(--game-cyan, #75cdbd);
  background: #1c3441;
  color: var(--game-text, #e5ebee);
  outline: none;
}

.panel-body {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 250px;
  min-height: 420px;
}

.inventory-section {
  padding: 22px;
}

.section-heading,
.hotbar-heading {
  margin-bottom: 12px;
}

.section-heading h3,
.hotbar-heading h3 {
  font-size: 14px;
}

.inventory-grid,
.hotbar-preview {
  display: grid;
  grid-template-columns: repeat(7, minmax(42px, 1fr));
  gap: 5px;
}

.inventory-slot {
  position: relative;
  display: grid;
  min-width: 0;
  min-height: 58px;
  place-items: center;
  padding: 0;
  border: 1px solid #3a5060;
  background: #172936;
  color: var(--game-text, #e5ebee);
  cursor: pointer;
}

.inventory-slot:hover,
.inventory-slot:focus-visible {
  border-color: var(--game-border-strong, #456173);
  background: #1c3441;
  outline: none;
}

.inventory-slot.selected {
  border-color: var(--game-gold, #c6a15b);
  background: #263422;
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

.slot-number {
  position: absolute;
  top: 4px;
  left: 5px;
  color: var(--game-muted, #8f9da6);
  font-size: 10px;
  line-height: 1;
}

.hotbar-heading {
  margin-top: 24px;
}

.hotbar-slot {
  min-height: 62px;
}

.detail-panel {
  display: flex;
  min-width: 0;
  flex-direction: column;
  padding: 22px 20px;
  border-left: 1px solid var(--game-border, #304656);
  background: var(--game-panel-deep, #0b1723);
}

.detail-art {
  display: grid;
  min-height: 130px;
  place-items: center;
  border: 1px solid var(--game-border-strong, #456173);
  border-radius: 7px;
  background: #182b39;
}

.detail-art-material {
  border-color: #80633f;
  background: #30291f;
}

.detail-art > span {
  color: var(--game-gold, #c6a15b);
  font-size: 56px;
  line-height: 1;
}

.detail-copy {
  display: flex;
  flex-direction: column;
  gap: 7px;
  margin-top: 20px;
}

.detail-copy h3 {
  margin: 0;
  font-size: 21px;
}

.detail-copy > span,
.detail-copy p,
.detail-meta span,
.detail-empty {
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
}

.detail-copy p {
  min-height: 42px;
  margin: 0;
  line-height: 1.7;
}

.detail-meta {
  display: flex;
  gap: 8px;
  margin-top: 18px;
}

.detail-meta > div {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 5px;
  padding: 9px;
  background: #142735;
}

.detail-meta strong {
  font-size: 12px;
}

.primary-button {
  width: 100%;
  min-height: 44px;
  margin-top: auto;
  padding: 0 12px;
  border: 1px solid var(--game-gold, #c6a15b);
  border-radius: 6px;
  background: #3a3020;
  color: #f0d99f;
  cursor: pointer;
  font: inherit;
}

.primary-button:hover,
.primary-button:focus-visible {
  border-color: #e0bb6d;
  background: #493a25;
  outline: none;
}

.detail-empty {
  display: flex;
  min-height: 220px;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  text-align: center;
}

.detail-empty strong {
  color: var(--game-text, #e5ebee);
}

.empty-icon {
  color: var(--game-gold, #c6a15b);
  font-size: 26px;
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

@media (max-width: 760px) {
  .panel-body {
    grid-template-columns: 1fr;
    max-height: calc(100vh - 110px);
    overflow-y: auto;
  }

  .detail-panel {
    min-height: 260px;
    border-top: 1px solid var(--game-border, #304656);
    border-left: 0;
  }
}

@media (max-width: 560px) {
  .overlay {
    padding: 12px;
  }

  .panel {
    max-height: calc(100vh - 24px);
  }

  .panel-header,
  .inventory-section,
  .detail-panel {
    padding: 16px;
  }

  .inventory-grid,
  .hotbar-preview {
    gap: 3px;
  }

  .inventory-slot {
    min-height: 48px;
  }
}
</style>
