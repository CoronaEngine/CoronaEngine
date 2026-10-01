/** Resource readiness is a loading fence, NOT a GPU-idle/release guarantee. */
export const LOAD_POLL_MS = 200;
export const LOAD_TIMEOUT_MS = 30_000;
const timedOutWork = new Set();
const pendingListeners = new Set();
const publishPending = () => { for (const listener of pendingListeners) listener(timedOutWork.size > 0); };
export function subscribePendingLoads(listener) { pendingListeners.add(listener); return () => pendingListeners.delete(listener); }
const unwrap = value => value?.data ?? value;
const normalize = value => String(value || '').trim().replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
export const hasPendingLoadTimeout = () => timedOutWork.size > 0;
export const loadBlockedError = () => Object.assign(new Error('上一阶段超时后仍有加载请求未结束，请等待请求结束后再操作。'),
  { code: 'WORLD_LOAD_PENDING', nativePending: true });

/** Never replace the real promise with its timeout in a work registry. */
export function withLoadTimeout(promise, phase, timeoutMs = LOAD_TIMEOUT_MS) {
  const actual = Promise.resolve(promise);
  let timer, settled = false;
  const finish = () => { settled = true; if (timedOutWork.delete(actual)) publishPending(); };
  actual.then(finish, finish);
  return Promise.race([actual, new Promise((_, reject) => {
    timer = setTimeout(() => {
      if (settled) return;
      timedOutWork.add(actual); publishPending();
      const error = Object.assign(new Error(`${phase}超时；原请求尚未结束，暂不能再次打开世界。`), { code: 'WORLD_LOAD_TIMEOUT', phase });
      Object.defineProperty(error, 'nativePending', { get: () => !settled });
      reject(error);
    }, timeoutMs);
  })]).finally(() => clearTimeout(timer));
}

let sequence = 0;
export function createLoadOperation({ source = '', target = '', operationId,
  isCurrent = () => true, trackWork = promise => promise, onUpdate = () => {},
  logger = record => console.info('[world-load]', JSON.stringify(record)), timeoutMs = LOAD_TIMEOUT_MS,
  now = () => performance.now(),
} = {}) {
  const id = operationId || `world-${Date.now().toString(36)}-${++sequence}`;
  let stopped = false, lastPhase = '准备加载', lastStarted = now();
  const check = () => {
    if (stopped || !isCurrent()) throw Object.assign(new Error('世界加载已取消'), { name: 'AbortError' });
  };
  const emit = (phase, status, started, details = {}) => {
    const record = { operationId: id, source, target, phase, status, elapsedMs: Math.round(now() - started), ...details };
    logger(record); onUpdate(record);
  };
  return {
    id, check,
    fail(error) {
      stopped = true;
      if (error.operationId === id) return;
      emit(lastPhase, error.name === 'AbortError' ? 'canceled' : 'error', lastStarted, { message: error.message, pending: Boolean(error.nativePending) });
    },
    async phase(label, task, { track = true, ...details } = {}) {
      check();
      const started = now();
      lastPhase = label; lastStarted = started;
      emit(label, 'loading', started, details);
      const submitted = Promise.resolve().then(() => { check(); return task(); });
      const actual = track ? trackWork(submitted) : submitted;
      let timedOut = false;
      actual.then(() => { if (timedOut) queueMicrotask(() => emit(label, 'settled', started, details)); },
        () => { if (timedOut) queueMicrotask(() => emit(label, 'settled', started, details)); });
      try {
        const result = await withLoadTimeout(actual, label, timeoutMs);
        check(); emit(label, 'ready', started, details);
        return result;
      } catch (error) {
        timedOut = error.code === 'WORLD_LOAD_TIMEOUT';
        stopped = true;
        error.phase ||= label;
        error.operationId ||= id;
        emit(label, error.name === 'AbortError' ? 'canceled' : 'error', started, { ...details, message: error.message, pending: Boolean(error.nativePending) });
        throw error;
      }
    },
    finish() { emit('', 'complete', now()); },
  };
}

/** Continues read-only polling after a UI timeout to retain the actual load fence.
 * Failed resources are terminal only when all other submitted resources settle.
 * A hung status request is awaited, never overlapped by another status request.
 */
export async function waitForProjectResources({ getStatus, projectPath, wait = sleep, allowFailed = false }) {
  for (;;) {
    const status = unwrap(await getStatus());
    if (status?.status === 'error' || status?.ok === false) throw new Error(status.message || '无法读取世界资源状态');
    if (!status || normalize(status.path) !== normalize(projectPath)) {
      throw new Error('资源状态不属于目标世界，已停止加载');
    }
    const pending = Number(status.pending), failed = Number(status.failed);
    if (!Number.isFinite(pending) || !Number.isFinite(failed) || pending < 0 || failed < 0) {
      throw new Error('引擎未返回有效的世界资源状态');
    }
    if (pending === 0 && status.loading !== true && status.archive_service_ready === true) {
      if (failed > 0 && !allowFailed) throw new Error(`世界资源加载失败（${failed} 个模型）`);
      return status;
    }
    await wait(LOAD_POLL_MS);
  }
}
