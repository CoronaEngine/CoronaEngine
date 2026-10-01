import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate } from 'node:timers';
import { editorApi } from '../../src/api/editorApi.js';
import { projectLauncherService, cancelPendingProjectOpen } from '../../src/services/projectLauncherService.js';
import { worldModeService } from '../../src/services/worldModeService.js';
import { beginWorldLoad, worldLoadingState } from '../../src/services/worldLoadingService.js';
import { hasPendingLoadTimeout } from '../../../../game/frontend/worldLoading.mjs';
import lanchat from '../../src/stores/lanchat.js';
const turn = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => { let resolve; const promise = new Promise(r => { resolve = r; }); return { promise, resolve }; };
const ready = path => ({ path, active: true, archive_service_ready: true, pending: 0, failed: 0, loading: false });
function fixture(t, options = {}) {
  let path = 'D:/main'; const calls = [], stored = [];
  const previous = { window: globalThis.window, CustomEvent: globalThis.CustomEvent };
  globalThis.window = { localStorage: { setItem: (...args) => stored.push(args) }, dispatchEvent() {} };
  globalThis.CustomEvent = class { constructor(type, data) { Object.assign(this, { type }, data); } };
  t.mock.method(console, 'info', () => {});
  t.mock.method(lanchat, 'finishWorldSession', async () => {});
  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => ({ project_path: path, mode: 'story' }));
  t.mock.method(editorApi.project, 'getProjectLoadStatus', async () => options.status ? options.status(path) : ready(path));
  t.mock.method(editorApi.project, 'openProject', async target => {
    calls.push(['open', target]); await options.open?.(target); path = target; return { ok: true, path };
  });
  t.mock.method(editorApi.project, 'createWorldProject', async () => {
    calls.push(['create']); await options.create?.(); return { path: 'D:/created' };
  });
  worldModeService.invalidate();
  t.after(() => { cancelPendingProjectOpen(); worldModeService.invalidate(); Object.assign(globalThis, previous); });
  return { calls, stored, get path() { return path; } };
}

test('identical opens deduplicate and scene readiness precedes mode publication', async t => {
  const gate = deferred();
  const f = fixture(t, { status: path => path === 'D:/child' ? gate.promise : ready(path) });
  const first = projectLauncherService.openProject('D:/child');
  assert.equal(projectLauncherService.openProject('D:/child'), first);
  await turn(); assert.deepEqual(f.calls, [['open', 'D:/child']]);
  assert.notEqual(worldModeService.state.status, 'ready'); assert.equal(worldLoadingState.busy, true);
  gate.resolve(ready('D:/child')); await first;
  assert.equal(worldModeService.state.status, 'ready'); assert.equal(worldLoadingState.busy, false);
});

test('native open timeout blocks both creation and reopening until the late response, without publishing it', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const gate = deferred(); const f = fixture(t, { open: () => gate.promise });
  const first = projectLauncherService.openProject('D:/child');
  const outcome = assert.rejects(first, { code: 'WORLD_LOAD_TIMEOUT' });
  await turn(); t.mock.timers.tick(30_001); await outcome;
  assert.equal(hasPendingLoadTimeout(), true); assert.equal(worldLoadingState.blocked, true);
  await assert.rejects(projectLauncherService.openProject('D:/child'), { code: 'WORLD_LOAD_PENDING' });
  await assert.rejects(projectLauncherService.createWorldProject({ mode: 'story' }), { code: 'WORLD_LOAD_PENDING' });
  assert.deepEqual(f.calls, [['open', 'D:/child']]);
  gate.resolve(); await turn();
  assert.equal(hasPendingLoadTimeout(), false); assert.equal(worldLoadingState.blocked, false);
  assert.notEqual(worldModeService.state.status, 'ready'); assert.equal(f.stored.length, 0);
  await projectLauncherService.openProject('D:/child');
  assert.equal(worldModeService.state.status, 'ready'); assert.equal(f.calls.length, 2);
});

test('an already queued different selection cannot replace a timed-out native scene still running', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const gate = deferred(); const f = fixture(t, { open: target => target === 'D:/first' ? gate.promise : undefined });
  const first = projectLauncherService.openProject('D:/first'); await turn();
  const second = projectLauncherService.openProject('D:/second');
  t.mock.timers.tick(30_001); await turn();
  assert.deepEqual(f.calls, [['open', 'D:/first']]); assert.equal(hasPendingLoadTimeout(), true);
  gate.resolve(); await first; await second;
  assert.deepEqual(f.calls, [['open', 'D:/first'], ['open', 'D:/second']]);
  assert.equal(worldModeService.state.projectPath, 'D:/second');
});

test('creation clicks share the same queue operation and timeout cannot create a second world', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const gate = deferred(); const f = fixture(t, { create: () => gate.promise });
  const first = projectLauncherService.createWorldProject({ mode: 'story' });
  assert.equal(projectLauncherService.createWorldProject({ mode: 'story' }), first);
  const outcome = assert.rejects(first, { code: 'WORLD_LOAD_TIMEOUT' });
  await turn(); t.mock.timers.tick(30_001); await outcome;
  await assert.rejects(projectLauncherService.createWorldProject({ mode: 'story' }), { code: 'WORLD_LOAD_PENDING' });
  gate.resolve(); await turn(); assert.deepEqual(f.calls, [['create']]);
  assert.equal(hasPendingLoadTimeout(), false);
});

test('unknown source load status fails closed without native replacement', async t => {
  const f = fixture(t, { status: () => ({ status: 'error', message: 'status unavailable' }) });
  await assert.rejects(projectLauncherService.openProject('D:/child'), /status unavailable/);
  assert.deepEqual(f.calls, []);
});

test('canceling an open permits a fresh selection of the same world after old work drains', async t => {
  const gate = deferred();
  const f = fixture(t, { open: () => gate.promise });
  const first = projectLauncherService.openProject('D:/child');
  await turn();
  cancelPendingProjectOpen();
  const second = projectLauncherService.openProject('D:/child');
  await turn();
  const callsBeforeRelease = [...f.calls];
  gate.resolve();
  const [canceled, reopened] = await Promise.all([first, second]);
  assert.deepEqual(callsBeforeRelease, [['open', 'D:/child']]);
  assert.equal(canceled.status, 'superseded');
  assert.equal(reopened.ok, true);
  assert.deepEqual(f.calls, [['open', 'D:/child'], ['open', 'D:/child']]);
  assert.equal(worldModeService.state.status, 'ready');
});

test('shared trace IDs do not let an earlier load hide the current loading phase', async t => {
  fixture(t);
  const previous = beginWorldLoad();
  const current = beginWorldLoad({ operationId: previous.id });
  const gate = deferred();
  const pending = current.phase('Initialize player', () => gate.promise);
  await turn();
  previous.finish();
  const busy = worldLoadingState.busy, phase = worldLoadingState.phase;
  gate.resolve();
  await pending;
  current.finish();
  assert.equal(busy, true);
  assert.equal(phase, 'Initialize player');
  assert.equal(worldLoadingState.busy, false);
});
