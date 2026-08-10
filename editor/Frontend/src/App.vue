<script setup>
import { computed, onMounted, onUnmounted } from 'vue';
import { useRoute } from 'vue-router';
import { useDockStore } from '@/stores/dockStore.js';
import { getPluginComponent } from '@/views/panelRegistry.js';
import DockLayout from '@/components/dock/DockLayout.vue';
import DockPanel from '@/components/dock/DockPanel.vue';
import { editorApi } from '@/api/editorApi.js';
import lanchat from '@/stores/lanchat.js';
import '@/utils/eventBus.js'; // init window.__coronaEmit

const route = useRoute();
const dockStore = useDockStore();

// DockLayout 只在编辑器主页面显示，StartScreen / launcher 等不显示
const isEditorRoute = computed(() => route.path === '/');
const isStandalonePanel = computed(() => route.query?.standalone === '1');

const centerPanels = computed(() => dockStore.panelsByZone('center'));
const standaloneResizeHandles = [
  'n',
  'e',
  's',
  'w',
  'se',
  'sw',
  'nw',
];

let gcTimer = null;
let lanChatEventCallbackToken = null;
let appUnmounted = false;
const SCRATCH_KEY_FORWARDED = '__coronaScratchKeyForwarded';

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

function forwardScratchKey(event, released = false) {
  // Inside the native editor SDL already feeds the engine input queue.
  // Forwarding the same DOM event would duplicate every gameplay input.
  if (window.coronaBridge) return;
  if (event[SCRATCH_KEY_FORWARDED]) return;
  if (!released && isEditableTarget(event.target)) return;

  const code = event.code || event.key || '';
  const displayKey = event.key || event.code || '';
  if (!code) return;

  event[SCRATCH_KEY_FORWARDED] = true;
  if (released) {
    editorApi.scratch.sendKeyUpEvent(code, displayKey).catch(() => {});
    return;
  }
  const modifiers = [
    event.ctrlKey || event.metaKey ? 'Ctrl' : '',
    event.shiftKey ? 'Shift' : '',
    event.altKey ? 'Alt' : '',
  ].filter(Boolean).join(',');
  editorApi.scratch.sendKeyEvent(code, modifiers, displayKey).catch(() => {});
}

function isGamePreviewActive() {
  const preview = window.__coronaGamePreviewState || {};
  return ['starting', 'running', 'stopping'].includes(preview.status)
    || Number(preview.runningCount ?? preview.running_count ?? 0) > 0
    || Boolean(preview.hasSnapshot ?? preview.has_snapshot);
}

function isGameplayKey(event) {
  if (event.ctrlKey || event.metaKey || event.altKey) return false;
  const code = event.code || '';
  return code === 'Space'
    || code === 'KeyW' || code === 'KeyA' || code === 'KeyS' || code === 'KeyD'
    || code === 'ArrowUp' || code === 'ArrowDown'
    || code === 'ArrowLeft' || code === 'ArrowRight';
}

function consumeNativeGameplayDomEvent(event) {
  if (!isGamePreviewActive() || isEditableTarget(event.target) || !isGameplayKey(event)) return;
  // The native SDL event still reaches Scratch. Suppress only the mirrored DOM
  // event so Space cannot click the still-focused preview button and WASD does
  // not trigger editor/browser shortcuts while a game is running.
  event.preventDefault();
  event.stopPropagation();
}

function onGlobalKeyDown(event) {
  consumeNativeGameplayDomEvent(event);
  forwardScratchKey(event, false);
  if (event.defaultPrevented) return;
  if (!isEditorRoute.value || !isEscapeKey(event)) return;

  const settingsOpen = Boolean(dockStore.panels.EditorSettings?.open);
  if (isEditableTarget(event.target) && !settingsOpen) return;

  event.preventDefault();
  event.stopPropagation();
  dockStore.togglePanel('EditorSettings');
}

function onGlobalKeyUp(event) {
  consumeNativeGameplayDomEvent(event);
  forwardScratchKey(event, true);
}

function onLanChatEvent(payload) {
  lanchat.handleEvent(payload);
}

async function registerLanChatEvent() {
  try {
    const callbackToken = await editorApi.events.onLanChatEvent(onLanChatEvent);
    if (appUnmounted) {
      await editorApi.off(callbackToken);
      return;
    }
    lanChatEventCallbackToken = callbackToken;
  } catch (error) {
    if (!appUnmounted) {
      console.warn('[App] failed to subscribe to LANChat events', error);
    }
  }
}

onMounted(() => {
  appUnmounted = false;
  if (!isStandalonePanel.value) {
    void registerLanChatEvent();
  }

  gcTimer = setInterval(() => {
    if (typeof window.gc === 'function') {
      try {
        window.gc();
      } catch {}
    }
  }, 60000);

  document.addEventListener('keydown', onGlobalKeyDown, true);
  document.addEventListener('keyup', onGlobalKeyUp, true);
});

onUnmounted(() => {
  appUnmounted = true;
  if (lanChatEventCallbackToken) {
    const callbackToken = lanChatEventCallbackToken;
    lanChatEventCallbackToken = null;
    editorApi.off(callbackToken).catch((error) => {
      console.warn('[App] failed to unsubscribe from LANChat events', error);
    });
  }

  if (gcTimer) {
    clearInterval(gcTimer);
    gcTimer = null;
  }
  document.removeEventListener('keydown', onGlobalKeyDown, true);
  document.removeEventListener('keyup', onGlobalKeyUp, true);
});
</script>

<template>
  <DockLayout v-if="isEditorRoute" :component-resolver="getPluginComponent" />
  <div v-else :class="isStandalonePanel ? 'standalone-route-shell' : null">
    <router-view />
    <template v-if="isStandalonePanel">
      <div
        v-for="handle in standaloneResizeHandles"
        :key="handle"
        class="standalone-resize-handle"
        :class="`standalone-resize-handle--${handle}`"
        aria-hidden="true"
      ></div>
    </template>
  </div>

  <!-- 全局中心面板覆盖层（所有页面可用） -->
  <template v-for="p in centerPanels" :key="p.id">
    <div class="global-center-overlay" @mousedown.self="dockStore.closePanel(p.id)">
      <div class="global-center-overlay-panel" :style="{ width: p.width + 'px', height: p.height + 'px' }">
        <DockPanel :panel-id="p.id" :component="getPluginComponent(p.id)" />
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

.standalone-route-shell {
  position: relative;
  width: 100vw;
  height: 100vh;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
}

.standalone-resize-handle {
  position: fixed;
  z-index: 1000000;
  pointer-events: auto;
  background: transparent;
}

.standalone-resize-handle--n,
.standalone-resize-handle--s {
  left: 12px;
  right: 12px;
  cursor: ns-resize;
}

.standalone-resize-handle--n {
  top: 0;
  right: 80px;
  height: 6px;
}

.standalone-resize-handle--s {
  bottom: 0;
  height: 8px;
}

.standalone-resize-handle--e,
.standalone-resize-handle--w {
  top: 12px;
  bottom: 12px;
  width: 8px;
  cursor: ew-resize;
}

.standalone-resize-handle--e {
  right: 0;
  top: 32px;
}

.standalone-resize-handle--w {
  left: 0;
}

.standalone-resize-handle--se,
.standalone-resize-handle--sw,
.standalone-resize-handle--nw {
  width: 14px;
  height: 14px;
}

.standalone-resize-handle--se {
  right: 0;
  bottom: 0;
  cursor: nwse-resize;
}

.standalone-resize-handle--sw {
  bottom: 0;
  left: 0;
  cursor: nesw-resize;
}

.standalone-resize-handle--nw {
  top: 0;
  left: 0;
  cursor: nwse-resize;
}
</style>
