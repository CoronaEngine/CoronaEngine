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
        <!-- 装备槽持有实际装备实例，不另行复制背包物品。 -->
        <aside class="equipment-panel" aria-label="装备栏">
          <h3>旅行者装备</h3>
          <svg class="figure" viewBox="0 0 120 190" aria-label="装备人物轮廓" role="img">
            <circle cx="60" cy="24" r="17" />
            <path
              d="M39 47 L81 47 L100 108 L86 114 L74 78 L74 124 L84 179 L66 179 L60 134 L54 179 L36 179 L46 124 L46 78 L34 114 L20 108 Z"
            />
          </svg>
          <button
            v-for="(label, slot) in EQUIPMENT_SLOTS"
            :key="slot"
            class="equip-slot"
            :class="{ compatible: dragged?.item?.slot === slot, occupied: equipment[slot] }"
            :aria-label="`${label}槽：${equipment[slot]?.name || '空'}`"
            :draggable="Boolean(equipment[slot])"
            @dragstart="startDrag($event, equipment[slot], slot)"
            @dragend="cancelDrag"
            @dragover.prevent
            @drop.prevent.stop="dropEquipment(slot)"
            @click="equipment[slot] && $emit('unequip', slot)"
          >
            <span>{{ label }}</span>
            <strong>{{ equipment[slot]?.name || '拖入装备' }}</strong>
          </button>
          <p class="equipment-help">
            点击已穿戴装备可卸下；刀与斧共用主手。护甲 {{ armorValue(equipment) }} / 20
          </p>
          <p v-if="dragMessage || transferError" class="drag-message" role="status">
            {{ dragMessage || transferError }}
          </p>
        </aside>
        <section class="inventory-section" aria-label="物品栏">
          <div class="section-heading">
            <h3>物品栏</h3>
            <span>7 × 3</span>
          </div>

          <div class="inventory-grid" @dragover.prevent @drop.prevent.stop="dropInventory">
            <button
              v-for="(item, index) in inventorySlots"
              :key="`inventory-${index}`"
              class="inventory-slot"
              :class="{ selected: selected?.id === item?.id }"
              type="button"
              :aria-label="item ? `${item.name}，数量 ${item.quantity}` : `空槽位 ${index + 1}`"
              :draggable="item?.category === 'equipment'"
              @dragstart="startDrag($event, item)"
              @dragend="cancelDrag"
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
              v-if="selected.category === 'equipment'"
              class="primary-button"
              type="button"
              @click="$emit('equip', { id: selected.id, slot: selected.slot })"
            >
              装备到{{ EQUIPMENT_SLOTS[selected.slot] }}槽
            </button>
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
import { EQUIPMENT_SLOTS, armorValue } from '../equipmentSystem.js';

const INVENTORY_SLOT_COUNT = 21;

const props = defineProps({
  // 父组件把原子转移失败的原因显示在弹窗内部，避免被弹窗遮挡。
  transferError: { type: String, default: '' },
  equipment: { type: Object, default: () => ({}) },
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

const emit = defineEmits(['close', 'use-orb', 'select-hotbar', 'equip', 'unequip']);
const selected = ref(null);
const dragged = ref(null);
const dragMessage = ref('');
// 拖拽数据仅在本组件内有效，外部拖入内容不能创建或转移背包物品。
function startDrag(event, item, sourceSlot = null) {
  if (item?.category !== 'equipment') {
    event.preventDefault();
    return;
  }
  dragged.value = { item, sourceSlot };
  dragMessage.value = '';
  event.dataTransfer.effectAllowed = 'move';
  event.dataTransfer.setData('text/plain', item.id);
}
function cancelDrag() {
  dragged.value = null;
}
function dropEquipment(slot) {
  if (!dragged.value) return;
  if (dragged.value.sourceSlot) dragMessage.value = '已穿戴装备请先拖回背包。';
  else if (dragged.value.item.slot !== slot) dragMessage.value = '装备类型与槽位不匹配。';
  else emit('equip', { id: dragged.value.item.id, slot });
  cancelDrag();
}
function dropInventory() {
  if (dragged.value?.sourceSlot) emit('unequip', dragged.value.sourceSlot);
  cancelDrag();
}

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
  if (item.category === 'equipment')
    return { head: '♜', chest: '◇', legs: 'Ⅱ', feet: '▰', mainHand: '⚔' }[item.slot];
  if (item.category === 'material') return '◆';
  if (item.id === 'world-orb-demo') return '●';
  if (item.id === 'world-fragment-demo') return '✦';
  return '◇';
}

function categoryLabel(category) {
  return (
    {
      equipment: '装备',
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
/* 普通界面表面沿用创造模式的黑金主题，危险提示保留红色。 */
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
  width: min(1120px, 100%);
  max-height: min(720px, calc(100vh - 56px));
  overflow: auto;
  border: 1px solid var(--game-border-strong, #6b5b36);
  border-radius: 10px;
  background: var(--game-panel, #171714);
  color: var(--game-text, #e8e3d6);
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
  border-bottom: 1px solid var(--game-border, #443c2a);
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
  color: var(--game-muted, #aaa594);
  font-size: 11px;
}

.icon-button {
  display: grid;
  width: 30px;
  height: 30px;
  place-items: center;
  border: 1px solid var(--game-border, #443c2a);
  border-radius: 5px;
  background: var(--ce-black-2);
  color: var(--game-muted, #aaa594);
  cursor: pointer;
  font-size: 20px;
  line-height: 1;
}

.icon-button:hover,
.icon-button:focus-visible {
  border-color: var(--game-cyan, #e8ca80);
  background: var(--ce-black-3);
  color: var(--game-text, #e8e3d6);
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
  border: 1px solid var(--game-border);
  background: var(--ce-black-2);
  color: var(--game-text, #e8e3d6);
  cursor: pointer;
}

.inventory-slot:hover,
.inventory-slot:focus-visible {
  border-color: var(--game-border-strong, #6b5b36);
  background: var(--ce-black-3);
  outline: none;
}

.inventory-slot.selected {
  border-color: var(--game-gold, #c6a15b);
  background: color-mix(in srgb, var(--ce-gold-primary) 16%, var(--ce-black-1));
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
  color: var(--game-text, #e8e3d6);
  font-size: 12px;
  font-weight: 700;
  line-height: 1;
  text-shadow: 1px 1px 0 var(--ce-black-0);
}

.slot-number {
  position: absolute;
  top: 4px;
  left: 5px;
  color: var(--game-muted, #aaa594);
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
  border-left: 1px solid var(--game-border, #443c2a);
  background: var(--game-panel-deep, #10100e);
}

.detail-art {
  display: grid;
  min-height: 130px;
  place-items: center;
  border: 1px solid var(--game-border-strong, #6b5b36);
  border-radius: 7px;
  background: var(--ce-black-2);
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
  color: var(--game-muted, #aaa594);
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
  background: var(--ce-black-2);
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
  color: var(--game-text, #e8e3d6);
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
    border-top: 1px solid var(--game-border, #443c2a);
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
/* 紧凑的人物轮廓与装备槽和现有的 7×3 背包并排显示。 */
.panel-body {
  grid-template-columns: 185px minmax(320px, 1fr) 215px;
}
.equipment-panel {
  min-width: 0;
  border-right: 1px solid var(--game-border);
  padding: 18px 12px;
}
.equipment-panel h3 {
  font-size: 13px;
  margin-bottom: 8px;
  color: var(--ce-gold-primary);
}
.figure {
  height: 135px;
  width: 100%;
  fill: var(--ce-black-3);
  stroke: var(--ce-gold-muted);
  stroke-width: 2;
}
.equip-slot {
  width: 100%;
  display: grid;
  grid-template-columns: 34px 1fr;
  text-align: left;
  gap: 5px;
  margin: 5px 0;
  padding: 8px 5px;
  border: 1px dashed var(--game-border);
  background: var(--game-panel-deep);
  font-size: 10px;
  color: var(--game-muted);
  cursor: pointer;
}
.equip-slot strong {
  color: var(--game-text);
  font-weight: 500;
}
.equip-slot.compatible,
.equip-slot:hover {
  border-color: var(--ce-gold-bright);
  background: var(--ce-black-3);
}
.equip-slot.occupied {
  border-style: solid;
}
.equipment-help,
.drag-message {
  font-size: 10px;
  line-height: 1.7;
  margin-top: 10px;
  color: var(--game-muted);
}
.drag-message {
  color: #f0ad7f;
}
@media (max-width: 900px) {
  .panel-body {
    grid-template-columns: 145px minmax(280px, 1fr);
  }
  .detail-panel {
    grid-column: 1 / -1;
  }
  .figure {
    height: 95px;
  }
}
@media (max-width: 550px) {
  .overlay {
    padding: 8px;
  }
  .panel-body {
    grid-template-columns: minmax(0, 1fr);
  }
  .equipment-panel {
    border-right: 0;
  }
  .figure {
    display: none;
  }
}
</style>
