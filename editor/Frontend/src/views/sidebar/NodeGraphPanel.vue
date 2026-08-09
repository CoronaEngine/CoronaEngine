<template>
  <div class="node-graph-panel flex h-full min-h-0 w-full flex-1 flex-col overflow-hidden">
    <DockTitleBar
      v-if="!isDocked"
      title="节点"
      routePath="/NodeGraph"
      @close="closeFloat"
    />
    <NodeGraphWorkspace
      ref="workspaceRef"
      class="min-h-0 flex-1"
      actor-name=""
      :scene-name="sceneName"
      target-type="project"
      :review-active="true"
    />
  </div>
</template>

<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue';
import NodeGraphWorkspace from '@/blockly/components/NodeGraphWorkspace.vue';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import { useDockPanel } from '@/composables/useDockPanel.js';
import { appService } from '@/services/appService.js';
import { coronaEventBus } from '@/utils/eventBus.js';

const { closePanel, isDocked } = useDockPanel();
// Keep the route empty until MainPage reports the active scene. Passing the generic
// DEFAULT_SCENE_NAME into listActorTree can make the native scene router reload a
// different scene, which resets the main viewport camera when this panel opens.
const sceneName = ref('');
const workspaceRef = ref(null);
let closeStopPromise = null;

function applyViewportState(state = {}) {
  const nextSceneName = String(state?.sceneId || '').trim();
  if (nextSceneName && nextSceneName !== sceneName.value) {
    sceneName.value = nextSceneName;
  }
}

function requestViewportState() {
  const controls = window.__coronaEditorControls;
  if (controls && typeof controls.getState === 'function') {
    try {
      applyViewportState(controls.getState());
      return;
    } catch {
      // A detached panel does not share the main page window; use the cross-tab path below.
    }
  }
  appService.crossTabBroadcast('viewport-controls-request', { action: 'getState' }).catch(() => {});
}

function stopNodeRunForClose() {
  if (!closeStopPromise) {
    closeStopPromise = Promise.resolve(workspaceRef.value?.stopForPanelClose?.());
  }
  return closeStopPromise;
}
function handleWindowClosing() {
  stopNodeRunForClose().catch(() => {});
}

onMounted(() => {
  // Read the scene initialized by MainPage. Calling MainView.on_init here would reset
  // the scene and camera every time the node panel is opened.
  coronaEventBus.on('viewport-controls-state', applyViewportState);
  window.addEventListener('pagehide', handleWindowClosing);
  window.addEventListener('beforeunload', handleWindowClosing);
  requestViewportState();
});

onBeforeUnmount(() => {
  coronaEventBus.off('viewport-controls-state', applyViewportState);
  window.removeEventListener('pagehide', handleWindowClosing);
  window.removeEventListener('beforeunload', handleWindowClosing);
  stopNodeRunForClose().catch(() => {});
});

async function closeFloat() {
  try {
    await stopNodeRunForClose();
  } finally {
    closePanel();
  }
}
</script>

<style scoped>
.node-graph-panel {
  position: relative;
  z-index: 2147483100;
  background: linear-gradient(180deg, rgba(33, 29, 18, 0.72), rgba(17, 16, 13, 0.7));
  border: 1px solid rgba(216, 184, 108, 0.18);
  border-radius: 8px;
  box-shadow: 0 18px 42px rgba(0, 0, 0, 0.34);
}
</style>
