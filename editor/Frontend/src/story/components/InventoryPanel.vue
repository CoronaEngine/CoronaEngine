<!-- 剧情模式背包 UI：负责展示物品分类、物品详情和世界小球使用入口。 -->
<template>
  <section class="overlay" @pointerdown.stop @click.stop>
    <div class="panel" role="dialog" aria-modal="true" aria-labelledby="inventory-title">
      <header class="panel-header">
        <div class="title-group">
          <h2 id="inventory-title">背包</h2>
        </div>
        <div class="header-actions">
          <span class="item-count">{{ items.length }} 类物品</span>
          <button class="icon-button" type="button" aria-label="关闭背包" @click="closePanel">
            ×
          </button>
        </div>
      </header>

      <div class="panel-body">
        <nav class="category-nav" aria-label="物品分类">
          <span class="nav-label">分类</span>
          <button
            v-for="category in categories"
            :key="category.id"
            type="button"
            class="category-button"
            :class="{ active: activeCategory === category.id }"
            @click="activeCategory = category.id"
          >
            <span class="category-icon" aria-hidden="true">{{ category.icon }}</span>
            <span>{{ category.label }}</span>
            <b>{{ categoryCount(category.id) }}</b>
          </button>
        </nav>

        <section class="item-section" aria-label="物品列表">
          <div class="section-heading">
            <h3>{{ currentCategoryLabel }}</h3>
            <span class="section-count">{{ filteredItems.length }} 项</span>
          </div>

          <div v-if="filteredItems.length" class="item-grid">
            <button
              v-for="item in filteredItems"
              :key="item.id"
              type="button"
              class="item-card"
              :class="[`item-card-${item.category}`, { active: selected?.id === item.id }]"
              @click="selected = item"
            >
              <span class="item-icon" aria-hidden="true">{{ itemIcon(item) }}</span>
              <span class="item-card-copy">
                <strong>{{ item.name }}</strong>
                <small>{{ categoryLabel(item.category) }}</small>
              </span>
              <span class="item-quantity">×{{ item.quantity }}</span>
            </button>
          </div>

          <div v-else class="empty-state">
            <span class="empty-icon" aria-hidden="true">◇</span>
            <strong>暂无该类物品</strong>
          </div>
        </section>

        <aside v-if="selected" class="detail-panel" aria-label="物品详情">
          <div class="detail-art" :class="`detail-art-${selected.category}`">
            <span aria-hidden="true">{{ itemIcon(selected) }}</span>
            <small>{{ categoryLabel(selected.category) }}</small>
          </div>
          <div class="detail-copy">
            <h3>{{ selected.name }}</h3>
            <p>{{ selected.description }}</p>
          </div>
          <div class="detail-meta">
            <div>
              <span>类别</span>
              <strong>{{ categoryLabel(selected.category) }}</strong>
            </div>
            <div>
              <span>数量</span>
              <strong>×{{ selected.quantity }}</strong>
            </div>
          </div>
          <div class="detail-divider"></div>
          <button
            v-if="selected.id === 'world-orb-demo'"
            class="primary-button"
            type="button"
            @click="useSelectedOrb"
          >
            进入空白世界
          </button>
          <button v-else class="secondary-button" type="button" disabled>暂无可用操作</button>
        </aside>
        <aside v-else class="detail-panel detail-empty" aria-label="物品详情">
          <span class="empty-icon" aria-hidden="true">✦</span>
          <strong>选择一件物品</strong>
        </aside>
      </div>
    </div>
  </section>
</template>

<script setup>
import { computed, ref, watch } from 'vue';

const props = defineProps({
  items: {
    type: Array,
    default: () => [],
  },
});

const emit = defineEmits(['close', 'use-orb']);

const categories = [
  { id: 'all', label: '全部', icon: '▦' },
  { id: 'material', label: '材料', icon: '◆' },
  { id: 'ugc', label: 'UGC 组件', icon: '✦' },
];

const activeCategory = ref('all');
const selected = ref(null);

const filteredItems = computed(() => {
  if (activeCategory.value === 'all') return props.items;
  return props.items.filter((item) => item.category === activeCategory.value);
});

const currentCategoryLabel = computed(
  () => categories.find((category) => category.id === activeCategory.value)?.label || '全部'
);

watch(
  () => props.items,
  (items) => {
    if (!selected.value || !items.some((item) => item.id === selected.value.id)) {
      selected.value = items[0] || null;
    }
  },
  { immediate: true, deep: true }
);

watch(activeCategory, () => {
  if (!filteredItems.value.some((item) => item.id === selected.value?.id)) {
    selected.value = filteredItems.value[0] || null;
  }
});

function categoryCount(categoryId) {
  if (categoryId === 'all') return props.items.length;
  return props.items.filter((item) => item.category === categoryId).length;
}

function categoryLabel(category) {
  return categories.find((value) => value.id === category)?.label || '其他';
}

function itemIcon(item) {
  if (item.category === 'material') return '◆';
  if (item.id === 'world-orb-demo') return '◉';
  return '✦';
}

function closePanel() {
  emit('close');
}

function useSelectedOrb() {
  if (selected.value) emit('use-orb', selected.value);
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
  width: min(1080px, 100%);
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
.section-heading,
.category-button,
.detail-meta,
.primary-button,
.secondary-button {
  display: flex;
  align-items: center;
}

.panel-header,
.section-heading,
.primary-button,
.secondary-button {
  justify-content: space-between;
}

.panel-header {
  padding: 22px 26px;
  border-bottom: 1px solid var(--game-border, #304656);
}

.title-group h2 {
  margin: 0;
  font-size: 26px;
  letter-spacing: 0.04em;
}

.header-actions {
  gap: 16px;
}

.item-count,
.section-count,
.nav-label {
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
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

.panel-body {
  display: grid;
  grid-template-columns: 180px minmax(0, 1fr) 250px;
  min-height: 430px;
}

.category-nav {
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 22px 14px;
  border-right: 1px solid var(--game-border, #304656);
  background: var(--game-panel-deep, #0b1723);
}

.nav-label {
  padding: 0 10px 5px;
}

.category-button {
  display: grid;
  grid-template-columns: 24px 1fr auto;
  gap: 9px;
  min-height: 42px;
  padding: 0 10px;
  border: 1px solid transparent;
  border-radius: 7px;
  background: transparent;
  color: var(--game-muted, #8f9da6);
  cursor: pointer;
  text-align: left;
  transition: 160ms ease;
}

.category-button:hover,
.category-button:focus-visible {
  border-color: var(--game-border-strong, #456173);
  background: #142936;
  color: var(--game-text, #e5ebee);
  outline: none;
}

.category-button.active {
  border-color: var(--game-cyan, #75cdbd);
  background: #18323c;
  color: var(--game-cyan, #75cdbd);
}

.category-icon {
  color: var(--game-gold, #c6a15b);
}

.category-button b {
  color: var(--game-text, #e5ebee);
  font-size: 11px;
  font-weight: 600;
}

.item-section {
  min-width: 0;
  padding: 22px;
}

.section-heading {
  margin-bottom: 14px;
}

.section-heading h3 {
  margin: 0;
  font-size: 18px;
}

.item-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 10px;
}

.item-card {
  position: relative;
  display: grid;
  grid-template-columns: 42px minmax(0, 1fr) auto;
  gap: 10px;
  align-items: center;
  min-height: 72px;
  padding: 10px;
  border: 1px solid var(--game-border, #304656);
  border-radius: 8px;
  background: #142735;
  color: var(--game-text, #e5ebee);
  cursor: pointer;
  text-align: left;
  transition: 160ms ease;
}

.item-card:hover,
.item-card:focus-visible,
.item-card.active {
  border-color: var(--game-cyan, #75cdbd);
  background: #1a3540;
  outline: none;
}

.item-icon {
  display: grid;
  width: 42px;
  height: 42px;
  place-items: center;
  border-radius: 6px;
  background: #263847;
  color: var(--game-gold, #c6a15b);
  font-size: 21px;
}

.item-card-material .item-icon {
  background: #3d3327;
  color: #d1a56d;
}

.item-card-copy {
  display: flex;
  min-width: 0;
  flex-direction: column;
  gap: 4px;
}

.item-card-copy strong {
  overflow: hidden;
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.item-card-copy small,
.detail-copy p,
.detail-meta span,
.empty-state,
.detail-empty {
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
}

.item-quantity {
  align-self: start;
  color: var(--game-gold, #c6a15b);
  font-size: 12px;
  font-weight: 800;
}

.empty-state,
.detail-empty {
  display: flex;
  min-height: 220px;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  text-align: center;
}

.empty-state strong,
.detail-empty strong {
  color: var(--game-text, #e5ebee);
}

.empty-icon {
  color: var(--game-gold, #c6a15b);
  font-size: 26px;
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
  display: flex;
  min-height: 118px;
  align-items: center;
  justify-content: space-between;
  padding: 18px;
  border: 1px solid var(--game-border-strong, #456173);
  border-radius: 8px;
  background: #182b39;
}

.detail-art-material {
  border-color: #80633f;
  background: #30291f;
}

.detail-art > span {
  color: var(--game-gold, #c6a15b);
  font-size: 54px;
  line-height: 1;
}

.detail-art small {
  align-self: flex-end;
  color: var(--game-muted, #8f9da6);
  font-size: 10px;
}

.detail-copy {
  display: flex;
  flex-direction: column;
  gap: 7px;
  margin-top: 20px;
}

.detail-copy h3 {
  margin: 0;
  font-size: 22px;
}

.detail-copy p {
  min-height: 40px;
  margin: 0;
  line-height: 1.7;
}

.detail-meta {
  gap: 8px;
  margin-top: 18px;
}

.detail-meta > div {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 5px;
  padding: 9px;
  border-radius: 6px;
  background: #142735;
}

.detail-meta strong {
  font-size: 12px;
}

.detail-divider {
  height: 1px;
  margin: 18px 0;
  background: var(--game-border, #304656);
}

.primary-button,
.secondary-button {
  gap: 12px;
  width: 100%;
  min-height: 44px;
  padding: 0 12px;
  border-radius: 7px;
  cursor: pointer;
  font: inherit;
  transition: 160ms ease;
}

.primary-button {
  justify-content: center;
  border: 1px solid var(--game-gold, #c6a15b);
  background: #3a3020;
  color: #f0d99f;
}

.primary-button:hover,
.primary-button:focus-visible {
  border-color: #e0bb6d;
  background: #493a25;
  outline: none;
}

.secondary-button {
  justify-content: center;
  border: 1px solid var(--game-border, #304656);
  background: #142735;
  color: var(--game-muted, #8f9da6);
  cursor: not-allowed;
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

@media (max-width: 900px) {
  .panel-body {
    grid-template-columns: 140px minmax(0, 1fr);
  }

  .detail-panel {
    display: none;
  }
}

@media (max-width: 640px) {
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

  .panel-body {
    display: block;
    min-height: 0;
    max-height: calc(100vh - 110px);
    overflow-y: auto;
  }

  .category-nav {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    padding: 12px;
    border-right: 0;
    border-bottom: 1px solid var(--game-border, #304656);
  }

  .nav-label {
    display: none;
  }

  .category-button {
    grid-template-columns: 20px 1fr;
    min-height: 38px;
    padding: 0 7px;
    font-size: 11px;
  }

  .category-button b {
    display: none;
  }

  .item-section {
    padding: 16px;
  }

  .item-grid {
    grid-template-columns: 1fr;
  }
}
</style>
