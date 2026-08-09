<template>
  <Teleport to="body" :disabled="!isFullscreen">
  <div
    ref="workspaceRootRef"
    class="node-graph-workspace"
    :class="{ fullscreen: isFullscreen, compact: isCompactLayout, narrow: isNarrowLayout }"
    :style="layoutStyle"
  >
    <div class="ng-toolbar">
      <div class="ng-title">
        <span class="ng-badge">节点</span>
      </div>
      <div class="ng-modes">
        <button
          type="button"
          class="ng-run"
          data-guidance="node-run"
          :class="{ running: codeRunning }"
          :disabled="runBusy || (globalPreviewActive && !codeRunning)"
          :title="globalPreviewActive && !codeRunning ? globalPreviewRunLabel : ''"
          @click.stop="handleRunButtonClick"
        >
          {{ codeRunning ? '停止' : '运行' }}
        </button>
        <span v-if="runStatus" class="ng-run-status" :title="runDetail || runStatus" @click="toggleRunDetail">{{ runStatus }}</span>
        <button class="ng-mode" data-guidance="node-select-mode" :class="{ active: mode === 'select' }" @click="setMode('select')">
          选择
        </button>
        <button
          class="ng-mode delete"
          data-guidance="node-delete-mode"
          :class="{ active: mode === 'delete' }"
          @click="setMode('delete')"
        >
          清除
        </button>
      </div>
    </div>
    <div v-if="!targetReady" class="ng-empty">请先选中一个物体</div>
    <div v-else class="ng-body">
      <aside class="ng-panel ng-toolbox" data-guidance="node-toolbox">
        <div class="ng-section-title">
          宏观节点
          <small>拖入中间</small>
        </div>
        <div
          class="ng-tool-card macro"
          data-guidance="node-state-tool"
          draggable="true"
          @pointerdown.left="beginMacroPointerDrag($event, 'state')"
          @dragstart="startMacroDrag($event, 'state')"
        >
          <div class="ng-tool-icon">态</div>
          <div>
            <div class="ng-tool-name">状态节点</div>
            <div class="ng-tool-desc">开始 / 结束 / 自定义状态</div>
          </div>
        </div>
        <div class="ng-section-title mt">
          {{ paletteWorkspaceRole === 'condition' ? '返回值积木' : '微观积木' }}
          <small v-if="paletteWorkspaceRole === 'condition'">用于组合跳转条件</small>
        </div>
        <BlocklyToolboxPalette
          ref="paletteRef"
          class="ng-native-palette"
          :workspace-role="paletteWorkspaceRole"
          @pick="handlePalettePick"
          @external-drag-start="handlePaletteDragStart"
          @external-drag-move="handlePaletteDragMove"
          @external-drag-end="handlePaletteDragEnd"
        />
      </aside>
      <div class="ng-splitter ng-toolbox-splitter vertical" title="拖动调整工具箱宽度" @pointerdown="beginLayoutResize($event, 'toolbox')"></div>
      <main
        ref="canvasRef"
        class="ng-panel ng-canvas" :class="{ 'drop-active': macroDropActive, panning: isCanvasPanning }"
        data-guidance="node-canvas"
        @dragover.prevent
        @drop.prevent="handleCanvasDrop"
        @wheel.prevent="handleCanvasWheel"
        @pointerdown="beginCanvasPan"
        @pointermove="handleCanvasPointerMove"
        @pointerleave="handleCanvasPointerLeave"
        @click="handleCanvasClick"
      >
        <div class="ng-canvas-head">
          <div>
            <strong>节点编辑区</strong>
            <small class="ng-canvas-hint">空白处按住拖动画布 · 中键拖动 · 滚轮缩放</small>
          </div>
          <div class="ng-canvas-actions">
            <span class="ng-zoom-value">{{ zoomText }}</span>
            <button type="button" class="ng-zoom-reset" @click.stop="resetZoom">恢复 100%</button>
          </div>
        </div>
        <div class="ng-world" :style="worldStyle">
          <div class="ng-grid"></div>
          <svg class="ng-edges" :width="canvasSize.width" :height="canvasSize.height">
          <defs>
            <marker
              id="ng-arrow"
              markerWidth="10"
              markerHeight="10"
              refX="9"
              refY="3"
              orient="auto"
              markerUnits="strokeWidth"
            >
              <path d="M0,0 L0,6 L9,3 z" fill="#d8b86c" />
            </marker>
          </defs>
          <path
            v-if="pendingEdgePath"
            class="ng-edge-preview"
            :d="pendingEdgePath"
          />
          <g v-for="edge in edges" :key="edge.id" :data-edge-id="edge.id">
            <path
              class="ng-edge-hit"
              :d="edgePath(edge)"
              @click.stop="handleEdgeClick(edge)"
              @dblclick.stop="renameEdge(edge)"
            />
            <path
              class="ng-edge-line"
              :class="{
                selected: selectedKind === 'edge' && selectedId === edge.id,
                delete: mode === 'delete',
              }"
              :d="edgePath(edge)"
              marker-end="url(#ng-arrow)"
            />
          </g>
        </svg>
        <div
          v-for="edge in edges"
          :key="`${edge.id}-condition`"
          class="ng-condition-block"
          :data-edge-id="edge.id"
          data-guidance="node-transition-condition"
          :class="{ selected: selectedKind === 'edge' && selectedId === edge.id }"
          :style="conditionStyle(edge)"
          @click.stop="selectEdge(edge)"
          @dblclick.stop="renameEdge(edge)"
        >
          {{ edge.name || '条件' }}
        </div>
        <div
          v-for="node in nodes"
          :key="node.id"
          class="ng-node"
          :data-node-id="node.id"
          :class="[
            `type-${node.nodeType}`,
            {
              selected: selectedKind === 'node' && selectedId === node.id,
              running: currentRunNodeId === node.id,
              delete: mode === 'delete',
            },
          ]"
          :style="nodeStyle(node)"
          @mousedown.left="startNodeDrag($event, node)"
          @click.stop="handleNodeClick(node)"
        >
          <div class="ng-node-head">
            <span class="ng-node-dot"></span>
            <span class="ng-node-type-badge">{{ nodeTypeLabel(node.nodeType) }}</span>
          </div>
          <div class="ng-node-display-name">{{ displayNodeName(node) }}</div>
          <template v-for="port in visiblePorts(node)" :key="`${node.id}-${port.side}-${port.index}`">
            <button
              v-if="shouldShowPort(node, port)"
              type="button"
              class="ng-port"
              :data-node-id="node.id"
              :data-port-side="port.side"
              :data-port-index="port.index"
              :class="[
                port.side,
                {
                  occupied: isPortUsed(node.id, port),
                  pending:
                    pendingPort &&
                    pendingPort.nodeId === node.id &&
                    pendingPort.side === port.side &&
                    pendingPort.index === port.index,
                  'connection-target': pendingPort && pendingPort.nodeId !== node.id,
                },
              ]"
              :style="portStyle(node, port)"
              :title="isPortUsed(node.id, port) ? '已连接' : '空连接点'"
              @click.stop="handlePortClick(node, port)"
            ></button>
          </template>
        </div>
        </div>
      </main>
      <div class="ng-splitter ng-inspector-splitter vertical" title="拖动调整内部编辑区宽度" @pointerdown="beginLayoutResize($event, 'inspector')"></div>
      <aside ref="inspectorRef" class="ng-panel ng-inspector">
        <section class="ng-vars">
          <div class="ng-section-title">
            全局变量池
          </div>
          <MiniBlocklyWorkspace
            ref="variablesBlocklyRef"
            :workspace-key="`${targetKey}:globals`"
            :initial-state="graph.globalVariablesWorkspace"
            :delete-mode="mode === 'delete'"
            workspace-role="global"
            @change="onGlobalWorkspaceChange"
            @reject="onWorkspaceReject"
            @block-added="onBlockAdded"
            @block-changed="onBlockChanged"
            @block-connected="onBlockConnected"
          />
        </section>
        <div class="ng-splitter horizontal" title="拖动调整全局变量池高度" @pointerdown="beginLayoutResize($event, 'variables')"></div>
        <section class="ng-editor" data-guidance="node-blockly-editor">
          <div class="ng-section-title">
            {{ activeEditorTitle }}
            <small>{{ activeEditorSubtitle }}</small>
          </div>
          <div v-if="selectedNode" class="ng-property-card">
            <label class="ng-property-label">节点类型</label>
            <div class="ng-type-tabs">
              <button
                v-for="option in nodeTypeOptions"
                :key="option.value"
                type="button"
                :class="{ active: selectedNode.nodeType === option.value }"
                :data-guidance="option.value === 'custom'
                  ? 'node-type-custom'
                  : option.value === 'start'
                    ? 'node-type-start'
                    : undefined"
                @click="setSelectedNodeType(option.value)"
              >
                {{ option.label }}
              </button>
            </div>
            <template v-if="selectedNode.nodeType === 'custom'">
              <label class="ng-property-label">节点名称</label>
              <div class="ng-name-row">
                <input
                  :value="selectedNode.customName"
                  maxlength="24"
                  placeholder="自定义节点"
                  @input="updateSelectedNodeName($event.target.value)"
                />
                <span>{{ String(selectedNode.customName || '').length }}/24</span>
              </div>
            </template>
          </div>
          <div v-else-if="selectedEdge" class="ng-property-card">
            <label class="ng-property-label">连线名称</label>
            <div class="ng-name-row">
              <input
                ref="edgeNameInputRef"
                :value="selectedEdge.name"
                maxlength="24"
                placeholder="条件"
                @input="updateSelectedEdgeName($event.target.value)"
              />
              <span>{{ String(selectedEdge.name || '').length }}/24</span>
            </div>
            <div class="ng-edge-meta">
              <span>起点：{{ edgeNodeName(selectedEdge.source.nodeId) }}</span>
              <span>终点：{{ edgeNodeName(selectedEdge.target.nodeId) }}</span>
            </div>
            <div class="ng-condition-note">{{ edgeConditionNote }}</div>
          </div>
          <MiniBlocklyWorkspace
            v-if="activeEditorKey"
            ref="activeBlocklyRef"
            :key="activeEditorKey"
            :workspace-key="activeEditorKey"
            :initial-state="activeEditorState"
            :delete-mode="mode === 'delete'"
            :workspace-role="selectedEdge ? 'condition' : 'node'"
            @change="onActiveWorkspaceChange"
            @reject="onWorkspaceReject"
            @block-added="onBlockAdded"
            @block-changed="onBlockChanged"
            @block-connected="onBlockConnected"
          />
          <div v-else class="ng-editor-empty">选择节点或连线后可编辑内部积木</div>
        </section>
      </aside>
    </div>
    <div v-if="runDetailVisible && runDetail" class="ng-run-detail">
      <div class="ng-run-detail-head">
        <strong>运行诊断</strong>
        <button type="button" @click="copyRunDetail">复制</button>
      </div>
      <pre>{{ runDetail }}</pre>
    </div>
  </div>
  <Teleport to="body">
    <div
      v-if="externalDrag.active"
      class="ng-drag-ghost"
      :style="dragGhostStyle"
    >
      {{ externalDrag.label }}
    </div>
  </Teleport>
  </Teleport>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, reactive, ref, watch } from 'vue';
import MiniBlocklyWorkspace from '@/blockly/components/MiniBlocklyWorkspace.vue';
import BlocklyToolboxPalette from '@/blockly/components/BlocklyToolboxPalette.vue';
import { useErrorHandler } from '@/composables/useErrorHandler.js';
import { editorApi } from '@/api/editorApi.js';
import { appService } from '@/services/appService.js';
import { coronaEventBus } from '@/utils/eventBus.js';
import { nodeGraphToCode, validateNodeGraph } from '@/blockly/generators/index.js';
import { generatedNodeGraphRevision, registerGeneratedNodeGraphConsumer } from '@/blockly/node-editor/aiNodeGraphService.js';
import { actorContextNameKey, actorContextRevision, actorRecordsFromSceneTree, normalizeActorContextName } from '@/blockly/utils/actorContext.js';
import { nextSaveAction, shouldResumeBlockedSave } from '@/blockly/utils/saveRetryPolicy.js';
import { graphRevision as reviewGraphRevision, reviewScopeId, startNodeGraphReview } from '@/services/nodeGraphReviewService.js';
import { useCabbageAssistantStore } from '@/stores/cabbageAssistantStore.js';
import {
  cabbageContextService,
  publishCabbageAssistantContext,
  publishCabbagePreWarning,
  publishCabbageProjectContext,
  readCabbageAssistantContext,
  subscribeCabbageAssistantContext,
} from '@/services/cabbageAssistantContextService.js';
import {
  publishProjectNodeGraphRuntimeState,
  registerProjectNodeGraphRuntimeHandler,
  registerProjectNodeGraphSaveHandler,
} from '@/services/nodeGraphRuntimeService.js';

const props = defineProps({
  actorName: { type: String, default: '' },
  sceneName: { type: String, default: '' },
  targetType: { type: String, default: 'actor' },
  reviewActive: { type: Boolean, default: true },
});
const { error: logError } = useErrorHandler('NodeGraphWorkspace');
const cabbageAssistant = useCabbageAssistantStore();
const NODE_WIDTH = 170,
  NODE_BASE_HEIGHT = 98,
  NODE_PORT_GAP = 20,
  CANVAS_WORLD_WIDTH = 4800,
  CANVAS_WORLD_HEIGHT = 3200,
  CANVAS_OVERSCROLL = 180,
  SAVE_DELAY = 300;
const isFullscreen = ref(false);
const fullscreenTransitionBusy = ref(false);
const mode = ref('select'),
  selectedKind = ref(''),
  selectedId = ref(''),
  pendingPort = ref(null),
  saveLabel = ref('');
const workspaceRootRef = ref(null),
  canvasRef = ref(null),
  paletteRef = ref(null),
  inspectorRef = ref(null),
  variablesBlocklyRef = ref(null),
  activeBlocklyRef = ref(null),
  edgeNameInputRef = ref(null);
const canvasSize = reactive({ width: CANVAS_WORLD_WIDTH, height: CANVAS_WORLD_HEIGHT });
const workspaceSize = reactive({ width: 0, height: 0 });
const isCompactLayout = computed(() => workspaceSize.width > 0 && workspaceSize.width < 1180);
const isNarrowLayout = computed(() => workspaceSize.width > 0 && workspaceSize.width < 680);
const viewport = reactive({ scale: 1, offsetX: 0, offsetY: 0 });
const isCanvasPanning = ref(false);
const connectionPointer = reactive({ active: false, x: 0, y: 0 });
const graph = reactive({ version: 1, nodes: [], edges: [], globalVariablesWorkspace: {} });
const codeRunning = ref(false);
const runBusy = ref(false);
const globalPreviewActive = ref(false);
const globalPreviewScope = ref('');
const runStatus = ref('');
const runDetail = ref('');
const runDetailVisible = ref(false);
const currentRunNodeId = ref('');
const runWarnings = ref([]);
const globalPreviewRunLabel = computed(() =>
  globalPreviewScope.value === 'scene'
    ? '当前节点图正在由全局运行执行'
    : '当前节点图正在由项目预览执行'
);
const NODE_GRAPH_INPUT_LOCK = 'node_graph';
function setEditorInputLock(reason, locked) {
  const locks = window.__coronaEditorInputLocks instanceof Set
    ? window.__coronaEditorInputLocks
    : new Set();
  window.__coronaEditorInputLocks = locks;
  if (locked) locks.add(reason);
  else locks.delete(reason);
  window.__coronaGamePreviewInputLocked = locks.size > 0;
}
function setNodeGraphInputLocked(locked) {
  setEditorInputLock(NODE_GRAPH_INPUT_LOCK, Boolean(locked));
}
const macroDropActive = ref(false);
const externalDrag = reactive({
  active: false,
  kind: '',
  blockType: '',
  macroType: '',
  label: '',
  clientX: 0,
  clientY: 0,
  pointerId: null,
});
const LAYOUT_STORAGE_KEY = 'corona-nodegraph-layout-v5';
const LAYOUT_LIMITS = Object.freeze({
  toolboxMin: 260,
  toolboxMax: 520,
  toolboxEmergencyMin: 220,
  inspectorMin: 360,
  inspectorMax: 620,
  inspectorEmergencyMin: 300,
  canvasMin: 360,
  gridChrome: 60,
});
const DEFAULT_LAYOUT = Object.freeze({ toolboxWidth: 360, inspectorWidth: 460, variablesHeight: 180 });
const layout = reactive({ ...DEFAULT_LAYOUT });
const effectiveVariablesHeight = computed(() => {
  if (!workspaceSize.height) return layout.variablesHeight;
  const ratio = isCompactLayout.value ? 0.2 : 0.4;
  return Math.max(110, Math.min(layout.variablesHeight, Math.floor(workspaceSize.height * ratio)));
});
const layoutStyle = computed(() => ({
  '--toolbox-width': `${layout.toolboxWidth}px`,
  '--inspector-width': `${layout.inspectorWidth}px`,
  '--variables-height': `${effectiveVariablesHeight.value}px`,
}));
async function toggleFullscreen() {
  if (fullscreenTransitionBusy.value) return;
  const next = !isFullscreen.value;
  fullscreenTransitionBusy.value = true;

  // The Object panel runs in an independent CEF surface. A CSS overlay cannot grow beyond
  // that surface, so detach the whole panel first and let the node editor fill the
  // resulting native window. Explicit desired-state commands avoid reversing a
  // still-pending detach transition.
  const screenWidth = Number(window.screen?.availWidth || window.screen?.width || 1368);
  const screenHeight = Number(window.screen?.availHeight || window.screen?.height || 768);
  // SDL expects physical pixels while the browser exposes screen dimensions in CSS pixels.
  // Scale the requested detached window geometry so fullscreen stays fullscreen on high-DPI
  // displays instead of shrinking to roughly 1 / devicePixelRatio of the desktop.
  const deviceScale = Math.max(1, Number(window.devicePixelRatio || 1));
  const marginX = 16;
  const marginTop = 16;
  const marginBottom = 56;
  const availableWidth = Math.max(1100, Math.round((screenWidth - marginX * 2) * deviceScale));
  const availableHeight = Math.max(700, Math.round((screenHeight - marginTop - marginBottom) * deviceScale));
  try {
    if (next) {
      await appService.detachPanel({
        x: Math.round(marginX * deviceScale),
        y: Math.round(marginTop * deviceScale),
        width: availableWidth,
        height: availableHeight,
        maximized: true,
      });
    } else {
      await appService.redockPanel();
    }
    isFullscreen.value = next;
  } catch (error) {
    logError(next ? '全屏编辑节点图失败' : '退出全屏编辑失败', error);
  } finally {
    window.setTimeout(() => {
      fullscreenTransitionBusy.value = false;
      nextTick(() => {
        updateCanvasSize();
        resizeEmbeddedWorkspaces();
      });
    }, 180);
  }
}
function handleFullscreenKey(event) {
  if (event.key === 'Escape' && isFullscreen.value) {
    event.preventDefault();
    toggleFullscreen();
  }
}
const zoomText = computed(() => `${Math.round(viewport.scale * 100)}%`);
const dragGhostStyle = computed(() => ({
  left: `${externalDrag.clientX + 14}px`,
  top: `${externalDrag.clientY + 14}px`,
}));
const worldStyle = computed(() => ({
  width: `${canvasSize.width}px`,
  height: `${canvasSize.height}px`,
  transform: `translate(${viewport.offsetX}px, ${viewport.offsetY}px) scale(${viewport.scale})`,
  transformOrigin: '0 0',
}));
let isLoading = false,
  saveTimer = null,
  preWarningTimer = null,
  resizeObserver = null,
  resizeFrame = 0,
  dragState = null,
  panState = null,
  suppressCanvasClick = false,
  macroPointerDrag = null,
  layoutResizeState = null,
  runPollTimer = null,
  gamePreviewGuardTimer = null,
  startedRunForTarget = false,
  panelClosing = false,
  panelCloseStopPromise = null,
  unregisterAiNodeGraphConsumer = null,
  stopNodeGraphReview = null,
  actorChangedCallbackToken = null,
  sceneTreeChangedCallbackToken = null,
  actorOptionsRefreshTimer = null,
  actorOptionsRefreshSequence = 0,
  graphLoadSequence = 0,
  unsubscribeCabbageContext = null,
  unregisterProjectNodeGraphSaveHandler = null,
  unregisterProjectNodeGraphRuntimeHandler = null,
  tutorialObservationTimer = null,
  lastTutorialObservationSignature = '',
  loadedProjectPath = '',
  componentMounted = false,
  initialLoadComplete = false,
  graphDirty = false,
  saveInFlight = null,
  saveQueued = false,
  saveBlockedProjectPath = '';
const targetEnabledByKey = new Map();
const nodeRunLifecycle = { active: false, terminalReported: false };
const sceneActorContext = reactive({
  sceneName: '',
  available: false,
  revision: '',
  actors: [],
});
function requestNodeGraphReview(delay = 250) {
  stopNodeGraphReview?.scanNow?.(delay);
}
function scheduleRememberedIssueCheck(delay = 140) {
  if (!componentMounted || isLoading || !initialLoadComplete || !isProjectTarget.value) return;
  if (preWarningTimer) window.clearTimeout(preWarningTimer);
  preWarningTimer = window.setTimeout(() => {
    preWarningTimer = null;
    if (!componentMounted || isLoading || !initialLoadComplete || !isProjectTarget.value) return;
    const snapshot = graphSnapshot();
    const scope = normalizeProjectPath(activeProjectPath.value || readActiveProjectPath());
    const warning = cabbageAssistant.evaluateRememberedIssuePatterns(
      snapshot,
      reviewGraphRevision(snapshot, scope),
      generatedProjectContext(),
    );
    if (warning) {
      publishCabbagePreWarning({
        projectScopeId: reviewScopeId(scope),
        worldId: cabbageAssistant.worldId,
        warning,
      });
    }
  }, Math.max(0, Number(delay) || 0));
}
function currentAssistanceProfile() {
  const profile = readCabbageAssistantContext()?.profile || {};
  return {
    score: Math.max(0, Math.min(100, Number(profile.score ?? profile.fluencyScore) || 0)),
    updatedAt: Math.max(0, Number(profile.updatedAt) || 0),
  };
}
function optimizationHintsEnabled() {
  const context = readCabbageAssistantContext() || {};
  const tutorialCompleted = String(context?.tutorialSession?.status || '') === 'completed';
  const hasAdaptiveScore = Number(context?.profile?.updatedAt || 0) > 0;
  return tutorialCompleted && hasAdaptiveScore;
}
function beginNodeRunAttempt() {
  nodeRunLifecycle.active = false;
  nodeRunLifecycle.terminalReported = false;
}
function reportNodeRunClicked() {
  void cabbageContextService.recordEvent({
    type: 'run_clicked',
    category: 'tutorial',
    success: true,
    details: { source: 'user' },
  });
}
function reportNodeRunStarted() {
  if (nodeRunLifecycle.active) return;
  nodeRunLifecycle.active = true;
  void cabbageContextService.recordEvent({
    type: 'run_started',
    category: 'runtime',
    success: true,
    details: { source: 'node_graph' },
  });
}
function reportNodeRunTerminal(success, error = '') {
  if (nodeRunLifecycle.terminalReported) return;
  nodeRunLifecycle.terminalReported = true;
  nodeRunLifecycle.active = false;
  const type = success ? 'run_succeeded' : 'run_failed';
  void cabbageContextService.recordEvent({
    type,
    category: 'runtime',
    success,
    details: { source: 'node_graph', error: String(error || '').slice(0, 500) },
  });
  if (!success) {
    requestNodeGraphReview(0);
    window.dispatchEvent(new CustomEvent('cabbage-run-failed', {
      detail: { source: 'node_graph', error: String(error || ''), contextRecorded: true },
    }));
  }
}
function resetNodeRunLifecycle() {
  nodeRunLifecycle.active = false;
  nodeRunLifecycle.terminalReported = false;
}
function readActiveProjectPath() {
  return String(window.localStorage?.getItem('corona.activeProjectPath') || '').trim();
}
function extractProjectPath(response) {
  const candidates = [
    response,
    response?.data,
    response?.result,
    response?.data?.data,
    response?.result?.data,
  ];
  for (const item of candidates) {
    const value = String(item?.project_path || item?.projectPath || '').trim();
    if (value) return value;
  }
  return '';
}
async function refreshActiveProjectPath() {
  try {
    const response = await editorApi.projectSettings.getActiveProjectInfo();
    const projectPath = extractProjectPath(response);
    if (projectPath) {
      activeProjectPath.value = projectPath;
      window.localStorage?.setItem('corona.activeProjectPath', projectPath);
      return projectPath;
    }
  } catch (error) {
    console.warn('\u8bfb\u53d6\u5f53\u524d\u9879\u76ee\u8def\u5f84\u5931\u8d25\uff0c\u5c06\u4f7f\u7528\u5df2\u6709\u9879\u76ee\u4e0a\u4e0b\u6587:', error);
  }
  return activeProjectPath.value || readActiveProjectPath();
}
function normalizeProjectPath(value) {
  return String(value || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/\/+$/, '')
    .toLocaleLowerCase('en-US');
}
const activeProjectPath = ref(readActiveProjectPath());
const projectStorageScope = computed(() =>
  encodeURIComponent(normalizeProjectPath(activeProjectPath.value) || 'unknown-project')
);
const normalizedTargetType = computed(() => (props.targetType === 'model' ? 'actor' : props.targetType || 'actor'));
const isProjectTarget = computed(() => normalizedTargetType.value === 'project');
const targetReady = computed(() => isProjectTarget.value || Boolean(props.actorName));
const targetKey = computed(
  () => `${projectStorageScope.value}:${normalizedTargetType.value}:${isProjectTarget.value ? '' : props.sceneName || ''}:${isProjectTarget.value ? '' : props.actorName || ''}`
);
const nodes = computed(() => graph.nodes),
  edges = computed(() => graph.edges);
const selectedNode = computed(() =>
  selectedKind.value === 'node' ? graph.nodes.find((n) => n.id === selectedId.value) : null
);
const selectedEdge = computed(() =>
  selectedKind.value === 'edge' ? graph.edges.find((e) => e.id === selectedId.value) : null
);
const paletteWorkspaceRole = computed(() =>
  selectedEdge.value ? 'condition' : selectedNode.value ? 'node' : 'global'
);
const activeNodeTutorialTask = computed(() =>
  (cabbageAssistant.activeTasks || []).find((task) =>
    task?.type === 'tutorial'
    && task?.status === 'active'
    && String(task?.taskKey || '').startsWith('tutorial.basics.')
  ) || null
);
const activeNodeTutorialTaskKey = computed(() => String(activeNodeTutorialTask.value?.taskKey || ''));
const tutorialNodeBindings = computed(() => cabbageAssistant.tutorialSession?.bindings || {});
const nodeTypeOptions = [
  { value: 'start', label: '开始节点' },
  { value: 'end', label: '结束节点' },
  { value: 'custom', label: '自定义' },
];
const edgeConditionNote = computed(() => {
  const blocks = selectedEdge.value?.conditionWorkspace?.blocks?.blocks;
  return Array.isArray(blocks) && blocks.length
    ? '最外层使用“与 / 或 / 非”组合多个判断，最终只保留一个顶层布尔条件'
    : '未设置条件，当前连线会被视为始终成立';
});
const pendingEdgePath = computed(() => {
  if (!pendingPort.value || !connectionPointer.active) return '';
  const node = graph.nodes.find((item) => item.id === pendingPort.value.nodeId);
  if (!node) return '';
  const source = portPoint(node, pendingPort.value);
  return previewEdgePath(source, connectionPointer, pendingPort.value.side);
});
const activeEditorKey = computed(() =>
  selectedNode.value
    ? `${targetKey.value}:node:${selectedNode.value.id}`
    : selectedEdge.value
      ? `${targetKey.value}:edge:${selectedEdge.value.id}:condition`
      : ''
);
const activeEditorState = computed(() =>
  selectedNode.value
    ? selectedNode.value.workspace || {}
    : selectedEdge.value
      ? selectedEdge.value.conditionWorkspace || {}
      : {}
);
const activeEditorTitle = computed(() =>
  selectedNode.value
    ? `节点内部编辑：${displayNodeName(selectedNode.value)}`
    : selectedEdge.value
      ? `连线条件编辑：${selectedEdge.value.name || '未命名连线'}`
      : '节点内部编辑'
);
const activeEditorSubtitle = computed(() =>
  selectedNode.value
    ? '拖入微观积木编辑该节点'
    : selectedEdge.value
      ? '从左侧“返回值”中拖入积木，可使用“与 / 或 / 非”组合条件'
      : '未选择'
);
function setMode(v, { source = 'user' } = {}) {
  mode.value = v;
  pendingPort.value = null;
  connectionPointer.active = false;
  void cabbageContextService.recordEvent({
    type: 'node_tool_mode_changed',
    category: 'tutorial',
    success: true,
    details: { mode: v, source },
  });
}
function dragData(event, payload, text) {
  event.dataTransfer?.setData('application/x-corona-nodegraph', JSON.stringify(payload));
  event.dataTransfer?.setData('text/plain', text);
  if (event.dataTransfer) event.dataTransfer.effectAllowed = 'copy';
}
function startMacroDrag(e, macroType) {
  dragData(e, { kind: 'macro-node', macroType }, `macro-node:${macroType}`);
}
function pointInCanvas(clientX, clientY) {
  const rect = canvasRef.value?.getBoundingClientRect?.();
  return Boolean(
    rect &&
      clientX >= rect.left &&
      clientX <= rect.right &&
      clientY >= rect.top &&
      clientY <= rect.bottom
  );
}
function addMacroNodeAt(macroType, clientX, clientY) {
  if (!pointInCanvas(clientX, clientY)) return false;
  const world = screenToWorld(clientX, clientY);
  const node = {
    id: makeId('node'),
    macroType: macroType || 'state',
    nodeType: graph.nodes.length === 0 ? 'start' : 'custom',
    name: graph.nodes.length === 0 ? '开始' : `状态${graph.nodes.length + 1}`,
    customName: `状态${graph.nodes.length + 1}`,
    x: Math.max(20, world.x - NODE_WIDTH / 2),
    y: Math.max(54, world.y - 32),
    workspace: {},
  };
  graph.nodes.push(node);
  // Keep creation ahead of the automatic selection event. The tutorial advances one
  // step at a time, so reversing these two events can make a fast drag look ignored.
  void cabbageContextService.recordEvent({
    type: 'node_created',
    category: 'node',
    success: true,
    details: {
      nodeId: String(node.id || ''),
      nodeType: String(node.nodeType || ''),
      startNodeCount: graph.nodes.filter((item) => item.nodeType === 'start').length,
      uniqueStart: graph.nodes.filter((item) => item.nodeType === 'start').length === 1,
    },
  });
  selectNode(node, { source: 'creation' });
  scheduleTutorialNodeStateObservation();
  scheduleSave();
  scheduleRememberedIssueCheck();
  return true;
}
function clearMacroPointerListeners() {
  window.removeEventListener('pointermove', moveMacroPointerDrag, true);
  window.removeEventListener('pointerup', finishMacroPointerDrag, true);
  window.removeEventListener('pointercancel', cancelMacroPointerDrag, true);
}
function beginMacroPointerDrag(event, macroType) {
  if (event.button !== 0) return;
  macroPointerDrag = {
    pointerId: event.pointerId,
    macroType,
    startX: event.clientX,
    startY: event.clientY,
    started: false,
  };
  event.preventDefault();
  window.addEventListener('pointermove', moveMacroPointerDrag, true);
  window.addEventListener('pointerup', finishMacroPointerDrag, true);
  window.addEventListener('pointercancel', cancelMacroPointerDrag, true);
}
function moveMacroPointerDrag(event) {
  if (!macroPointerDrag || event.pointerId !== macroPointerDrag.pointerId) return;
  const distance = Math.hypot(
    event.clientX - macroPointerDrag.startX,
    event.clientY - macroPointerDrag.startY
  );
  if (!macroPointerDrag.started && distance < 5) return;
  macroPointerDrag.started = true;
  externalDrag.active = true;
  externalDrag.kind = 'macro';
  externalDrag.macroType = macroPointerDrag.macroType;
  externalDrag.label = '状态节点';
  externalDrag.clientX = event.clientX;
  externalDrag.clientY = event.clientY;
  externalDrag.pointerId = event.pointerId;
  macroDropActive.value = pointInCanvas(event.clientX, event.clientY);
  event.preventDefault();
}
function finishMacroPointerDrag(event) {
  if (!macroPointerDrag || event.pointerId !== macroPointerDrag.pointerId) return;
  const drag = macroPointerDrag;
  macroPointerDrag = null;
  clearMacroPointerListeners();
  if (drag.started) addMacroNodeAt(drag.macroType, event.clientX, event.clientY);
  clearExternalDrag();
}
function cancelMacroPointerDrag(event) {
  if (!macroPointerDrag || (event?.pointerId != null && event.pointerId !== macroPointerDrag.pointerId)) return;
  macroPointerDrag = null;
  clearMacroPointerListeners();
  clearExternalDrag();
}
function clearWorkspaceDropHighlights() {
  activeBlocklyRef.value?.setDropActive?.(false);
  variablesBlocklyRef.value?.setDropActive?.(false);
}
function clearExternalDrag() {
  clearWorkspaceDropHighlights();
  macroDropActive.value = false;
  externalDrag.active = false;
  externalDrag.kind = '';
  externalDrag.blockType = '';
  externalDrag.macroType = '';
  externalDrag.label = '';
  externalDrag.pointerId = null;
}
function onWorkspaceReject(message) {
  saveLabel.value = String(message || '\u65e0\u6cd5\u653e\u5165\u8be5\u79ef\u6728');
}
function updatePaletteDropTarget(clientX, clientY) {
  const activeHit = Boolean(activeBlocklyRef.value?.hitTest?.(clientX, clientY));
  const globalsHit = Boolean(variablesBlocklyRef.value?.hitTest?.(clientX, clientY));
  const activeValid = activeHit && Boolean(activeBlocklyRef.value?.canAcceptBlock?.(externalDrag.blockType));
  const globalsValid = globalsHit && Boolean(variablesBlocklyRef.value?.canAcceptBlock?.(externalDrag.blockType));
  activeBlocklyRef.value?.setDropActive?.(activeHit, activeValid);
  variablesBlocklyRef.value?.setDropActive?.(!activeHit && globalsHit, globalsValid);
  return activeValid ? activeBlocklyRef.value : (!activeHit && globalsValid ? variablesBlocklyRef.value : null);
}
function handlePaletteDragStart(payload) {
  if (!payload?.blockType) return;
  externalDrag.active = true;
  externalDrag.kind = 'micro';
  externalDrag.blockType = payload.blockType;
  externalDrag.label = payload.blockType;
  externalDrag.clientX = payload.clientX;
  externalDrag.clientY = payload.clientY;
  updatePaletteDropTarget(payload.clientX, payload.clientY);
}
function handlePaletteDragMove(payload) {
  if (!externalDrag.active || externalDrag.kind !== 'micro') return;
  externalDrag.clientX = payload.clientX;
  externalDrag.clientY = payload.clientY;
  updatePaletteDropTarget(payload.clientX, payload.clientY);
}
function handlePaletteDragEnd(payload) {
  if (!externalDrag.active || externalDrag.kind !== 'micro') return;
  const target = payload?.cancelled ? null : updatePaletteDropTarget(payload.clientX, payload.clientY);
  if (target?.addBlock?.(payload.blockType, payload.clientX, payload.clientY)) {
    refreshEmbeddedWorkspaceStates();
    scheduleSave();
  }
  clearExternalDrag();
}
function handlePalettePick(blockType) {
  if (!blockType) return;
  const active = activeBlocklyRef.value;
  const globals = variablesBlocklyRef.value;
  const targetWorkspace = active?.canAcceptBlock?.(blockType)
    ? active
    : globals?.canAcceptBlock?.(blockType)
      ? globals
      : null;
  if (!targetWorkspace?.addBlock) {
    saveLabel.value = '\u8bf7\u5148\u9009\u62e9\u8282\u70b9\u6216\u8fde\u7ebf\uff1b\u6b64\u79ef\u6728\u4e0d\u9002\u7528\u4e8e\u5168\u5c40\u53d8\u91cf\u6c60';
    return;
  }
  targetWorkspace.addBlock(blockType);
  refreshEmbeddedWorkspaceStates();
  scheduleSave();
}
function makeId(p) {
  return `${p}_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`;
}
function screenToWorld(clientX, clientY) {
  const r = canvasRef.value?.getBoundingClientRect?.();
  if (!r) return { x: 0, y: 0 };
  return {
    x: (clientX - r.left - viewport.offsetX) / viewport.scale,
    y: (clientY - r.top - viewport.offsetY) / viewport.scale,
  };
}
function clampViewport() {
  const r = canvasRef.value?.getBoundingClientRect?.();
  if (!r) return;
  const scaledWidth = canvasSize.width * viewport.scale;
  const scaledHeight = canvasSize.height * viewport.scale;
  const minX = Math.min(CANVAS_OVERSCROLL, r.width - scaledWidth - CANVAS_OVERSCROLL);
  const minY = Math.min(CANVAS_OVERSCROLL, r.height - scaledHeight - CANVAS_OVERSCROLL);
  viewport.offsetX = Math.min(CANVAS_OVERSCROLL, Math.max(minX, viewport.offsetX));
  viewport.offsetY = Math.min(CANVAS_OVERSCROLL, Math.max(minY, viewport.offsetY));
}
function handleCanvasWheel(e) {
  if (!canvasRef.value) return;
  const r = canvasRef.value.getBoundingClientRect();
  const localX = e.clientX - r.left;
  const localY = e.clientY - r.top;
  const world = screenToWorld(e.clientX, e.clientY);
  const factor = e.deltaY < 0 ? 1.1 : 0.9;
  const nextScale = Math.min(2, Math.max(0.4, viewport.scale * factor));
  if (Math.abs(nextScale - viewport.scale) < 0.0001) return;
  viewport.scale = nextScale;
  viewport.offsetX = localX - world.x * nextScale;
  viewport.offsetY = localY - world.y * nextScale;
  clampViewport();
}
function beginCanvasPan(e) {
  if (e.button !== 0 && e.button !== 1) return;
  if (e.button === 0 && e.target?.closest?.('.ng-node,.ng-port,.ng-condition-block,.ng-canvas-head,.ng-edge-hit,.ng-edge-line,button,input,textarea,select,.blocklySvg')) return;
  e.preventDefault();
  panState = {
    pointerId: e.pointerId,
    startX: e.clientX,
    startY: e.clientY,
    originX: viewport.offsetX,
    originY: viewport.offsetY,
    moved: false,
  };
  isCanvasPanning.value = true;
  window.addEventListener('pointermove', moveCanvasPan);
  window.addEventListener('pointerup', endCanvasPan, { once: true });
  window.addEventListener('pointercancel', endCanvasPan, { once: true });
}
function moveCanvasPan(e) {
  if (!panState || e.pointerId !== panState.pointerId) return;
  const dx = e.clientX - panState.startX;
  const dy = e.clientY - panState.startY;
  if (Math.abs(dx) + Math.abs(dy) > 4) panState.moved = true;
  viewport.offsetX = panState.originX + dx;
  viewport.offsetY = panState.originY + dy;
  clampViewport();
}
function endCanvasPan(e) {
  if (!panState || (e?.pointerId != null && e.pointerId !== panState.pointerId)) return;
  suppressCanvasClick = Boolean(panState.moved);
  panState = null;
  isCanvasPanning.value = false;
  window.removeEventListener('pointermove', moveCanvasPan);
  window.removeEventListener('pointerup', endCanvasPan);
  window.removeEventListener('pointercancel', endCanvasPan);
}
function persistLayout() {
  try {
    window.localStorage?.setItem(LAYOUT_STORAGE_KEY, JSON.stringify({
      toolboxWidth: layout.toolboxWidth,
      inspectorWidth: layout.inspectorWidth,
      variablesHeight: layout.variablesHeight,
    }));
  } catch (_) {}
}
function loadLayout() {
  try {
    const saved = JSON.parse(window.localStorage?.getItem(LAYOUT_STORAGE_KEY) || '{}');
    layout.toolboxWidth = Math.min(
      LAYOUT_LIMITS.toolboxMax,
      Math.max(LAYOUT_LIMITS.toolboxMin, Number(saved.toolboxWidth) || DEFAULT_LAYOUT.toolboxWidth)
    );
    layout.inspectorWidth = Math.min(
      LAYOUT_LIMITS.inspectorMax,
      Math.max(LAYOUT_LIMITS.inspectorMin, Number(saved.inspectorWidth) || DEFAULT_LAYOUT.inspectorWidth)
    );
    layout.variablesHeight = Math.max(120, Number(saved.variablesHeight) || DEFAULT_LAYOUT.variablesHeight);
  } catch (_) {
    Object.assign(layout, DEFAULT_LAYOUT);
  }
}
function readWorkspaceSize() {
  const rect = workspaceRootRef.value?.getBoundingClientRect?.();
  if (!rect) return;
  workspaceSize.width = Math.max(0, Math.round(rect.width));
  workspaceSize.height = Math.max(0, Math.round(rect.height));
}
function performEmbeddedWorkspaceResize() {
  readWorkspaceSize();
  const clamped = clampLayoutWidths(layout.toolboxWidth, layout.inspectorWidth);
  if (clamped.toolbox !== layout.toolboxWidth) layout.toolboxWidth = clamped.toolbox;
  if (clamped.inspector !== layout.inspectorWidth) layout.inspectorWidth = clamped.inspector;
  nextTick(() => {
    updateCanvasSize();
    paletteRef.value?.resizeBlockly?.();
    variablesBlocklyRef.value?.resizeBlockly?.();
    activeBlocklyRef.value?.resizeBlockly?.();
  });
}
function resizeEmbeddedWorkspaces() {
  if (resizeFrame) return;
  resizeFrame = window.requestAnimationFrame(() => {
    resizeFrame = 0;
    performEmbeddedWorkspaceResize();
  });
}
function clampLayoutWidths(toolboxWidth, inspectorWidth) {
  const total = workspaceRootRef.value?.getBoundingClientRect?.().width || window.innerWidth;
  const available = Math.max(0, total - LAYOUT_LIMITS.gridChrome);
  let toolbox = Math.min(LAYOUT_LIMITS.toolboxMax, Math.max(LAYOUT_LIMITS.toolboxMin, toolboxWidth));
  let inspector = Math.min(LAYOUT_LIMITS.inspectorMax, Math.max(LAYOUT_LIMITS.inspectorMin, inspectorWidth));

  // In compact mode the inspector moves below the canvas, so only the toolbox
  // competes with the canvas for horizontal space.
  if (total < 1180) {
    const maxToolbox = Math.max(
      LAYOUT_LIMITS.toolboxEmergencyMin,
      Math.min(LAYOUT_LIMITS.toolboxMax, available - Math.min(320, LAYOUT_LIMITS.canvasMin))
    );
    toolbox = Math.min(maxToolbox, Math.max(LAYOUT_LIMITS.toolboxEmergencyMin, toolbox));
    return { toolbox: Math.round(toolbox), inspector: Math.round(inspector) };
  }

  const maxSides = Math.max(
    LAYOUT_LIMITS.toolboxEmergencyMin + LAYOUT_LIMITS.inspectorEmergencyMin,
    available - LAYOUT_LIMITS.canvasMin
  );
  if (toolbox + inspector > maxSides) {
    let overflow = toolbox + inspector - maxSides;
    if (layoutResizeState?.kind === 'toolbox') {
      const reduce = Math.min(overflow, toolbox - LAYOUT_LIMITS.toolboxEmergencyMin);
      toolbox -= reduce;
      overflow -= reduce;
      inspector = Math.max(LAYOUT_LIMITS.inspectorEmergencyMin, inspector - overflow);
    } else {
      const reduce = Math.min(overflow, inspector - LAYOUT_LIMITS.inspectorEmergencyMin);
      inspector -= reduce;
      overflow -= reduce;
      toolbox = Math.max(LAYOUT_LIMITS.toolboxEmergencyMin, toolbox - overflow);
    }
  }
  return { toolbox: Math.round(toolbox), inspector: Math.round(inspector) };
}
function beginLayoutResize(event, kind) {
  event.preventDefault();
  event.stopPropagation();
  layoutResizeState = {
    kind,
    startX: event.clientX,
    startY: event.clientY,
    toolboxWidth: layout.toolboxWidth,
    inspectorWidth: layout.inspectorWidth,
    variablesHeight: layout.variablesHeight,
    inspectorHeight: inspectorRef.value?.getBoundingClientRect?.().height || 600,
  };
  window.addEventListener('pointermove', handleLayoutResize);
  window.addEventListener('pointerup', stopLayoutResize, { once: true });
}
function handleLayoutResize(event) {
  if (!layoutResizeState) return;
  if (layoutResizeState.kind === 'variables') {
    const maxHeight = Math.max(120, layoutResizeState.inspectorHeight - 366);
    layout.variablesHeight = Math.min(maxHeight, Math.max(120, layoutResizeState.variablesHeight + event.clientY - layoutResizeState.startY));
  } else {
    const nextToolbox = layoutResizeState.kind === 'toolbox'
      ? layoutResizeState.toolboxWidth + event.clientX - layoutResizeState.startX
      : layoutResizeState.toolboxWidth;
    const nextInspector = layoutResizeState.kind === 'inspector'
      ? layoutResizeState.inspectorWidth - event.clientX + layoutResizeState.startX
      : layoutResizeState.inspectorWidth;
    const clamped = clampLayoutWidths(nextToolbox, nextInspector);
    layout.toolboxWidth = clamped.toolbox;
    layout.inspectorWidth = clamped.inspector;
  }
  resizeEmbeddedWorkspaces();
}
function stopLayoutResize() {
  if (!layoutResizeState) return;
  layoutResizeState = null;
  window.removeEventListener('pointermove', handleLayoutResize);
  persistLayout();
  resizeEmbeddedWorkspaces();
}
function toggleRunDetail() {
  if (runDetail.value) runDetailVisible.value = !runDetailVisible.value;
}
async function copyRunDetail() {
  if (!runDetail.value) return;
  try { await navigator.clipboard?.writeText(runDetail.value); } catch (_) {}
}
function resetZoom() {
  viewport.scale = 1;
  viewport.offsetX = 0;
  viewport.offsetY = 0;
  clampViewport();
}
function handleCanvasDrop(e) {
  const raw = e.dataTransfer?.getData('application/x-corona-nodegraph');
  if (!raw) return;
  let payload;
  try {
    payload = JSON.parse(raw);
  } catch {
    return;
  }
  if (payload?.kind === 'macro-node') addMacroNodeAt(payload.macroType, e.clientX, e.clientY);
}
function handleCanvasPointerMove(event) {
  if (!pendingPort.value) return;
  const world = screenToWorld(event.clientX, event.clientY);
  connectionPointer.x = world.x;
  connectionPointer.y = world.y;
  connectionPointer.active = true;
}
function handleCanvasPointerLeave() {
  connectionPointer.active = false;
}
function handleCanvasClick() {
  if (suppressCanvasClick) {
    suppressCanvasClick = false;
    return;
  }
  if (mode.value !== 'delete') {
    pendingPort.value = null;
    connectionPointer.active = false;
  }
}
function handleNodeClick(node) {
  if (mode.value === 'delete') {
    deleteNode(node.id, { source: 'user' });
    return;
  }
  selectNode(node);
}
function syncActiveBeforeSelection(nextKind, nextId) {
  if (
    selectedKind.value &&
    selectedId.value &&
    (selectedKind.value !== nextKind || selectedId.value !== nextId)
  )
    refreshEmbeddedWorkspaceStates();
}
function selectNode(node, { source = 'user' } = {}) {
  if (!node) return;
  const isConnecting = Boolean(pendingPort.value);
  syncActiveBeforeSelection('node', node.id);
  selectedKind.value = 'node';
  selectedId.value = node.id;
  if (!isConnecting) {
    pendingPort.value = null;
    connectionPointer.active = false;
  }
  const startNodeCount = graph.nodes.filter((item) => item.nodeType === 'start').length;
  void cabbageContextService.recordEvent({
    type: 'node_selected',
    category: 'node',
    success: true,
    details: {
      nodeId: String(node.id || ''),
      nodeType: String(node.nodeType || ''),
      startNodeCount,
      uniqueStart: startNodeCount === 1,
      mode: mode.value,
      source,
    },
  });
  nextTick(() => {
    activeBlocklyRef.value?.resizeBlockly?.();
    scheduleTutorialNodeStateObservation();
  });
}
function selectEdge(edge, { allowDelete = true, source = 'user' } = {}) {
  if (!edge) return;
  if (allowDelete && mode.value === 'delete') {
    deleteEdge(edge.id);
    return;
  }
  syncActiveBeforeSelection('edge', edge.id);
  selectedKind.value = 'edge';
  selectedId.value = edge.id;
  pendingPort.value = null;
  connectionPointer.active = false;
  void cabbageContextService.recordEvent({
    type: 'edge_selected',
    category: 'node',
    success: true,
    details: { edgeId: String(edge.id || ''), source },
  });
  nextTick(() => {
    activeBlocklyRef.value?.resizeBlockly?.();
    scheduleTutorialNodeStateObservation();
  });
}
function handleEdgeClick(edge) {
  if (mode.value === 'delete') {
    deleteEdge(edge.id);
    return;
  }
  selectEdge(edge);
}

function serializedWorkspaceContainsBlock(value, blockId, seen = new Set()) {
  if (!value || !blockId || typeof value !== 'object') return false;
  if (seen.has(value)) return false;
  seen.add(value);
  if (String(value.id || '') === blockId) return true;
  if (Array.isArray(value)) {
    return value.some((item) => serializedWorkspaceContainsBlock(item, blockId, seen));
  }
  return Object.values(value).some((item) => serializedWorkspaceContainsBlock(item, blockId, seen));
}

function guidanceBlockOwner(blockId) {
  if (!blockId) return null;
  refreshEmbeddedWorkspaceStates();
  if (serializedWorkspaceContainsBlock(graph.globalVariablesWorkspace, blockId)) {
    return { kind: 'global' };
  }
  const node = graph.nodes.find((item) => serializedWorkspaceContainsBlock(item.workspace, blockId));
  if (node) return { kind: 'node', node };
  const edge = graph.edges.find((item) => serializedWorkspaceContainsBlock(item.conditionWorkspace, blockId));
  if (edge) return { kind: 'edge', edge };
  if (activeBlocklyRef.value?.hasBlock?.(blockId)) {
    return selectedNode.value
      ? { kind: 'node', node: selectedNode.value }
      : selectedEdge.value
        ? { kind: 'edge', edge: selectedEdge.value }
        : null;
  }
  if (variablesBlocklyRef.value?.hasBlock?.(blockId)) return { kind: 'global' };
  return null;
}

function focusWorldPointIfNeeded(worldX, worldY) {
  const rect = canvasRef.value?.getBoundingClientRect?.();
  if (!rect || !Number.isFinite(worldX) || !Number.isFinite(worldY)) return;
  const localX = viewport.offsetX + worldX * viewport.scale;
  const localY = viewport.offsetY + worldY * viewport.scale;
  const marginX = Math.min(140, Math.max(64, rect.width * 0.18));
  const marginY = Math.min(110, Math.max(60, rect.height * 0.18));
  const visible = localX >= marginX
    && localX <= rect.width - marginX
    && localY >= marginY
    && localY <= rect.height - marginY;
  if (visible) return;
  viewport.offsetX = rect.width / 2 - worldX * viewport.scale;
  viewport.offsetY = rect.height / 2 - worldY * viewport.scale;
  clampViewport();
}

function focusGuidanceNode(node) {
  if (!node) return false;
  selectNode(node, { source: 'guidance' });
  focusWorldPointIfNeeded(
    (Number(node.x) || 0) + NODE_WIDTH / 2,
    (Number(node.y) || 0) + nodeHeight(node) / 2
  );
  return true;
}

function focusGuidanceEdge(edge) {
  if (!edge) return false;
  selectEdge(edge, { allowDelete: false, source: 'guidance' });
  const source = graph.nodes.find((node) => node.id === edge.source?.nodeId);
  const target = graph.nodes.find((node) => node.id === edge.target?.nodeId);
  if (source && target) {
    focusWorldPointIfNeeded(
      ((Number(source.x) || 0) + (Number(target.x) || 0) + NODE_WIDTH) / 2,
      ((Number(source.y) || 0) + (Number(target.y) || 0) + (nodeHeight(source) + nodeHeight(target)) / 2) / 2
    );
  }
  return true;
}

async function focusGuidanceBlock(blockId) {
  const owner = guidanceBlockOwner(blockId);
  if (!owner) return false;
  if (owner.kind === 'node') focusGuidanceNode(owner.node);
  if (owner.kind === 'edge') focusGuidanceEdge(owner.edge);
  await nextTick();
  await new Promise((resolve) => window.requestAnimationFrame(resolve));
  const targetWorkspace = owner.kind === 'global' ? variablesBlocklyRef.value : activeBlocklyRef.value;
  targetWorkspace?.resizeBlockly?.();
  return Boolean(targetWorkspace?.focusBlock?.(blockId));
}

function handleGuidancePrepare(event) {
  const detail = event?.detail || {};
  if (detail.panelId !== 'NodeGraphPanel') return;
  const blockId = String(detail.blockId || '');
  if (blockId) {
    void focusGuidanceBlock(blockId);
    return;
  }
  const blockType = String(detail.blockType || '');
  if (blockType) {
    void paletteRef.value?.focusBlockType?.(blockType);
    return;
  }
  const edgeId = String(detail.edgeId || '');
  if (edgeId) {
    focusGuidanceEdge(graph.edges.find((edge) => String(edge.id) === edgeId));
    return;
  }
  const nodeId = String(detail.nodeId || '');
  if (nodeId) focusGuidanceNode(graph.nodes.find((node) => String(node.id) === nodeId));
}
function deleteNode(id, { source = 'system' } = {}) {
  const i = graph.nodes.findIndex((n) => n.id === id);
  if (i < 0) return;
  const node = graph.nodes[i];
  graph.nodes.splice(i, 1);
  graph.edges = graph.edges.filter((e) => e.source.nodeId !== id && e.target.nodeId !== id);
  if (selectedId.value === id) {
    selectedKind.value = '';
    selectedId.value = '';
  }
  pendingPort.value = null;
  void cabbageContextService.recordEvent({
    type: 'node_deleted',
    category: 'node',
    success: true,
    details: {
      nodeId: String(node.id || ''),
      nodeType: String(node.nodeType || ''),
      mode: mode.value,
      source,
    },
  });
  scheduleSave();
  scheduleRememberedIssueCheck();
}
function deleteEdge(id) {
  const i = graph.edges.findIndex((e) => e.id === id);
  if (i < 0) return;
  graph.edges.splice(i, 1);
  if (selectedKind.value === 'edge' && selectedId.value === id) {
    selectedKind.value = '';
    selectedId.value = '';
  }
  pendingPort.value = null;
  scheduleSave();
  scheduleRememberedIssueCheck();
}
function renameEdge(edge) {
  if (mode.value === 'delete') return;
  selectEdge(edge);
  nextTick(() => {
    const input = Array.isArray(edgeNameInputRef.value) ? edgeNameInputRef.value[0] : edgeNameInputRef.value;
    input?.focus?.();
    input?.select?.();
  });
}
function setSelectedNodeType(nextType) {
  const node = selectedNode.value;
  if (!node || !['start', 'end', 'custom'].includes(nextType)) return;
  if (nextType === 'start') {
    for (const other of graph.nodes) {
      if (other.id !== node.id && other.nodeType === 'start') {
        other.nodeType = 'custom';
        const fallback = String(other.customName || '').trim() || '自定义节点';
        other.customName = fallback;
        other.name = fallback;
      }
    }
  }
  node.nodeType = nextType;
  if (nextType === 'start') node.name = '开始';
  else if (nextType === 'end') node.name = '结束';
  else node.name = String(node.customName || '').trim() || '自定义节点';
  scheduleTutorialNodeStateObservation();
  scheduleSave();
  scheduleRememberedIssueCheck();
}
function updateSelectedNodeName(value) {
  const node = selectedNode.value;
  if (!node) return;
  node.customName = String(value || '').slice(0, 24);
  node.name = node.customName.trim() || '自定义节点';
  scheduleSave();
}
function updateSelectedEdgeName(value) {
  const edge = selectedEdge.value;
  if (!edge) return;
  edge.name = String(value || '').slice(0, 24).trimStart();
  scheduleSave();
}
function edgeNodeName(nodeId) {
  return displayNodeName(getNode(nodeId)) || '未知节点';
}
function nodeTypeLabel(t) {
  return t === 'start' ? '开始节点' : t === 'end' ? '结束节点' : '自定义';
}
function displayNodeName(n) {
  return n
    ? n.nodeType === 'custom'
      ? n.customName || n.name || '自定义节点'
      : nodeTypeLabel(n.nodeType)
    : '';
}
function nodeHeight(node) {
  const sidePortCount = Math.max(
    ...['left', 'right'].map(
      (side) => visiblePorts(node).filter((port) => port.side === side).length
    ),
    1
  );
  return Math.max(NODE_BASE_HEIGHT, 70 + sidePortCount * NODE_PORT_GAP);
}
function nodeStyle(n) {
  return {
    left: `${n.x}px`,
    top: `${n.y}px`,
    width: `${NODE_WIDTH}px`,
    minHeight: `${nodeHeight(n)}px`,
  };
}
function startNodeDrag(e, node) {
  if (mode.value !== 'select') return;
  if (e.target?.closest?.('button,.ng-port')) return;
  selectNode(node);
  const world = screenToWorld(e.clientX, e.clientY);
  dragState = {
    node,
    offsetX: world.x - node.x,
    offsetY: world.y - node.y,
    startX: Number(node.x) || 0,
    startY: Number(node.y) || 0,
  };
  window.addEventListener('mousemove', onDragMove);
  window.addEventListener('mouseup', stopNodeDrag, { once: true });
}
function onDragMove(e) {
  if (!dragState || !canvasRef.value) return;
  const world = screenToWorld(e.clientX, e.clientY);
  dragState.node.x = Math.max(
    8,
    Math.min(canvasSize.width - NODE_WIDTH - 8, world.x - dragState.offsetX)
  );
  dragState.node.y = Math.max(
    48,
    Math.min(canvasSize.height - NODE_BASE_HEIGHT - 8, world.y - dragState.offsetY)
  );
}
function stopNodeDrag() {
  const finished = dragState;
  if (finished) {
    scheduleSave();
    const moved = Math.abs((Number(finished.node.x) || 0) - finished.startX) > 0.5
      || Math.abs((Number(finished.node.y) || 0) - finished.startY) > 0.5;
    if (moved) {
      void cabbageContextService.recordEvent({
        type: 'node_moved',
        category: 'node',
        success: true,
        details: {
          nodeId: String(finished.node.id || ''),
          nodeType: String(finished.node.nodeType || ''),
          actualDelta: Math.hypot(
            (Number(finished.node.x) || 0) - finished.startX,
            (Number(finished.node.y) || 0) - finished.startY,
          ),
          mode: mode.value,
          source: 'user',
        },
      });
    }
  }
  dragState = null;
  window.removeEventListener('mousemove', onDragMove);
}
function usedPortIndexes(id, side) {
  const a = [];
  for (const e of graph.edges) {
    if (e.source.nodeId === id && e.source.side === side) a.push(e.source.index || 0);
    if (e.target.nodeId === id && e.target.side === side) a.push(e.target.index || 0);
  }
  return a;
}
function visiblePorts(node) {
  const ports = [];
  for (const side of ['left', 'right', 'bottom']) {
    const used = usedPortIndexes(node.id, side);
    const count = Math.max(1, Math.max(-1, ...used) + 2);
    for (let index = 0; index < count; index++) ports.push({ side, index });
  }
  return ports;
}
function isPortUsed(id, p) {
  return graph.edges.some(
    (e) =>
      (e.source.nodeId === id && e.source.side === p.side && (e.source.index || 0) === p.index) ||
      (e.target.nodeId === id && e.target.side === p.side && (e.target.index || 0) === p.index)
  );
}
function shouldShowPort(node, port) {
  return (
    isPortUsed(node.id, port) ||
    (selectedKind.value === 'node' && selectedId.value === node.id) ||
    Boolean(pendingPort.value)
  );
}
function portStyle(node, p) {
  const pt = portPoint(node, p);
  return { left: `${pt.x - node.x - 6}px`, top: `${pt.y - node.y - 6}px` };
}
function portPoint(node, p) {
  const h = nodeHeight(node);
  if (p.side === 'left') return { x: node.x, y: node.y + 34 + p.index * NODE_PORT_GAP };
  if (p.side === 'right')
    return { x: node.x + NODE_WIDTH, y: node.y + 34 + p.index * NODE_PORT_GAP };
  const n = visiblePorts(node).filter((i) => i.side === 'bottom').length;
  return { x: node.x + (NODE_WIDTH / (n + 1)) * (p.index + 1), y: node.y + h };
}
function handlePortClick(node, p) {
  if (mode.value === 'delete' || isPortUsed(node.id, p)) return;
  const clicked = { nodeId: node.id, side: p.side, index: p.index };
  if (!pendingPort.value) {
    pendingPort.value = clicked;
    const point = portPoint(node, p);
    connectionPointer.x = point.x;
    connectionPointer.y = point.y;
    connectionPointer.active = true;
    return;
  }
  if (
    pendingPort.value.nodeId === clicked.nodeId &&
    pendingPort.value.side === clicked.side &&
    pendingPort.value.index === clicked.index
  ) {
    pendingPort.value = null;
    connectionPointer.active = false;
    return;
  }
  const newEdge = {
    id: makeId('edge'),
    source: { ...pendingPort.value },
    target: clicked,
    name: '',
    conditionWorkspace: {},
  };
  graph.edges.push(newEdge);
  void cabbageContextService.recordEvent({
    type: 'node_connected',
    category: 'node',
    success: true,
    details: {
      edgeId: String(newEdge.id || ''),
      sourceNodeId: String(newEdge.source.nodeId || ''),
      targetNodeId: String(newEdge.target.nodeId || ''),
    },
  });
  pendingPort.value = null;
  connectionPointer.active = false;
  scheduleTutorialNodeStateObservation();
  scheduleSave();
  scheduleRememberedIssueCheck();
}
function getNode(id) {
  return graph.nodes.find((n) => n.id === id);
}
function edgeEndpoints(e) {
  const s = getNode(e.source.nodeId),
    t = getNode(e.target.nodeId);
  if (!s || !t) return null;
  return { source: portPoint(s, e.source), target: portPoint(t, e.target) };
}
function portVector(side) {
  if (side === 'left') return { x: -1, y: 0 };
  if (side === 'right') return { x: 1, y: 0 };
  return { x: 0, y: 1 };
}
function edgeCurve(source, target, sourceSide, targetSide) {
  const distance = Math.hypot(target.x - source.x, target.y - source.y);
  const handle = Math.max(52, Math.min(220, distance * 0.42));
  const sourceVector = portVector(sourceSide);
  const targetVector = portVector(targetSide);
  return {
    source,
    target,
    control1: {
      x: source.x + sourceVector.x * handle,
      y: source.y + sourceVector.y * handle,
    },
    control2: {
      x: target.x + targetVector.x * handle,
      y: target.y + targetVector.y * handle,
    },
  };
}
function curvePath(curve) {
  return `M ${curve.source.x} ${curve.source.y} C ${curve.control1.x} ${curve.control1.y}, ${curve.control2.x} ${curve.control2.y}, ${curve.target.x} ${curve.target.y}`;
}
function cubicPoint(curve, t = 0.5) {
  const mt = 1 - t;
  return {
    x:
      mt * mt * mt * curve.source.x +
      3 * mt * mt * t * curve.control1.x +
      3 * mt * t * t * curve.control2.x +
      t * t * t * curve.target.x,
    y:
      mt * mt * mt * curve.source.y +
      3 * mt * mt * t * curve.control1.y +
      3 * mt * t * t * curve.control2.y +
      t * t * t * curve.target.y,
  };
}
function edgeCurveFor(e) {
  const p = edgeEndpoints(e);
  return p ? edgeCurve(p.source, p.target, e.source.side, e.target.side) : null;
}
function edgePath(e) {
  const curve = edgeCurveFor(e);
  return curve ? curvePath(curve) : '';
}
function previewEdgePath(source, target, sourceSide) {
  const distance = Math.hypot(target.x - source.x, target.y - source.y);
  const handle = Math.max(48, Math.min(180, distance * 0.4));
  const sourceVector = portVector(sourceSide);
  const dx = target.x - source.x;
  const dy = target.y - source.y;
  const length = Math.max(1, Math.hypot(dx, dy));
  return curvePath({
    source,
    target,
    control1: {
      x: source.x + sourceVector.x * handle,
      y: source.y + sourceVector.y * handle,
    },
    control2: {
      x: target.x - (dx / length) * handle,
      y: target.y - (dy / length) * handle,
    },
  });
}
function edgeMidpoint(e) {
  const curve = edgeCurveFor(e);
  return curve ? cubicPoint(curve, 0.5) : { x: 0, y: 0 };
}
function conditionStyle(e) {
  const p = edgeMidpoint(e);
  return { left: `${p.x - 56}px`, top: `${p.y - 17}px` };
}
function tutorialBlockFieldDetails(payload = {}) {
  return {
    modelName: String(payload.NAME ?? payload.modelName ?? ''),
    direction: String(payload.DIRECTION ?? payload.direction ?? ''),
    speed: payload.SPEED ?? payload.speed ?? '',
    x: payload.X ?? payload.x ?? '',
    y: payload.Y ?? payload.y ?? '',
    z: payload.Z ?? payload.z ?? '',
  };
}

function onBlockAdded(payload = {}) {
  scheduleRememberedIssueCheck();
  scheduleTutorialNodeStateObservation();
  void cabbageContextService.recordEvent({
    type: 'block_added',
    category: 'node',
    success: true,
    details: {
      nodeId: selectedNode.value?.id || '',
      edgeId: selectedEdge.value?.id || '',
      blockId: String(payload.blockId || ''),
      blockType: String(payload.blockType || ''),
      workspaceRole: String(payload.workspaceRole || paletteWorkspaceRole.value || ''),
      interaction: String(payload.interaction || ''),
      parentBlockType: String(payload.parentBlockType || ''),
      connected: payload.connected === true,
      fieldName: String(payload.fieldName || ''),
      oldValue: payload.oldValue ?? '',
      newValue: payload.newValue ?? payload.value ?? '',
      value: payload.value ?? payload.newValue ?? '',
      ...tutorialBlockFieldDetails(payload),
    },
  });
}

function onBlockChanged(payload = {}) {
  scheduleRememberedIssueCheck();
  scheduleTutorialNodeStateObservation();
  void cabbageContextService.recordEvent({
    type: 'block_parameter_changed',
    category: 'node',
    success: true,
    details: {
      nodeId: selectedNode.value?.id || '',
      edgeId: selectedEdge.value?.id || '',
      blockId: String(payload.blockId || ''),
      blockType: String(payload.blockType || ''),
      fieldName: String(payload.fieldName || ''),
      workspaceRole: String(payload.workspaceRole || paletteWorkspaceRole.value || ''),
      parentBlockType: String(payload.parentBlockType || ''),
      connected: payload.connected === true,
      oldValue: payload.oldValue ?? '',
      newValue: payload.newValue ?? payload.value ?? '',
      value: payload.value ?? payload.newValue ?? '',
      ...tutorialBlockFieldDetails(payload),
    },
  });
}

function onBlockConnected(payload = {}) {
  scheduleRememberedIssueCheck();
  scheduleTutorialNodeStateObservation();
  void cabbageContextService.recordEvent({
    type: 'block_connected',
    category: 'node',
    success: true,
    details: {
      nodeId: selectedNode.value?.id || '',
      edgeId: selectedEdge.value?.id || '',
      blockId: String(payload.blockId || ''),
      blockType: String(payload.blockType || ''),
      parentBlockType: String(payload.parentBlockType || ''),
      connected: payload.connected === true,
      workspaceRole: String(payload.workspaceRole || paletteWorkspaceRole.value || ''),
      newValue: payload.newValue ?? payload.value ?? '',
      value: payload.value ?? payload.newValue ?? '',
      ...tutorialBlockFieldDetails(payload),
    },
  });
}

function onGlobalWorkspaceChange(s) {
  if (isLoading) return;
  graph.globalVariablesWorkspace = s || {};
  scheduleSave();
  scheduleRememberedIssueCheck();
}
function onActiveWorkspaceChange(s) {
  if (isLoading) return;
  if (selectedNode.value) selectedNode.value.workspace = s || {};
  if (selectedEdge.value) selectedEdge.value.conditionWorkspace = s || {};
  scheduleTutorialNodeStateObservation();
  scheduleSave();
  scheduleRememberedIssueCheck();
}
function refreshEmbeddedWorkspaceStates() {
  const variablesEditor = variablesBlocklyRef.value;
  if (variablesEditor?.isReady?.()) {
    const state = variablesEditor.getState?.();
    if (state && typeof state === 'object') graph.globalVariablesWorkspace = state;
  }
  const activeEditor = activeBlocklyRef.value;
  if (activeEditor?.isReady?.()) {
    const state = activeEditor.getState?.();
    if (state && typeof state === 'object') {
      if (selectedNode.value) selectedNode.value.workspace = state;
      if (selectedEdge.value) selectedEdge.value.conditionWorkspace = state;
    }
  }
}
function graphSnapshot() {
  refreshEmbeddedWorkspaceStates();
  try {
    return JSON.parse(
      JSON.stringify({
        version: 1,
        nodes: graph.nodes,
        edges: graph.edges,
        globalVariablesWorkspace: graph.globalVariablesWorkspace || {},
      })
    );
  } catch {
    return { version: 1, nodes: [], edges: [], globalVariablesWorkspace: {} };
  }
}

function canonicalJson(value) {
  const normalize = (item) => {
    if (Array.isArray(item)) return item.map(normalize);
    if (!item || typeof item !== 'object') return item;
    return Object.keys(item).sort().reduce((result, key) => {
      const normalized = normalize(item[key]);
      if (normalized !== undefined) result[key] = normalized;
      return result;
    }, {});
  };
  try { return JSON.stringify(normalize(value)); } catch (_) { return ''; }
}

function serializedBlockRecordsById(workspace = {}) {
  const records = new Map();
  const seen = new Set();
  const visit = (value, parentBlockType = '', connected = false) => {
    if (Array.isArray(value)) {
      value.forEach((item) => visit(item, parentBlockType, connected));
      return;
    }
    if (!value || typeof value !== 'object' || seen.has(value)) return;
    seen.add(value);

    const blockType = typeof value.type === 'string' ? value.type : '';
    const blockId = blockType && value.id ? String(value.id) : '';
    if (!blockId) {
      Object.values(value).forEach((item) => visit(item, parentBlockType, connected));
      return;
    }

    records.set(blockId, {
      id: blockId,
      blockType,
      parentBlockType,
      connected,
      fields: value.fields && typeof value.fields === 'object' ? value.fields : {},
      block: value,
    });

    const nested = [];
    const inputs = value.inputs && typeof value.inputs === 'object' ? value.inputs : {};
    Object.values(inputs).forEach((input) => {
      if (!input || typeof input !== 'object') return;
      if (input.block) nested.push(input.block);
      else if (input.shadow) nested.push(input.shadow);
    });
    const nextBlock = value.next?.block || value.next?.shadow;
    if (nextBlock) nested.push(nextBlock);
    nested.forEach((item) => visit(item, blockType, true));

    Object.entries(value).forEach(([key, item]) => {
      if (['inputs', 'next', 'fields', 'type', 'id'].includes(key)) return;
      visit(item, blockType, connected);
    });
  };
  visit(workspace);
  return records;
}

function serializedBlocksById(workspace = {}) {
  return new Map(
    [...serializedBlockRecordsById(workspace)].map(([blockId, record]) => [blockId, record.block])
  );
}

function serializedBlockRecord(workspace, blockType, preferredId = '') {
  const records = serializedBlockRecordsById(workspace);
  if (preferredId) {
    const preferred = records.get(String(preferredId));
    if (preferred?.blockType === blockType) return preferred;
  }
  return [...records.values()].find((record) => record.blockType === blockType) || null;
}

function tutorialNodeStateObservationDetails() {
  if (!componentMounted || isLoading || !initialLoadComplete) return null;
  const taskKey = activeNodeTutorialTaskKey.value;
  if (!taskKey.startsWith('tutorial.basics.')) return null;
  const bindings = tutorialNodeBindings.value || {};
  const baseline = cabbageAssistant.tutorialSession?.baseline?.nodeGraph;
  const base = { observedTaskKey: taskKey, source: 'state_observation' };

  if (taskKey === 'tutorial.basics.confirm_start_node') {
    const startNodes = graph.nodes.filter((node) => String(node.nodeType || '').toLowerCase() === 'start');
    if (startNodes.length !== 1) return null;
    const nodeId = String(startNodes[0].id || '');
    const hasBaseline = Boolean(baseline && typeof baseline === 'object');
    const baselineNodeIds = new Set(
      hasBaseline && Array.isArray(baseline.nodeIds) ? baseline.nodeIds.map(String) : []
    );
    return {
      ...base,
      nodeId,
      nodeType: 'start',
      startNodeCount: 1,
      uniqueStart: true,
      // Missing baseline data must never make a pre-existing user node look
      // tutorial-created. The node can still satisfy the task, but restoration
      // will conservatively keep it.
      createdByTutorial: hasBaseline ? !baselineNodeIds.has(nodeId) : false,
    };
  }

  if (taskKey === 'tutorial.basics.create_custom_node') {
    if (!baseline || typeof baseline !== 'object') return null;
    const baselineNodeIds = new Set((baseline.nodeIds || []).map(String));
    const node = graph.nodes.find((item) =>
      String(item.nodeType || '').toLowerCase() === 'custom'
      && !baselineNodeIds.has(String(item.id || ''))
    );
    return node ? { ...base, nodeId: String(node.id || ''), nodeType: 'custom' } : null;
  }

  const startNodeId = String(bindings.startNodeId || '');
  const customNodeId = String(bindings.customNodeId || '');
  const edgeId = String(bindings.edgeId || '');
  const customNode = graph.nodes.find((node) => String(node.id || '') === customNodeId);

  if (taskKey === 'tutorial.basics.connect_nodes') {
    const edge = graph.edges.find((item) =>
      String(item.source?.nodeId || '') === startNodeId
      && String(item.target?.nodeId || '') === customNodeId
      && String(item.source?.side || '') === 'right'
      && String(item.target?.side || '') === 'left'
    );
    return edge ? {
      ...base,
      edgeId: String(edge.id || ''),
      sourceNodeId: startNodeId,
      targetNodeId: customNodeId,
    } : null;
  }

  if (taskKey === 'tutorial.basics.open_custom_node') {
    return selectedKind.value === 'node' && String(selectedId.value || '') === customNodeId
      ? { ...base, nodeId: customNodeId }
      : null;
  }

  if (taskKey === 'tutorial.basics.add_when_enter') {
    const block = customNode && serializedBlockRecord(
      customNode.workspace || {},
      'node_when_enter',
      bindings.whenEnterBlockId,
    );
    return block ? {
      ...base,
      nodeId: customNodeId,
      blockId: block.id,
      blockType: block.blockType,
      workspaceRole: 'node',
    } : null;
  }

  if (taskKey === 'tutorial.basics.add_set_position') {
    const block = customNode && serializedBlockRecord(
      customNode.workspace || {},
      'object_set_position',
      bindings.setPositionBlockId,
    );
    if (!block || !block.connected || block.parentBlockType !== 'node_when_enter') return null;
    return {
      ...base,
      nodeId: customNodeId,
      blockId: block.id,
      blockType: block.blockType,
      parentBlockType: block.parentBlockType,
      connected: true,
      workspaceRole: 'node',
    };
  }

  if (['tutorial.basics.set_position_model', 'tutorial.basics.set_start_x'].includes(taskKey)) {
    const block = customNode && serializedBlockRecord(
      customNode.workspace || {},
      'object_set_position',
      bindings.setPositionBlockId,
    );
    const modelName = String(block?.fields?.NAME || '');
    const x = Number(block?.fields?.X);
    if (
      !block
      || block.id !== String(bindings.setPositionBlockId || '')
      || !block.connected
      || block.parentBlockType !== 'node_when_enter'
    ) return null;
    if (taskKey === 'tutorial.basics.set_position_model' && !modelName) return null;
    if (taskKey === 'tutorial.basics.set_start_x' && !Number.isFinite(x)) return null;
    const fieldName = taskKey === 'tutorial.basics.set_position_model' ? 'NAME' : 'X';
    const value = fieldName === 'NAME' ? modelName : x;
    return {
      ...base,
      nodeId: customNodeId,
      blockId: block.id,
      blockType: block.blockType,
      parentBlockType: block.parentBlockType,
      connected: true,
      workspaceRole: 'node',
      fieldName,
      newValue: value,
      value,
      modelName,
      x,
      y: Number(block?.fields?.Y),
      z: Number(block?.fields?.Z),
    };
  }

  if (taskKey === 'tutorial.basics.add_while_active') {
    const block = customNode && serializedBlockRecord(
      customNode.workspace || {},
      'node_while_active',
      bindings.whileActiveBlockId,
    );
    return block ? {
      ...base,
      nodeId: customNodeId,
      blockId: block.id,
      blockType: block.blockType,
      workspaceRole: 'node',
    } : null;
  }

  if (taskKey === 'tutorial.basics.add_move_direction') {
    const block = customNode && serializedBlockRecord(
      customNode.workspace || {},
      'object_move_direction',
      bindings.moveDirectionBlockId,
    );
    if (!block || !block.connected || block.parentBlockType !== 'node_while_active') return null;
    return {
      ...base,
      nodeId: customNodeId,
      blockId: block.id,
      blockType: block.blockType,
      parentBlockType: block.parentBlockType,
      connected: true,
      workspaceRole: 'node',
    };
  }

  if ([
    'tutorial.basics.set_move_model',
    'tutorial.basics.set_move_direction',
    'tutorial.basics.set_move_speed',
  ].includes(taskKey)) {
    const block = customNode && serializedBlockRecord(
      customNode.workspace || {},
      'object_move_direction',
      bindings.moveDirectionBlockId,
    );
    const modelName = String(block?.fields?.NAME || '');
    const direction = String(block?.fields?.DIRECTION || '').toUpperCase();
    const speed = Number(block?.fields?.SPEED);
    if (
      !block
      || block.id !== String(bindings.moveDirectionBlockId || '')
      || !block.connected
      || block.parentBlockType !== 'node_while_active'
    ) return null;
    const fieldName = taskKey === 'tutorial.basics.set_move_model'
      ? 'NAME'
      : taskKey === 'tutorial.basics.set_move_direction' ? 'DIRECTION' : 'SPEED';
    const value = fieldName === 'NAME' ? modelName : fieldName === 'DIRECTION' ? direction : speed;
    if (fieldName === 'NAME' && !modelName) return null;
    if (fieldName === 'DIRECTION' && !direction) return null;
    if (fieldName === 'SPEED' && !Number.isFinite(speed)) return null;
    return {
      ...base,
      nodeId: customNodeId,
      blockId: block.id,
      blockType: block.blockType,
      parentBlockType: block.parentBlockType,
      connected: true,
      workspaceRole: 'node',
      fieldName,
      newValue: value,
      value,
      modelName,
      direction,
      speed,
    };
  }

  return null;
}

function observeTutorialNodeState() {
  tutorialObservationTimer = null;
  refreshEmbeddedWorkspaceStates();
  const details = tutorialNodeStateObservationDetails();
  if (!details) return;
  const signature = `${details.observedTaskKey}|${canonicalJson(details)}`;
  if (signature === lastTutorialObservationSignature) return;
  lastTutorialObservationSignature = signature;
  void cabbageContextService.recordEvent({
    type: 'tutorial_node_state_observed',
    category: 'tutorial',
    success: true,
    details,
  }).catch(() => {
    if (lastTutorialObservationSignature === signature) lastTutorialObservationSignature = '';
  });
}

function scheduleTutorialNodeStateObservation(delay = 80) {
  if (!componentMounted) return;
  if (tutorialObservationTimer) window.clearTimeout(tutorialObservationTimer);
  tutorialObservationTimer = window.setTimeout(observeTutorialNodeState, Math.max(0, delay));
}

function changedSerializedBlockIds(previousWorkspace = {}, nextWorkspace = {}) {
  const previousBlocks = serializedBlocksById(previousWorkspace);
  const nextBlocks = serializedBlocksById(nextWorkspace);
  const changed = [];
  for (const [blockId, block] of nextBlocks) {
    const previousBlock = previousBlocks.get(blockId);
    if (!previousBlock || canonicalJson(previousBlock) !== canonicalJson(block)) changed.push(blockId);
  }
  return { changed, visibleBlockCount: nextBlocks.size };
}

function findGeneratedGraphFocus(previous = {}, candidate = {}, selection = {}) {
  const previousNodes = new Map((previous.nodes || []).map((node) => [String(node.id || ''), node]));
  const nodeChoices = [];
  for (const node of candidate.nodes || []) {
    const nodeId = String(node.id || '');
    const oldNode = previousNodes.get(nodeId);
    const blockChange = changedSerializedBlockIds(oldNode?.workspace || {}, node.workspace || {});
    const workspaceChanged = !oldNode || canonicalJson(oldNode.workspace || {}) !== canonicalJson(node.workspace || {});
    const metadataChanged = !oldNode
      || oldNode.nodeType !== node.nodeType
      || String(oldNode.name || '') !== String(node.name || '')
      || String(oldNode.customName || '') !== String(node.customName || '');
    if (!workspaceChanged && !metadataChanged) continue;
    let score = workspaceChanged ? 500 : 200;
    if (!oldNode) score += 120;
    if (selection.kind === 'node' && selection.id === nodeId) score += 80;
    score += Math.min(60, blockChange.changed.length * 12 + blockChange.visibleBlockCount);
    nodeChoices.push({
      kind: 'node',
      id: nodeId,
      blockId: blockChange.changed[0] || '',
      visibleBlockCount: blockChange.visibleBlockCount,
      nodeName: displayNodeName(node),
      score,
    });
  }
  if (nodeChoices.length) return nodeChoices.sort((a, b) => b.score - a.score)[0];

  const previousEdges = new Map((previous.edges || []).map((edge) => [String(edge.id || ''), edge]));
  const edgeChoices = [];
  for (const edge of candidate.edges || []) {
    const edgeId = String(edge.id || '');
    const oldEdge = previousEdges.get(edgeId);
    const blockChange = changedSerializedBlockIds(oldEdge?.conditionWorkspace || {}, edge.conditionWorkspace || {});
    const changed = !oldEdge || canonicalJson(oldEdge) !== canonicalJson(edge);
    if (!changed) continue;
    let score = 420 + Math.min(50, blockChange.changed.length * 12 + blockChange.visibleBlockCount);
    if (selection.kind === 'edge' && selection.id === edgeId) score += 80;
    edgeChoices.push({
      kind: 'edge',
      id: edgeId,
      blockId: blockChange.changed[0] || '',
      visibleBlockCount: blockChange.visibleBlockCount,
      edgeName: String(edge.name || '\u6761\u4ef6'),
      score,
    });
  }
  if (edgeChoices.length) return edgeChoices.sort((a, b) => b.score - a.score)[0];

  if (selection.kind === 'node' && (candidate.nodes || []).some((node) => node.id === selection.id)) {
    const node = candidate.nodes.find((item) => item.id === selection.id);
    return {
      kind: 'node', id: selection.id, blockId: '',
      visibleBlockCount: serializedBlocksById(node?.workspace || {}).size,
      nodeName: displayNodeName(node), score: 0,
    };
  }
  if (selection.kind === 'edge' && (candidate.edges || []).some((edge) => edge.id === selection.id)) {
    const edge = candidate.edges.find((item) => item.id === selection.id);
    return {
      kind: 'edge', id: selection.id, blockId: '',
      visibleBlockCount: serializedBlocksById(edge?.conditionWorkspace || {}).size,
      edgeName: String(edge?.name || '\u6761\u4ef6'), score: 0,
    };
  }

  const fallbackNode = [...(candidate.nodes || [])]
    .sort((a, b) => serializedBlocksById(b.workspace || {}).size - serializedBlocksById(a.workspace || {}).size)[0];
  return fallbackNode ? {
    kind: 'node', id: String(fallbackNode.id || ''), blockId: '',
    visibleBlockCount: serializedBlocksById(fallbackNode.workspace || {}).size,
    nodeName: displayNodeName(fallbackNode), score: 0,
  } : null;
}

async function revealGeneratedGraphFocus(focus) {
  if (!focus) return true;
  if (focus.kind === 'node') {
    const node = graph.nodes.find((item) => String(item.id) === focus.id);
    if (node) {
      focusWorldPointIfNeeded(
        (Number(node.x) || 0) + NODE_WIDTH / 2,
        (Number(node.y) || 0) + nodeHeight(node) / 2
      );
    }
  } else if (focus.kind === 'edge') {
    const edge = graph.edges.find((item) => String(item.id) === focus.id);
    if (edge) focusGuidanceEdge(edge);
  }
  if (!focus.blockId) return true;
  await nextTick();
  await new Promise((resolve) => window.requestAnimationFrame(resolve));
  activeBlocklyRef.value?.resizeBlockly?.();
  return activeBlocklyRef.value?.focusBlock?.(focus.blockId) === true;
}
const PORT_SIDES = ['left', 'right', 'bottom'];
function normalizeEndpoint(e) {
  const rawIndex = Number(e?.index);
  return {
    nodeId: e?.nodeId || '',
    side: PORT_SIDES.includes(e?.side) ? e.side : 'right',
    index: Number.isFinite(rawIndex) ? Math.max(0, Math.trunc(rawIndex)) : 0,
  };
}
function endpointPortKey(endpoint) {
  return `${endpoint.nodeId}:${endpoint.side}:${endpoint.index}`;
}
function redistributeDuplicatePorts(edges) {
  const reserved = new Set();
  for (const edge of edges) {
    reserved.add(endpointPortKey(edge.source));
    reserved.add(endpointPortKey(edge.target));
  }

  const seen = new Map();
  let changed = false;
  for (const edge of edges) {
    for (const role of ['source', 'target']) {
      const endpoint = edge[role];
      const originalKey = endpointPortKey(endpoint);
      const occurrence = seen.get(originalKey) || 0;
      seen.set(originalKey, occurrence + 1);
      if (occurrence === 0) continue;

      let nextIndex = 0;
      let nextKey = `${endpoint.nodeId}:${endpoint.side}:${nextIndex}`;
      while (reserved.has(nextKey)) {
        nextIndex += 1;
        nextKey = `${endpoint.nodeId}:${endpoint.side}:${nextIndex}`;
      }
      endpoint.index = nextIndex;
      reserved.add(nextKey);
      changed = true;
    }
  }
  return changed;
}
function normalizeGraph(raw) {
  const n = raw && typeof raw === 'object' ? raw : {};
  return {
    version: 1,
    nodes: Array.isArray(n.nodes)
      ? n.nodes.map((node, i) => {
          const nodeType = ['start', 'end', 'custom'].includes(node.nodeType)
            ? node.nodeType
            : i === 0
              ? 'start'
              : 'custom';
          const legacyCustomName =
            nodeType === 'custom' && node.name && node.name !== '开始' && node.name !== '结束'
              ? String(node.name).trim()
              : '';
          const hasStoredCustomName = Object.prototype.hasOwnProperty.call(node, 'customName');
          const customName = hasStoredCustomName
            ? String(node.customName || '').slice(0, 24)
            : (legacyCustomName || `状态${i + 1}`).slice(0, 24);
          return {
            id: node.id || makeId('node'),
            macroType: node.macroType || 'state',
            nodeType,
            name: nodeType === 'start' ? '开始' : nodeType === 'end' ? '结束' : customName,
            customName,
            x: Number.isFinite(Number(node.x)) ? Number(node.x) : 40 + i * 24,
            y: Number.isFinite(Number(node.y)) ? Number(node.y) : 80 + i * 24,
            workspace: node.workspace && typeof node.workspace === 'object' ? node.workspace : {},
          };
        })
      : [],
    edges: Array.isArray(n.edges)
      ? n.edges
          .filter((e) => e?.source && e?.target)
          .map((e) => ({
            id: e.id || makeId('edge'),
            source: normalizeEndpoint(e.source),
            target: normalizeEndpoint(e.target),
            name: e.name || '',
            conditionWorkspace:
              e.conditionWorkspace && typeof e.conditionWorkspace === 'object'
                ? e.conditionWorkspace
                : {},
          }))
      : [],
    globalVariablesWorkspace:
      n.globalVariablesWorkspace && typeof n.globalVariablesWorkspace === 'object'
        ? n.globalVariablesWorkspace
        : {},
  };
}
function getNodeFrom(list, id) {
  return list.find((n) => n.id === id);
}
function applyGraph(next) {
  graph.version = 1;
  graph.nodes = next.nodes;
  const validEdges = next.edges.filter(
    (e) => getNodeFrom(next.nodes, e.source.nodeId) && getNodeFrom(next.nodes, e.target.nodeId)
  );
  const portsRedistributed = redistributeDuplicatePorts(validEdges);
  graph.edges = validEdges;
  graph.globalVariablesWorkspace = next.globalVariablesWorkspace || {};
  selectedKind.value = graph.nodes.length ? 'node' : '';
  selectedId.value = graph.nodes[0]?.id || '';
  pendingPort.value = null;
  return portsRedistributed;
}
function scheduleSave() {
  if (isLoading || !initialLoadComplete || !targetReady.value) return;
  graphDirty = true;
  saveLabel.value = '正在保存到当前世界...';
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = setTimeout(() => {
    saveTimer = null;
    saveNow();
  }, SAVE_DELAY);
}

function storageKeyForTarget(target) {
  const projectScope = encodeURIComponent(
    normalizeProjectPath(target?.projectPath || activeProjectPath.value || readActiveProjectPath()) || 'unknown-project'
  );
  return `corona-nodegraph-ui:v2:${projectScope}:${target.targetType || 'actor'}:${target.sceneName || ''}:${target.actorName || ''}`;
}
function currentTarget(projectPathOverride = '') {
  return {
    targetType: normalizedTargetType.value,
    sceneName: isProjectTarget.value ? '' : props.sceneName || '',
    actorName: isProjectTarget.value ? '' : props.actorName || '',
    projectPath: projectPathOverride || loadedProjectPath || activeProjectPath.value || readActiveProjectPath(),
  };
}
async function saveNow(targetOverride = null, { force = false } = {}) {
  if (isLoading || (!initialLoadComplete && !force)) return false;
  if (!graphDirty && !force) return true;
  if (saveInFlight) {
    saveQueued = true;
    const currentSaveSucceeded = await saveInFlight;
    if (!currentSaveSucceeded) return false;
    if (graphDirty) return saveNow(targetOverride, { force });
    return true;
  }

  const target = { ...currentTarget(), ...(targetOverride || {}) };
  const expectedProjectPath = normalizeProjectPath(target.projectPath);
  const currentProjectPath = normalizeProjectPath(activeProjectPath.value || readActiveProjectPath());
  if (expectedProjectPath && currentProjectPath && expectedProjectPath !== currentProjectPath) {
    saveLabel.value = '项目已切换，已跳过旧节点图保存';
    return false;
  }
  const isProject = (target.targetType === 'model' ? 'actor' : target.targetType || 'actor') === 'project';
  if (!isProject && !target.actorName) return false;
  if (saveTimer) {
    clearTimeout(saveTimer);
    saveTimer = null;
  }
  const snapshot = graphSnapshot();
  const snapshotFingerprint = JSON.stringify(snapshot);
  let runnable = true;
  let code = '';
  const validationErrors = [];
  try {
    validateNodeGraph(snapshot);
    code = nodeGraphToCode(snapshot);
  } catch (error) {
    runnable = false;
    validationErrors.push(String(error?.message || error));
  }
  try {
    window.localStorage?.setItem(storageKeyForTarget(target), snapshotFingerprint);
  } catch (error) {
    logError('保存本地节点图失败', error);
  }

  saveInFlight = (async () => {
    try {
      const response = bridgeResult(await editorApi.scratch.saveBlocklyTarget({
        target_type: target.targetType === 'model' ? 'actor' : target.targetType || 'actor',
        scene_name: isProject ? '' : target.sceneName || '',
        actor_name: isProject ? '' : target.actorName || '',
        script_kind: 'node_graph',
        project_path: target.projectPath || '',
        workspace: snapshot,
        code,
        enabled: targetEnabledByKey.get(storageKeyForTarget(target)) ?? true,
        runnable,
        validation_errors: validationErrors,
      }));
      if (response?.status === 'error') {
        if (response.code === 'NO_ACTIVE_PROJECT' || response.code === 'PROJECT_CONTEXT_CHANGED') {
          saveBlockedProjectPath = String(response.project_path || target.projectPath || '').trim();
        }
        throw new Error(response.message || '保存节点图失败');
      }
      graphDirty = JSON.stringify(graphSnapshot()) !== snapshotFingerprint;
      saveBlockedProjectPath = '';
      saveLabel.value = runnable
        ? `已实时保存到当前世界 ${new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit', second: '2-digit' })}`
        : `已实时保存（不可运行：${validationErrors[0]}）`;
      return true;
    } catch (error) {
      graphDirty = true;
      logError('保存项目节点图失败', error);
      saveLabel.value = '项目保存失败（本地副本已保留）';
      return false;
    }
  })();

  let saveSucceeded = false;
  try {
    saveSucceeded = await saveInFlight;
    return saveSucceeded;
  } finally {
    saveInFlight = null;
    const action = nextSaveAction({
      succeeded: saveSucceeded,
      saveQueued,
      graphDirty,
    });
    saveQueued = false;
    if (action === 'resave' && componentMounted && initialLoadComplete && !saveTimer) {
      saveTimer = setTimeout(() => {
        saveTimer = null;
        saveNow();
      }, SAVE_DELAY);
    }
  }
}

async function loadGraphForCurrentTarget() {
  const loadSequence = ++graphLoadSequence;
  resetZoom();
  initialLoadComplete = false;
  if (!targetReady.value) {
    applyGraph(normalizeGraph({}));
    initialLoadComplete = true;
    graphDirty = false;
    saveLabel.value = '等待当前世界加载';
    return;
  }
  isLoading = true;
  saveLabel.value = '\u9879\u76ee\u52a0\u8f7d\u4e2d...';
  let shouldMigrateLocal = false;
  let loadCancelled = false;
  try {
    await refreshActiveProjectPath();
    if (!componentMounted || loadSequence !== graphLoadSequence) return;
    let requestProjectPath = activeProjectPath.value || readActiveProjectPath();
    let target = null;
    let response = null;

    for (let attempt = 0; attempt < 2; attempt += 1) {
      target = currentTarget(requestProjectPath);
      const isProject = (target.targetType === 'model' ? 'actor' : target.targetType || 'actor') === 'project';
      response = bridgeResult(await editorApi.scratch.loadBlocklyTarget({
        target_type: target.targetType === 'model' ? 'actor' : target.targetType || 'actor',
        scene_name: isProject ? '' : target.sceneName || '',
        actor_name: isProject ? '' : target.actorName || '',
        script_kind: 'node_graph',
        project_path: requestProjectPath,
      }));
      if (!componentMounted || loadSequence !== graphLoadSequence) return;

      if (response?.status === 'error' && response?.code === 'PROJECT_CONTEXT_CHANGED' && attempt === 0) {
        const backendProjectPath = String(response.project_path || '').trim();
        if (backendProjectPath) {
          requestProjectPath = backendProjectPath;
          activeProjectPath.value = backendProjectPath;
          window.localStorage?.setItem('corona.activeProjectPath', backendProjectPath);
          continue;
        }
      }
      break;
    }

    const responseProjectPath = String(response?.project_path || requestProjectPath || '').trim();
    const currentProjectPath = activeProjectPath.value || readActiveProjectPath();
    if (normalizeProjectPath(responseProjectPath) !== normalizeProjectPath(currentProjectPath)) return;

    activeProjectPath.value = responseProjectPath;
    loadedProjectPath = responseProjectPath;
    window.localStorage?.setItem('corona.activeProjectPath', responseProjectPath);
    target.projectPath = responseProjectPath;

    if (response?.status === 'error') throw new Error(response.message || '\u8282\u70b9\u56fe\u52a0\u8f7d\u5931\u8d25');
    if (response?.status === 'loaded') {
      targetEnabledByKey.set(storageKeyForTarget(target), response.target?.enabled !== false);
      shouldMigrateLocal = applyGraph(normalizeGraph(response.workspace || {}));
      saveLabel.value = '\u9879\u76ee\u8282\u70b9\u56fe\u5df2\u52a0\u8f7d';
    } else {
      targetEnabledByKey.set(storageKeyForTarget(target), true);
      const raw = window.localStorage?.getItem(storageKeyForTarget(target));
      const portsRedistributed = applyGraph(normalizeGraph(raw ? JSON.parse(raw) : {}));
      shouldMigrateLocal = Boolean(raw) || portsRedistributed;
      saveLabel.value = raw ? '\u5df2\u52a0\u8f7d\u5f53\u524d\u9879\u76ee\u7684\u672c\u5730\u8282\u70b9\u56fe\uff0c\u6b63\u5728\u8fc1\u79fb...' : '\u65b0\u8282\u70b9\u56fe';
    }
  } catch (error) {
    logError('\u52a0\u8f7d\u9879\u76ee\u8282\u70b9\u56fe\u5931\u8d25', error);
    try {
      const target = currentTarget(activeProjectPath.value || readActiveProjectPath());
      const raw = window.localStorage?.getItem(storageKeyForTarget(target));
      const portsRedistributed = applyGraph(normalizeGraph(raw ? JSON.parse(raw) : {}));
      shouldMigrateLocal = Boolean(raw) || portsRedistributed;
      saveLabel.value = raw ? '\u9879\u76ee\u52a0\u8f7d\u5931\u8d25\uff0c\u5df2\u4f7f\u7528\u5f53\u524d\u9879\u76ee\u672c\u5730\u526f\u672c' : '\u8282\u70b9\u56fe\u52a0\u8f7d\u5931\u8d25';
    } catch (_) {
      applyGraph(normalizeGraph({}));
      saveLabel.value = '\u8282\u70b9\u56fe\u52a0\u8f7d\u5931\u8d25';
    }
  } finally {
    loadCancelled = !componentMounted || loadSequence !== graphLoadSequence;
    if (!loadCancelled) {
      initialLoadComplete = true;
      graphDirty = false;
      isLoading = false;
      await loadEmbeddedWorkspaceStates();
      loadCancelled = !componentMounted || loadSequence !== graphLoadSequence;
      if (!loadCancelled) {
        reconcileSceneActorReferenceIssues();
        requestNodeGraphReview();
      }
    }
  }
  if (loadCancelled) return;
  if (shouldMigrateLocal && componentMounted && loadSequence === graphLoadSequence) {
    graphDirty = true;
    await saveNow();
  }
}

function bridgeResult(response) {
  return response?.data?.data ?? response?.data ?? response ?? {};
}

function normalizeNodeGraphScriptStatus(response = {}) {
  const status = bridgeResult(response) || {};
  return {
    ...status,
    status: String(status.status || status.state || '').trim().toLowerCase(),
    currentNodeId: String(status.currentNodeId || status.current_node_id || ''),
    waitingEdgeId: String(status.waitingEdgeId || status.waiting_edge_id || ''),
    waitingEdgeName: String(status.waitingEdgeName || status.waiting_edge_name || ''),
  };
}

async function loadEmbeddedWorkspaceStates({ strict = false, verifyActive = false } = {}) {
  await nextTick();

  const loadEditor = async (editor, state, label, required) => {
    if (!editor) {
      if (required) throw new Error(`${label}尚未挂载`);
      return null;
    }
    const ready = await editor.whenReady?.();
    if (ready?.success === false) {
      if (required) throw new Error(`${label}初始化失败：${ready.error || '未知错误'}`);
      return ready;
    }
    const loaded = editor.loadState?.(state || {});
    if (loaded?.success !== true) {
      if (required) throw new Error(`${label}加载失败：${loaded?.error || '工作区尚未就绪'}`);
      return loaded || null;
    }
    return loaded;
  };

  const variables = await loadEditor(
    variablesBlocklyRef.value,
    graph.globalVariablesWorkspace || {},
    '全局变量积木区',
    strict
  );
  const activeRequired = strict && Boolean(activeEditorKey.value);
  const active = await loadEditor(
    activeBlocklyRef.value,
    activeEditorState.value || {},
    selectedEdge.value ? '连线条件积木区' : '节点内部积木区',
    activeRequired
  );

  if (verifyActive && activeEditorKey.value) {
    const expectedIds = [...serializedBlocksById(activeEditorState.value || {}).keys()];
    const loadedIds = new Set(Array.isArray(active?.blockIds) ? active.blockIds.map(String) : []);
    const missingIds = expectedIds.filter((blockId) => !loadedIds.has(String(blockId)));
    if (missingIds.length || Number(active?.blockCount || 0) !== expectedIds.length) {
      throw new Error(
        `生成的积木没有完整加载到编辑区（期望 ${expectedIds.length} 个，实际 ${Number(active?.blockCount || 0)} 个）`
      );
    }
  }

  updateCanvasSize();
  return { variables, active };
}

async function handleGeneratedNodeGraph(result) {
  if (!isProjectTarget.value) {
    return { success: false, errors: ['\u5185\u90e8 AI \u7ed3\u679c\u53ea\u80fd\u5e94\u7528\u5230\u9879\u76ee\u5e38\u9a7b\u8282\u70b9\u56fe'], warnings: [] };
  }

  const scope = normalizeProjectPath(activeProjectPath.value || readActiveProjectPath());
  const previous = graphSnapshot();
  const previousSelection = { kind: selectedKind.value, id: selectedId.value };
  const currentProjectScopeId = reviewScopeId(scope);
  const currentRevision = generatedNodeGraphRevision({
    workspace: previous,
    projectContext: generatedProjectContext(),
  }, scope);
  if (String(result?.projectScopeId || '') !== currentProjectScopeId) {
    return { success: false, errors: ['AI \u7ed3\u679c\u5c5e\u4e8e\u53e6\u4e00\u4e2a\u4e16\u754c\uff0c\u5df2\u62d2\u7edd\u5e94\u7528'], warnings: [] };
  }
  if (String(result?.baseGraphRevision || '') !== currentRevision) {
    return { success: false, errors: ['\u751f\u6210\u671f\u95f4\u8282\u70b9\u903b\u8f91\u5df2\u6539\u53d8\uff0c\u8fdf\u5230\u7684 AI \u7ed3\u679c\u672a\u8986\u76d6\u5f53\u524d\u7f16\u8f91'], warnings: [] };
  }
  const storageKey = storageKeyForTarget(currentTarget());
  let previousLocal = null;
  try {
    previousLocal = window.localStorage?.getItem(storageKey) ?? null;
  } catch (_) {}

  try {
    // The service validates the raw envelope first. Normalize only after that so
    // missing IDs, invalid positions, and dangling edges cannot be silently repaired.
    const candidate = normalizeGraph(result.workspace);
    const analysis = validateNodeGraph(candidate);
    nodeGraphToCode(candidate); // Deserializes every visible block and validates conditions/generators.
    const focus = findGeneratedGraphFocus(previous, candidate, previousSelection);

    isLoading = true;
    applyGraph(candidate);
    if (focus?.kind === 'node' && graph.nodes.some((node) => String(node.id) === focus.id)) {
      selectedKind.value = 'node';
      selectedId.value = focus.id;
    } else if (focus?.kind === 'edge' && graph.edges.some((edge) => String(edge.id) === focus.id)) {
      selectedKind.value = 'edge';
      selectedId.value = focus.id;
    }
    const embeddedLoad = await loadEmbeddedWorkspaceStates({ strict: true, verifyActive: true });
    const focused = await revealGeneratedGraphFocus(focus);
    if (focus?.blockId && !focused) throw new Error('生成的目标积木没有出现在当前积木编辑区');
    isLoading = false;
    graphDirty = true;

    if (!(await saveNow())) throw new Error('\u5185\u90e8 AI \u8282\u70b9\u56fe\u4fdd\u5b58\u5931\u8d25');
    requestNodeGraphReview(0);
    saveLabel.value = '\u5185\u90e8 AI \u8282\u70b9\u56fe\u5df2\u5e94\u7528';
    return {
      success: true,
      errors: [],
      warnings: Array.isArray(analysis?.warnings) ? analysis.warnings : [],
      summary: {
        nodeCount: candidate.nodes.length,
        edgeCount: candidate.edges.length,
        focusedKind: focus?.kind || '',
        focusedId: focus?.id || '',
        focusedNodeName: focus?.nodeName || '',
        focusedEdgeName: focus?.edgeName || '',
        visibleBlockCount: Number(embeddedLoad?.active?.blockCount || 0),
      },
    };
  } catch (error) {
    isLoading = true;
    applyGraph(normalizeGraph(previous));
    if (previousSelection.kind === 'node' && graph.nodes.some((node) => node.id === previousSelection.id)) {
      selectedKind.value = 'node';
      selectedId.value = previousSelection.id;
    } else if (previousSelection.kind === 'edge' && graph.edges.some((edge) => edge.id === previousSelection.id)) {
      selectedKind.value = 'edge';
      selectedId.value = previousSelection.id;
    }
    await loadEmbeddedWorkspaceStates();
    isLoading = false;
    try {
      if (previousLocal == null) window.localStorage?.removeItem(storageKey);
      else window.localStorage?.setItem(storageKey, previousLocal);
    } catch (_) {}
    saveLabel.value = `\u5185\u90e8 AI \u5e94\u7528\u5931\u8d25\uff1a${error?.message || error}`;
    return { success: false, errors: [String(error?.message || error)], warnings: [] };
  } finally {
    isLoading = false;
  }
}

function generatedProjectContext() {
  return {
    sceneName: sceneActorContext.sceneName || props.sceneName || 'default',
    actorContextAvailable: sceneActorContext.available === true,
    actorContextRevision: sceneActorContext.revision,
    actors: sceneActorContext.actors.map((actor) => ({ ...actor })),
  };
}

function generatedNodeGraphSnapshot() {
  const scope = normalizeProjectPath(activeProjectPath.value || readActiveProjectPath());
  const workspace = graphSnapshot();
  const projectContext = generatedProjectContext();
  return {
    targetId: 'node_graph:project:global',
    projectScopeId: reviewScopeId(scope),
    graphRevision: generatedNodeGraphRevision({ workspace, projectContext }, scope),
    workspace,
    projectContext,
  };
}

function syncGeneratedNodeGraphConsumer() {
  unregisterAiNodeGraphConsumer?.();
  unregisterAiNodeGraphConsumer = null;
  if (componentMounted && isProjectTarget.value) {
    unregisterAiNodeGraphConsumer = registerGeneratedNodeGraphConsumer({
      handler: handleGeneratedNodeGraph,
      getSnapshot: generatedNodeGraphSnapshot,
      instanceId: `project-node-graph:${projectStorageScope.value}`,
    });
  }
}



function clearRunPoll() {
  if (runPollTimer) window.clearInterval(runPollTimer);
  runPollTimer = null;
}
const STATUS_STARTING = "\u542f\u52a8\u4e2d...";
const STATUS_WAITING = "\u7b49\u5f85\u8fde\u7ebf\u6761\u4ef6\uff1a";
const STATUS_RUNNING = "\u8fd0\u884c\u4e2d\uff1a";
const STATUS_RUNNING_BASE = "\u8fd0\u884c\u4e2d";
const STATUS_COMPLETED = "\u5df2\u5b8c\u6210";
const STATUS_STOPPED = "\u5df2\u505c\u6b62";
const STATUS_FAILED = "\u6267\u884c\u5931\u8d25\uff1a";
const STATUS_UNKNOWN_ERROR = "\u672a\u77e5\u9519\u8bef";
const STATUS_QUERY_FAILED = "\u72b6\u6001\u67e5\u8be2\u5931\u8d25";
const LOG_QUERY_FAILED = "\u67e5\u8be2\u8282\u70b9\u56fe\u8fd0\u884c\u72b6\u6001\u5931\u8d25";
function formatRunState(status) {
  const state = status?.status || 'idle';
  if (state === 'starting') return STATUS_STARTING;
  if (state === 'running') {
    if (status?.waitingEdgeName) return `${STATUS_WAITING}${status.waitingEdgeName}`;
    if (status?.currentNodeName) return `${STATUS_RUNNING}${status.currentNodeName}`;
    return STATUS_RUNNING_BASE;
  }
  if (state === 'completed') return STATUS_COMPLETED;
  if (state === 'stopped') return STATUS_STOPPED;
  if (state === 'error') return `${STATUS_FAILED}${status?.error || STATUS_UNKNOWN_ERROR}`;
  return '';
}
function formatRunDetail(status) {
  if (!status || typeof status !== 'object') return '';
  const lines = [];
  if (status.error) lines.push(`错误：${status.error}`);
  if (isProjectTarget.value) {
    const sceneName = status.resolvedSceneName
      || status.nativeScene
      || sceneActorContext.sceneName
      || props.sceneName
      || '当前场景';
    const actorNames = sceneActorContext.actors
      .map((actor) => String(actor?.name || '').trim())
      .filter(Boolean);
    lines.push('运行作用域：当前场景（项目节点图）');
    lines.push(`运行场景：${sceneName}`);
    lines.push('对象来源：场景管理中已导入的物体，由各积木的对象参数指定');
    if (actorNames.length) lines.push(`可用物体：${actorNames.join(', ')}`);
    else if (!sceneActorContext.available) lines.push('可用物体：暂时无法读取场景对象列表');
  } else {
    if (status.requestedScene || status.requestedActor) lines.push(`请求目标：${status.requestedScene || '(空场景)'} / ${status.requestedActor || '(空物体)'}`);
    if (status.resolvedSceneName || status.resolvedActorName) lines.push(`绑定目标：${status.resolvedSceneName || '(空场景)'} / ${status.resolvedActorName || '(空物体)'}`);
    if (status.bindingMode) lines.push(`绑定模式：${status.bindingMode === 'native_editor' ? 'Native Editor' : 'Python Scene'}`);
    if (Array.isArray(status.pythonScenes)) lines.push(`Python 场景：${status.pythonScenes.join(', ') || '(空)'}`);
    if (status.nativeScene) lines.push(`原生场景：${status.nativeScene}`);
    if (Array.isArray(status.actorCandidates) && status.actorCandidates.length) lines.push(`物体候选：${status.actorCandidates.join(', ')}`);
  }
  return lines.join('\n');
}
function startRunPoll() {
  clearRunPoll();
  runPollTimer = window.setInterval(async () => {
    if (!codeRunning.value) return;
    try {
      const status = normalizeNodeGraphScriptStatus(await editorApi.scratch.getScriptStatus());
      currentRunNodeId.value = status?.currentNodeId || '';
      setNodeGraphInputLocked(Boolean(status?.inputLocked));
      runStatus.value = formatRunState(status);
      runDetail.value = formatRunDetail(status);
      if (!['starting', 'running'].includes(status?.status)) {
        if (status?.status === 'completed') reportNodeRunTerminal(true);
        else if (status?.status === 'error') reportNodeRunTerminal(false, status?.error || runStatus.value);
        else resetNodeRunLifecycle();
        clearRunPoll();
        codeRunning.value = false;
        runBusy.value = false;
        startedRunForTarget = false;
        setNodeGraphInputLocked(false);
      }
    } catch (error) {
      reportNodeRunTerminal(false, error?.message || error);
      clearRunPoll();
      codeRunning.value = false;
      startedRunForTarget = false;
      currentRunNodeId.value = '';
      setNodeGraphInputLocked(false);
      runStatus.value = STATUS_QUERY_FAILED;
      logError(LOG_QUERY_FAILED, error);
    }
  }, 300);
}
function scriptStatusBelongsToCurrentTarget(status = {}) {
  const backendTargetType = String(status?.targetType || '').trim().toLowerCase();
  return !backendTargetType || backendTargetType === normalizedTargetType.value;
}
function scriptStatusNeedsStop(status = {}) {
  const state = String(status?.status || '').trim().toLowerCase();
  return ['starting', 'running', 'stopping'].includes(state)
    || Boolean(status?.threadAlive)
    || Boolean(status?.inputLocked)
    || Boolean(status?.snapshotCaptured ?? status?.hasSnapshot ?? status?.has_snapshot);
}
async function stopNodeGraphRun(statusText = '已停止', restoreState = false, verifyBackend = false) {
  clearRunPoll();
  let shouldStop = startedRunForTarget || codeRunning.value || runBusy.value;
  if (verifyBackend && !shouldStop) {
    try {
      const status = bridgeResult(await editorApi.scratch.getScriptStatus()) || {};
      shouldStop = scriptStatusBelongsToCurrentTarget(status) && scriptStatusNeedsStop(status);
    } catch (error) {
      // This endpoint only controls the standalone node/script executor, not game preview.
      // If status lookup fails while closing, stopping is the safer fallback.
      shouldStop = true;
      logError('关闭节点窗口前查询运行状态失败', error);
    }
  }
  try {
    if (shouldStop) {
      const response = bridgeResult(await editorApi.scratch.stopScriptExecution(Boolean(restoreState)));
      if (restoreState) {
        if (response?.restored) {
          runStatus.value = '已停止并恢复运行前状态';
          runDetail.value = '';
        } else if (response?.restoreError) {
          runStatus.value = `已停止，但场景恢复失败：${response.restoreError}`;
          runDetail.value = response.restoreError;
        } else {
          runStatus.value = statusText;
        }
      } else {
        runStatus.value = statusText;
      }
    }
  } catch (error) {
    runStatus.value = `停止节点图失败：${error?.message || error}`;
    logError('停止节点图失败', error);
  } finally {
    startedRunForTarget = false;
    codeRunning.value = false;
    runBusy.value = false;
    currentRunNodeId.value = '';
    setNodeGraphInputLocked(false);
    resetNodeRunLifecycle();
  }
}
async function stopForPanelClose() {
  panelClosing = true;
  if (!panelCloseStopPromise) {
    // Closing can be reported by the title bar, pagehide, beforeunload and both
    // parent/child unmount hooks. Keep one promise for the rest of this component
    // lifetime so the backend stop and input unlock are never issued repeatedly.
    panelCloseStopPromise = stopNodeGraphRun('\u8282\u70b9\u7a97\u53e3\u5df2\u5173\u95ed\uff0c\u8fd0\u884c\u5df2\u505c\u6b62', false, true);
  }
  return panelCloseStopPromise;
}

defineExpose({ stopForPanelClose });

function normalizeGamePreviewStatus(payload = {}) {
  const status = payload?.data ?? payload ?? {};
  const runningCount = Number(status.runningCount ?? status.running_count ?? 0);
  const hasSnapshot = Boolean(status.hasSnapshot ?? status.has_snapshot);
  const active = ['starting', 'running', 'stopping'].includes(status.status)
    || runningCount > 0
    || hasSnapshot;
  return { ...status, active, scope: status.scope || 'project' };
}
function applyGamePreviewGuard(payload = {}) {
  const preview = normalizeGamePreviewStatus(payload);
  const previousLabel = globalPreviewRunLabel.value;
  const wasPreviewMessage = runStatus.value === previousLabel;
  globalPreviewActive.value = preview.active;
  globalPreviewScope.value = preview.active ? preview.scope : '';
  if (preview.active && !codeRunning.value) {
    runStatus.value = globalPreviewRunLabel.value;
    const errors = Array.isArray(preview.errors) ? preview.errors : [];
    const warnings = Array.isArray(preview.warnings) ? preview.warnings : [];
    runDetail.value = [...errors, ...warnings].filter(Boolean).join('\n');
  } else if (!preview.active && wasPreviewMessage) {
    runStatus.value = '';
    runDetail.value = '';
  }
  return preview;
}
function onGamePreviewStatus(event) {
  applyGamePreviewGuard(event?.detail || {});
}
function onViewportControlsState(state = {}) {
  if (state?.preview && typeof state.preview === 'object') {
    applyGamePreviewGuard(state.preview);
  }
}
function startGamePreviewGuardPoll() {
  if (gamePreviewGuardTimer) window.clearInterval(gamePreviewGuardTimer);
  gamePreviewGuardTimer = window.setInterval(() => {
    refreshGamePreviewGuard().catch(() => {});
  }, 800);
}
async function refreshGamePreviewGuard() {
  if (window.__coronaPreviewActionPending) {
    return applyGamePreviewGuard({
      status: 'starting',
      scope: window.__coronaPreviewPendingScope || 'project',
    });
  }
  try {
    return applyGamePreviewGuard(await editorApi.scratch.getGamePreviewStatus());
  } catch (error) {
    logError('查询全局运行状态失败', error);
    return { active: false, scope: '' };
  }
}

function handleRunButtonClick() {
  if (!codeRunning.value) reportNodeRunClicked();
  return handleToggleRun();
}

async function handleToggleRun() {
  if (panelClosing || runBusy.value) return;
  runBusy.value = true;
  try {
    if (codeRunning.value) {
      await stopNodeGraphRun('已停止', true);
      return;
    }
    beginNodeRunAttempt();
    if (!targetReady.value) {
      runStatus.value = '请先选择运行目标';
      reportNodeRunTerminal(false, runStatus.value);
      return;
    }
    const preview = await refreshGamePreviewGuard();
    if (panelClosing || !componentMounted) return;
    if (preview.active) {
      runStatus.value = globalPreviewRunLabel.value;
      resetNodeRunLifecycle();
      return;
    }
    // A terminal error is a diagnostic, not a latch. Keep it visible until the
    // next click, then rebuild from the current workspaces and start a fresh run.
    clearRunPoll();
    codeRunning.value = false;
    startedRunForTarget = false;
    currentRunNodeId.value = '';
    runWarnings.value = [];
    runDetail.value = '';
    runDetailVisible.value = false;
    setNodeGraphInputLocked(false);
    refreshEmbeddedWorkspaceStates();
    await saveNow();
    if (panelClosing || !componentMounted) return;
    let code;
    try {
      const snapshot = graphSnapshot();
      const analysis = validateNodeGraph(snapshot);
      runWarnings.value = analysis.warnings || [];
      code = nodeGraphToCode(snapshot);
    } catch (error) {
      runStatus.value = `生成失败：${error?.message || error}`;
      reportNodeRunTerminal(false, error?.message || error);
      logError('生成节点图代码失败', error);
      return;
    }
    runStatus.value = '启动中...';
    const response = bridgeResult(
      await editorApi.scratch.executePythonCode(
        code,
        0,
        isProjectTarget.value ? '' : props.sceneName || '',
        isProjectTarget.value ? '' : props.actorName,
        normalizedTargetType.value
      )
    );
    if (panelClosing || !componentMounted) {
      await editorApi.scratch.stopScriptExecution(false).catch(() => {});
      setNodeGraphInputLocked(false);
      return;
    }
    if (response?.outcome === 'preview_running') {
      // A global preview owns this target. Treat the race as an ownership handoff
      // instead of leaving a misleading node-graph execution error.
      resetNodeRunLifecycle();
      applyGamePreviewGuard({
        status: response.previewStatus || 'running',
        scope: response.previewScope || 'project',
        runningCount: 1,
      });
      return;
    }
    if (response?.status === 'error' || response?.success === false) {
      throw new Error(response?.message || '后端拒绝执行节点图');
    }
    startedRunForTarget = true;
    codeRunning.value = true;
    reportNodeRunStarted();
    setNodeGraphInputLocked(true);
    runStatus.value = runWarnings.value.length ? `\u8fd0\u884c\u4e2d\uff08${runWarnings.value[0]}\uff09` : '\u8fd0\u884c\u4e2d';
    startRunPoll();
  } catch (error) {
    startedRunForTarget = false;
    codeRunning.value = false;
    clearRunPoll();
    currentRunNodeId.value = '';
    setNodeGraphInputLocked(false);
    runStatus.value = `执行失败：${error?.message || error}`;
    reportNodeRunTerminal(false, error?.message || error);
    logError('执行节点图失败', error);
  } finally {
    runBusy.value = false;
  }
}

function currentProjectNodeGraphRuntimeState() {
  return {
    ready: Boolean(componentMounted && unregisterProjectNodeGraphRuntimeHandler),
    running: codeRunning.value,
    busy: runBusy.value,
    status: runStatus.value,
    detail: runDetail.value,
  };
}
function syncProjectNodeGraphRuntimeState() {
  if (!isProjectTarget.value || !unregisterProjectNodeGraphRuntimeHandler) return;
  publishProjectNodeGraphRuntimeState(currentProjectNodeGraphRuntimeState());
}
watch(
  [codeRunning, runBusy, runStatus, runDetail],
  syncProjectNodeGraphRuntimeState
);
watch(
  () => [activeNodeTutorialTaskKey.value, canonicalJson(tutorialNodeBindings.value)],
  () => {
    lastTutorialObservationSignature = '';
    scheduleTutorialNodeStateObservation(0);
  }
);

async function persistResolvedActorIssues(actions = []) {
  if (!actions.length) return;
  publishCabbageAssistantContext(cabbageAssistant);
  for (const action of actions) {
    try {
      await cabbageContextService.updateTask(action);
    } catch (error) {
      logError('Failed to resolve stale actor issue', error);
    }
  }
}
function reconcileSceneActorReferenceIssues() {
  if (!sceneActorContext.available || !initialLoadComplete || !isProjectTarget.value) return [];
  const snapshot = graphSnapshot();
  const scope = normalizeProjectPath(activeProjectPath.value || readActiveProjectPath());
  const actions = cabbageAssistant.reconcileActorReferenceIssues(
    snapshot,
    generatedProjectContext(),
    reviewGraphRevision(snapshot, scope),
  );
  if (actions.length) void persistResolvedActorIssues(actions);
  return actions;
}
async function refreshSceneActorOptions({ rescan = false } = {}) {
  const refreshSequence = ++actorOptionsRefreshSequence;
  const requestedSceneName = String(props.sceneName || '').trim();
  try {
    const response = await editorApi.scene.listActorTree(requestedSceneName);
    if (response?.success === false || response?.status === 'error') {
      throw new Error(response?.message || 'Actor tree query failed');
    }
    const payload = response?.data ?? response?.result ?? response;
    const hasActorContainer = Array.isArray(payload) || (
      payload
      && typeof payload === 'object'
      && ['actors', 'data', 'result', 'children', 'items'].some((field) => field in payload)
    );
    if (!hasActorContainer) throw new Error('Actor tree response has no actor list');
    const actorsByName = new Map();
    actorRecordsFromSceneTree(payload).forEach((actor) => {
      actorsByName.set(actorContextNameKey(actor.name), actor);
    });
    if (props.actorName) {
      const name = normalizeActorContextName(props.actorName);
      const key = actorContextNameKey(name);
      if (name && !actorsByName.has(key)) actorsByName.set(key, { name, type: 'actor', tags: [], aliases: [] });
    }
    const actors = Array.from(actorsByName.values())
      .sort((left, right) => left.name.localeCompare(right.name, 'zh-CN'));
    if (!componentMounted || refreshSequence !== actorOptionsRefreshSequence) return false;

    const revision = actorContextRevision(requestedSceneName, actors);
    const changed = sceneActorContext.available !== true
      || sceneActorContext.sceneName !== requestedSceneName
      || sceneActorContext.revision !== revision;
    sceneActorContext.sceneName = requestedSceneName;
    sceneActorContext.available = true;
    sceneActorContext.revision = revision;
    sceneActorContext.actors = actors;
    window.__coronaBlocklyActorOptions = actors.map((actor) => [actor.name, actor.name]);

    reconcileSceneActorReferenceIssues();
    if (changed) {
      const projectContext = generatedProjectContext();
      cabbageAssistant.updateProjectContext(projectContext);
      publishCabbageProjectContext(projectContext, reviewScopeId(
        normalizeProjectPath(activeProjectPath.value || readActiveProjectPath()),
      ));
      scheduleRememberedIssueCheck(80);
      if (rescan) requestNodeGraphReview(0);
    }
    return changed;
  } catch (error) {
    if (componentMounted && refreshSequence === actorOptionsRefreshSequence) {
      sceneActorContext.sceneName = requestedSceneName;
      sceneActorContext.available = false;
      sceneActorContext.revision = '';
      sceneActorContext.actors = [];
      window.__coronaBlocklyActorOptions = [];
      const projectContext = generatedProjectContext();
      cabbageAssistant.updateProjectContext(projectContext);
      publishCabbageProjectContext(projectContext, reviewScopeId(
        normalizeProjectPath(activeProjectPath.value || readActiveProjectPath()),
      ));
    }
    logError('Failed to refresh scene actor options', error);
    return false;
  }
}
function scheduleSceneActorOptionsRefresh(delay = 120) {
  if (!componentMounted) return;
  if (actorOptionsRefreshTimer) window.clearTimeout(actorOptionsRefreshTimer);
  actorOptionsRefreshTimer = window.setTimeout(async () => {
    actorOptionsRefreshTimer = null;
    await refreshSceneActorOptions({ rescan: true });
  }, Math.max(0, Number(delay) || 0));
}
function normalizeSceneReference(value) {
  return String(value || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/^\.\//, '')
    .replace(/\/+/g, '/')
    .toLocaleLowerCase('en-US');
}
function sceneReferencesMatch(left, right) {
  const normalizedLeft = normalizeSceneReference(left);
  const normalizedRight = normalizeSceneReference(right);
  if (!normalizedLeft || !normalizedRight) return true;
  if (normalizedLeft === normalizedRight) return true;
  const leftFile = normalizedLeft.split('/').pop();
  const rightFile = normalizedRight.split('/').pop();
  return Boolean(leftFile && rightFile && leftFile === rightFile);
}
function onNodeGraphActorChanged(payload, maybeSceneId) {
  const changedScene = typeof payload === 'object'
    ? payload?.scene ?? payload?.sceneName ?? maybeSceneId ?? ''
    : maybeSceneId ?? '';
  if (!sceneReferencesMatch(changedScene, props.sceneName)) return;
  scheduleSceneActorOptionsRefresh(80);
}
function onNodeGraphSceneTreeChanged(payload) {
  const changedScene = typeof payload === 'string'
    ? payload
    : payload?.scene ?? payload?.sceneName ?? '';
  if (!sceneReferencesMatch(changedScene, props.sceneName)) return;
  scheduleSceneActorOptionsRefresh(80);
}
function updateCanvasSize() {
  const r = canvasRef.value?.getBoundingClientRect?.();
  if (!r) return;
  canvasSize.width = Math.max(CANVAS_WORLD_WIDTH, Math.ceil(r.width / 0.4));
  canvasSize.height = Math.max(CANVAS_WORLD_HEIGHT, Math.ceil(r.height / 0.4));
  clampViewport();
}
async function onActiveProjectChanged(event) {
  const nextProjectPath = String(event?.detail?.projectPath || readActiveProjectPath()).trim();
  if (!nextProjectPath) return;
  if (normalizeProjectPath(nextProjectPath) === normalizeProjectPath(activeProjectPath.value)) {
    if (shouldResumeBlockedSave({
      dirty: graphDirty,
      blockedProjectPath: saveBlockedProjectPath,
      eventProjectPath: nextProjectPath,
    })) {
      saveBlockedProjectPath = '';
      await saveNow(null, { force: true });
    }
    return;
  }
  if (saveTimer) {
    clearTimeout(saveTimer);
    saveTimer = null;
  }
  if (preWarningTimer) {
    clearTimeout(preWarningTimer);
    preWarningTimer = null;
  }
  cabbageAssistant.clearPreWarning();
  activeProjectPath.value = nextProjectPath;
  loadedProjectPath = '';
  targetEnabledByKey.clear();
  applyGraph(normalizeGraph({}));
  runStatus.value = '';
  currentRunNodeId.value = '';
  runWarnings.value = [];
  if (!componentMounted) return;
  await loadGraphForCurrentTarget();
  await refreshSceneActorOptions();
}
function onProjectStorageChanged(event) {
  if (event?.key !== 'corona.activeProjectPath') return;
  onActiveProjectChanged({ detail: { projectPath: event.newValue || '' } });
}


watch(
  () => [props.sceneName, props.actorName, props.targetType],
  async (next, old) => {
    if (!componentMounted || !old) return;

    const nextTargetType = next?.[2] === 'model' ? 'actor' : next?.[2] || 'actor';
    const oldTargetType = old?.[2] === 'model' ? 'actor' : old?.[2] || 'actor';
    const sceneChanged = String(next?.[0] || '') !== String(old?.[0] || '');
    const targetChanged = nextTargetType !== oldTargetType
      || (nextTargetType !== 'project' && (
        sceneChanged || String(next?.[1] || '') !== String(old?.[1] || '')
      ));

    // The project node graph does not change when MainPage reports the active scene.
    // Only refresh object choices; reloading the whole graph here used to duplicate the
    // expensive Blockly initialization every time the node window was opened.
    if (!targetChanged) {
      if (sceneChanged && initialLoadComplete && !isLoading) {
        await refreshSceneActorOptions();
        if (componentMounted) reconcileSceneActorReferenceIssues();
      }
      return;
    }

    const oldTarget = {
      targetType: oldTargetType,
      sceneName: old?.[0] || '',
      actorName: old?.[1] || '',
    };
    if (startedRunForTarget || codeRunning.value) await stopNodeGraphRun('\u5df2\u505c\u6b62');
    if (!componentMounted) return;
    clearExternalDrag();
    cancelMacroPointerDrag();
    if ((oldTarget.targetType === 'project' || oldTarget.actorName) && !isLoading) await saveNow(oldTarget);
    if (!componentMounted) return;
    runStatus.value = '';
    currentRunNodeId.value = '';
    runWarnings.value = [];
    syncGeneratedNodeGraphConsumer();
    await refreshSceneActorOptions();
    if (componentMounted) await loadGraphForCurrentTarget();
  }
);
function registerNodeGraphFlusher() {
  const flushers = window.__coronaNodeGraphFlushers instanceof Set
    ? window.__coronaNodeGraphFlushers
    : new Set();
  window.__coronaNodeGraphFlushers = flushers;
  flushers.add(saveNow);
  window.__coronaNodeGraphFlushSave = async () => {
    const results = await Promise.all(
      Array.from(window.__coronaNodeGraphFlushers || []).map((flush) => Promise.resolve().then(() => flush()))
    );
    if (results.some((result) => result === false)) {
      throw new Error('\u8282\u70b9\u56fe\u4fdd\u5b58\u5931\u8d25\uff0c\u5df2\u53d6\u6d88\u5168\u5c40\u8fd0\u884c');
    }
    return true;
  };
}
function unregisterNodeGraphFlusher() {
  window.__coronaNodeGraphFlushers?.delete?.(saveNow);
  if (!window.__coronaNodeGraphFlushers?.size) {
    delete window.__coronaNodeGraphFlushSave;
    delete window.__coronaNodeGraphFlushers;
  }
}
onMounted(async () => {
  componentMounted = true;
  activeProjectPath.value = readActiveProjectPath() || activeProjectPath.value;

  // The node graph runs in its own CEF page and therefore owns a separate Pinia
  // instance. Hydrate it before the first actor scan so it cannot broadcast an
  // empty task list over the main page's already-loaded world context.
  const currentCabbageScopeId = () => reviewScopeId(
    normalizeProjectPath(activeProjectPath.value || readActiveProjectPath()),
  );
  const cachedCabbageContext = readCabbageAssistantContext(currentCabbageScopeId());
  if (cachedCabbageContext) cabbageAssistant.hydrateContext(cachedCabbageContext);
  unsubscribeCabbageContext = subscribeCabbageAssistantContext(
    (snapshot) => {
      if (componentMounted) {
        cabbageAssistant.hydrateContext(snapshot);
        scheduleTutorialNodeStateObservation();
      }
    },
    { projectScopeId: currentCabbageScopeId, emitCurrent: true },
  );

  window.addEventListener('corona-active-project-changed', onActiveProjectChanged);
  window.addEventListener('storage', onProjectStorageChanged);
  window.addEventListener('cabbage-guidance-prepare', handleGuidancePrepare);

  // Establish the visible layout before waiting for bridge subscriptions and data loads,
  // so the native floating surface paints immediately instead of appearing frozen.
  loadLayout();
  resizeObserver = new ResizeObserver(resizeEmbeddedWorkspaces);
  if (workspaceRootRef.value) resizeObserver.observe(workspaceRootRef.value);
  resizeEmbeddedWorkspaces();
  syncGeneratedNodeGraphConsumer();

  // Event subscription is useful for later object updates, but it must not block the
  // first graph paint. Some native bridge shutdown/startup transitions can make an
  // event registration noticeably slower than loading the saved graph itself.
  Promise.allSettled([
    editorApi.events.onActorChanged(onNodeGraphActorChanged),
    editorApi.events.onSceneTreeChanged(onNodeGraphSceneTreeChanged),
  ]).then((subscriptionResults) => {
    const [actorSubscription, sceneTreeSubscription] = subscriptionResults;
    if (actorSubscription.status === 'fulfilled') {
      if (componentMounted) actorChangedCallbackToken = actorSubscription.value;
      else editorApi.off(actorSubscription.value).catch(() => {});
    } else {
      logError('Failed to subscribe to actor changes', actorSubscription.reason);
    }
    if (sceneTreeSubscription.status === 'fulfilled') {
      if (componentMounted) sceneTreeChangedCallbackToken = sceneTreeSubscription.value;
      else editorApi.off(sceneTreeSubscription.value).catch(() => {});
    } else {
      logError('Failed to subscribe to scene tree changes', sceneTreeSubscription.reason);
    }
  });

  // Actor choices are needed by object-reference dropdowns, so finish the first scan
  // before hydrating Blockly. Unlike the event subscriptions above, this request is on
  // the critical path and is guarded against a panel that was closed while awaiting it.
  await refreshSceneActorOptions();
  if (!componentMounted) return;

  // Initial graph data used to be loaded once by the immediate watcher and again by
  // onMounted. Keep a single load after the actor scan has completed.
  await loadGraphForCurrentTarget();
  if (!componentMounted) return;
  scheduleTutorialNodeStateObservation(0);
  if (sceneActorContext.sceneName !== String(props.sceneName || '').trim()) {
    await refreshSceneActorOptions();
    if (!componentMounted) return;
  }

  if (isProjectTarget.value) {
    stopNodeGraphReview = startNodeGraphReview({
      getWorkspace: () => ({
        version: graph.version,
        nodes: graph.nodes,
        edges: graph.edges,
        globalVariablesWorkspace: graph.globalVariablesWorkspace,
      }),
      getRevisionScope: () => normalizeProjectPath(activeProjectPath.value || readActiveProjectPath()),
      getProjectContext: () => ({
        ...generatedProjectContext(),
        assistanceProfile: currentAssistanceProfile(),
        optimizationHintsEnabled: optimizationHintsEnabled(),
      }),
      enabled: () => componentMounted && props.reviewActive && isProjectTarget.value && !isLoading,
      intervalMs: 10000,
    });
    requestNodeGraphReview(500);
  }
  window.addEventListener('corona-game-preview-status', onGamePreviewStatus);
  window.addEventListener('keydown', handleFullscreenKey);
  coronaEventBus.on('viewport-controls-state', onViewportControlsState);
  if (window.__coronaGamePreviewState) {
    applyGamePreviewGuard(window.__coronaGamePreviewState);
  }
  refreshGamePreviewGuard();
  startGamePreviewGuardPoll();
  registerNodeGraphFlusher();
  if (isProjectTarget.value) {
    unregisterProjectNodeGraphSaveHandler = registerProjectNodeGraphSaveHandler(saveNow);
    unregisterProjectNodeGraphRuntimeHandler = registerProjectNodeGraphRuntimeHandler({
      toggle: handleToggleRun,
      getState: currentProjectNodeGraphRuntimeState,
    });
    syncProjectNodeGraphRuntimeState();
  }
});
onBeforeUnmount(() => {
  componentMounted = false;
  window.removeEventListener('corona-active-project-changed', onActiveProjectChanged);
  window.removeEventListener('storage', onProjectStorageChanged);
  window.removeEventListener('cabbage-guidance-prepare', handleGuidancePrepare);
  unsubscribeCabbageContext?.();
  unsubscribeCabbageContext = null;
  actorOptionsRefreshSequence += 1;
  graphLoadSequence += 1;
  if (actorOptionsRefreshTimer) window.clearTimeout(actorOptionsRefreshTimer);
  actorOptionsRefreshTimer = null;
  if (actorChangedCallbackToken) {
    editorApi.off(actorChangedCallbackToken).catch((error) => {
      logError('Failed to unsubscribe from actor changes', error);
    });
    actorChangedCallbackToken = null;
  }
  if (sceneTreeChangedCallbackToken) {
    editorApi.off(sceneTreeChangedCallbackToken).catch((error) => {
      logError('Failed to unsubscribe from scene tree changes', error);
    });
    sceneTreeChangedCallbackToken = null;
  }
  stopNodeGraphReview?.();
  stopNodeGraphReview = null;
  unregisterProjectNodeGraphRuntimeHandler?.();
  unregisterProjectNodeGraphRuntimeHandler = null;
  unregisterProjectNodeGraphSaveHandler?.();
  unregisterProjectNodeGraphSaveHandler = null;
  unregisterAiNodeGraphConsumer?.();
  unregisterAiNodeGraphConsumer = null;
  window.removeEventListener('corona-game-preview-status', onGamePreviewStatus);
  window.removeEventListener('keydown', handleFullscreenKey);
  if (isFullscreen.value) {
    appService.redockPanel().catch(() => {});
    isFullscreen.value = false;
  }
  coronaEventBus.off('viewport-controls-state', onViewportControlsState);
  if (gamePreviewGuardTimer) {
    window.clearInterval(gamePreviewGuardTimer);
    gamePreviewGuardTimer = null;
  }
  unregisterNodeGraphFlusher();
  if (saveTimer) clearTimeout(saveTimer);
  saveTimer = null;
  if (preWarningTimer) window.clearTimeout(preWarningTimer);
  preWarningTimer = null;
  if (tutorialObservationTimer) window.clearTimeout(tutorialObservationTimer);
  tutorialObservationTimer = null;
  lastTutorialObservationSignature = '';
  if (initialLoadComplete && graphDirty) saveNow(null, { force: true });
  resizeObserver?.disconnect?.();
  if (resizeFrame) window.cancelAnimationFrame(resizeFrame);
  resizeFrame = 0;
  clearRunPoll();
  clearExternalDrag();
  cancelMacroPointerDrag();
  endCanvasPan();
  stopForPanelClose().catch(() => {});
  setNodeGraphInputLocked(false);
  window.removeEventListener('mousemove', onDragMove);
  window.removeEventListener('mouseup', stopNodeDrag);
});
</script>

<style scoped>
.node-graph-workspace {
  position: relative;
  width: 100%;
  height: 100%;
  min-width: 0;
  min-height: 0;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  font-size: 17px;
  color: #f2ead5;
  background: radial-gradient(circle at 28% 0, #2b230f 0, #15130d 42%, #080806 100%);
  border: 1px solid #30281c;
  border-radius: 12px;
}
.node-graph-workspace.fullscreen {
  position: fixed;
  inset: 12px;
  z-index: 2147483000;
  width: auto;
  height: auto;
  min-height: 0;
  border-radius: 10px;
  box-shadow: 0 24px 80px rgba(0, 0, 0, 0.72);
}
.node-graph-workspace.fullscreen::before {
  content: '';
  position: fixed;
  inset: 0;
  z-index: -1;
  background: rgba(8, 8, 6, 0.86);
}
.ng-toolbar {
  position: relative;
  min-height: 42px;
  flex: 0 0 auto;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  padding: 0 10px;
  border-bottom: 1px solid #30281c;
  background: rgba(25, 23, 17, 0.96);
}
.ng-title {
  min-width: 0;
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 13px;
}
.ng-badge {
  padding: 4px 9px;
  border-radius: 999px;
  background: #8c6f36;
  color: #fff;
  font-weight: 800;
}
.ng-modes {
  flex: 0 0 auto;
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 6px;
  margin-left: auto;
}
.ng-run {
  height: 28px;
  min-width: 58px;
  padding: 0 13px;
  border: 1px solid #d8b86c;
  border-radius: 8px;
  background: #665025;
  color: #fff7dc;
  font-weight: 800;
  cursor: pointer;
}
.ng-run:hover:not(:disabled) {
  background: #8c6f36;
}
.ng-run.running {
  border-color: #fb7185;
  background: #9f1239;
}
.ng-run:disabled {
  opacity: 0.55;
  cursor: wait;
}
.ng-run-status {
  max-width: 240px;
  overflow: hidden;
  color: #e9dfc5;
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.ng-mode {
  height: 28px;
  padding: 0 12px;
  border-radius: 8px;
  border: 1px solid #4a3d1d;
  background: #15130d;
  color: #e9dfc5;
  cursor: pointer;
  font-size: 13px;
}
.ng-mode.active {
  background: #8c6f36;
  border-color: #d8b86c;
  color: #fff;
}
.ng-mode.delete.active {
  background: #b91c1c;
  border-color: #f87171;
}
.ng-empty {
  height: 100%;
  display: grid;
  place-items: center;
  color: #b9ad8f;
  font-size: 13px;
}
.ng-body {
  min-height: 0;
  flex: 1 1 auto;
  display: grid;
  grid-template-columns: var(--toolbox-width) 6px minmax(420px, 1fr) 6px var(--inspector-width);
  gap: 8px;
  padding: 8px;
}
.node-graph-workspace.compact .ng-toolbar {
  flex-wrap: wrap;
  padding-top: 6px;
  padding-bottom: 6px;
}
.node-graph-workspace.compact .ng-title {
  flex: 1 1 220px;
}
.node-graph-workspace.compact .ng-modes {
  flex: 0 1 auto;
  flex-wrap: wrap;
}
.node-graph-workspace.compact .ng-body {
  grid-template-columns: minmax(220px, var(--toolbox-width)) 6px minmax(0, 1fr);
  grid-template-rows: minmax(250px, 1fr) minmax(280px, 0.92fr);
  overflow-y: auto;
}
.node-graph-workspace.compact .ng-toolbox {
  grid-column: 1;
  grid-row: 1;
}
.node-graph-workspace.compact .ng-toolbox-splitter {
  grid-column: 2;
  grid-row: 1;
}
.node-graph-workspace.compact .ng-canvas {
  grid-column: 3;
  grid-row: 1;
  min-width: 0;
}
.node-graph-workspace.compact .ng-inspector-splitter {
  display: none;
}
.node-graph-workspace.compact .ng-inspector {
  grid-column: 1 / -1;
  grid-row: 2;
  min-width: 0;
  grid-template-rows: minmax(110px, var(--variables-height)) 6px minmax(160px, 1fr);
}
.node-graph-workspace.narrow .ng-body {
  grid-template-columns: minmax(0, 1fr);
  grid-template-rows: 230px minmax(280px, 1fr) minmax(300px, 1fr);
}
.node-graph-workspace.narrow .ng-toolbox,
.node-graph-workspace.narrow .ng-canvas,
.node-graph-workspace.narrow .ng-inspector {
  grid-column: 1;
}
.node-graph-workspace.narrow .ng-toolbox {
  grid-row: 1;
}
.node-graph-workspace.narrow .ng-canvas {
  grid-row: 2;
}
.node-graph-workspace.narrow .ng-inspector {
  grid-row: 3;
}
.node-graph-workspace.narrow .ng-toolbox-splitter,
.node-graph-workspace.narrow .ng-inspector-splitter {
  display: none;
}
.ng-panel {
  min-height: 0;
  border: 1px solid #30281c;
  background: rgba(21, 19, 13, 0.96);
  border-radius: 12px;
  overflow: hidden;
}
.ng-toolbox {
  display: flex;
  flex-direction: column;
  gap: 0;
  padding: 10px;
  overflow: hidden;
}
.ng-native-palette {
  flex: 1 1 auto;
  min-height: 0;
}
.ng-section-title {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin: 2px 0 8px;
  color: #fff7dc;
  font-size: 14px;
  font-weight: 800;
}
.ng-section-title.mt {
  margin-top: 14px;
}
.ng-section-title small {
  color: #b9ad8f;
  font-size: 11px;
  font-weight: 500;
}
.ng-tool-card {
  display: flex;
  align-items: flex-start;
  gap: 9px;
  padding: 9px;
  margin-bottom: 8px;
  border-radius: 10px;
  border: 1px solid #3f3018;
  background: #191711;
  cursor: grab;
}
.ng-tool-card.macro {
  border-color: rgba(216, 184, 108, 0.55);
}
.ng-tool-icon,
.ng-mini {
  display: grid;
  place-items: center;
  flex: 0 0 auto;
  color: #fff;
  font-weight: 900;
  background: #8c6f36;
}
.ng-tool-icon {
  width: auto;
  min-width: 30px;
  height: 30px;
  padding: 0 7px;
  border-radius: 9px;
  white-space: nowrap;
}
.ng-tool-card > div:last-child {
  min-width: 0;
}
.ng-tool-name {
  font-size: 14px;
  font-weight: 800;
  color: #fff7dc;
}
.ng-tool-name,
.ng-tool-desc {
  overflow-wrap: anywhere;
}
.ng-tool-desc {
  margin-top: 3px;
  font-size: 12px;
  line-height: 1.35;
  color: #b9ad8f;
}
.ng-cat {
  margin-bottom: 7px;
  border: 1px solid #30281c;
  border-radius: 10px;
  overflow: hidden;
  background: #15130d;
}
.ng-cat summary {
  display: flex;
  align-items: center;
  gap: 7px;
  padding: 8px;
  list-style: none;
  cursor: pointer;
  font-size: 13px;
  font-weight: 800;
}
.ng-cat summary::-webkit-details-marker {
  display: none;
}
.ng-cat-dot {
  width: 9px;
  height: 9px;
  border-radius: 50%;
  background: #d8b86c;
}
.ng-count {
  margin-left: auto;
  color: #b9ad8f;
  font-size: 11px;
}
.ng-block-list {
  display: grid;
  gap: 6px;
  padding: 0 7px 8px;
}
.ng-block-chip {
  display: grid;
  grid-template-columns: 24px 1fr;
  gap: 7px;
  align-items: center;
  text-align: left;
  border: 1px solid #4a3d1d;
  background: #211d12;
  color: #fff3c8;
  border-radius: 9px;
  padding: 6px;
  cursor: grab;
}
.ng-mini {
  width: 24px;
  height: 24px;
  border-radius: 7px;
  font-size: 12px;
  background: #9d9278;
}
.ng-block-chip b {
  display: block;
  font-size: 12px;
  line-height: 1.2;
}
.ng-block-chip small {
  display: block;
  margin-top: 1px;
  color: #b9ad8f;
  font-size: 10px;
  word-break: break-all;
}
.ng-canvas {
  position: relative;
  overflow: hidden;
  cursor: grab;
  background: #0b0a08;
  transition: box-shadow 120ms ease, border-color 120ms ease;
}
.ng-canvas.panning {
  cursor: grabbing;
  user-select: none;
}
.ng-canvas.drop-active {
  border-color: #d8b86c;
  box-shadow: inset 0 0 0 3px rgba(216, 184, 108, 0.25);
}
.ng-drag-ghost {
  position: fixed;
  z-index: 10000;
  max-width: 220px;
  padding: 8px 12px;
  border: 1px solid rgba(229, 199, 127, 0.8);
  border-radius: 9px;
  background: rgba(75, 57, 28, 0.92);
  box-shadow: 0 10px 24px rgba(0, 0, 0, 0.35);
  color: #fff7dc;
  font-size: 13px;
  font-weight: 800;
  pointer-events: none;
  transform: translateZ(0);
}
.ng-world {
  position: absolute;
  inset: 0 auto auto 0;
  will-change: transform;
}
.ng-grid {
  position: absolute;
  inset: 0;
  pointer-events: none;
  background-image:
    linear-gradient(rgba(216, 184, 108, 0.08) 1px, transparent 1px),
    linear-gradient(90deg, rgba(216, 184, 108, 0.08) 1px, transparent 1px);
  background-size: 24px 24px;
}
.ng-canvas-head {
  position: absolute;
  z-index: 5;
  left: 12px;
  top: 10px;
  right: 12px;
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  pointer-events: none;
}
.ng-canvas-head strong {
  display: block;
  font-size: 15px;
}
.ng-canvas-head span {
  display: block;
  color: #b9ad8f;
  font-size: 12px;
  margin-top: 2px;
}
.ng-canvas-hint {
  display: block;
  margin-top: 3px;
  color: #a99c7d;
  font-size: 12px;
  font-weight: 500;
}
.ng-canvas-actions {
  display: flex;
  align-items: center;
  gap: 6px;
  pointer-events: auto;
}
.ng-zoom-value {
  margin-top: 0 !important;
  min-width: 42px;
  text-align: center;
  color: #fff3c8 !important;
}
.ng-zoom-reset {
  height: 26px;
  padding: 0 9px;
  border: 1px solid #665025;
  border-radius: 7px;
  background: #191711;
  color: #fff3c8;
  font-size: 12px;
  cursor: pointer;
}
.ng-zoom-reset:hover {
  border-color: #d8b86c;
}
.ng-edges {
  position: absolute;
  inset: 0;
  z-index: 1;
  pointer-events: none;
}
.ng-edge-hit {
  fill: none;
  stroke: transparent;
  stroke-width: 18;
  pointer-events: stroke;
  cursor: pointer;
}
.ng-edge-line {
  fill: none;
  stroke: #d8b86c;
  stroke-width: 2.4;
  filter: drop-shadow(0 0 5px rgba(216, 184, 108, 0.55));
  pointer-events: none;
}
.ng-edge-line.selected {
  stroke: #facc15;
  stroke-width: 3;
  filter: drop-shadow(0 0 4px rgba(250, 204, 21, 0.45));
}
.ng-edge-hit:hover + .ng-edge-line {
  stroke: #e9dfc5;
  stroke-width: 3;
}
.ng-edge-preview {
  fill: none;
  stroke: #facc15;
  stroke-width: 2.4;
  stroke-dasharray: 8 6;
  pointer-events: none;
  filter: drop-shadow(0 0 4px rgba(250, 204, 21, 0.35));
}
.ng-edge-line.delete {
  stroke: #fb7185;
}
.ng-node {
  cursor: move;
  position: absolute;
  z-index: 3;
  padding: 10px 11px;
  border-radius: 14px;
  border: 1px solid #b8924a;
  background: linear-gradient(180deg, #242016, #11100d);
  box-shadow: 0 12px 22px rgba(0, 0, 0, 0.35);
  cursor: grab;
  user-select: none;
}
.ng-node.selected {
  border-color: #facc15;
  box-shadow:
    0 0 0 2px rgba(250, 204, 21, 0.28),
    0 14px 24px rgba(0, 0, 0, 0.36);
}
.ng-node.delete {
  border-color: #fb7185;
  cursor: not-allowed;
}
.ng-node.type-start {
  border-color: #d8b86c;
}
.ng-node.type-end {
  border-color: #fb7185;
}
.ng-node-head {
  display: flex;
  align-items: center;
  gap: 7px;
}
.ng-node-dot {
  width: 9px;
  height: 9px;
  border-radius: 50%;
  background: #d8b86c;
}
.ng-node-type,
.ng-node-name {
  min-width: 0;
  width: 100%;
  border: 1px solid #4a3d1d;
  border-radius: 7px;
  background: #0f0e0a;
  color: #fff3c8;
  font-size: 12px;
  padding: 4px 6px;
}
.ng-node-name {
  margin-top: 8px;
}
.ng-node-fixed-name {
  margin-top: 8px;
  font-size: 14px;
  font-weight: 900;
  color: #fff7dc;
}
.ng-node-hint {
  margin-top: 7px;
  color: #b9ad8f;
  font-size: 11px;
}
.ng-node.running {
  border-color: #facc15;
  box-shadow:
    0 0 0 3px rgba(250, 204, 21, 0.2),
    0 0 22px rgba(250, 204, 21, 0.32),
    0 14px 24px rgba(0, 0, 0, 0.36);
}
.ng-node-type-badge {
  flex: 0 0 auto;
  padding: 3px 7px;
  border: 1px solid rgba(216, 184, 108, 0.48);
  border-radius: 999px;
  background: rgba(216, 184, 108, 0.16);
  color: #f2ead5;
  font-size: 11px;
  font-weight: 800;
  line-height: 1.2;
}
.type-start .ng-node-type-badge {
  border-color: rgba(216, 184, 108, 0.5);
  background: rgba(216, 184, 108, 0.14);
  color: #f2ead5;
}
.type-end .ng-node-type-badge {
  border-color: rgba(251, 113, 133, 0.5);
  background: rgba(190, 24, 93, 0.16);
  color: #fecdd3;
}
.ng-node-display-name {
  min-width: 0;
  flex: 1 1 auto;
  overflow: hidden;
  color: #fff7dc;
  font-size: 14px;
  font-weight: 900;
  line-height: 1.35;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.ng-port {
  position: absolute;
  width: 12px;
  height: 12px;
  border: 2px solid #fff7aa;
  border-radius: 50%;
  background: #facc15;
  box-shadow: 0 0 0 3px rgba(250, 204, 21, 0.2);
  cursor: crosshair;
  z-index: 6;
}
.ng-port.occupied {
  background: #a16207;
  border-color: #fde68a;
  opacity: 0.78;
  cursor: not-allowed;
}
.ng-port.pending {
  background: #d8b86c;
  box-shadow: 0 0 0 5px rgba(216, 184, 108, 0.25);
}
.ng-port.connection-target:not(.occupied) {
  animation: ng-port-pulse 1s ease-in-out infinite;
  border-color: #fffde0;
  box-shadow: 0 0 0 4px rgba(250, 204, 21, 0.28);
}
@keyframes ng-port-pulse {
  0%,
  100% {
    transform: scale(1);
  }
  50% {
    transform: scale(1.22);
  }
}
.ng-condition-block {
  position: absolute;
  z-index: 4;
  min-width: 112px;
  max-width: 150px;
  height: 34px;
  padding: 0 18px;
  display: flex;
  align-items: center;
  justify-content: center;
  text-align: center;
  color: #281600;
  background: #facc15;
  border: 1px solid #fde68a;
  font-size: 13px;
  font-weight: 900;
  clip-path: polygon(13% 0, 87% 0, 100% 50%, 87% 100%, 13% 100%, 0 50%);
  cursor: pointer;
  filter: drop-shadow(0 8px 15px rgba(0, 0, 0, 0.35));
  overflow: hidden;
  white-space: nowrap;
  text-overflow: ellipsis;
}
.ng-condition-block:hover {
  transform: translateY(-1px);
  filter: brightness(1.06) drop-shadow(0 5px 8px rgba(0, 0, 0, 0.24));
}
.ng-condition-block.selected {
  background: #fde047;
  outline: 2px solid rgba(250, 204, 21, 0.45);
}
.ng-inspector {
  display: grid;
  grid-template-rows: minmax(120px, var(--variables-height)) 6px minmax(360px, 1fr);
  gap: 0;
  padding: 8px;
  background: rgba(17, 16, 13, 0.94);
}
.ng-vars,
.ng-editor {
  min-height: 0;
  display: flex;
  flex-direction: column;
  gap: 4px;
}
.ng-vars :deep(.mini-blockly-shell),
.ng-editor :deep(.mini-blockly-shell) {
  flex: 1 1 auto;
  min-height: 0;
}
.ng-property-card {
  flex: 0 0 auto;
  display: flex;
  flex-direction: column;
  gap: 7px;
  padding: 9px;
  margin-bottom: 4px;
  border: 1px solid #4a3d1d;
  border-radius: 10px;
  background: rgba(17, 16, 13, 0.88);
}
.ng-property-label {
  color: #e9dfc5;
  font-size: 12px;
  font-weight: 800;
}
.ng-type-tabs {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 5px;
}
.ng-type-tabs button {
  min-width: 0;
  height: 30px;
  padding: 0 5px;
  border: 1px solid #665025;
  border-radius: 7px;
  background: #191711;
  color: #e9dfc5;
  cursor: pointer;
  font-size: 12px;
  font-weight: 700;
}
.ng-type-tabs button:hover { border-color: #d8b86c; }
.ng-type-tabs button.active {
  border-color: #facc15;
  background: rgba(161, 98, 7, 0.34);
  color: #fef3c7;
  box-shadow: inset 0 0 0 1px rgba(250, 204, 21, 0.16);
}
.ng-name-row {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  align-items: center;
  gap: 7px;
}
.ng-name-row input {
  min-width: 0;
  width: 100%;
  height: 30px;
  padding: 0 9px;
  border: 1px solid #665025;
  border-radius: 7px;
  outline: none;
  background: #0f0e0a;
  color: #fff7dc;
  font-size: 13px;
}
.ng-name-row input:focus {
  border-color: #facc15;
  box-shadow: 0 0 0 2px rgba(250, 204, 21, 0.12);
}
.ng-name-row span {
  color: #b9ad8f;
  font-size: 11px;
  font-variant-numeric: tabular-nums;
}
.ng-edge-meta {
  display: grid;
  gap: 3px;
  color: #b9ad8f;
  font-size: 11px;
}
.ng-condition-note {
  padding: 6px 7px;
  border-radius: 7px;
  background: rgba(36, 32, 22, 0.76);
  color: #e9dfc5;
  font-size: 11px;
  line-height: 1.4;
}
.ng-editor-empty {
  flex: 1 1 auto;
  display: grid;
  place-items: center;
  border-radius: 10px;
  border: 1px dashed rgba(216, 184, 108, 0.28);
  color: #b9ad8f;
  font-size: 13px;
  text-align: center;
  padding: 16px;
}

.ng-splitter {
  position: relative;
  z-index: 20;
  background: rgba(74, 61, 29, 0.52);
  transition: background 120ms ease;
  touch-action: none;
  user-select: none;
}
.ng-splitter:hover,
.ng-splitter:active {
  background: #d8b86c;
}
.ng-splitter.vertical {
  cursor: col-resize;
}
.ng-splitter.horizontal {
  cursor: row-resize;
}
.ng-run-status {
  cursor: pointer;
}
.ng-run-detail {
  position: absolute;
  top: 48px;
  right: 12px;
  z-index: 80;
  width: min(620px, calc(100% - 24px));
  max-height: 45%;
  padding: 10px;
  border: 1px solid #746137;
  border-radius: 10px;
  background: rgba(8, 8, 6, 0.98);
  box-shadow: 0 16px 35px rgba(0, 0, 0, 0.45);
  color: #f2ead5;
}
.ng-run-detail-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 7px;
}
.ng-run-detail-head button {
  border: 1px solid #746137;
  border-radius: 6px;
  padding: 3px 9px;
  background: #242016;
  color: #fff3c8;
  cursor: pointer;
}
.ng-run-detail pre {
  max-height: 280px;
  overflow: auto;
  margin: 0;
  color: #fecaca;
  font: 12px/1.55 Consolas, monospace;
  white-space: pre-wrap;
  word-break: break-word;
}
</style>
