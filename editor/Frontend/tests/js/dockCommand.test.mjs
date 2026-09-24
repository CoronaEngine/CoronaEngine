import assert from 'node:assert/strict';
import test from 'node:test';
import { setImmediate } from 'node:timers/promises';
import { watch, nextTick } from 'vue';
import { Bridge, editorApi } from '../../src/api/editorApi.js';
import { appService } from '../../src/services/appService.js';
import { projectLauncherService } from '../../src/services/projectLauncherService.js';
import { worldModeService, worldModeState } from '../../src/services/worldModeService.js';

function nativeSurface(t) {
  const sent = [];
  const surface = { coronaBridge: { dockCommand: payload => sent.push(JSON.parse(payload)) },
    localStorage: { setItem() {} }, dispatchEvent() {} };
  const previous = globalThis.window;
  globalThis.window = surface;
  t.after(() => { globalThis.window = previous; });
  return { sent, surface, reply: (index, error = null) => surface.__dockCallback(sent[index].requestId, error, { index }) };
}
for (const order of [[0, 1, 2, 3], [3, 2, 1, 0], [1, 3, 0, 2]]) {
  test(`Dock dispatcher independently settles ${order}`, async (t) => {
    const { surface, reply } = nativeSurface(t);
    const requests = Array.from({ length: 4 }, (_, i) => Bridge.callDockCommand({ cmd: `command${i}` }));
    const callback = surface.__dockCallback;
    for (const i of order) { reply(i); reply(i); }
    surface.__dockCallback('unknown', null, {});
    assert.deepEqual(await Promise.all(requests), [0, 1, 2, 3].map(index => ({ index })));
    assert.equal(surface.__dockCallback, callback);
  });
}
test('Dock native errors and synchronous send failures isolate requests', async (t) => {
  const { sent, surface, reply } = nativeSurface(t);
  const failed = Bridge.callDockCommand({ cmd: 'nativeError' });
  const rejection = assert.rejects(failed, /nativeError.*dock_.*denied/);
  surface.coronaBridge.dockCommand = () => { throw new Error('send failed'); };
  await assert.rejects(Bridge.callDockCommand({ cmd: 'sendError' }), /sendError.*send failed/);
  surface.coronaBridge.dockCommand = payload => sent.push(JSON.parse(payload));
  const good = Bridge.callDockCommand({ cmd: 'good' });
  reply(0, { message: 'denied' }); reply(1);
  await rejection;
  assert.deepEqual(await good, { index: 1 });
});
test('Dock timeout does not retry and late replies cannot affect other requests', async (t) => {
  const { sent, reply } = nativeSurface(t);
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const request = Bridge.callDockCommand({ cmd: 'slow' });
  const rejection = assert.rejects(request, /slow.*dock_.*30 seconds/);
  t.mock.timers.tick(29_999);
  const other = Bridge.callDockCommand({ cmd: 'other' });
  t.mock.timers.tick(1); await rejection;
  reply(0); reply(1);
  assert.deepEqual(await other, { index: 1 });
  assert.equal(sent.length, 2);
});
test('real Vue watcher and launcher concurrent cleanup opens native project once', async (t) => {
  const { sent, reply } = nativeSurface(t);
  let nativeOpens = 0;
  t.mock.method(editorApi.project, 'openProject', async path => { nativeOpens++; return { data: { ok: true, path } }; });
  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => ({ project_path: 'story', mode: 'story' }));
  worldModeService.invalidate();
  const watched = [];
  const stop = watch(() => worldModeState.revision, () => {
    if (worldModeState.status === 'loading') watched.push(appService.setEditorUiEnabled(false));
  });
  t.after(stop);
  const opened = projectLauncherService.openProject('story');
  await nextTick(); await setImmediate();
  assert.equal(sent.length, 2);
  reply(0); reply(1); await setImmediate();
  for (let i = 2; i < sent.length; i++) reply(i);
  await opened; await Promise.all(watched);
  assert.equal(nativeOpens, 1);
  assert.equal(worldModeState.mode, 'story');
  assert.equal(worldModeService.opening, false);
});
test('failed retirement blocks opening but leaves queue available for retry', async (t) => {
  const { sent, reply } = nativeSurface(t);
  let opens = 0;
  t.mock.method(editorApi.project, 'openProject', async path => { opens++; return { ok: true, path }; });
  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => ({ project_path: 'story', mode: 'story' }));
  worldModeService.invalidate();
  const failed = projectLauncherService.openProject('story');
  const rejected = assert.rejects(failed, /retirement failed/);
  await setImmediate(); reply(0, 'retirement failed'); await rejected;
  assert.equal(opens, 0); assert.equal(worldModeService.opening, false);
  const retry = projectLauncherService.openProject('story');
  await setImmediate(); reply(sent.length - 1); await retry;
  assert.equal(opens, 1); assert.equal(worldModeService.opening, false);
});
