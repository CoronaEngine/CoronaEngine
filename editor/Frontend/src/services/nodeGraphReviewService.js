import { aiService } from '@/services/aiService.js';
import { coronaEventBus } from '@/utils/eventBus.js';

const CHANNEL_NAME = 'corona-node-graph-review-v1';
const REVIEW_EVENT = 'node-graph-review-result';
const REVIEW_INTERVAL_MS = 10000;
const STATUS_POLL_INTERVAL_MS = 2000;
const OMITTED_SNAPSHOT_KEYS = new Set(['x', 'y', 'selected', 'viewport', 'zoom']);
let channel = null;

function getChannel() {
  if (typeof BroadcastChannel === 'undefined') return null;
  if (!channel) channel = new BroadcastChannel(CHANNEL_NAME);
  return channel;
}

function clone(value) {
  return JSON.parse(JSON.stringify(value || {}));
}

function createReviewSnapshot(workspace) {
  return JSON.parse(JSON.stringify(workspace || {}, (key, value) => (
    OMITTED_SNAPSHOT_KEYS.has(key) ? undefined : value
  )));
}

function hashText(text) {
  let hash = 2166136261;
  for (let i = 0; i < text.length; i += 1) {
    hash ^= text.charCodeAt(i);
    hash = Math.imul(hash, 16777619);
  }
  return (hash >>> 0).toString(16).padStart(8, '0');
}

export function normalizeReviewScope(scope) {
  return String(scope || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/\/+$/, '')
    .toLocaleLowerCase('en-US');
}

export function reviewScopeId(scope) {
  return hashText(normalizeReviewScope(scope));
}

export function graphRevision(workspace, scope = '') {
  const snapshot = createReviewSnapshot(workspace);
  return hashText(`${normalizeReviewScope(scope)}\n${JSON.stringify(snapshot)}`);
}

function snapshotWithRevision(workspace, scope = '') {
  const snapshot = createReviewSnapshot(workspace);
  if (!Array.isArray(snapshot?.nodes) || !Array.isArray(snapshot?.edges)) return null;
  const normalizedScope = normalizeReviewScope(scope);
  return {
    workspace: snapshot,
    revision: hashText(`${normalizedScope}\n${JSON.stringify(snapshot)}`),
    projectScopeId: reviewScopeId(normalizedScope),
  };
}

export function publishNodeGraphReview(result) {
  const payload = { ...result, publishedAt: Date.now() };
  coronaEventBus.emit(REVIEW_EVENT, payload);
  try { getChannel()?.postMessage(payload); } catch (_) {}
}

export function subscribeNodeGraphReviews(listener) {
  const local = (payload) => listener(payload);
  const broadcast = (event) => listener(event.data);
  coronaEventBus.on(REVIEW_EVENT, local);
  const currentChannel = getChannel();
  currentChannel?.addEventListener('message', broadcast);
  return () => {
    coronaEventBus.off(REVIEW_EVENT, local);
    currentChannel?.removeEventListener('message', broadcast);
  };
}

export function startNodeGraphReview({
  getWorkspace,
  getRevisionScope = () => '',
  getProjectContext = () => ({}),
  enabled = () => true,
  intervalMs = REVIEW_INTERVAL_MS,
}) {
  let stopped = false;
  let scanTimer = null;
  let firstScanTimer = null;
  let statusTimer = null;
  let scanBusy = false;
  let requestSequence = 0;
  let activeTask = null;
  let pendingReview = null;
  let lastSuccessfulReviewKey = '';
  let lastErrorKey = '';

  // Floating CEF panels may report document.visibilityState === 'hidden' even while
  // they are open in the main editor. Review availability must follow the panel lifecycle,
  // not browser visibility, otherwise background node checks silently stop.
  const canReview = () => !stopped && enabled();

  const currentCandidate = () => {
    const snapshot = snapshotWithRevision(getWorkspace?.(), getRevisionScope?.());
    if (!snapshot) return null;
    const projectContext = clone(getProjectContext?.());
    return {
      ...snapshot,
      projectContext,
      reviewKey: hashText(`${snapshot.revision}\n${JSON.stringify(projectContext)}`),
    };
  };

  const publishErrorOnce = (result, requestId, revision, projectScopeId = '') => {
    const errorKey = String(result?.error || result?.message || 'AI_REVIEW_FAILED');
    if (errorKey === lastErrorKey) return;
    lastErrorKey = errorKey;
    publishNodeGraphReview({ ...result, requestId, graphRevision: revision, projectScopeId });
  };

  const scheduleStatusPoll = (delay = STATUS_POLL_INTERVAL_MS) => {
    if (stopped || !activeTask) return;
    if (statusTimer) window.clearTimeout(statusTimer);
    statusTimer = window.setTimeout(pollActiveTask, delay);
  };

  const startCandidate = async (candidate) => {
    if (!candidate || stopped || activeTask || !canReview()) return;
    if (candidate.reviewKey === lastSuccessfulReviewKey) return;

    const requestId = 'node_review_' + Date.now() + '_' + (++requestSequence);
    const task = {
      requestId,
      revision: candidate.revision,
      reviewKey: candidate.reviewKey,
      workspace: candidate.workspace,
      projectContext: candidate.projectContext,
      projectScopeId: candidate.projectScopeId,
      taskId: '',
    };
    activeTask = task;
    try {
      const response = await aiService.startNodeGraphReview({
        schemaVersion: 1,
        requestId,
        graphRevision: candidate.revision,
        targetId: 'node_graph:project:global',
        workspace: candidate.workspace,
        projectScopeId: candidate.projectScopeId,
        projectContext: candidate.projectContext,
      });
      if (stopped || activeTask !== task) return;
      if (response?.success !== true || !response?.taskId) {
        activeTask = null;
        publishErrorOnce(response || { success: false, status: 'error' }, requestId, candidate.revision, candidate.projectScopeId);
        return;
      }
      task.taskId = String(response.taskId);
      scheduleStatusPoll(response.status === 'completed' ? 0 : STATUS_POLL_INTERVAL_MS);
    } catch (error) {
      if (activeTask === task) activeTask = null;
      if (!stopped) {
        publishErrorOnce({
          success: false,
          status: 'error',
          error: 'BRIDGE_ERROR',
          message: String(error?.message || error || 'BRIDGE_ERROR'),
        }, requestId, candidate.revision, candidate.projectScopeId);
      }
    }
  };

  const continueWithPending = () => {
    if (stopped || activeTask || !pendingReview || !canReview()) return;
    const candidate = pendingReview;
    pendingReview = null;
    if (candidate.reviewKey !== lastSuccessfulReviewKey) {
      window.setTimeout(() => startCandidate(candidate), 0);
    }
  };

  async function pollActiveTask() {
    statusTimer = null;
    const task = activeTask;
    if (!task || stopped || !task.taskId) return;
    try {
      const response = await aiService.getNodeGraphReviewStatus(task.taskId);
      if (stopped || activeTask !== task) return;
      if (response?.success !== true) {
        activeTask = null;
        publishErrorOnce(response || { success: false, status: 'error' }, task.requestId, task.revision, task.projectScopeId);
        continueWithPending();
        return;
      }
      if (response.status !== 'completed') {
        scheduleStatusPoll();
        return;
      }

      activeTask = null;
      const result = response.result || {};
      const latest = currentCandidate();
      if (!latest || latest.reviewKey !== task.reviewKey) {
        if (latest) pendingReview = latest;
        continueWithPending();
        return;
      }

      if (result?.success === true && result?.status === 'ok') {
        lastSuccessfulReviewKey = task.reviewKey;
        lastErrorKey = '';
        publishNodeGraphReview({
          ...result,
          requestId: task.requestId,
          graphRevision: task.revision,
          projectScopeId: task.projectScopeId,
          graphExcerpt: task.workspace,
          projectContext: task.projectContext,
        });
      } else {
        publishErrorOnce(result, task.requestId, task.revision, task.projectScopeId);
      }
      continueWithPending();
    } catch (error) {
      if (activeTask === task) activeTask = null;
      if (!stopped) {
        publishErrorOnce({
          success: false,
          status: 'error',
          error: 'BRIDGE_ERROR',
          message: String(error?.message || error || 'BRIDGE_ERROR'),
        }, task.requestId, task.revision, task.projectScopeId);
      }
      continueWithPending();
    }
  }

  const scan = async () => {
    if (scanBusy || !canReview()) return;
    scanBusy = true;
    try {
      const candidate = currentCandidate();
      if (!candidate) return;
      if (activeTask) {
        if (candidate.reviewKey !== activeTask.reviewKey) pendingReview = candidate;
        return;
      }
      if (candidate.reviewKey === lastSuccessfulReviewKey) return;
      await startCandidate(candidate);
    } finally {
      scanBusy = false;
    }
  };

  const period = Math.max(1000, Number(intervalMs) || REVIEW_INTERVAL_MS);
  scanTimer = window.setInterval(scan, period);
  firstScanTimer = window.setTimeout(scan, Math.min(750, period));

  const stop = () => {
    stopped = true;
    if (firstScanTimer) window.clearTimeout(firstScanTimer);
    if (scanTimer) window.clearInterval(scanTimer);
    if (statusTimer) window.clearTimeout(statusTimer);
    firstScanTimer = null;
    scanTimer = null;
    statusTimer = null;
    activeTask = null;
    pendingReview = null;
  };
  stop.scanNow = (delay = 0) => {
    if (stopped) return;
    if (firstScanTimer) window.clearTimeout(firstScanTimer);
    firstScanTimer = window.setTimeout(() => {
      firstScanTimer = null;
      scan();
    }, Math.max(0, Number(delay) || 0));
  };
  return stop;
}
