import assert from 'node:assert/strict';
import test from 'node:test';
import { createLoadOperation, withLoadTimeout, hasPendingLoadTimeout, waitForProjectResources } from '../../frontend/worldLoading.mjs';
import { deferred, projectReady } from './fixtures.mjs';
const turn = () => new Promise(resolve => setImmediate(resolve));

test('resource fence polls every 200ms, validates path, and waits past native open acknowledgement', async () => {
  const delays = []; let reads = 0;
  const status = await waitForProjectResources({ projectPath: 'D:\\world\\',
    getStatus: async () => ({ data: { ...projectReady('d:/world'), pending: ++reads < 3 ? 1 : 0, loading: reads < 3 } }),
    wait: async ms => delays.push(ms),
  });
  assert.equal(status.pending, 0); assert.deepEqual(delays, [200, 200]);
  await assert.rejects(waitForProjectResources({ projectPath: 'D:/new', getStatus: async () => projectReady('D:/old') }), /不属于目标世界/);
});
test('failure cannot release the fence while other resources remain pending', async () => {
  const gate = deferred(); let reads = 0, settled = false;
  const loading = waitForProjectResources({ projectPath: 'D:/world', getStatus: async () => ({
    ...projectReady('D:/world'), failed: 1, pending: ++reads === 1 ? 1 : 0,
  }), wait: () => gate.promise });
  const outcome = assert.rejects(loading, /1 个模型/).then(() => { settled = true; });
  await turn(); assert.equal(settled, false); gate.resolve(); await outcome;
  await waitForProjectResources({ projectPath: 'D:/world', allowFailed: true,
    getStatus: async () => ({ ...projectReady('D:/world'), failed: 1 }) });
});
test('a phase timeout retains real work and a late reply cannot start the next phase', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const gate = deferred(), tracked = [], records = [];
  const op = createLoadOperation({ source: 'main', target: 'child', trackWork: work => { tracked.push(work); return work; }, logger: r => records.push(r) });
  const loading = op.phase('打开目标世界', () => gate.promise);
  const rejected = assert.rejects(loading, error => error.nativePending && error.phase === '打开目标世界');
  await turn(); t.mock.timers.tick(30_001); await rejected;
  assert.equal(hasPendingLoadTimeout(), true);
  let mutation = false;
  await assert.rejects(op.phase('新增先知', () => { mutation = true; }), { name: 'AbortError' });
  gate.resolve({ ok: true }); await Promise.allSettled(tracked); await turn();
  assert.equal(mutation, false); assert.equal(hasPendingLoadTimeout(), false);
  assert.ok(records.every(r => r.operationId === op.id && r.source === 'main' && r.target === 'child'));
  assert.ok(records.some(r => r.status === 'error' && r.elapsedMs >= 0));
});
test('hung status queries do not overlap and remain fenced after timeout', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const gate = deferred(); let reads = 0;
  const work = waitForProjectResources({ projectPath: 'D:/world', getStatus: () => { reads++; return gate.promise; } });
  const timeout = assert.rejects(withLoadTimeout(work, '等待资源'), /超时/);
  t.mock.timers.tick(30_001); await timeout;
  assert.equal(reads, 1); assert.equal(hasPendingLoadTimeout(), true);
  gate.resolve(projectReady('D:/world')); await work; await turn();
  assert.equal(reads, 1); assert.equal(hasPendingLoadTimeout(), false);
});
test('ordinary failure and successful load do not leave a timeout lock', async () => {
  await assert.rejects(withLoadTimeout(Promise.reject(new Error('failed')), 'load'), /failed/);
  assert.equal(await withLoadTimeout(Promise.resolve(42), 'load'), 42);
  assert.equal(hasPendingLoadTimeout(), false);
});
