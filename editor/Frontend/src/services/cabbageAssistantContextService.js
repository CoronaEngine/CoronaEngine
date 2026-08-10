import { aiService } from '@/services/aiService.js';
import { createServiceResponseError } from '@/utils/serviceInitialization.js';

const CHANNEL_NAME = 'corona-cabbage-assistant-context-v2';
const TRANSIENT_CHANNEL_NAME = 'corona-cabbage-assistant-transient-v1';
const STORAGE_KEY = 'corona.cabbageAssistantContext.v2';
const CONTEXT_MESSAGE_TYPE = 'cabbage-assistant-context';
const PRE_WARNING_MESSAGE_TYPE = 'cabbage-pre-warning';
const TRANSFORM_DEBOUNCE_MS = 650;
const PROFILE_POLL_MS = 1200;
const GOAL_PLAN_POLL_MS = 1000;
const GOAL_PLAN_TIMEOUT_MS = 60000;

let channel = null;
let transientChannel = null;
let latestSnapshot = null;
let writeChain = Promise.resolve();
let activeScoreUpdateTaskId = '';
const activeGoalPlanPolls = new Map();
const pendingTransforms = new Map();
const localSubscribers = new Set();
const localPreWarningSubscribers = new Set();

function getChannel() {
  if (typeof BroadcastChannel === 'undefined') return null;
  if (!channel) channel = new BroadcastChannel(CHANNEL_NAME);
  return channel;
}

function getTransientChannel() {
  if (typeof BroadcastChannel === 'undefined') return null;
  if (!transientChannel) transientChannel = new BroadcastChannel(TRANSIENT_CHANNEL_NAME);
  return transientChannel;
}

function clone(value, fallback) {
  try { return JSON.parse(JSON.stringify(value)); } catch (_) { return fallback; }
}

function currentProjectPath() {
  return String(window.localStorage?.getItem('corona.activeProjectPath') || '')
    .trim().replace(/\\/g, '/').replace(/\/+$/, '');
}

function currentWorldId() {
  const path = currentProjectPath();
  if (!path) return '';
  const parts = path.split('/').filter(Boolean);
  return String(parts[parts.length - 1] || '');
}

function currentScopeId() {
  const path = currentProjectPath().toLocaleLowerCase('en-US');
  let hash = 2166136261;
  for (let index = 0; index < path.length; index += 1) {
    hash ^= path.charCodeAt(index);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(16).padStart(8, '0');
}

function normalizeTask(task) {
  if (!task || typeof task !== 'object') return null;
  const taskKey = String(task.taskKey || task.issueKey || task.code || '').trim();
  if (!taskKey) return null;
  return { ...clone(task, {}), taskKey, issueKey: taskKey };
}

function normalizeSnapshot(value) {
  if (!value || typeof value !== 'object') return null;
  const context = value.context && typeof value.context === 'object' ? value.context : value;
  const activeTasks = (Array.isArray(context.activeTasks) ? context.activeTasks : []).map(normalizeTask).filter(Boolean);
  const taskHistory = (Array.isArray(context.taskHistory) ? context.taskHistory : []).map(normalizeTask).filter(Boolean);
  const selectedTaskKey = String(value.selectedTaskKey || context.selectedTaskKey || '');
  return {
    schemaVersion: Number(context.schemaVersion) || 2,
    worldId: String(context.worldId || ''),
    projectScopeId: String(value.projectScopeId || context.projectScopeId || currentScopeId()),
    graphRevision: String(value.graphRevision || context.graphRevision || ''),
    graphExcerpt: clone(value.graphExcerpt || context.graphExcerpt || {}, {}),
    projectContext: clone(value.projectContext || context.projectContext || {}, {}),
    profile: clone(context.profile || {}, {}),
    profileHistory: clone(context.profileHistory || [], []),
    issueMemory: clone(context.issueMemory || {}, {}),
    metrics: clone(context.metrics || {}, {}),
    worldGoal: clone(context.worldGoal || {}, {}),
    goalTaskPlan: clone(context.goalTaskPlan || {}, {}),
    goalSignalCounts: clone(context.goalSignalCounts || {}, {}),
    tutorialSession: clone(context.tutorialSession || {}, {}),
    activeTasks,
    taskHistory,
    chatMessages: clone(context.chatMessages || [], []),
    recentOperationEvents: clone(context.recentOperationEvents || [], []),
    selectedTaskKey: [...activeTasks, ...taskHistory].some((task) => task.taskKey === selectedTaskKey)
      ? selectedTaskKey
      : '',
    updatedAt: Number(context.updatedAt || value.updatedAt) || Date.now(),
  };
}

function normalizePreWarningEnvelope(value) {
  if (!value || typeof value !== 'object') return null;
  const warning = clone(value.warning && typeof value.warning === 'object' ? value.warning : value, {});
  const code = String(warning.code || '').trim();
  const graphRevision = String(warning.graphRevision || '').trim();
  if (!code || !graphRevision) return null;
  return {
    projectScopeId: String(value.projectScopeId || warning.projectScopeId || currentScopeId()),
    worldId: String(value.worldId || warning.worldId || currentWorldId()),
    publishedAt: Number(value.publishedAt) || Date.now(),
    warning: {
      ...warning,
      code,
      graphRevision,
    },
  };
}

function publishSnapshot(snapshot) {
  const normalized = normalizeSnapshot(snapshot);
  if (!normalized) return null;
  latestSnapshot = normalized;
  try { window.localStorage?.setItem(STORAGE_KEY, JSON.stringify(normalized)); } catch (_) {}
  // BroadcastChannel and the storage event do not notify the document that performed
  // the write. Notify in-page subscribers explicitly so completed tutorial tasks are
  // removed and their replacements appear without reopening the world or Dock.
  for (const subscriber of Array.from(localSubscribers)) {
    try { subscriber(normalized); } catch (error) {
      console.warn('[CabbageContext] local subscriber failed', error?.message || error);
    }
  }
  try { getChannel()?.postMessage({ type: CONTEXT_MESSAGE_TYPE, payload: normalized }); } catch (_) {}
  return normalized;
}

function enqueue(operation) {
  const run = () => Promise.resolve().then(operation);
  const pending = writeChain.then(run, run);
  writeChain = pending.catch((error) => {
    console.warn('[CabbageContext]', error?.message || error);
  });
  return pending;
}

function responseContext(response) {
  if (response?.success === true && response?.context) return response.context;
  return null;
}

function publishBackendContext(context) {
  const scope = currentScopeId();
  const current = latestSnapshot?.projectScopeId === scope ? latestSnapshot : null;
  return publishSnapshot({
    context,
    projectScopeId: scope,
    graphRevision: current?.graphRevision || '',
    graphExcerpt: current?.graphExcerpt || {},
    projectContext: current?.projectContext || {},
    selectedTaskKey: current?.selectedTaskKey || '',
  });
}

function transformKey(event) {
  const details = event?.details || {};
  return [event.type, details.sceneName || '', details.actorName || details.actor || ''].join(':');
}

async function sendEvent(event) {
  const response = await aiService.recordCabbageEvent({
    ...event,
    worldId: String(event?.worldId || latestSnapshot?.worldId || ''),
    timestamp: Number(event?.timestamp) || Date.now(),
  });
  const context = responseContext(response);
  if (context) publishBackendContext(context);
  if (response?.success === true && (
    Array.isArray(response.completedTaskKeys) && response.completedTaskKeys.length
    || ['run_started', 'run_succeeded', 'run_failed', 'node_issue_found', 'node_issue_fixed'].includes(event?.type)
  )) {
    void requestProfileScoreUpdate().catch(() => {});
  }
  return response;
}

export function createCabbageAssistantContext(source = {}) {
  return normalizeSnapshot({
    ...source,
    activeTasks: source.activeTasks || source.tasks,
    chatMessages: source.chatMessages || source.messages,
    updatedAt: Date.now(),
  });
}

export function publishCabbageAssistantContext(source = {}) {
  return publishSnapshot(createCabbageAssistantContext(source));
}

function sameSerializableValue(left, right) {
  try { return JSON.stringify(left ?? {}) === JSON.stringify(right ?? {}); } catch (_) { return false; }
}

export function publishCabbageProjectContext(projectContext = {}, projectScopeId = '') {
  const scope = String(projectScopeId || currentScopeId());
  const current = readCabbageAssistantContext(scope);
  // A secondary CEF page (for example the node window) must never create a
  // task-less assistant snapshot before the main page has loaded the world context.
  if (!current) return null;

  const nextProjectContext = clone(projectContext, {});
  if (sameSerializableValue(current.projectContext, nextProjectContext)) return current;

  return publishSnapshot({
    ...current,
    projectScopeId: scope,
    projectContext: nextProjectContext,
    updatedAt: Math.max(Date.now(), Number(current.updatedAt || 0) + 1),
  });
}

export function readCabbageAssistantContext(projectScopeId = '') {
  const expectedScope = String(projectScopeId || '');
  let snapshot = latestSnapshot;
  if (!snapshot || (expectedScope && snapshot.projectScopeId !== expectedScope)) {
    try { snapshot = normalizeSnapshot(JSON.parse(window.localStorage?.getItem(STORAGE_KEY) || 'null')); } catch (_) { snapshot = null; }
  }
  if (!snapshot) return null;
  if (expectedScope && snapshot.projectScopeId !== expectedScope) return null;
  return clone(snapshot, null);
}

export async function loadCurrentWorld() {
  cancelPendingTransformEvents();
  const expectedWorldId = currentWorldId();
  const response = await aiService.loadCabbageContext({ worldId: expectedWorldId });
  if (response?.success !== true || !response?.context) {
    throw createServiceResponseError(response, '加载当前世界的包菜上下文失败');
  }
  const loadedWorldId = String(response.context.worldId || '');
  if (expectedWorldId && loadedWorldId !== expectedWorldId) {
    throw new Error(`后端返回了其他世界的上下文（${loadedWorldId || '未知'}），已拒绝加载`);
  }
  return publishSnapshot({ context: response.context, projectScopeId: currentScopeId() });
}

function goalPlanError(response, fallback) {
  const result = response?.result;
  return new Error(
    result?.message || response?.message || fallback || '生成当前世界的专属任务失败',
  );
}

async function pollGoalPlan(taskId, expectedScopeId = '') {
  const deadline = Date.now() + GOAL_PLAN_TIMEOUT_MS;
  while (Date.now() < deadline) {
    const response = await aiService.getCabbageGoalPlanStatus(taskId);
    if (response?.success !== true) throw goalPlanError(response);
    if (response.status === 'completed') {
      const result = response.result || {};
      const stillCurrent = !expectedScopeId || currentScopeId() === expectedScopeId;
      if (result?.context && stillCurrent) publishBackendContext(result.context);
      // The backend writes the result to the world that started the request. If the
      // user changed worlds while it was running, do not publish that old context
      // under the new world's front-end scope.
      if (!stillCurrent) return null;
      if (result?.success !== true || !result?.context) throw goalPlanError(response);
      return readCabbageAssistantContext(expectedScopeId);
    }
    await new Promise((resolve) => window.setTimeout(resolve, GOAL_PLAN_POLL_MS));
  }
  throw new Error('生成当前世界的专属任务超时，请进入世界后稍后重试');
}

function trackGoalPlan(taskId, expectedScopeId = '') {
  const key = `${String(taskId || '')}:${String(expectedScopeId || '')}`;
  const existing = activeGoalPlanPolls.get(key);
  if (existing) return existing;
  const pending = pollGoalPlan(taskId, expectedScopeId).finally(() => {
    if (activeGoalPlanPolls.get(key) === pending) activeGoalPlanPolls.delete(key);
  });
  activeGoalPlanPolls.set(key, pending);
  return pending;
}

export async function initializeWorldTasks({ prompt = '', mode = 'story', waitForCompletion = true } = {}) {
  const expectedScopeId = currentScopeId();
  const response = await aiService.startCabbageGoalPlan({
    worldId: currentWorldId(),
    prompt: String(prompt || '').trim(),
    mode: String(mode || 'story'),
  });
  if (response?.success !== true) throw goalPlanError(response);
  if (response.status === 'completed' && response.context) {
    return currentScopeId() === expectedScopeId ? publishBackendContext(response.context) : null;
  }
  const taskId = String(response.taskId || '');
  if (!taskId) throw new Error('世界任务生成请求没有返回任务编号');
  if (!waitForCompletion) {
    void trackGoalPlan(taskId, expectedScopeId).catch((error) => {
      console.warn('[CabbageContext] 世界专属任务后台生成失败：', error?.message || error);
    });
    return { success: true, status: 'pending', taskId };
  }
  return trackGoalPlan(taskId, expectedScopeId);
}

export function recordEvent(event = {}) {
  const normalized = { ...clone(event, {}), timestamp: Number(event.timestamp) || Date.now() };
  if (['transform_position', 'transform_rotation', 'transform_scale'].includes(normalized.type)) {
    const key = transformKey(normalized);
    const previous = pendingTransforms.get(key);
    if (previous?.timer) window.clearTimeout(previous.timer);
    const entry = {
      event: normalized,
      timer: window.setTimeout(() => {
        pendingTransforms.delete(key);
        enqueue(() => sendEvent(entry.event));
      }, TRANSFORM_DEBOUNCE_MS),
    };
    pendingTransforms.set(key, entry);
    return Promise.resolve({ success: true, status: 'queued' });
  }
  return enqueue(() => sendEvent(normalized));
}

export function updateTask(payload = {}) {
  return enqueue(async () => {
    const response = await aiService.updateCabbageTask({
      ...payload,
      worldId: String(payload?.worldId || latestSnapshot?.worldId || ''),
    });
    const context = responseContext(response);
    if (context) publishBackendContext(context);
    return response;
  });
}

export function appendMessage(message = {}) {
  return enqueue(async () => {
    const response = await aiService.appendCabbageMessage({
      ...message,
      worldId: String(message?.worldId || latestSnapshot?.worldId || ''),
    });
    const context = responseContext(response);
    if (context) publishBackendContext(context);
    return response;
  });
}

async function pollScoreUpdate(taskId) {
  while (activeScoreUpdateTaskId === taskId) {
    const response = await aiService.getCabbageProfileScoreStatus(taskId);
    if (response?.success !== true) break;
    if (response.status === 'completed') {
      activeScoreUpdateTaskId = '';
      const result = response.result || {};
      if (result?.success === true && result?.profile) {
        const snapshot = readCabbageAssistantContext() || {};
        publishSnapshot({ ...snapshot, profile: result.profile, updatedAt: Date.now() });
      }
      return result;
    }
    await new Promise((resolve) => window.setTimeout(resolve, PROFILE_POLL_MS));
  }
  return null;
}

export async function requestProfileScoreUpdate(options = {}) {
  if (activeScoreUpdateTaskId) return { success: true, status: 'pending', taskId: activeScoreUpdateTaskId };
  const response = await aiService.startCabbageProfileScoreUpdate(options);
  if (response?.status === 'skipped' && response.profile) {
    const snapshot = readCabbageAssistantContext() || {};
    publishSnapshot({ ...snapshot, profile: response.profile, updatedAt: Date.now() });
    return response;
  }
  if (response?.success === true && response?.taskId) {
    activeScoreUpdateTaskId = String(response.taskId);
    void pollScoreUpdate(activeScoreUpdateTaskId).catch((error) => {
      activeScoreUpdateTaskId = '';
      console.warn('[CabbageContext] profile score update failed', error?.message || error);
    });
  }
  return response;
}

export function subscribeCabbageAssistantContext(listener, { projectScopeId = '', emitCurrent = true } = {}) {
  let latestUpdatedAt = 0;
  const getExpectedScope = () => String(typeof projectScopeId === 'function' ? projectScopeId() : projectScopeId || '');
  const accept = (value) => {
    const snapshot = normalizeSnapshot(value);
    if (!snapshot) return;
    const expected = getExpectedScope();
    if (expected && snapshot.projectScopeId !== expected) return;
    if (snapshot.updatedAt < latestUpdatedAt) return;
    latestUpdatedAt = snapshot.updatedAt;
    latestSnapshot = snapshot;
    listener(clone(snapshot, snapshot));
  };
  const onBroadcast = (event) => {
    if (event?.data?.type === CONTEXT_MESSAGE_TYPE) accept(event.data.payload);
  };
  const onStorage = (event) => {
    if (event?.key !== STORAGE_KEY || !event.newValue) return;
    try { accept(JSON.parse(event.newValue)); } catch (_) {}
  };
  const currentChannel = getChannel();
  localSubscribers.add(accept);
  currentChannel?.addEventListener('message', onBroadcast);
  window.addEventListener('storage', onStorage);
  if (emitCurrent) {
    const current = readCabbageAssistantContext(getExpectedScope());
    if (current) accept(current);
  }
  return () => {
    localSubscribers.delete(accept);
    currentChannel?.removeEventListener('message', onBroadcast);
    window.removeEventListener('storage', onStorage);
  };
}

// Repeated-issue warnings are intentionally transient. They travel between the
// node graph window and the main task board, but are never written to localStorage
// or the world's CabbageAssistant/context.json.
export function publishCabbagePreWarning(warning = {}) {
  const envelope = normalizePreWarningEnvelope(warning);
  if (!envelope) return null;
  for (const subscriber of Array.from(localPreWarningSubscribers)) {
    try { subscriber(envelope); } catch (error) {
      console.warn('[CabbageContext] local pre-warning subscriber failed', error?.message || error);
    }
  }
  try {
    getTransientChannel()?.postMessage({ type: PRE_WARNING_MESSAGE_TYPE, payload: envelope });
  } catch (_) {}
  return clone(envelope.warning, envelope.warning);
}

export function subscribeCabbagePreWarnings(listener, { projectScopeId = '', worldId = '' } = {}) {
  const getExpectedScope = () => String(typeof projectScopeId === 'function' ? projectScopeId() : projectScopeId || '');
  const getExpectedWorld = () => String(typeof worldId === 'function' ? worldId() : worldId || '');
  const accept = (value) => {
    const envelope = normalizePreWarningEnvelope(value);
    if (!envelope) return;
    const expectedScope = getExpectedScope();
    const expectedWorld = getExpectedWorld();
    if (expectedScope && envelope.projectScopeId !== expectedScope) return;
    if (expectedWorld && envelope.worldId && envelope.worldId !== expectedWorld) return;
    listener(clone(envelope.warning, envelope.warning), clone(envelope, envelope));
  };
  const onBroadcast = (event) => {
    if (event?.data?.type === PRE_WARNING_MESSAGE_TYPE) accept(event.data.payload);
  };
  const currentChannel = getTransientChannel();
  localPreWarningSubscribers.add(accept);
  currentChannel?.addEventListener('message', onBroadcast);
  return () => {
    localPreWarningSubscribers.delete(accept);
    currentChannel?.removeEventListener('message', onBroadcast);
  };
}

export function cancelPendingTransformEvents() {
  for (const entry of pendingTransforms.values()) {
    if (entry.timer) window.clearTimeout(entry.timer);
  }
  pendingTransforms.clear();
}

export async function flush() {
  const entries = Array.from(pendingTransforms.values());
  pendingTransforms.clear();
  for (const entry of entries) {
    if (entry.timer) window.clearTimeout(entry.timer);
    enqueue(() => sendEvent(entry.event));
  }
  await writeChain;
}

export const cabbageContextService = {
  loadCurrentWorld,
  initializeWorldTasks,
  recordEvent,
  updateTask,
  appendMessage,
  requestProfileScoreUpdate,
  publishPreWarning: publishCabbagePreWarning,
  subscribePreWarnings: subscribeCabbagePreWarnings,
  flush,
};
