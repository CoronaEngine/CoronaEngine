<script setup>
import { computed, onMounted, onUnmounted, ref, watch } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { useDockStore } from '@/stores/dockStore.js';
import { getPluginComponent } from '@/views/panelRegistry.js';
import DockLayout from '@/components/dock/DockLayout.vue';
import DockPanel from '@/components/dock/DockPanel.vue';
import { editorApi } from '@/api/editorApi.js';
import { appService } from '@/services/appService.js';
import { LAUNCHER_ROUTES, normalizeProjectPath, editorUiAllowed, worldModeService, worldModeState } from '@/services/worldModeService.js';
import lanchat from '@/stores/lanchat.js';
import { cancelPendingProjectOpen } from '@/services/projectLauncherService.js';
import { notifyWorldError } from '@/services/worldSessionLifecycle.js';
import '@/utils/eventBus.js'; // init window.__coronaEmit

const route = useRoute();
const router = useRouter();
const dockStore = useDockStore();

// DockLayout 只在编辑器主页面显示，StartScreen / launcher 等不显示
const isEditorRoute = computed(() => route.path === '/' && editorUiAllowed());
const isLauncherRoute = computed(() => LAUNCHER_ROUTES.has(route.path));
const preparedRevision = ref(-1);
const worldReady = computed(() => worldModeState.status === 'ready'
  && preparedRevision.value === worldModeState.revision);
const mayRenderRoute = computed(() => isLauncherRoute.value || (worldReady.value
  && (route.path === '/' || editorUiAllowed())));
const isStandalonePanel = computed(() => route.query?.standalone === '1');

const centerPanels = computed(() => isEditorRoute.value && worldReady.value
  ? dockStore.panelsByZone('center') : []);
const standaloneResizeHandles = [
  'n',
  'e',
  's',
  'w',
  'se',
  'sw',
  'nw',
];

let appUnmounted = false;
let projectOpenedToken = null;
let refreshRevision = 0;
let preparationRevision = 0;

// Only the main surface controls native windows. Standalone pages independently
// resolve the same native metadata before mounting their panel component.
watch(() => [worldModeState.status, worldModeState.revision, route.path, route.matched.length], async () => {
  if (!route.matched.length) return;
  const preparation = ++preparationRevision;
  const revision = worldModeState.revision;
  const opening = worldModeService.opening;
  preparedRevision.value = -1;
  try {
    if (!isStandalonePanel.value && typeof window.coronaBridge?.dockCommand === 'function') {
      await appService.setEditorUiEnabled(editorUiAllowed() && !isLauncherRoute.value);
    } else if (isStandalonePanel.value && typeof window.coronaBridge?.dockCommand === 'function') {
      const policy = await appService.readEditorUiPolicy();
      if (!policy?.enabled) { await appService.closeThisTab(''); return; }
    }
    if (preparation !== preparationRevision || revision !== worldModeState.revision || appUnmounted) return;
    if (worldModeState.status !== 'ready') return;
    if (worldModeState.mode === 'story') {
      if (isStandalonePanel.value) {
        await appService.closeThisTab('');
        return;
      }
      dockStore.clearSession();
      await lanchat.finishWorldSession();
      if (!isLauncherRoute.value && route.path !== '/') await router.replace('/');
    }
    if (preparation === preparationRevision && revision === worldModeState.revision && !appUnmounted) preparedRevision.value = revision;
  } catch (error) {
    if (preparation !== preparationRevision || revision !== worldModeState.revision || appUnmounted) return;
    if (opening) return; // The opening caller owns the failure and its single notification.
    if (isStandalonePanel.value) { await appService.closeThisTab('').catch(() => {}); return; }
    await router.replace('/StartScreen');
    notifyWorldError(error, '世界界面初始化失败');
  }
}, { immediate: true });

async function refreshWorldMode(projectPath = '', force = false) {
  if (worldModeService.opening) return;
  if (!force && worldModeState.status === 'ready' && projectPath
    && normalizeProjectPath(projectPath) === normalizeProjectPath(worldModeState.projectPath)) return;
  const request = ++refreshRevision;
  try {
    await worldModeService.resolve(projectPath, { force: true });
  } catch (error) {
    if (request !== refreshRevision || appUnmounted) return;
    if (isStandalonePanel.value) {
      await appService.closeThisTab('').catch(() => {});
      return;
    }
    await router.replace('/StartScreen');
    notifyWorldError(error, '读取世界模式失败');
  }
}
function onActiveProjectChanged(event) {
  void refreshWorldMode(event.detail?.projectPath || '');
}
function onProjectStorageChanged(event) {
  if (event.key === 'corona.activeProjectPath') void refreshWorldMode(event.newValue || '');
}

let gcTimer = null;
let lanChatEventCallbackToken = null;
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
  // The route may not be mounted yet while native mode/window preparation waits.
  if (isEscapeKey(event) && !isStandalonePanel.value
    && (worldModeService.opening || (!isLauncherRoute.value && !worldReady.value))) {
    event.preventDefault();
    event.stopPropagation();
    cancelPendingProjectOpen();
    void router.replace('/StartScreen');
    return;
  }
  if (isLauncherRoute.value || !editorUiAllowed() || !worldReady.value) return;
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
  if (isLauncherRoute.value || !editorUiAllowed() || !worldReady.value) return;
  consumeNativeGameplayDomEvent(event);
  forwardScratchKey(event, true);
}

function onLanChatEvent(payload) {
  if (editorUiAllowed()) lanchat.handleEvent(payload);
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
  window.addEventListener('corona-active-project-changed', onActiveProjectChanged);
  window.addEventListener('storage', onProjectStorageChanged);
  void editorApi.events.onProjectOpened((payload) => {
    void refreshWorldMode(payload?.path || '', true);
  }).then((token) => {
    if (appUnmounted) return editorApi.off(token);
    projectOpenedToken = token;
  }).catch((error) => console.warn('[App] project mode subscription failed', error));
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
  ++refreshRevision;
  window.removeEventListener('corona-active-project-changed', onActiveProjectChanged);
  window.removeEventListener('storage', onProjectStorageChanged);
  if (projectOpenedToken) void editorApi.off(projectOpenedToken).catch(() => {});
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
  <DockLayout v-if="isEditorRoute && worldReady" :key="worldModeState.projectPath" :component-resolver="getPluginComponent" />
  <div v-else-if="mayRenderRoute" :key="isLauncherRoute ? route.path : worldModeState.projectPath" :class="isStandalonePanel ? 'standalone-route-shell' : null">
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
