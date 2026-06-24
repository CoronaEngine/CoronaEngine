<script setup>
import { computed, onMounted, onUnmounted } from 'vue';
import { useRoute } from 'vue-router';
import { useDockStore } from '@/stores/dockStore.js';
import { getPluginManifest } from '@/config/pluginManifest.js';
import DockLayout from '@/components/dock/DockLayout.vue';
import DockPanel from '@/components/dock/DockPanel.vue';
import { Bridge } from '@/utils/bridge.js';
import { shouldDisableEditorCameraInput } from '@/utils/editorInputFocusGate.js';
import '@/utils/eventBus.js'; // init window.__coronaEmit

const route = useRoute();
const dockStore = useDockStore();

// DockLayout 只在编辑器主页面显示，StartScreen / launcher 等不显示
const isEditorRoute = computed(() => route.path === '/');

const centerPanels = computed(() => dockStore.panelsByZone('center'));

let gcTimer = null;
let editorCameraInputDisabledForFocus = false;
let editorCameraInputFocusTimer = null;

function isEscapeKey(event) {
  const modifierKeys = new Set([
    'Shift', 'Control', 'Alt', 'Meta',
    'ShiftLeft', 'ShiftRight',
    'ControlLeft', 'ControlRight',
    'AltLeft', 'AltRight',
    'MetaLeft', 'MetaRight',
  ]);
  if (modifierKeys.has(event.key) || modifierKeys.has(event.code)) return false;
  return event.key === 'Escape' && (event.code === 'Escape' || event.keyCode === 27 || event.which === 27);
}

function isEditableTarget(target) {
  const tag = target?.tagName;
  return Boolean(
    target?.isContentEditable ||
      tag === 'INPUT' ||
      tag === 'TEXTAREA' ||
      tag === 'SELECT'
  );
}

function onGlobalKeyDown(event) {
  if (event.defaultPrevented) return;
  if (!isEditorRoute.value || !isEscapeKey(event)) return;

  const settingsOpen = Boolean(dockStore.panels.EditorSettings?.open);
  if (isEditableTarget(event.target) && !settingsOpen) return;

  event.preventDefault();
  event.stopPropagation();
  dockStore.togglePanel('EditorSettings');
}

function canCallCEF() {
  return typeof window !== 'undefined' && typeof window.cefQuery === 'function';
}

function setEditorCameraInputForFocus(enabled) {
  if (enabled && window.__coronaGamePreviewInputLocked) return;
  if (!canCallCEF()) return;
  Bridge.callCEF('CoronaEditor', 'set_editor_camera_input_enabled', [enabled, 'focus']).catch(() => {});
}

function syncEditorCameraInputFocusGate() {
  const shouldDisable = shouldDisableEditorCameraInput(document);
  if (shouldDisable === editorCameraInputDisabledForFocus) return;
  editorCameraInputDisabledForFocus = shouldDisable;
  setEditorCameraInputForFocus(!shouldDisable);
}

function scheduleEditorCameraInputFocusGate() {
  if (editorCameraInputFocusTimer) {
    window.clearTimeout(editorCameraInputFocusTimer);
  }
  editorCameraInputFocusTimer = window.setTimeout(() => {
    editorCameraInputFocusTimer = null;
    syncEditorCameraInputFocusGate();
  }, 0);
}

function releaseEditorCameraInputFocusGate() {
  if (editorCameraInputFocusTimer) {
    window.clearTimeout(editorCameraInputFocusTimer);
    editorCameraInputFocusTimer = null;
  }
  if (!editorCameraInputDisabledForFocus) return;
  editorCameraInputDisabledForFocus = false;
  setEditorCameraInputForFocus(true);
}

onMounted(() => {
  gcTimer = setInterval(() => {
    if (typeof window.gc === 'function') {
      try {
        window.gc();
      } catch {}
    }
  }, 60000);

  document.addEventListener('keydown', onGlobalKeyDown, true);
  document.addEventListener('focusin', syncEditorCameraInputFocusGate, true);
  document.addEventListener('focusout', scheduleEditorCameraInputFocusGate, true);
  window.addEventListener('blur', releaseEditorCameraInputFocusGate);
});

onUnmounted(() => {
  if (gcTimer) {
    clearInterval(gcTimer);
    gcTimer = null;
  }
  document.removeEventListener('keydown', onGlobalKeyDown, true);
  document.removeEventListener('focusin', syncEditorCameraInputFocusGate, true);
  document.removeEventListener('focusout', scheduleEditorCameraInputFocusGate, true);
  window.removeEventListener('blur', releaseEditorCameraInputFocusGate);
  releaseEditorCameraInputFocusGate();
});
</script>

<template>
  <DockLayout v-if="isEditorRoute" />
  <router-view v-else />

  <!-- 全局中心面板覆盖层（所有页面可用） -->
  <template v-for="p in centerPanels" :key="p.id">
    <div class="global-center-overlay" @mousedown.self="dockStore.closePanel(p.id)">
      <div class="global-center-overlay-panel" :style="{ width: p.width + 'px', height: p.height + 'px' }">
        <DockPanel :panel-id="p.id" :component="getPluginManifest(p.id)?.component" />
      </div>
    </div>
  </template>
</template>

<style>
.global-center-overlay {
  position: fixed;
  inset: 0;
  z-index: 100000;
  background: rgba(0, 0, 0, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
}
.global-center-overlay-panel {
  max-width: 90vw;
  max-height: 85vh;
  border-radius: 8px;
  box-shadow: 0 8px 32px rgba(0, 0, 0, 0.6);
  display: flex;
  flex-direction: column;
  overflow: hidden;
}
</style>
