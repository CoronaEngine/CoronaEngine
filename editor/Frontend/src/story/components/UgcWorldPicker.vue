<!-- 小世界选择器：在进入前选择创建新世界或打开当前项目已有的小世界。 -->
<template>
  <div class="picker-backdrop" @click.self="$emit('cancel')">
    <section class="picker" role="dialog" aria-modal="true" aria-labelledby="picker-title">
      <header class="picker-header">
        <div>
          <p class="eyebrow">世界小球</p>
          <h2 id="picker-title">选择小世界</h2>
        </div>
        <button class="close-button" type="button" aria-label="关闭" @click="$emit('cancel')">
          ×
        </button>
      </header>

      <p v-if="error" class="error-message">{{ error }}</p>
      <p v-else-if="loading" class="empty-message">正在读取当前项目的小世界……</p>

      <button class="create-button" type="button" @click="$emit('create')">
        <span class="action-title">创建新小世界</span>
        <span class="action-description">从空白场景开始建造</span>
      </button>

      <div v-if="!loading && worlds.length" class="world-list">
        <h3>已有小世界</h3>
        <button
          v-for="world in worlds"
          :key="world.worldId"
          class="world-row"
          type="button"
          :disabled="loading"
          @click="$emit('open', world.worldId)"
        >
          <span class="world-name">{{ world.name || '未命名小世界' }}</span>
          <span class="world-id">{{ world.worldId }}</span>
        </button>
      </div>
      <p v-else-if="!loading" class="empty-message">当前项目还没有已保存的小世界。</p>
    </section>
  </div>
</template>

<script setup>
defineProps({
  worlds: { type: Array, default: () => [] },
  loading: { type: Boolean, default: false },
  error: { type: String, default: '' },
});

defineEmits(['cancel', 'create', 'open']);
</script>

<style scoped>
.picker-backdrop {
  position: absolute;
  z-index: 30;
  inset: 0;
  display: grid;
  place-items: center;
  padding: 20px;
  background: rgb(4 10 16 / 64%);
  pointer-events: auto;
}

.picker {
  width: min(440px, 100%);
  max-height: min(560px, 100%);
  overflow: auto;
  padding: 20px;
  border: 1px solid var(--game-border-strong, #456173);
  background: var(--game-panel, #101d2a);
  box-shadow: 0 12px 28px rgb(0 0 0 / 32%);
  color: var(--game-text, #e5ebee);
}

.picker-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 14px;
  margin-bottom: 16px;
}

.eyebrow {
  margin: 0 0 4px;
  color: var(--game-gold, #c6a15b);
  font-size: 10px;
  letter-spacing: 0.08em;
}

h2,
h3,
p {
  margin-top: 0;
}

h2 {
  margin-bottom: 0;
  font-size: 19px;
}

h3 {
  margin-bottom: 8px;
  color: var(--game-muted, #8f9da6);
  font-size: 12px;
  font-weight: 500;
}

.close-button {
  width: 30px;
  height: 30px;
  border: 1px solid var(--game-border, #304656);
  background: var(--game-panel-deep, #0b1723);
  color: var(--game-muted, #8f9da6);
  cursor: pointer;
  font-size: 20px;
  line-height: 1;
}

.create-button,
.world-row {
  display: flex;
  width: 100%;
  flex-direction: column;
  align-items: flex-start;
  gap: 4px;
  border: 1px solid var(--game-border, #304656);
  background: var(--game-panel-deep, #0b1723);
  color: var(--game-text, #e5ebee);
  cursor: pointer;
  font: inherit;
  text-align: left;
}

.create-button {
  padding: 13px;
  border-color: var(--game-gold, #c6a15b);
  margin-bottom: 18px;
}

.world-list {
  display: flex;
  flex-direction: column;
}

.world-row {
  padding: 10px 12px;
  border-bottom: 0;
}

.world-row:last-child {
  border-bottom: 1px solid var(--game-border, #304656);
}

.create-button:hover,
.create-button:focus-visible,
.world-row:hover:not(:disabled),
.world-row:focus-visible,
.close-button:hover,
.close-button:focus-visible {
  border-color: var(--game-cyan, #75cdbd);
  background: #172936;
  outline: none;
}

.action-title,
.world-name {
  font-size: 13px;
  font-weight: 600;
}

.action-description,
.world-id,
.empty-message,
.error-message {
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
}

.world-id {
  overflow: hidden;
  max-width: 100%;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.error-message {
  margin-bottom: 12px;
  color: #e0a2a2;
}

.world-row:disabled {
  cursor: wait;
  opacity: 0.55;
}
</style>
