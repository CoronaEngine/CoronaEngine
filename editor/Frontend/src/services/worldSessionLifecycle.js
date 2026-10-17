/** Work that must finish against its originating world before native replacement. */
const pending = new Set();
export function withWorldTimeout(promise, operation, timeoutMs = 30_000) {
  let timer;
  return Promise.race([promise, new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${operation}超时，已取消切换世界，请重试。`)), timeoutMs);
  })]).finally(() => clearTimeout(timer));
}
export function trackWorldSessionWork(promise) {
  const work = Promise.resolve(promise);
  pending.add(work);
  work.then(() => pending.delete(work), () => pending.delete(work));
  return work;
}
export async function drainWorldSession() {
  // A timeout leaves unfinished work registered. A retry cannot bypass it.
  while (pending.size) await withWorldTimeout(Promise.allSettled([...pending]), '等待旧世界初始化结束');
}
const notifiedErrors = new WeakSet();
export function notifyWorldError(error, fallback = '打开世界失败') {
  if (error && typeof error === 'object') {
    if (notifiedErrors.has(error)) return;
    notifiedErrors.add(error);
  }
  window.alert(error?.message || fallback);
}

// Persistent finalizers survive component teardown and failed opens. Unlike
// initialization work, a rejected save MUST block native scene replacement.
const saves = new Set();
export function registerWorldSessionSave(save) {
  const entry = { retired: false, pending: null, flush: null };
  entry.flush = async () => {
    if (!entry.pending) {
      const work = Promise.resolve().then(save);
      entry.pending = work;
      work.then(() => {
        if (entry.pending === work) entry.pending = null;
        if (entry.retired) saves.delete(entry);
      }, () => { if (entry.pending === work) entry.pending = null; });
    }
    await withWorldTimeout(entry.pending, '保存玩家位置');
  };
  saves.add(entry);
  return {
    flush: entry.flush,
    // Retire, rather than delete: beginOpen invalidates/unmounts the old page
    // before drainWorldSession and the native open execute.
    retire() { entry.retired = true; },
    release() { saves.delete(entry); },
  };
}
export async function flushWorldSessionSaves() {
  for (const entry of [...saves]) await entry.flush();
}
