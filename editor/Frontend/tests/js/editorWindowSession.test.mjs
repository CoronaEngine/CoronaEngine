import assert from 'node:assert/strict';
import { Buffer } from 'node:buffer';
import test from 'node:test';
import { setImmediate } from 'node:timers/promises';
import { createEditorWindowSession } from '../../src/services/editorWindowSession.js';
import { portableWorldMode, readWorldProjectInfo } from '../../src/services/worldModeService.js';
import { memoryStorage, deferred } from './windowSessionFixtures.mjs';

function harness(overrides = {}) {
  const storage = memoryStorage();
  const world = { status: 'ready', mode: 'creative', revision: 1 };
  const commands = [];
  let tabId = 0;
  const send = async command => {
    commands.push(command);
    return command.cmd.startsWith('create') ? { tab_id: ++tabId } : {};
  };
  const options = { storage, getWorld: () => world, send, getQuery: () => new URLSearchParams(),
    wait: () => setImmediate(), ...overrides };
  const main = createEditorWindowSession(options);
  const child = command => createEditorWindowSession({ ...options,
    getQuery: () => new URLSearchParams(command.routePath.split('?')[1] + '&standalone=1'),
  });
  return { storage, world, commands, main, child, send, options };
}
const panel = { cmd: 'createPanelTab', panelId: 'Object', routePath: '/Object' };
const camera = { cmd: 'createCameraView', sceneId: 'scene.ini', cameraId: 'camera1', cameraHandle: 1,
  routePath: '/CameraView?scene=scene.ini&camera=camera1' };

test('main policy is Web-owned; standalone windows inherit immutable session tokens', async () => {
  const h = harness();
  await h.main.prepare(true);
  h.commands.length = 0;
  const result = await h.main.open(panel);
  const child = h.child(h.commands[0]);
  assert.equal(child.policy().enabled, true);
  await assert.rejects(child.prepare(false), /主窗口/);
  await child.broadcast('panel-redock-request', { panelId: 'Object' });
  const payload = h.commands.at(-1).payload;
  assert.equal(payload.tabId, result.tab_id);
  assert.equal(h.main.accepts(payload), true);
  assert.equal(h.main.accepts({ panelId: 'Object' }), false);
  h.world.revision++;
  await h.main.prepare(false);
  assert.equal(child.policy().enabled, false);
  assert.ok(h.commands.some(command => command.cmd === 'closePanelTab' && command.tabId === result.tab_id));
  await h.main.prepare(true);
  assert.equal(h.main.accepts(payload), false);
  await assert.rejects(child.open(panel), /不允许/);
  await assert.rejects(child.broadcast('panel-redock-request', {}), /不允许/);
  assert.ok(h.commands.every(command => !['setEditorUiEnabled', 'getEditorUiPolicy'].includes(command.cmd)));
});

test('rapid policy preparation shares retirement but does not lose a following enable', async () => {
  const barrier = deferred();
  let fences = 0;
  const h = harness({ send: async () => { if (++fences === 1) await barrier.promise; return {}; } });
  const first = h.main.prepare(false);
  assert.equal(h.main.prepare(false), first);
  const enabled = h.main.prepare(true);
  barrier.resolve();
  await first; await enabled;
  assert.equal(h.main.policy().enabled, true);
});

test('retirement waits for late creation, disposes old tab, and fences before a new world', async () => {
  const pending = deferred();
  const h = harness();
  const send = h.options.send;
  h.options.send = async command => command.cmd === 'createDetachedPanel' ? pending.promise : send(command);
  const main = createEditorWindowSession(h.options);
  await main.prepare(true);
  const opening = main.open({ ...panel, cmd: 'createDetachedPanel' });
  const rejected = assert.rejects(opening, /世界已切换/);
  h.world.revision++;
  let retired = false;
  const retirement = main.prepare(false).then(() => { retired = true; });
  await setImmediate();
  assert.equal(retired, false);
  pending.resolve({ tab_id: 17 });
  await rejected; await retirement;
  const closeIndex = h.commands.findIndex(command => command.cmd === 'closePanelTab' && command.tabId === 17);
  assert.ok(closeIndex >= 0);
  assert.equal(h.commands.at(-1).cmd, 'suspendCameraViews');
  assert.equal(h.commands.at(-1).sceneId, '');
  assert.ok(h.commands.length > closeIndex + 1);
  assert.equal(h.commands.filter(command => command.cmd === 'closePanelTab').length, 1);
  assert.equal(h.storage.length, 1); // Only the policy remains, no resurrected window record.
});

test('native queued camera creation is fenced, and retirement never writes camera.open', async () => {
  const h = harness();
  h.options.send = async command => {
    h.commands.push(command);
    return command.cmd === 'createCameraView' ? { queued: true, existing: false } : {};
  };
  const main = createEditorWindowSession(h.options);
  await main.prepare(true);
  h.commands.length = 0;
  await main.open(camera);
  const child = h.child(h.commands[0]);
  assert.deepEqual(h.commands.map(command => command.cmd), ['createCameraView', 'suspendCameraViews']);
  h.world.revision++;
  const retired = main.prepare(false);
  await child.closeThis(''); // Retired camera pages leave suspension to main.
  await retired;
  assert.ok(h.commands.some(command => command.cmd === 'suspendCameraViews' && command.sceneId === 'scene.ini'));
  assert.ok(h.commands.every(command => !['closeCameraView', 'closePanelTab', 'closeThisTab'].includes(command.cmd)));
});

test('camera opened by a secondary window is retired by the main owner', async () => {
  const h = harness();
  await h.main.prepare(true);
  await h.main.open(panel);
  const child = h.child(h.commands.find(command => command.cmd === 'createPanelTab'));
  await child.open(camera);
  h.world.revision++;
  await h.main.prepare(false);
  assert.ok(h.commands.some(command => command.cmd === 'suspendCameraViews' && command.sceneId === 'scene.ini'));
});

test('close notifications carry session and tab id, never ambiguous native panel ids', async () => {
  const h = harness();
  await h.main.prepare(true);
  const { tab_id } = await h.main.open(panel);
  await h.main.closeTab(tab_id, panel.panelId);
  const close = h.commands.find(command => command.cmd === 'closePanelTab');
  assert.equal(close.panelId, '');
  const event = h.commands.at(-1);
  assert.equal(event.event, 'panel-closed');
  assert.equal(event.payload.tabId, tab_id);
  assert.equal(h.main.accepts(event.payload), true);
  h.world.revision++;
  await h.main.prepare(true);
  assert.equal(h.main.accepts(event.payload), false);
});

test('timed-out native creates block retries until the late callback retires them', async () => {
  let onLate;
  const h = harness({ timeoutMs: 0 });
  const send = h.options.send;
  h.options.send = async (command, options) => {
    if (command.cmd !== panel.cmd) return send(command);
    onLate = options.onLateResult;
    throw Object.assign(new Error('native timeout'), { code: 'DOCK_TIMEOUT' });
  };
  const main = createEditorWindowSession(h.options);
  await main.prepare(true);
  await assert.rejects(main.open(panel), /native timeout/);
  h.world.revision++;
  await assert.rejects(main.prepare(false), /取消切换/);
  await assert.rejects(main.prepare(false), /取消切换/);
  await onLate(null, { tab_id: 19 });
  await main.prepare(false);
  assert.ok(h.commands.some(command => command.cmd === 'closePanelTab' && command.tabId === 19));
});

test('failed close remains registered for retry', async () => {
  let fail = true;
  const h = harness();
  const send = h.options.send;
  h.options.send = async command => {
    if (command.cmd === 'closePanelTab' && fail) throw new Error('close failed');
    return send(command);
  };
  const main = createEditorWindowSession(h.options);
  await main.prepare(true);
  await main.open(panel);
  h.world.revision++;
  await assert.rejects(main.prepare(false), /close failed/);
  fail = false;
  await main.prepare(false);
  assert.equal(h.commands.filter(command => command.cmd === 'closePanelTab').length, 1);
});

test('story and unreadable storage fail closed before sending window commands', async () => {
  const h = harness();
  await h.main.prepare(true);
  h.world.mode = 'story';
  await assert.rejects(h.main.open(panel), /不允许/);
  h.world.mode = 'creative';
  h.storage.getItem = () => { throw new Error('storage unavailable'); };
  await assert.rejects(h.main.open(panel), /storage unavailable/);
  assert.equal(h.commands.filter(command => command.cmd === panel.cmd).length, 0);
});

const format = '[format]\ntype=corona_scene_folder\nversion=1\n';
test('portable defaults use actual markers instead of the native story fallback', async () => {
  for (const mode of [undefined, '', 'creative', '3d', 'multiplayer', 'Story']) {
    assert.equal(portableWorldMode(format + (mode === undefined ? '' : `[world]\ntype=${mode}`)), 'creative');
  }
  assert.equal(portableWorldMode('\uFEFF' + format + '[WORLD]\n TYPE = story\r\n'), 'story');
  assert.equal(portableWorldMode(format + '[world]\ntype=story\ntype=creative'), 'creative');
  assert.equal(portableWorldMode('[world]\ntype=story', 'story'), 'story'); // legacy
  const info = { project_path: 'D:\\中文世界\\', mode: 'story', entrance_scene: 'scene.ini' };
  const reads = [];
  const api = { projectSettings: { getActiveProjectInfo: async () => ({ data: info }) }, ai: {
    readLocalFileAsBase64: async path => {
      reads.push(path);
      return { data: `data:application/octet-stream;base64,${Buffer.from(format).toString('base64')}` };
    },
  } };
  const resolved = await readWorldProjectInfo(api);
  assert.equal(resolved.mode, 'creative');
  assert.equal(resolved.project_path, info.project_path);
  assert.deepEqual(reads, ['D:\\中文世界/scene.ini']);
  api.ai.readLocalFileAsBase64 = async () => '';
  await assert.rejects(readWorldProjectInfo(api), /无法读取/);
  info.mode = '3d';
  assert.equal((await readWorldProjectInfo(api)).mode, '3d');
});

test('a same-revision disable supersedes an enable awaiting retirement', async () => {
  const barrier = deferred();
  let calls = 0;
  const h = harness({ send: async () => { if (++calls === 1) await barrier.promise; return {}; } });
  const enabled = h.main.prepare(true);
  await setImmediate();
  const disabled = h.main.prepare(false);
  barrier.resolve();
  await enabled;
  await disabled;
  assert.equal(h.main.policy().enabled, false);
  await h.main.prepare(true);
  assert.equal(h.main.policy().enabled, true);
});

test('retired children stay alive until their queued operations settle', async () => {
  const h = harness();
  await h.main.prepare(true);
  await h.main.open(panel);
  const child = h.child(h.commands.find(command => command.cmd === panel.cmd));
  h.world.revision++;
  const retirement = h.main.prepare(false);
  await child.closeThis(panel.panelId);
  await retirement;
  assert.ok(h.commands.every(command => command.cmd !== 'closeThisTab'));
  assert.ok(h.commands.some(command => command.cmd === 'closePanelTab'));
});

test('user close broadcasts identity before destroying its callback context', async () => {
  const h = harness();
  await h.main.prepare(true);
  await h.main.open(panel);
  const child = h.child(h.commands.find(command => command.cmd === panel.cmd));
  h.commands.length = 0;
  await child.closeThis(panel.panelId);
  assert.deepEqual(h.commands.map(command => command.cmd), ['broadcast', 'closeThisTab']);
  assert.equal(h.commands[0].payload.panelId, panel.panelId);
  assert.equal(h.main.accepts(h.commands[0].payload), true);
  assert.equal(h.storage.length, 1);
});

for (const initiatedByMain of [false, true]) {
  test(`closing a panel drains its outstanding requests (main=${initiatedByMain})`, async () => {
    const pending = deferred();
    const h = harness();
    const send = h.options.send;
    h.options.send = async command => {
      if (command.cmd === 'togglePanelWindowMode') {
        h.commands.push(command);
        return pending.promise;
      }
      return send(command);
    };
    const main = createEditorWindowSession(h.options);
    await main.prepare(true);
    const { tab_id } = await main.open(panel);
    const child = h.child(h.commands.find(command => command.cmd === panel.cmd));
    const toggled = child.open({ cmd: 'togglePanelWindowMode' });
    const rejected = assert.rejects(toggled, /世界已切换/);
    const closed = initiatedByMain ? main.closeTab(tab_id, panel.panelId) : child.closeThis(panel.panelId);
    await setImmediate();
    assert.ok(h.commands.every(command => !['closePanelTab', 'closeThisTab'].includes(command.cmd)));
    await assert.rejects(child.open(panel), /不允许/);
    pending.resolve({ queued: true });
    await rejected;
    await closed;
    assert.equal(h.storage.length, 1);
    assert.ok(h.commands.some(command => command.cmd === (initiatedByMain ? 'closePanelTab' : 'closeThisTab')));
  });
}

test('main-page reload retires persisted windows before issuing a fresh generation', async () => {
  const h = harness({ owner: 'main-surface' });
  await h.main.prepare(true);
  const { tab_id } = await h.main.open(panel);
  const child = h.child(h.commands.find(command => command.cmd === panel.cmd));
  const previous = h.main.policy().generation;
  const reloaded = createEditorWindowSession(h.options);
  await reloaded.prepare(true);
  assert.notEqual(reloaded.policy().generation, previous);
  assert.equal(child.policy().enabled, false);
  assert.ok(h.commands.some(command => command.cmd === 'closePanelTab' && command.tabId === tab_id));
  assert.equal(h.storage.length, 1);
});

test('secondary identity survives navigation and closed pages cannot rejoin a session', async () => {
  const h = harness();
  await h.main.prepare(true);
  await h.main.open(panel);
  let query = new URLSearchParams(h.commands.find(command => command.cmd === panel.cmd).routePath.split('?')[1] + '&standalone=1');
  const child = createEditorWindowSession({ ...h.options, getQuery: () => query });
  query = new URLSearchParams(); // A route navigation cannot turn a panel into main.
  await assert.rejects(child.prepare(true), /主窗口/);
  assert.equal(child.policy().enabled, true);
  await child.closeThis(panel.panelId);
  assert.equal(child.policy().enabled, false);
  await assert.rejects(child.open(panel), /不允许/);
});
