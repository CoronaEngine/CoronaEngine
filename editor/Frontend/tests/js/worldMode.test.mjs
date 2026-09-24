import { setImmediate } from 'node:timers';
import assert from 'node:assert/strict';
import test from 'node:test';
import { createWorldModeController, createWorldModeRouteGuard, normalizeWorldMode, normalizeProjectPath, unwrapProjectInfo, worldModeService } from '../../src/services/worldModeService.js';
import { editorApi, Bridge } from '../../src/api/editorApi.js';
import { appService } from '../../src/services/appService.js';
import { projectLauncherService, stopWorldRuntimeBeforeOpen } from '../../src/services/projectLauncherService.js';

const deferred = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; }); return { promise, resolve, reject }; };
const info = (path, mode) => ({ data: { project_path: path, mode } });

test('only an explicit story marker selects story; legacy and multiplayer stay creative', () => {
  assert.equal(normalizeWorldMode('story'), 'story');
  for (const mode of [undefined, null, '', 'creative', '3d', 'multiplayer', 'Story']) assert.equal(normalizeWorldMode(mode), 'creative');
  assert.equal(normalizeProjectPath('D:\\Worlds\\Demo\\'), 'd:/worlds/demo');
  assert.equal(unwrapProjectInfo({ data: info('demo', 'story') }).mode, 'story');
  for (const invalid of [{}, { data: {} }, { project_path: '  ' }]) assert.throws(() => unwrapProjectInfo(invalid));
});

test('mode resolution is fail closed and does not mount an editor while pending', async () => {
  const request = deferred(), states = [];
  const controller = createWorldModeController({ readProjectInfo: () => request.promise, publish: (state) => states.push(state) });
  const result = controller.resolve('D:/story');
  assert.equal(controller.state.status, 'loading');
  assert.equal(controller.state.mode, null);
  request.reject(new Error('offline'));
  await assert.rejects(result, /offline/);
  assert.equal(controller.state.status, 'error');
  assert.ok(states.every((state) => state.mode === null));
});

test('deduplicates reads and rejects mismatched authoritative project paths', async () => {
  let calls = 0;
  const request = deferred();
  const controller = createWorldModeController({ readProjectInfo: () => { calls++; return request.promise; } });
  const first = controller.resolve('D:/story'), second = controller.resolve('d:\\story\\');
  request.resolve(info('D:/story', 'story'));
  assert.equal((await first).mode, 'story');
  assert.equal((await second).mode, 'story');
  assert.equal(calls, 1);
  await assert.rejects(controller.resolve('D:/different'), /改变/);
  assert.equal(controller.state.mode, null);
});

test('stale success and failure cannot overwrite the most recent world', async () => {
  for (const fail of [false, true]) {
    const requests = [deferred(), deferred()]; let n = 0;
    const controller = createWorldModeController({ readProjectInfo: () => requests[n++].promise });
    const old = controller.resolve('old');
    const current = controller.resolve('current');
    requests[1].resolve(info('current', 'story'));
    await current;
    if (fail) requests[0].reject(new Error('old failure')); else requests[0].resolve(info('old', 'creative'));
    assert.equal(await old, null);
    assert.equal(controller.state.mode, 'story');
    assert.equal(controller.state.projectPath, 'current');
  }
});

test('open ownership token survives mode resolution but not a newer open', async () => {
  const controller = createWorldModeController({ readProjectInfo: async () => info('world', 'story') });
  const old = controller.beginOpen('old');
  const token = controller.beginOpen('world');
  await controller.resolve('world', { force: true });
  assert.equal(controller.finishOpen(old), false);
  assert.equal(controller.opening, true);
  assert.equal(controller.finishOpen(token), true);
  assert.equal(controller.opening, false);
});

test('route guard permits launcher and world but closes story standalone editor routes', async () => {
  let mode = 'story', closed = 0, reads = 0;
  const controller = createWorldModeController({ readProjectInfo: async () => { reads++; return info('world', mode); } });
  const guard = createWorldModeRouteGuard({ controller, closePanel: async () => { closed++; }, notify: assert.fail });
  assert.equal(await guard({ path: '/NewGame' }), true);
  assert.equal(reads, 0);
  assert.equal(await guard({ path: '/' }), true);
  for (const path of ['/SetUp', '/NodeGraph', '/CameraView', '/CabbageChat']) {
    assert.equal(await guard({ path }), '/');
    assert.equal(await guard({ path, query: { standalone: '1' } }), false);
  }
  assert.equal(closed, 4);
  mode = 'creative'; controller.invalidate();
  assert.equal(await guard({ path: '/NodeGraph', query: { standalone: '1' } }), true);
  const token = controller.beginOpen('next');
  assert.equal(await guard({ path: '/' }), false);
  controller.finishOpen(token);
});

test('failed mode reads return to launcher and never fall back to creative', async () => {
  const errors = [];
  const controller = createWorldModeController({ readProjectInfo: async () => { throw new Error('metadata unavailable'); } });
  const guard = createWorldModeRouteGuard({ controller, closePanel: async () => {}, notify: (e) => errors.push(e) });
  assert.equal(await guard({ path: '/' }), '/StartScreen');
  assert.deepEqual(errors, ['metadata unavailable']);
  assert.equal(controller.state.mode, null);
});

test('late native panel completion is closed even after creative -> story -> creative', async (t) => {
  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => info('creative', 'creative'));
  await worldModeService.resolve('creative', { force: true });
  const request = deferred(), commands = [];
  t.mock.method(Bridge, 'callDockCommand', async (command) => {
    commands.push(command);
    return command.cmd === 'createPanelTab' ? request.promise : {};
  });
  const pending = appService.createPanelTab('Object', '/Object', 400, 600, 'right');
  worldModeService.invalidate('story');
  await worldModeService.resolve('creative', { force: true });
  request.resolve({ tab_id: 7 });
  await assert.rejects(pending, /世界已切换/);
  assert.equal(commands[1].cmd, 'closePanelTab');
  assert.equal(commands[1].tabId, 7);
  worldModeService.invalidate();
  await assert.rejects(appService.createDetachedPanel({ panelId: 'Object' }), /不允许/);
});

test('preview restoration finishes before script shutdown and times out safely', async () => {
  const calls = [];
  await stopWorldRuntimeBeforeOpen({
    stopGamePreview: async () => ({ status: 'stopping' }),
    getGamePreviewStatus: async () => { calls.push('restore'); return { status: 'stopped' }; },
    stopScriptExecution: async (restore) => { calls.push(`script:${restore}`); return { status: 'stopped' }; },
  }, { wait: async () => {} });
  assert.deepEqual(calls, ['restore', 'script:true']);
  await assert.rejects(stopWorldRuntimeBeforeOpen({
    stopGamePreview: async () => ({ status: 'stopping' }),
    getGamePreviewStatus: async () => ({ stop_pending: true }),
    stopScriptExecution: assert.fail,
  }, { wait: async () => {}, attempts: 2 }), /取消切换/);
});

test('project opening is serialized and stores only verified authoritative worlds', async (t) => {
  const first = deferred(), paths = [], saved = [];
  let activePath = 'first';
  t.mock.method(editorApi.project, 'openProject', async (path) => {
    paths.push(path); if (path === 'first') await first.promise;
    activePath = path; return { data: { ok: true, path } };
  });
  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => info(activePath, 'story'));
  const previousWindow = globalThis.window, previousEvent = globalThis.CustomEvent;
  globalThis.window = { localStorage: { setItem: (...args) => saved.push(args) }, dispatchEvent() {} };
  globalThis.CustomEvent = class { constructor(type, options) { Object.assign(this, { type }, options); } };
  t.after(() => { globalThis.window = previousWindow; globalThis.CustomEvent = previousEvent; });
  worldModeService.invalidate();
  const a = projectLauncherService.openProject('first');
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(paths, ['first']);
  const skipped = projectLauncherService.openProject('middle');
  const b = projectLauncherService.openProject('second');
  first.resolve();
  assert.equal((await a).status, 'superseded');
  assert.equal((await skipped).status, 'superseded');
  await b;
  assert.deepEqual(paths, ['first', 'second']);
  assert.equal(worldModeService.state.projectPath, 'second');
  assert.equal(worldModeService.opening, false);
  assert.deepEqual(saved.filter(([key]) => key === 'corona.activeProjectPath').map(([,path]) => path), ['second']);
});


test('world cleanup accepts stopped scripts without a snapshot or restoration', async () => {
  for (const wrap of [result => result, result => ({ data: result })]) {
    const calls = [];
    await stopWorldRuntimeBeforeOpen({
      stopGamePreview: async () => { calls.push('preview'); return wrap({ status: 'stopped' }); },
      getGamePreviewStatus: assert.fail,
      stopScriptExecution: async (restore) => {
        calls.push(`script:${restore}`);
        return wrap({ status: 'stopped', restored: false, snapshotCaptured: false, restoreStatus: 'idle' });
      },
    });
    assert.deepEqual(calls, ['preview', 'script:true']);
  }
});

test('world cleanup still rejects real script restoration and stop failures', async (t) => {
  const failures = [
    { status: 'stopped', restored: false, restoreError: 'scene restore failed' },
    { status: 'error', restored: false, pendingThreads: ['pending-script'] },
    { status: 'stopping' },
    { status: 'stopped', threadAlive: true },
  ];
  for (const failure of failures) {
    await t.test(JSON.stringify(failure), async () => {
      for (const wrap of [result => result, result => ({ data: result })]) {
        await assert.rejects(stopWorldRuntimeBeforeOpen({
          stopGamePreview: async () => wrap({ status: 'stopped' }),
          getGamePreviewStatus: assert.fail,
          stopScriptExecution: async () => wrap(failure),
        }), /旧世界脚本尚未停止或恢复失败，已取消切换世界/);
      }
    });
  }
});


test('creative to story opens only after safe cleanup and can retry genuine failures', async (t) => {
  const calls = [];
  const idle = { status: 'stopped', restored: false, snapshotCaptured: false, restoreStatus: 'idle' };
  let stopResult = idle;
  let activePath = 'creative';
  let activeMode = 'creative';
  const previousWindow = globalThis.window, previousEvent = globalThis.CustomEvent;
  globalThis.window = { localStorage: { setItem() {} }, dispatchEvent() {} };
  globalThis.CustomEvent = class { constructor(type, options) { Object.assign(this, { type }, options); } };
  t.after(() => {
    globalThis.window = previousWindow;
    globalThis.CustomEvent = previousEvent;
    worldModeService.invalidate();
  });
  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => info(activePath, activeMode));
  t.mock.method(editorApi.scratch, 'stopGamePreview', async () => { calls.push('preview'); return { status: 'stopped' }; });
  t.mock.method(editorApi.scratch, 'stopScriptExecution', async restore => { calls.push(`script:${restore}`); return { data: stopResult }; });
  t.mock.method(editorApi.project, 'openProject', async path => {
    calls.push(`open:${path}`);
    activePath = path;
    activeMode = 'story';
    return { data: { ok: true, path } };
  });

  for (const initialResult of [
    idle,
    { status: 'stopped', restored: false, restoreError: 'scene restore failed' },
    { status: 'error', restored: false, pendingThreads: ['pending-script'] },
  ]) {
    activePath = 'creative';
    activeMode = 'creative';
    await worldModeService.resolve('creative', { force: true });
    calls.length = 0;
    stopResult = initialResult;
    if (initialResult !== idle) {
      await assert.rejects(projectLauncherService.openProject('story'), /已取消切换世界/);
      assert.deepEqual(calls, ['preview', 'script:true']);
      assert.equal(activePath, 'creative');
      assert.equal(worldModeService.opening, false);
      calls.length = 0;
      stopResult = idle;
    }
    assert.equal((await projectLauncherService.openProject('story')).data.ok, true);
    assert.deepEqual(calls, ['preview', 'script:true', 'open:story']);
    assert.equal(worldModeService.state.mode, 'story');
    assert.equal(worldModeService.state.projectPath, 'story');
    assert.equal(worldModeService.opening, false);
  }
});


test('script cleanup reports concrete failures without treating restored=false as an error', async (t) => {
  const failures = [
    [{ status: 'stopped', restoreError: 'scene restore failed' }, 'scene restore failed'],
    [{ status: 'error', message: 'cooperative stop timed out' }, 'cooperative stop timed out'],
    [{ status: 'error', error: 'runtime unavailable' }, 'runtime unavailable'],
    [{ status: 'stopped', pendingThreads: ['child-worker'] }, 'child-worker'],
    [{ status: 'running' }, 'running'],
    [{ status: 'stopped', snapshotCaptured: true }, '快照'],
    [{ status: 'stopped', restoreStatus: 'error' }, 'error'],
    [{ status: 'stopped', restoreStatus: 'restoring' }, 'restoring'],
    [{ status: 'stopped', threadAlive: true }, '线程'],
  ];
  for (const [result, detail] of failures) {
    await t.test(JSON.stringify(result), async () => {
      for (const wrap of [value => value, value => ({ data: value })]) {
        await assert.rejects(stopWorldRuntimeBeforeOpen({
          stopGamePreview: async () => wrap({ status: 'stopped' }),
          getGamePreviewStatus: assert.fail,
          stopScriptExecution: async () => wrap(result),
        }), error => {
          assert.match(error.message, /旧世界脚本尚未停止或恢复失败，已取消切换世界/);
          assert.ok(error.message.includes(detail), error.message);
          return true;
        });
      }
    });
  }
});

test('preview cleanup preserves restoration diagnostics and never stops scripts after failure', async () => {
  await assert.rejects(stopWorldRuntimeBeforeOpen({
    stopGamePreview: async () => ({ data: { status: 'stopped', restore_error: 'preview scene restore failed' } }),
    getGamePreviewStatus: assert.fail,
    stopScriptExecution: assert.fail,
  }), /旧世界预览.*preview scene restore failed/);
});
