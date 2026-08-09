import { editorApi } from '@/api/editorApi.js';
import { appService } from '@/services/appService.js';
import { cabbageContextService } from '@/services/cabbageAssistantContextService.js';
import { coronaEventBus } from '@/utils/eventBus.js';

const SAVE_REQUEST_EVENT = 'node-graph-save-request';
const SAVE_RESULT_EVENT = 'node-graph-save-result';
const SAVE_ACCEPTED_EVENT = 'node-graph-save-accepted';
const GLOBAL_NODE_TARGET_ID = 'node_graph:project:global';
const RUNTIME_TOGGLE_REQUEST_EVENT = 'node-graph-runtime-toggle-request';
const RUNTIME_TOGGLE_ACCEPTED_EVENT = 'node-graph-runtime-toggle-accepted';
const RUNTIME_TOGGLE_RESULT_EVENT = 'node-graph-runtime-toggle-result';
const RUNTIME_STATE_REQUEST_EVENT = 'node-graph-runtime-state-request';
const RUNTIME_STATE_EVENT = 'node-graph-runtime-state';

function broadcastRuntimeEvent(event, payload) {
  coronaEventBus.emit(event, payload);
  appService.crossTabBroadcast(event, payload).catch(() => {});
}

function requestId() {
  return `node_graph_save_${Date.now()}_${Math.random().toString(36).slice(2, 9)}`;
}

async function runLocalFlusher() {
  if (typeof window.__coronaNodeGraphFlushSave !== 'function') return false;
  const result = await window.__coronaNodeGraphFlushSave();
  if (result === false) throw new Error('\u8282\u70b9\u56fe\u4fdd\u5b58\u5931\u8d25\uff0c\u5df2\u53d6\u6d88\u5168\u5c40\u8fd0\u884c');
  return true;
}

/**
 * Flush the project node graph before global preview starts. If the node editor
 * lives in another Dock/CEF context, request a save over the existing cross-tab
 * event channel. A short owner timeout means the editor is not mounted and the
 * latest persisted graph can be used. Once an editor accepts the request, wait
 * for the real save result so global run never races a slow disk write.
 */
export async function flushProjectNodeGraphBeforeRun({
  ownerTimeoutMs = 800,
  saveTimeoutMs = 6000,
} = {}) {
  if (await runLocalFlusher()) return { success: true, source: 'local' };

  const id = requestId();
  return new Promise((resolve, reject) => {
    let settled = false;
    let timer = null;
    const cleanup = () => {
      if (timer) window.clearTimeout(timer);
      timer = null;
      coronaEventBus.off(SAVE_ACCEPTED_EVENT, onAccepted);
      coronaEventBus.off(SAVE_RESULT_EVENT, onResult);
    };
    const finish = (result, error = null) => {
      if (settled) return;
      settled = true;
      cleanup();
      if (error) reject(error);
      else resolve(result);
    };
    const armTimer = (delay, callback) => {
      if (timer) window.clearTimeout(timer);
      timer = window.setTimeout(callback, Math.max(250, Number(delay) || 0));
    };
    const onAccepted = (payload = {}) => {
      if (String(payload.requestId || '') !== id) return;
      armTimer(saveTimeoutMs, () => {
        finish(null, new Error('\u8282\u70b9\u56fe\u4fdd\u5b58\u8d85\u65f6\uff0c\u5df2\u53d6\u6d88\u5168\u5c40\u8fd0\u884c'));
      });
    };
    const onResult = (payload = {}) => {
      if (String(payload.requestId || '') !== id) return;
      if (payload.success === false) {
        finish(null, new Error(payload.message || '\u8282\u70b9\u56fe\u4fdd\u5b58\u5931\u8d25\uff0c\u5df2\u53d6\u6d88\u5168\u5c40\u8fd0\u884c'));
        return;
      }
      finish({ success: true, source: 'dock' });
    };

    coronaEventBus.on(SAVE_ACCEPTED_EVENT, onAccepted);
    coronaEventBus.on(SAVE_RESULT_EVENT, onResult);
    armTimer(ownerTimeoutMs, () => {
      finish({ success: true, source: 'persisted', skipped: true });
    });

    const payload = { requestId: id, targetId: GLOBAL_NODE_TARGET_ID };
    // Same-window Dock.
    coronaEventBus.emit(SAVE_REQUEST_EVENT, payload);
    // Detached Dock/CEF windows.
    appService.crossTabBroadcast(SAVE_REQUEST_EVENT, payload).catch(() => {});
  });
}

/** Register the scene-side project node editor as the only global save owner. */
export function registerProjectNodeGraphSaveHandler(save) {
  const handled = new Set();
  const onRequest = async (payload = {}) => {
    const id = String(payload.requestId || '');
    if (!id || handled.has(id)) return;
    if (payload.targetId && payload.targetId !== GLOBAL_NODE_TARGET_ID) return;
    handled.add(id);
    window.setTimeout(() => handled.delete(id), 8000);

    const accepted = { requestId: id, targetId: GLOBAL_NODE_TARGET_ID };
    coronaEventBus.emit(SAVE_ACCEPTED_EVENT, accepted);
    appService.crossTabBroadcast(SAVE_ACCEPTED_EVENT, accepted).catch(() => {});

    let success = false;
    let message = '';
    try {
      success = (await save()) !== false;
      if (!success) message = '\u8282\u70b9\u56fe\u4fdd\u5b58\u5931\u8d25\uff0c\u5df2\u53d6\u6d88\u5168\u5c40\u8fd0\u884c';
    } catch (error) {
      message = String(error?.message || error || '\u8282\u70b9\u56fe\u4fdd\u5b58\u5931\u8d25');
    }

    const result = {
      requestId: id,
      targetId: GLOBAL_NODE_TARGET_ID,
      success,
      message,
    };
    coronaEventBus.emit(SAVE_RESULT_EVENT, result);
    appService.crossTabBroadcast(SAVE_RESULT_EVENT, result).catch(() => {});
  };

  coronaEventBus.on(SAVE_REQUEST_EVENT, onRequest);
  return () => coronaEventBus.off(SAVE_REQUEST_EVENT, onRequest);
}

function bridgeResult(response) {
  return response?.data?.data ?? response?.data ?? response ?? {};
}

function readActiveProjectPath() {
  return String(window.localStorage?.getItem('corona.activeProjectPath') || '').trim();
}

function normalizeProjectPath(value) {
  return String(value || '').trim().replaceAll('\\', '/').replace(/\/+$/, '').toLowerCase();
}

function normalizePersistedGraph(rawGraph) {
  const graph = rawGraph && typeof rawGraph === 'object' ? rawGraph : {};
  return {
    version: 1,
    nodes: Array.isArray(graph.nodes) ? graph.nodes : [],
    edges: Array.isArray(graph.edges) ? graph.edges : [],
    globalVariablesWorkspace:
      graph.globalVariablesWorkspace && typeof graph.globalVariablesWorkspace === 'object'
        ? graph.globalVariablesWorkspace
        : {},
  };
}

const BACKGROUND_INPUT_LOCK = 'node_graph';
const backgroundRuntimeState = {
  ready: false,
  running: false,
  busy: false,
  status: '',
  detail: '',
  currentNodeId: '',
  updatedAt: 0,
};
let backgroundRunPollTimer = null;
let backgroundOwnsExecution = false;
let backgroundLifecycleActive = false;
let backgroundTerminalReported = false;
let backgroundStateRequestInstalled = false;

function setBackgroundInputLocked(locked) {
  const locks = window.__coronaEditorInputLocks instanceof Set
    ? window.__coronaEditorInputLocks
    : new Set();
  window.__coronaEditorInputLocks = locks;
  if (locked) locks.add(BACKGROUND_INPUT_LOCK);
  else locks.delete(BACKGROUND_INPUT_LOCK);
  window.__coronaGamePreviewInputLocked = locks.size > 0;
}

function clearBackgroundRunPoll() {
  if (backgroundRunPollTimer) window.clearInterval(backgroundRunPollTimer);
  backgroundRunPollTimer = null;
}

function updateBackgroundRuntimeState(patch = {}) {
  Object.assign(backgroundRuntimeState, patch, { updatedAt: Date.now() });
  return publishProjectNodeGraphRuntimeState(backgroundRuntimeState);
}

function formatRuntimeState(status = {}) {
  const state = String(status.status || 'idle');
  if (state === 'starting') return '启动中...';
  if (state === 'running') {
    if (status.waitingEdgeName) return `等待条件：${status.waitingEdgeName}`;
    if (status.currentNodeName) return `运行中：${status.currentNodeName}`;
    return '运行中';
  }
  if (state === 'completed') return '已完成';
  if (state === 'stopped') return '已停止';
  if (state === 'error') return `执行失败：${status.error || '未知错误'}`;
  return '';
}

function formatRuntimeDetail(status = {}) {
  const lines = [];
  if (status.error) lines.push(`错误：${status.error}`);
  lines.push('运行作用域：当前场景（项目节点图）');
  const sceneName = status.resolvedSceneName || status.nativeScene;
  if (sceneName) lines.push(`运行场景：${sceneName}`);
  lines.push('对象来源：场景管理中已导入的物体，由各积木的对象参数指定');
  if (Array.isArray(status.actorCandidates) && status.actorCandidates.length) {
    lines.push(`可用物体：${status.actorCandidates.join(', ')}`);
  }
  return lines.join('\n');
}

function beginBackgroundRunLifecycle() {
  backgroundLifecycleActive = false;
  backgroundTerminalReported = false;
}

function reportBackgroundRunStarted() {
  if (backgroundLifecycleActive) return;
  backgroundLifecycleActive = true;
  void cabbageContextService.recordEvent({
    type: 'run_started',
    category: 'runtime',
    success: true,
    details: { source: 'node_graph' },
  });
}

function reportBackgroundRunTerminal(success, error = '') {
  if (backgroundTerminalReported) return;
  backgroundTerminalReported = true;
  backgroundLifecycleActive = false;
  void cabbageContextService.recordEvent({
    type: success ? 'run_succeeded' : 'run_failed',
    category: 'runtime',
    success,
    details: { source: 'node_graph', error: String(error || '').slice(0, 500) },
  });
  if (!success) {
    window.dispatchEvent(new CustomEvent('cabbage-run-failed', {
      detail: { source: 'node_graph', error: String(error || ''), contextRecorded: true },
    }));
  }
}

function resetBackgroundRunLifecycle() {
  backgroundLifecycleActive = false;
  backgroundTerminalReported = false;
}

function scriptStatusNeedsStop(status = {}) {
  const state = String(status.status || '').trim().toLowerCase();
  return ['starting', 'running', 'stopping'].includes(state)
    || Boolean(status.threadAlive)
    || Boolean(status.inputLocked)
    || Boolean(status.snapshotCaptured ?? status.hasSnapshot ?? status.has_snapshot);
}

function onBackgroundRuntimeStateRequest(payload = {}) {
  if (payload.targetId && payload.targetId !== GLOBAL_NODE_TARGET_ID) return;
  publishProjectNodeGraphRuntimeState({
    ...backgroundRuntimeState,
    requestId: String(payload.requestId || ''),
  });
}

function installBackgroundStateResponder() {
  if (backgroundStateRequestInstalled) return;
  backgroundStateRequestInstalled = true;
  coronaEventBus.on(RUNTIME_STATE_REQUEST_EVENT, onBackgroundRuntimeStateRequest);
}

function uninstallBackgroundStateResponder() {
  if (!backgroundStateRequestInstalled) return;
  backgroundStateRequestInstalled = false;
  coronaEventBus.off(RUNTIME_STATE_REQUEST_EVENT, onBackgroundRuntimeStateRequest);
}

async function loadPersistedProjectNodeGraph() {
  let projectPath = readActiveProjectPath();
  let response = null;
  for (let attempt = 0; attempt < 2; attempt += 1) {
    response = bridgeResult(await editorApi.scratch.loadBlocklyTarget({
      target_type: 'project',
      scene_name: '',
      actor_name: '',
      script_kind: 'node_graph',
      project_path: projectPath,
    }));
    if (response?.status === 'error' && response?.code === 'PROJECT_CONTEXT_CHANGED' && attempt === 0) {
      const backendProjectPath = String(response.project_path || '').trim();
      if (backendProjectPath) {
        projectPath = backendProjectPath;
        window.localStorage?.setItem('corona.activeProjectPath', backendProjectPath);
        continue;
      }
    }
    break;
  }
  if (response?.status === 'error') throw new Error(response.message || '节点图加载失败');
  if (response?.status !== 'loaded') throw new Error('当前世界还没有可运行的项目节点图');
  const responseProjectPath = String(response.project_path || projectPath || '').trim();
  const currentProjectPath = readActiveProjectPath();
  if (currentProjectPath && responseProjectPath
    && normalizeProjectPath(responseProjectPath) !== normalizeProjectPath(currentProjectPath)) {
    throw new Error('当前世界已切换，已取消运行旧世界的节点图');
  }
  return normalizePersistedGraph(response.workspace || {});
}

async function queryGamePreviewState() {
  const preview = bridgeResult(await editorApi.scratch.getGamePreviewStatus()) || {};
  const active = ['starting', 'running', 'stopping'].includes(String(preview.status || ''))
    || Number(preview.runningCount ?? preview.running_count ?? 0) > 0
    || Boolean(preview.hasSnapshot ?? preview.has_snapshot);
  return { ...preview, active };
}

function startBackgroundRunPoll() {
  clearBackgroundRunPoll();
  backgroundRunPollTimer = window.setInterval(async () => {
    if (!backgroundOwnsExecution || !backgroundRuntimeState.running) return;
    try {
      const status = bridgeResult(await editorApi.scratch.getScriptStatus()) || {};
      const nextStatus = formatRuntimeState(status);
      setBackgroundInputLocked(Boolean(status.inputLocked));
      updateBackgroundRuntimeState({
        running: ['starting', 'running'].includes(String(status.status || '')),
        busy: false,
        status: nextStatus,
        detail: formatRuntimeDetail(status),
        currentNodeId: String(status.currentNodeId || ''),
      });
      if (!['starting', 'running'].includes(String(status.status || ''))) {
        if (status.status === 'completed') reportBackgroundRunTerminal(true);
        else if (status.status === 'error') reportBackgroundRunTerminal(false, status.error || nextStatus);
        else resetBackgroundRunLifecycle();
        clearBackgroundRunPoll();
        backgroundOwnsExecution = false;
        setBackgroundInputLocked(false);
        uninstallBackgroundStateResponder();
      }
    } catch (error) {
      reportBackgroundRunTerminal(false, error?.message || error);
      clearBackgroundRunPoll();
      backgroundOwnsExecution = false;
      setBackgroundInputLocked(false);
      updateBackgroundRuntimeState({
        running: false,
        busy: false,
        status: '状态查询失败',
        detail: String(error?.message || error || ''),
        currentNodeId: '',
      });
      uninstallBackgroundStateResponder();
    }
  }, 300);
}

/** Stop a project node graph started without opening the node editor. */
export async function stopPersistedProjectNodeGraphRuntime({ restoreState = true } = {}) {
  clearBackgroundRunPoll();
  updateBackgroundRuntimeState({ busy: true, status: '正在停止并恢复...', detail: '' });
  try {
    let shouldStop = backgroundOwnsExecution || backgroundRuntimeState.running;
    if (!shouldStop) {
      const status = bridgeResult(await editorApi.scratch.getScriptStatus()) || {};
      const targetType = String(status.targetType || '').trim().toLowerCase();
      shouldStop = (!targetType || targetType === 'project') && scriptStatusNeedsStop(status);
    }
    let response = {};
    if (shouldStop) {
      response = bridgeResult(await editorApi.scratch.stopScriptExecution(Boolean(restoreState))) || {};
    }
    let status = '已停止';
    let detail = '';
    if (restoreState && response.restored) status = '已停止并恢复运行前状态';
    else if (restoreState && response.restoreError) {
      status = `已停止，但场景恢复失败：${response.restoreError}`;
      detail = String(response.restoreError);
    }
    backgroundOwnsExecution = false;
    setBackgroundInputLocked(false);
    resetBackgroundRunLifecycle();
    return updateBackgroundRuntimeState({
      running: false,
      busy: false,
      status,
      detail,
      currentNodeId: '',
    });
  } catch (error) {
    backgroundOwnsExecution = false;
    setBackgroundInputLocked(false);
    updateBackgroundRuntimeState({
      running: false,
      busy: false,
      status: `停止节点图失败：${error?.message || error}`,
      detail: String(error?.message || error || ''),
      currentNodeId: '',
    });
    throw error;
  } finally {
    uninstallBackgroundStateResponder();
  }
}

/**
 * Run the latest saved project node graph directly from the main page. This path
 * intentionally does not mount, open, focus or resize the node editor window.
 */
export async function togglePersistedProjectNodeGraphRuntime() {
  if (backgroundRuntimeState.busy) return normalizeRuntimeState(backgroundRuntimeState);
  if (backgroundRuntimeState.running || backgroundOwnsExecution) {
    return stopPersistedProjectNodeGraphRuntime({ restoreState: true });
  }

  installBackgroundStateResponder();
  beginBackgroundRunLifecycle();
  updateBackgroundRuntimeState({
    ready: false,
    running: false,
    busy: true,
    status: '启动中...',
    detail: '',
    currentNodeId: '',
  });

  try {
    const preview = await queryGamePreviewState();
    if (preview.active) {
      resetBackgroundRunLifecycle();
      return updateBackgroundRuntimeState({
        running: false,
        busy: false,
        status: preview.scope === 'scene' ? '当前节点图正在由全局运行执行' : '当前节点图正在由项目预览执行',
        detail: '',
      });
    }

    await flushProjectNodeGraphBeforeRun();
    const workspace = await loadPersistedProjectNodeGraph();
    const { nodeGraphToCode, validateNodeGraph } = await import('@/blockly/generators/index.js');
    const analysis = validateNodeGraph(workspace);
    const code = nodeGraphToCode(workspace);
    const warnings = Array.isArray(analysis?.warnings) ? analysis.warnings : [];
    const response = bridgeResult(await editorApi.scratch.executePythonCode(code, 0, '', '', 'project'));
    if (response?.outcome === 'preview_running') {
      resetBackgroundRunLifecycle();
      return updateBackgroundRuntimeState({
        running: false,
        busy: false,
        status: response.previewScope === 'scene' ? '当前节点图正在由全局运行执行' : '当前节点图正在由项目预览执行',
        detail: '',
      });
    }
    if (response?.status === 'error' || response?.success === false) {
      throw new Error(response?.message || '后端拒绝执行节点图');
    }

    backgroundOwnsExecution = true;
    setBackgroundInputLocked(true);
    reportBackgroundRunStarted();
    const state = updateBackgroundRuntimeState({
      running: true,
      busy: false,
      status: warnings.length ? `运行中（${warnings[0]}）` : '运行中',
      detail: '',
      currentNodeId: '',
    });
    startBackgroundRunPoll();
    return state;
  } catch (error) {
    clearBackgroundRunPoll();
    backgroundOwnsExecution = false;
    setBackgroundInputLocked(false);
    reportBackgroundRunTerminal(false, error?.message || error);
    updateBackgroundRuntimeState({
      running: false,
      busy: false,
      status: `执行失败：${error?.message || error}`,
      detail: String(error?.message || error || ''),
      currentNodeId: '',
    });
    uninstallBackgroundStateResponder();
    throw error;
  }
}

function normalizeRuntimeState(state = {}) {
  return {
    ...state,
    targetId: GLOBAL_NODE_TARGET_ID,
    ready: Boolean(state.ready),
    running: Boolean(state.running),
    busy: Boolean(state.busy),
    status: String(state.status || ''),
    detail: String(state.detail || ''),
    updatedAt: Number(state.updatedAt) || Date.now(),
  };
}

/** Publish the project-node runtime state to the main editor and detached panels. */
export function publishProjectNodeGraphRuntimeState(state = {}) {
  const payload = normalizeRuntimeState(state);
  broadcastRuntimeEvent(RUNTIME_STATE_EVENT, payload);
  return payload;
}

/**
 * Register the mounted project node graph as the single owner of run/stop.
 * The supplied toggle callback is the same handler used by the node toolbar button,
 * so callers never duplicate graph saving, validation, code generation or input locks.
 */
export function registerProjectNodeGraphRuntimeHandler({ toggle, getState }) {
  if (typeof toggle !== 'function') {
    throw new TypeError('Project node graph runtime requires a toggle handler');
  }
  let registered = true;
  const readState = () => normalizeRuntimeState({
    ...(typeof getState === 'function' ? getState() : {}),
    ready: registered,
  });
  const handled = new Set();

  const onToggleRequest = async (payload = {}) => {
    const id = String(payload.requestId || '');
    if (!id || handled.has(id)) return;
    if (payload.targetId && payload.targetId !== GLOBAL_NODE_TARGET_ID) return;
    handled.add(id);
    window.setTimeout(() => handled.delete(id), 60000);

    broadcastRuntimeEvent(RUNTIME_TOGGLE_ACCEPTED_EVENT, {
      requestId: id,
      targetId: GLOBAL_NODE_TARGET_ID,
    });

    let result;
    try {
      await toggle();
      result = {
        requestId: id,
        targetId: GLOBAL_NODE_TARGET_ID,
        success: true,
        state: readState(),
      };
    } catch (error) {
      result = {
        requestId: id,
        targetId: GLOBAL_NODE_TARGET_ID,
        success: false,
        message: String(error?.message || error || '\u8282\u70b9\u56fe\u8fd0\u884c\u5207\u6362\u5931\u8d25'),
        state: readState(),
      };
    }
    publishProjectNodeGraphRuntimeState(result.state);
    broadcastRuntimeEvent(RUNTIME_TOGGLE_RESULT_EVENT, result);
  };

  const onStateRequest = (payload = {}) => {
    if (payload.targetId && payload.targetId !== GLOBAL_NODE_TARGET_ID) return;
    publishProjectNodeGraphRuntimeState({
      ...readState(),
      requestId: String(payload.requestId || ''),
    });
  };

  coronaEventBus.on(RUNTIME_TOGGLE_REQUEST_EVENT, onToggleRequest);
  coronaEventBus.on(RUNTIME_STATE_REQUEST_EVENT, onStateRequest);
  publishProjectNodeGraphRuntimeState(readState());

  return () => {
    registered = false;
    coronaEventBus.off(RUNTIME_TOGGLE_REQUEST_EVENT, onToggleRequest);
    coronaEventBus.off(RUNTIME_STATE_REQUEST_EVENT, onStateRequest);
    publishProjectNodeGraphRuntimeState({
      ready: false,
      running: false,
      busy: false,
      status: '',
      detail: '',
    });
  };
}

/**
 * Ask the mounted project node workspace to invoke its existing run button handler.
 * Requests are repeated until the heavy detached node page finishes mounting, then
 * stop as soon as that page accepts ownership of the command.
 */
export function requestProjectNodeGraphToggle({
  ownerTimeoutMs = 15000,
  resultTimeoutMs = 30000,
  retryIntervalMs = 250,
} = {}) {
  const id = `node_graph_runtime_${Date.now()}_${Math.random().toString(36).slice(2, 9)}`;
  return new Promise((resolve, reject) => {
    let settled = false;
    let accepted = false;
    let retryTimer = null;
    let timeoutTimer = null;

    const cleanup = () => {
      if (retryTimer) window.clearInterval(retryTimer);
      if (timeoutTimer) window.clearTimeout(timeoutTimer);
      retryTimer = null;
      timeoutTimer = null;
      coronaEventBus.off(RUNTIME_TOGGLE_ACCEPTED_EVENT, onAccepted);
      coronaEventBus.off(RUNTIME_TOGGLE_RESULT_EVENT, onResult);
    };
    const finish = (result, error = null) => {
      if (settled) return;
      settled = true;
      cleanup();
      if (error) reject(error);
      else resolve(result);
    };
    const armTimeout = (delay, message, code) => {
      if (timeoutTimer) window.clearTimeout(timeoutTimer);
      timeoutTimer = window.setTimeout(() => {
        const error = new Error(message);
        error.code = code;
        finish(null, error);
      }, Math.max(500, Number(delay) || 0));
    };
    const sendRequest = () => {
      broadcastRuntimeEvent(RUNTIME_TOGGLE_REQUEST_EVENT, {
        requestId: id,
        targetId: GLOBAL_NODE_TARGET_ID,
      });
    };
    const onAccepted = (payload = {}) => {
      if (String(payload.requestId || '') !== id || accepted) return;
      accepted = true;
      if (retryTimer) window.clearInterval(retryTimer);
      retryTimer = null;
      armTimeout(resultTimeoutMs, '\u8282\u70b9\u56fe\u8fd0\u884c\u8bf7\u6c42\u6267\u884c\u8d85\u65f6', 'NODE_GRAPH_RUNTIME_RESULT_TIMEOUT');
    };
    const onResult = (payload = {}) => {
      if (String(payload.requestId || '') !== id) return;
      if (payload.success === false) {
        const error = new Error(payload.message || '\u8282\u70b9\u56fe\u8fd0\u884c\u5207\u6362\u5931\u8d25');
        error.code = 'NODE_GRAPH_RUNTIME_TOGGLE_FAILED';
        error.state = payload.state;
        finish(null, error);
        return;
      }
      finish(payload);
    };

    coronaEventBus.on(RUNTIME_TOGGLE_ACCEPTED_EVENT, onAccepted);
    coronaEventBus.on(RUNTIME_TOGGLE_RESULT_EVENT, onResult);
    armTimeout(ownerTimeoutMs, '\u8282\u70b9\u7a97\u53e3\u5c1a\u672a\u51c6\u5907\u5b8c\u6210\uff0c\u65e0\u6cd5\u8fd0\u884c\u8282\u70b9\u56fe', 'NODE_GRAPH_RUNTIME_UNAVAILABLE');
    sendRequest();
    retryTimer = window.setInterval(sendRequest, Math.max(100, Number(retryIntervalMs) || 250));
  });
}

/** Subscribe to the shared project-node runtime state and request the current value. */
export function subscribeProjectNodeGraphRuntimeState(callback, { requestCurrent = true } = {}) {
  const handler = (payload = {}) => {
    if (payload.targetId && payload.targetId !== GLOBAL_NODE_TARGET_ID) return;
    callback(normalizeRuntimeState(payload));
  };
  coronaEventBus.on(RUNTIME_STATE_EVENT, handler);
  if (requestCurrent) {
    broadcastRuntimeEvent(RUNTIME_STATE_REQUEST_EVENT, {
      requestId: `node_graph_state_${Date.now()}_${Math.random().toString(36).slice(2, 9)}`,
      targetId: GLOBAL_NODE_TARGET_ID,
    });
  }
  return () => coronaEventBus.off(RUNTIME_STATE_EVENT, handler);
}
