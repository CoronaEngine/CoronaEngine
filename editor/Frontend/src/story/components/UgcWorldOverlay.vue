<!-- 小世界覆盖层：组合工具栏、资源面板和未保存退出确认，不复用创造模式 Dock。 -->
<template>
  <div class="ugc-overlay" @pointerdown.stop>
    <UgcWorldToolbar
      :world="viewState.world"
      :mode="viewState.mode"
      :dirty="viewState.dirty"
      :saving="viewState.saving"
      @set-mode="$emit('set-mode', $event)"
      @save="$emit('save')"
      @exit="$emit('exit')"
    />

    <UgcWorldResourcePanel
      :world="viewState.world"
      :resources="viewState.resources"
      :mode="viewState.mode"
      :error="error"
      @place="$emit('place', $event)"
      @delete="$emit('delete', $event)"
      @bind="$emit('bind', $event)"
    />

    <div v-if="error" class="toast error-toast">{{ error }}</div>

    <div v-if="exitConfirm" class="confirm-backdrop" @click.self="$emit('cancel-exit')">
      <section
        class="confirm-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="ugc-exit-title"
      >
        <h2 id="ugc-exit-title">小世界有未保存修改</h2>
        <p>保存后再退出，或直接放弃本次修改。</p>
        <div class="confirm-actions">
          <button type="button" @click="$emit('save-exit')">保存并退出</button>
          <button type="button" @click="$emit('discard-exit')">放弃修改</button>
          <button type="button" @click="$emit('cancel-exit')">取消</button>
        </div>
      </section>
    </div>
  </div>
</template>

<script setup>
import UgcWorldResourcePanel from './UgcWorldResourcePanel.vue';
import UgcWorldToolbar from './UgcWorldToolbar.vue';

defineProps({
  viewState: {
    type: Object,
    default: () => ({ world: null, resources: {}, mode: 'build', dirty: false, saving: false }),
  },
  exitConfirm: { type: Boolean, default: false },
  error: { type: String, default: '' },
});

defineEmits([
  'set-mode',
  'save',
  'exit',
  'save-exit',
  'discard-exit',
  'cancel-exit',
  'place',
  'delete',
  'bind',
]);
</script>

<style scoped>
.ugc-overlay {
  position: absolute;
  z-index: 6;
  inset: 0;
  pointer-events: none;
}

.toast {
  position: absolute;
  z-index: 10;
  top: 84px;
  left: 50%;
  max-width: min(420px, calc(100% - 32px));
  padding: 9px 12px;
  border: 1px solid var(--game-border-strong, #456173);
  background: var(--game-panel, #101d2a);
  color: var(--game-text, #e5ebee);
  font-size: 12px;
  pointer-events: auto;
  transform: translateX(-50%);
}

.error-toast {
  border-color: #704e53;
  color: #e0a2a2;
}

.confirm-backdrop {
  position: absolute;
  z-index: 20;
  inset: 0;
  display: grid;
  place-items: center;
  background: rgb(4 10 16 / 56%);
  pointer-events: auto;
}

.confirm-dialog {
  width: min(360px, calc(100% - 32px));
  padding: 20px;
  border: 1px solid var(--game-border-strong, #456173);
  background: var(--game-panel, #101d2a);
  box-shadow: 0 10px 24px rgb(0 0 0 / 30%);
}

.confirm-dialog h2 {
  margin: 0 0 8px;
  font-size: 16px;
}

.confirm-dialog p {
  margin: 0 0 18px;
  color: var(--game-muted, #8f9da6);
  font-size: 12px;
}

.confirm-actions {
  display: flex;
  justify-content: flex-end;
  gap: 7px;
}

.confirm-actions button {
  min-height: 32px;
  padding: 0 10px;
  border: 1px solid var(--game-border, #304656);
  background: var(--game-panel-deep, #0b1723);
  color: var(--game-text, #e5ebee);
  cursor: pointer;
  font: inherit;
  font-size: 12px;
}

.confirm-actions button:first-child {
  border-color: var(--game-gold, #c6a15b);
  color: #e3c985;
}

.confirm-actions button:hover,
.confirm-actions button:focus-visible {
  border-color: var(--game-cyan, #75cdbd);
  outline: none;
}
</style>
