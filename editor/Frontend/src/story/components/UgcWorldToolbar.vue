<!-- 小世界顶部工具栏：显示世界名称、建造/试玩模式和真实保存状态。 -->
<template>
  <header class="ugc-toolbar" @pointerdown.stop>
    <div class="world-heading">
      <strong>{{ world?.name || '未命名小世界' }}</strong>
      <span :class="['save-state', { dirty }]">{{ statusLabel }}</span>
    </div>

    <nav class="mode-switch" aria-label="小世界模式">
      <button
        type="button"
        :class="{ active: mode === 'build' }"
        :disabled="saving"
        @click="$emit('set-mode', 'build')"
      >
        建造模式
      </button>
      <button
        type="button"
        :class="{ active: mode === 'play' }"
        :disabled="saving"
        @click="$emit('set-mode', 'play')"
      >
        试玩模式
      </button>
    </nav>

    <div class="toolbar-actions">
      <button type="button" :disabled="saving || !dirty" @click="$emit('save')">
        {{ saving ? '保存中…' : '保存' }}
      </button>
      <button type="button" :disabled="saving" @click="$emit('exit')">退出小世界</button>
    </div>
  </header>
</template>

<script setup>
import { computed } from 'vue';

const props = defineProps({
  world: { type: Object, default: null },
  mode: { type: String, default: 'build' },
  dirty: { type: Boolean, default: false },
  saving: { type: Boolean, default: false },
});

defineEmits(['set-mode', 'save', 'exit']);

const statusLabel = computed(() => {
  if (props.saving) return '保存中';
  if (props.dirty) return '未保存';
  return props.mode === 'play' ? '试玩中' : '已保存';
});
</script>

<style scoped>
.ugc-toolbar {
  position: absolute;
  z-index: 8;
  top: 20px;
  left: 20px;
  right: 20px;
  display: flex;
  min-height: 52px;
  align-items: center;
  gap: 22px;
  padding: 8px 12px;
  border: 1px solid var(--game-border, #304656);
  background: var(--game-panel, #101d2a);
  box-shadow: 0 10px 24px rgb(0 0 0 / 24%);
  color: var(--game-text, #e5ebee);
  pointer-events: auto;
}

.world-heading {
  display: flex;
  min-width: 150px;
  flex-direction: column;
  gap: 3px;
}

.world-heading strong {
  overflow: hidden;
  font-size: 14px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.save-state {
  color: var(--game-muted, #8f9da6);
  font-size: 11px;
}

.save-state.dirty {
  color: var(--game-gold, #c6a15b);
}

.mode-switch,
.toolbar-actions {
  display: flex;
  align-items: center;
  gap: 6px;
}

.mode-switch {
  flex: 1;
}

.ugc-toolbar button {
  min-height: 32px;
  padding: 0 11px;
  border: 1px solid var(--game-border, #304656);
  background: var(--game-panel-deep, #0b1723);
  color: var(--game-muted, #8f9da6);
  cursor: pointer;
  font: inherit;
  font-size: 12px;
}

.ugc-toolbar button:hover:not(:disabled),
.ugc-toolbar button:focus-visible {
  border-color: var(--game-border-strong, #456173);
  background: #172936;
  color: var(--game-text, #e5ebee);
  outline: none;
}

.mode-switch button.active {
  border-color: var(--game-cyan, #75cdbd);
  background: #18323c;
  color: var(--game-text, #e5ebee);
}

.toolbar-actions button:first-child {
  border-color: var(--game-gold, #c6a15b);
  color: #e3c985;
}

.ugc-toolbar button:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

@media (max-width: 760px) {
  .ugc-toolbar {
    top: 10px;
    left: 10px;
    right: 10px;
    flex-wrap: wrap;
    gap: 8px;
  }

  .mode-switch {
    order: 3;
    width: 100%;
  }
}
</style>
