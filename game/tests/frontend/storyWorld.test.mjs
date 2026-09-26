import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import test from 'node:test';
import { createPlayerController } from '../../frontend/playerController.mjs';
import { createPlayerSave } from '../../frontend/playerSave.mjs';
import { ensureStoryCharacters } from '../../frontend/storyActors.mjs';
import * as gameplayModule from '../../frontend/storyGameplay.mjs';
import { STORY_CHARACTERS } from '../../frontend/storyCharacters.mjs';
import { actorFixture } from './fixtures.mjs';
import { createStoryNavigationController } from '../../frontend/storyNavigation.mjs';
import { createStoryCameraController } from '../../../editor/Frontend/src/utils/viewportStoryCamera.js';
import { editorApi } from '../../../editor/Frontend/src/api/editorApi.js';
import * as launcher from '../../../editor/Frontend/src/services/projectLauncherService.js';
import * as worldMode from '../../../editor/Frontend/src/services/worldModeService.js';
import * as lifecycle from '../../../editor/Frontend/src/services/worldSessionLifecycle.js';
import lanchat from '../../../editor/Frontend/src/stores/lanchat.js';

const require = createRequire(new URL('../../../editor/Frontend/package.json', import.meta.url));
const vue = require('vue');
const { ref, proxyRefs } = vue;
const { parse, compileScript, compileTemplate, babelParse } = require('vue/compiler-sfc');
const { descriptor } = parse(fs.readFileSync(new URL('../../../editor/Frontend/src/views/layout/StoryWorld.vue', import.meta.url), 'utf8'));
const compiled = compileScript(descriptor, { id: 'story-navigation-test', genDefaultAs: 'StoryWorld' });
let setupSource = compiled.content;
for (const node of babelParse(compiled.content, { sourceType: 'module' }).program.body
  .filter(node => node.type === 'ImportDeclaration').reverse()) {
  const bindings = node.specifiers.map(specifier => `${JSON.stringify(specifier.imported.name)}: ${specifier.local.name}`).join(', ');
  setupSource = setupSource.slice(0, node.start) + `const { ${bindings} } = modules[${JSON.stringify(node.source.value)}];`
    + setupSource.slice(node.end);
}
const makeComponent = new Function('modules', `${setupSource}; return StoryWorld;`);
const template = compileTemplate({ source: descriptor.template.content, filename: 'StoryWorld.vue',
  id: 'story-ui-test', compilerOptions: { mode: 'function' } });
assert.deepEqual(template.errors, []);
const renderStory = new Function('Vue', template.code)(vue);
const styles = require('postcss').parse(descriptor.styles.map(style => style.content).join('\n'));
function findNode(node, predicate) {
  if (!node || typeof node !== 'object') return null;
  if (predicate(node)) return node;
  for (const child of Array.isArray(node.children) ? node.children : []) {
    const match = findNode(child, predicate);
    if (match) return match;
  }
  return null;
}
const turn = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; }); return { promise, resolve, reject }; };
const MAIN = 'D:/story', CHILD = `${MAIN}/.game/subworld`;
const event = (props = {}) => ({ code: 'KeyO', preventDefault() { this.defaultPrevented = true; }, stopPropagation() {}, ...props });
const pose = position => ({ name: 'main', position, forward: [0, 0, 1], world_up: [0, 1, 0], fov: 65 });

async function fixture(t, options = {}) {
  const globals = ['window', 'document', 'CustomEvent', 'ResizeObserver', 'requestAnimationFrame', 'cancelAnimationFrame'];
  const previous = Object.fromEntries(globals.map(key => [key, globalThis[key]]));
  const calls = [], alerts = [], frames = new Map(), pages = [];
  let frameTime = performance.now();
  let activePath = MAIN, route = '/', frameId = 0, page = null, handle = 10;
  const poses = new Map([[MAIN, pose([1, 2, 3])], [CHILD, pose([5, 6, 7])]]);
  const actors = new Map([MAIN, CHILD].map(path => [path, STORY_CHARACTERS.map(character => actorFixture(character))]));
  function surface() {
    const listeners = new Map();
    return {
      listeners,
      addEventListener(name, fn) { if (!listeners.has(name)) listeners.set(name, new Set()); listeners.get(name).add(fn); },
      removeEventListener(name, fn) { listeners.get(name)?.delete(fn); },
      dispatchEvent(event) { for (const fn of listeners.get(event.type) || []) fn(event); },
    };
  }
  const bridge = Object.fromEntries(['actorTransform', 'cameraMove', 'setCameraViewport', 'setViewportGizmoTarget', 'setViewportUiMode', 'setViewportSystemCursorHidden']
    .map(name => [name, (...args) => { calls.push([name, ...args]); return true; }]));
  globalThis.window = Object.assign(surface(), { coronaBridge: bridge, devicePixelRatio: 1,
    location: { href: 'file:///D:/engine/editor/Frontend/dist/index.html' },
    localStorage: { setItem() {} }, alert: message => alerts.push(message) });
  globalThis.document = Object.assign(surface(), { hidden: false, hasFocus: () => true });
  globalThis.CustomEvent = class { constructor(type, options) { Object.assign(this, { type }, options); } };
  globalThis.ResizeObserver = class { observe() {} disconnect() { calls.push(['disconnect']); } };
  globalThis.requestAnimationFrame = callback => { frames.set(++frameId, callback); return frameId; };
  globalThis.cancelAnimationFrame = id => frames.delete(id);

  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => ({ project_path: activePath, mode: 'story' }));
  t.mock.method(editorApi.main, 'onInit', async () => options.init ? options.init() : { scenes: [{ path: 'scene.ini' }], active_index: 0 });
  t.mock.method(editorApi.scene, 'setActorTransform', async (sceneId, guid, transform) => {
    const source = activePath;
    calls.push(['playerSave', source, sceneId, guid, structuredClone(transform)]);
    await options.save?.();
    assert.equal(activePath, source, 'a pending save must never cross native scene replacement');
    const actor = actors.get(source).find(item => item.actor_guid === guid);
    for (const key of ['position', 'rotation']) if (transform[key]) actor.geometry[key] = [...transform[key]];
    return { status: 'success', actor: structuredClone(actor) };
  });
  t.mock.method(editorApi.scene, 'getSnapshot', async () => ({ scene: 'scene.ini', active_camera_name: 'main',
    actors: structuredClone(actors.get(activePath)),
    cameras: [{ ...poses.get(activePath), handle: ++handle }] }));
  t.mock.method(editorApi.viewport, 'setCameraPose', async (sceneId, name, camera) => {
    calls.push(['pose', activePath, sceneId, name, camera]);
    await options.pose?.();
    poses.set(activePath, { name, ...structuredClone(camera) });
    return { data: { status: 'success' } };
  });
  t.mock.method(editorApi.sceneTools, 'setActorState', async (scene, guid, state) => {
    const actor = actors.get(activePath).find(item => item.actor_guid === guid);
    Object.assign(actor, state);
    return { status: 'success', actor: structuredClone(actor) };
  });
  t.mock.method(editorApi.scratch, 'sendKeyEvent', async (key, mods, displayKey) => {
    if (key === gameplayModule.GAMEPLAY_KEY) {
      if (options.gameplay) return options.gameplay(JSON.parse(displayKey));
      return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
        state: { version: 1, revision: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } },
        config: { playerHp: 100, playerMp: 100, bossHp: 200, damage: 20, cooldownMs: 400,
          bossBarRadius: 10, meleeRange: 2.5, meleeHalfAngle: Math.PI / 3, pickupRange: 2 } };
    }
    calls.push(['key', key, mods, displayKey]);
    if (options.prepare) return options.prepare(key);
    if ((activePath === CHILD && key === 'KeyO') || (activePath === MAIN && key === 'KeyP')) return { status: 'noop' };
    return { data: { status: 'ok', navigation: { source: activePath, target: activePath === MAIN ? CHILD : MAIN,
      direction: key === 'KeyO' ? 'enter' : 'exit', mode: 'story' } } };
  });
  t.mock.method(editorApi.project, 'openProject', async path => {
    calls.push(['nativeOpen', path]);
    const response = options.open ? await options.open(path) : { ok: true, path };
    if (response?.ok) activePath = path;
    return response;
  });
  t.mock.method(lanchat, 'finishWorldSession', async () => { calls.push(['chatCleanup']); });
  const realOpen = launcher.projectLauncherService.openProject;
  t.mock.method(launcher.projectLauncherService, 'openProject', (...args) => {
    const result = realOpen(...args);
    page?.unmount(); // App.vue removes the routed component on beginOpen.
    return result;
  });
  await worldMode.worldModeService.resolve(MAIN, { force: true });
  function mount() {
    const mounted = [], unmounted = [];
    const component = makeComponent({
      vue: { ref, onMounted: fn => mounted.push(fn), onUnmounted: fn => unmounted.push(fn) },
      'vue-router': { onBeforeRouteLeave() {}, useRouter: () => ({ replace: async path => { route = path; } }) },
      '@/api/editorApi.js': { editorApi },
      '@/services/worldModeService.js': worldMode,
      '@/services/projectLauncherService.js': launcher,
      '@/services/worldSessionLifecycle.js': lifecycle,
      '@/utils/viewportStoryCamera.js': { createStoryCameraController },
      '../../../../../game/frontend/storyNavigation.mjs': { createStoryNavigationController },
      '../../../../../game/frontend/storyActors.mjs': { ensureStoryCharacters },
      '../../../../../game/frontend/playerController.mjs': { createPlayerController },
      '../../../../../game/frontend/playerSave.mjs': { createPlayerSave },
      '../../../../../game/frontend/storyGameplay.mjs': gameplayModule,
      '../../../../../game/frontend/storyCharacters.mjs': { STORY_CHARACTERS },
    });
    const instance = component.setup({}, { expose() {} });
    instance.surface.value = { focus() {}, getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 600 }) };
    let disposed = false;
    page = { instance, mount: () => Promise.all(mounted.map(fn => fn())),
      unmount() { if (!disposed) { disposed = true; for (const fn of unmounted) fn(); } } };
    pages.push(page);
    return page;
  }
  t.after(async () => {
    for (const p of pages) p.unmount();
    await lifecycle.drainWorldSession();
    await lifecycle.flushWorldSessionSaves();
    worldMode.worldModeService.invalidate();
    Object.assign(globalThis, previous);
  });
  return { mount, calls, alerts, frames, poses, actors, get route() { return route; },
    step(ms = 50) { frameTime = Math.max(frameTime, performance.now()) + ms;
      const pending = [...frames.values()]; frames.clear(); pending.forEach(fn => fn(frameTime)); },
    opens: () => calls.filter(call => call[0] === 'nativeOpen').map(call => call[1]) };
}

test('real story page O/P retains independent poses, stays story, and never reuses old handles', async t => {
  const f = await fixture(t);
  let page = f.mount(); await page.mount();
  page.instance.camera.wheel(event({ deltaY: -100 })); f.step();
  page.instance.onKeyDown(event({ code: 'KeyW' }));
  const stale = [...f.frames.values()][0];
  const mainPose = page.instance.camera.snapshotPose().camera;
  page.instance.onKeyDown(event());
  assert.equal(page.instance.navigation.busy, true);
  const afterLock = page.instance.camera.snapshotPose().camera;
  page.instance.camera.wheel(event({ deltaY: -100 }));
  assert.deepEqual(page.instance.camera.snapshotPose().camera, afterLock);
  await turn();
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(f.frames.size, 0);
  const count = f.calls.length; stale(100);
  assert.equal(f.calls.length, count);
  assert.deepEqual(f.poses.get(MAIN).position, mainPose.position);
  assert.equal(worldMode.worldModeState.mode, 'story');
  assert.equal(f.route, '/');
  assert.equal(document.listeners.get('keydown').size, 0);
  assert.equal(window.listeners.get('pointermove').size, 0);

  page = f.mount(); await page.mount();
  page.instance.camera.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
  page.instance.camera.pointerMove(event({ buttons: 2, clientX: 50, clientY: 20 }));
  page.instance.camera.wheel(event({ deltaY: -1 }));
  f.step();
  const childPose = page.instance.camera.snapshotPose().camera;
  page.instance.onKeyDown(event({ code: 'KeyP' }));
  await turn();
  assert.deepEqual(f.opens(), [CHILD, MAIN]);
  page = f.mount(); await page.mount();
  assert.deepEqual(page.instance.camera.snapshotPose().camera, mainPose);
  page.instance.onKeyDown(event()); await turn();
  page = f.mount(); await page.mount();
  assert.deepEqual(page.instance.camera.snapshotPose().camera, childPose);
  // Rebinding must restore the controller orbit too, not just display one
  // saved frame before jumping back to default yaw/pitch on the next input.
  page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  const resumedPose = page.instance.camera.snapshotPose().camera;
  for (let i = 0; i < 3; ++i) assert.ok(Math.abs(resumedPose.forward[i] - childPose.forward[i]) < 1e-9);
  assert.deepEqual(f.alerts, []);
  assert.ok(f.calls.filter(call => call[0] === 'setViewportUiMode').every(call => call[2] === 'flat2d'));
});

test('O is ignored before bind; Escape cancels pending init and never binds a stale viewport', async t => {
  const init = deferred();
  const f = await fixture(t, { init: () => init.promise });
  const page = f.mount(); const mounted = page.mount();
  page.instance.onKeyDown(event());
  assert.ok(!f.calls.some(call => call[0] === 'key'));
  page.instance.onKeyDown(event({ code: 'Escape' }));
  init.resolve({ path: 'scene.ini' }); await mounted; await turn();
  assert.equal(f.route, '/StartScreen');
  assert.ok(!f.calls.some(call => call[0] === 'setCameraViewport'));
});

test('real page filters repeated/editable/composition O and locks camera while preparation runs', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  for (const props of [{ repeat: true }, { ctrlKey: true }, { altKey: true }, { metaKey: true },
    { isComposing: true }, { target: { isContentEditable: true } }]) page.instance.onKeyDown(event(props));
  assert.ok(!f.calls.some(call => call[0] === 'pose'));
  page.instance.onKeyDown(event()); await turn();
  const pose = page.instance.camera.snapshotPose();
  page.instance.onKeyDown(event({ code: 'KeyW' }));
  page.instance.camera.pointerDown(event({ button: 2 }));
  page.instance.camera.wheel(event({ deltaY: -1 }));
  page.instance.onKeyDown(event());
  assert.equal(f.frames.size, 0);
  assert.deepEqual(page.instance.camera.snapshotPose(), pose);
  prep.resolve({ status: 'error', message: 'copy failed' }); await turn();
  assert.deepEqual(f.opens(), []);
  assert.deepEqual(f.alerts, ['copy failed']);
  assert.equal(page.instance.navigation.busy, false);
});

test('Escape during copy ignores its late response and leaves the game page', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event()); await turn();
  page.instance.onKeyDown(event({ code: 'Escape' }));
  prep.resolve({ status: 'ok', navigation: { source: MAIN, target: CHILD, direction: 'enter', mode: 'story' } });
  await turn();
  assert.deepEqual(f.opens(), []);
  assert.equal(f.route, '/StartScreen');
  assert.deepEqual(f.alerts, []);
});

test('a newer canonical open drains preparation and prevents late subworld navigation', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event()); await turn();
  const other = launcher.projectLauncherService.openProject('D:/other'); await turn();
  assert.deepEqual(f.opens(), []); // Python still works against the source scene.
  prep.resolve({ status: 'ok', navigation: { source: MAIN, target: CHILD, direction: 'enter', mode: 'story' } });
  await other; await turn();
  assert.deepEqual(f.opens(), ['D:/other']);
  assert.equal(worldMode.worldModeState.projectPath, 'D:/other');
  assert.deepEqual(f.alerts, []);
});

test('failed target open recovers through canonical launcher after source disposal', async t => {
  const f = await fixture(t, { open: path => path === CHILD ? { ok: false, message: 'bad asset' } : { ok: true, path } });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event()); await turn();
  assert.deepEqual(f.opens(), [CHILD, MAIN]);
  assert.equal(worldMode.worldModeState.status, 'ready');
  assert.equal(worldMode.worldModeState.mode, 'story');
  assert.match(f.alerts[0], /已恢复来源世界.*bad asset/);
  const restored = f.mount(); await restored.mount();
  assert.ok(restored.instance.camera.snapshotPose());
});

test('failed recovery routes to launcher and explains both failures', async t => {
  const f = await fixture(t, { open: () => { throw new Error('native open failed'); } });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event()); await turn();
  assert.deepEqual(f.opens(), [CHILD, MAIN]);
  assert.equal(f.route, '/StartScreen');
  assert.match(f.alerts[0], /恢复来源世界也失败/);
});

test('global Escape during native open invalidates recovery and late mode changes', async t => {
  const opened = deferred();
  const f = await fixture(t, { open: () => opened.promise });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event()); await turn();
  launcher.cancelPendingProjectOpen(); // App.vue owns Escape after source-page removal.
  opened.reject(new Error('late failure')); await turn();
  assert.deepEqual(f.opens(), [CHILD]);
  assert.deepEqual(f.alerts, []);
  assert.notEqual(worldMode.worldModeState.status, 'ready');
});


test('Escape saves the final moved player once; reopening restores position and facing', async t => {
  const f = await fixture(t); let page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyD' })); f.step();
  const moved = page.instance.camera.snapshotPlayer();
  assert.ok(moved.position[0] > 0);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 0);
  await page.instance.exitStory();
  assert.equal(f.route, '/StartScreen'); assert.equal(f.frames.size, 0);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
  page.unmount(); await worldMode.worldModeService.resolve(MAIN, { force: true });
  page = f.mount(); await page.mount();
  assert.deepEqual(page.instance.camera.snapshotPlayer().position, moved.position);
  assert.deepEqual(page.instance.camera.snapshotPlayer().rotation, moved.rotation);
  assert.ok(page.instance.camera.snapshotPose().camera.position[0] < moved.position[0]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
});

test('failed exit save keeps the route and supports a successful retry', async t => {
  let fail = true;
  const f = await fixture(t, { save: async () => { if (fail) throw new Error('disk is full'); } });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  await page.instance.exitStory();
  assert.equal(f.route, '/'); assert.deepEqual(f.opens(), []);
  assert.deepEqual(f.alerts, ['disk is full']); assert.equal(f.frames.size, 0);
  fail = false; await page.instance.exitStory();
  assert.equal(f.route, '/StartScreen'); assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 2);
});

test('canonical open waits for the old player save; later selections supersede earlier ones', async t => {
  const saved = deferred();
  const f = await fixture(t, { save: () => saved.promise }); const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyD' })); f.step();
  const stale = [...f.frames.values()][0];
  const first = launcher.projectLauncherService.openProject(CHILD); await turn();
  const second = launcher.projectLauncherService.openProject(MAIN); await turn();
  assert.deepEqual(f.opens(), []); assert.equal(f.frames.size, 0);
  const count = f.calls.length; stale(performance.now() + 100); assert.equal(f.calls.length, count);
  saved.resolve(); assert.equal((await first).status, 'superseded'); await second;
  assert.deepEqual(f.opens(), [MAIN]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
});

test('canonical open cannot replace a scene after save failure; retry retains the old pose', async t => {
  let fail = true;
  const f = await fixture(t, { save: async () => { if (fail) throw new Error('cannot write scene'); } });
  const page = f.mount(); await page.mount(); page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  await assert.rejects(launcher.projectLauncherService.openProject(CHILD), /cannot write scene/);
  assert.deepEqual(f.opens(), []);
  fail = false; await launcher.projectLauncherService.openProject(CHILD);
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 2);
});

test('a timed-out save blocks movement and replacement until its real acknowledgement', async t => {
  const saved = deferred();
  const f = await fixture(t, { save: () => saved.promise }); const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const leaving = page.instance.exitStory(); await turn();
  t.mock.timers.tick(30_001); await leaving;
  assert.equal(f.route, '/'); assert.match(f.alerts[0], /超时/);
  page.instance.onKeyDown(event({ code: 'KeyW' })); assert.equal(f.frames.size, 0);
  const opening = launcher.projectLauncherService.openProject(CHILD); await turn();
  assert.deepEqual(f.opens(), []);
  saved.resolve(); await opening;
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
});

test('world replacement drains pending initialization and never binds its late result', async t => {
  const init = deferred(); const f = await fixture(t, { init: () => init.promise });
  const page = f.mount(); const mounting = page.mount();
  const opening = launcher.projectLauncherService.openProject(CHILD); await turn();
  assert.deepEqual(f.opens(), []);
  init.resolve({ path: 'scene.ini' }); await mounting; await opening;
  assert.deepEqual(f.opens(), [CHILD]);
  assert.ok(!f.calls.some(c => ['setCameraViewport', 'actorTransform', 'playerSave'].includes(c[0])));
});


test('Escape fences a late O result even when the exit save fails', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event()); await turn();
  // Simulate a failed finalizer, independently of the already saved O snapshot.
  const failed = t.mock.method(page.instance.saveRegistration, 'flush', async () => { throw new Error('exit save failed'); });
  await page.instance.exitStory();
  assert.equal(f.route, '/');
  prep.resolve({ status: 'ok', navigation: { source: MAIN, target: CHILD, direction: 'enter', mode: 'story' } });
  await turn();
  assert.deepEqual(f.opens(), []);
  assert.deepEqual(f.alerts, ['exit save failed']);
  assert.equal(page.instance.navigation.busy, false);
  failed.mock.restore();
});

test('Tab captures gameplay, repeated Tab/F are ignored and Escape closes inventory first', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 }));
  page.instance.onKeyDown(event({ code: 'Tab' }));
  assert.equal(page.instance.inventoryOpen.value, true); assert.equal(f.frames.size, 0);
  page.instance.onKeyDown(event({ code: 'Tab', repeat: true }));
  assert.equal(page.instance.inventoryOpen.value, true);
  const before = page.instance.camera.snapshotPose();
  page.instance.onKeyDown(event({ code: 'KeyW' }));
  page.instance.camera.pointerMove(event({ clientX: 600, clientY: 300 }));
  page.instance.onPointerDown(event({ button: 0 }));
  page.instance.onKeyDown(event()); page.instance.onKeyDown(event({ code: 'KeyF' }));
  await turn();
  assert.deepEqual(page.instance.camera.snapshotPose(), before); assert.deepEqual(f.opens(), []);
  assert.equal(page.instance.actionBusy.value, false);
  page.instance.onKeyDown(event({ code: 'Escape' })); await turn();
  assert.equal(page.instance.inventoryOpen.value, false); assert.equal(f.route, '/');
  page.instance.onKeyDown(event({ code: 'KeyF', repeat: true }));
  assert.equal(page.instance.actionBusy.value, false);
  page.instance.onKeyDown(event({ code: 'Escape' })); await turn();
  assert.equal(f.route, '/StartScreen');
});

test('blur, hidden page and inventory close reseed mouse without a jump', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 })); f.step();
  page.instance.onBlur(); assert.equal(f.frames.size, 0);
  const before = page.instance.camera.snapshotPose();
  page.instance.onFocus();
  page.instance.camera.pointerMove(event({ clientX: 400, clientY: 300 })); f.step();
  assert.deepEqual(page.instance.camera.snapshotPose(), before);
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 }));
  document.hidden = true; page.instance.onVisibilityChange(); assert.equal(f.frames.size, 0);
  document.hidden = false; page.instance.onFocus();
});

test('unacknowledged combat blocks native replacement; retry does not change operation identity', async t => {
  let fail = true; const commands = [];
  const state = { version: 1, revision: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } };
  const config = { playerHp: 100, playerMp: 100, bossHp: 200, damage: 20, cooldownMs: 400,
    bossBarRadius: 10, meleeRange: 2.5, meleeHalfAngle: Math.PI / 3, pickupRange: 2 };
  const f = await fixture(t, { gameplay: async request => {
    if (request.action !== 'load') {
      commands.push(request);
      if (fail) throw new Error('combat disk full');
      state.boss.hp = 180; state.revision = 1;
    }
    return { status: 'ok', role: 'main', state: structuredClone(state), config };
  } });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyW' }));
  for (let i = 0; i < 60; i++) f.step(50);
  page.instance.camera.keyUp(event({ code: 'KeyW' }));
  assert.equal(page.instance.bossNearby.value, true);
  page.instance.onPointerDown(event({ button: 0 })); await turn();
  assert.match(page.instance.gameplayError.value, /combat disk full/);
  assert.equal(page.instance.gameplay.needsSave, true);
  await assert.rejects(launcher.projectLauncherService.openProject(CHILD), /combat disk full/);
  assert.deepEqual(f.opens(), []);
  fail = false;
  await launcher.projectLauncherService.openProject(CHILD);
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(new Set(commands.map(c => c.operationId)).size, 1);
  assert.ok(commands.every(c => c.projectPath === MAIN));
});


test('inventory template captures pointer input and its close button restores control without an attack or jump', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  const attack = t.mock.method(page.instance.gameplay, 'attack', async () => {});
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 })); f.step();
  page.instance.onKeyDown(event({ code: 'Tab' }));
  const before = page.instance.camera.snapshotPose();
  const tree = renderStory(proxyRefs(page.instance), []);
  const overlay = findNode(tree, node => node.props?.class === 'inventory-overlay');
  const close = findNode(overlay, node => node.type === 'button' && node.props?.['aria-label'] === '关闭背包');
  assert.ok(overlay); assert.ok(close);

  for (const selector of ['.inventory-overlay', '.inventory-panel']) {
    let interactive = false;
    styles.walkRules(rule => {
      if (rule.selectors.includes(selector)) rule.walkDecls('pointer-events', declaration => {
        interactive = declaration.value === 'auto';
      });
    });
    assert.ok(interactive, `${selector} must explicitly participate in hit testing`);
  }
  for (const name of ['onPointerdown', 'onPointermove', 'onWheel']) {
    let stopped = false;
    const input = event({ button: 0, clientX: 799, clientY: 300, deltaY: 200,
      stopPropagation() { stopped = true; } });
    overlay.props[name](input);
    assert.ok(stopped, `${name} must not bubble into the viewport`);
    if (name === 'onWheel') assert.ok(input.defaultPrevented);
  }
  assert.equal(page.instance.inventoryOpen.value, true);
  assert.equal(f.frames.size, 0);
  close.props.onClick(event()); await turn();
  assert.equal(page.instance.inventoryOpen.value, false);
  assert.equal(attack.mock.callCount(), 0);
  assert.equal(f.route, '/');
  page.instance.camera.pointerMove(event({ clientX: 400, clientY: 300 })); f.step();
  assert.deepEqual(page.instance.camera.snapshotPose(), before);
});


test('real story page settles airborne Tab, blur, pointerleave and hidden-page interruptions', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  const groundY = page.instance.camera.snapshotPlayer().position[1];
  const stopCases = [
    [() => page.instance.onKeyDown(event({ code: 'Tab' })), () => page.instance.toggleInventory()],
    [() => page.instance.onBlur(), () => page.instance.onFocus()],
    [() => page.instance.camera.pointerLeave(), () => {}],
    [() => { document.hidden = true; page.instance.onVisibilityChange(); },
      () => { document.hidden = false; page.instance.onVisibilityChange(); }],
  ];
  for (const [stop, resume] of stopCases) {
    page.instance.onKeyDown(event({ code: 'Space' })); f.step();
    const airborne = page.instance.camera.snapshotPlayer();
    assert.equal(airborne.grounded, false); assert.ok(airborne.position[1] > groundY);
    const stale = [...f.frames.values()][0]; stop();
    const landed = page.instance.camera.snapshotPlayer();
    assert.equal(landed.grounded, true); assert.equal(landed.position[1], groundY);
    assert.equal(landed.position[2], airborne.position[2]);
    assert.equal(f.frames.size, 0); const count = f.calls.length; stale(50000); assert.equal(f.calls.length, count);
    // Locks reject new Space; pointerleave instead clears held state.
    if (document.hidden || page.instance.inventoryOpen.value) {
      page.instance.onKeyDown(event({ code: 'Space' })); assert.equal(f.frames.size, 0);
    }
    resume();
    const camera = page.instance.camera.snapshotPose();
    page.instance.camera.pointerMove(event({ clientX: 400, clientY: 300 })); f.step();
    assert.deepEqual(page.instance.camera.snapshotPose(), camera);
  }
  await page.instance.saveRegistration.flush();
  assert.equal(f.actors.get(MAIN)[0].geometry.position[1], groundY);
  assert.deepEqual(f.alerts, []);
});
test('half-jump O/P saves ground and the corrected model once, preserves camera and keeps child combat hidden', async t => {
  const f = await fixture(t); let page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'Space' })); f.step(); f.step();
  const airborne = page.instance.camera.snapshotPlayer(), groundY = f.actors.get(MAIN)[0].geometry.position[1];
  page.instance.onKeyDown(event({ code: 'KeyO' })); await turn();
  assert.deepEqual(f.opens(), [CHILD]);
  const saved = f.actors.get(MAIN)[0].geometry;
  assert.equal(saved.position[1], groundY); assert.equal(saved.position[2], airborne.position[2]);
  assert.deepEqual(saved.rotation, airborne.rotation);
  const savedCamera = structuredClone(f.poses.get(MAIN));
  page = f.mount(); await page.mount();
  assert.equal(page.instance.bossNearby.value, false); assert.equal(f.actors.get(CHILD)[1].visible, false);
  page.instance.onKeyDown(event({ code: 'KeyP' })); await turn();
  page = f.mount(); await page.mount();
  const restored = page.instance.camera.snapshotPlayer();
  assert.equal(restored.grounded, true); assert.deepEqual(restored.position, saved.position);
  assert.deepEqual(restored.rotation, saved.rotation); assert.equal(restored.facingYaw, airborne.facingYaw);
  assert.deepEqual(page.instance.camera.snapshotPose().camera.position, savedCamera.position);
  assert.deepEqual(f.alerts, []);
});
test('half-jump Escape waits for the ground save, blocks Space and remains in world on save failure', async t => {
  const gate = deferred(); let fail = true, block = true;
  const f = await fixture(t, { save: async () => { if (block) await gate.promise; if (fail) throw new Error('landing disk full'); } });
  const page = f.mount(); await page.mount(); const groundY = page.instance.camera.snapshotPlayer().position[1];
  page.instance.onKeyDown(event({ code: 'Space' })); f.step();
  const before = page.instance.camera.snapshotPlayer(); page.instance.onKeyDown(event({ code: 'Escape' }));
  await turn(); assert.equal(f.route, '/');
  page.instance.onKeyDown(event({ code: 'Space' })); assert.equal(f.frames.size, 0);
  gate.resolve(); await turn();
  assert.equal(f.route, '/'); assert.match(page.instance.gameplayError.value, /landing disk full/);
  assert.equal(page.instance.camera.snapshotPlayer().position[1], groundY);
  const writes = f.calls.filter(c => c[0] === 'playerSave'); assert.equal(writes.length, 1);
  assert.equal(writes[0][4].position[1], groundY); assert.equal(writes[0][4].position[2], before.position[2]);
  fail = false; block = false; page.instance.onKeyDown(event({ code: 'Escape' })); await turn();
  assert.equal(f.route, '/StartScreen'); assert.deepEqual(f.actors.get(MAIN)[0].geometry.position, writes[0][4].position);
});
test('unrelated world replacement settles locally after invalidation and saves without using old handles', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  const groundY = page.instance.camera.snapshotPlayer().position[1];
  page.instance.onKeyDown(event({ code: 'Space' })); f.step();
  const before = page.instance.camera.snapshotPlayer(), stale = [...f.frames.values()][0];
  const index = f.calls.length;
  await launcher.projectLauncherService.openProject('D:/other'); stale(99999);
  assert.deepEqual(f.opens(), ['D:/other']);
  assert.ok(!f.calls.slice(index).some(c => ['actorTransform', 'cameraMove'].includes(c[0])));
  const write = f.calls.slice(index).find(c => c[0] === 'playerSave');
  assert.equal(write[1], MAIN); assert.equal(write[4].position[1], groundY);
  assert.equal(write[4].position[2], before.position[2]);
});
test('Space cannot start during an acknowledged save; a late save never leaves the player airborne', async t => {
  const gate = deferred(); const f = await fixture(t, { save: () => gate.promise });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'Space' })); f.step();
  const saved = page.instance.saveRegistration.flush(); await turn();
  page.instance.onKeyDown(event({ code: 'Space' })); page.instance.onKeyDown(event({ code: 'KeyO' }));
  assert.equal(f.frames.size, 0); assert.deepEqual(f.opens(), []);
  gate.resolve(); await saved;
  assert.equal(page.instance.camera.snapshotPlayer().grounded, true);
  assert.equal(page.instance.camera.snapshotPlayer().position[1], f.actors.get(MAIN)[0].geometry.position[1]);
});
test('HUD puts feedback/errors above compact bottom vitals and lists Space with side controls', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  page.instance.feedback.value = '获得 世界碎片 ×1'; page.instance.canPickup.value = true;
  page.instance.gameplayError.value = '保存失败，请重试';
  const tree = renderStory(proxyRefs(page.instance), []);
  const bottom = findNode(tree, node => node.props?.class === 'story-bottom-stack');
  assert.deepEqual(bottom.children.filter(node => node.type !== vue.Comment).map(node => node.props?.class),
    ['gameplay-error', 'story-feedback', 'player-vitals']);
  const controls = findNode(tree, node => node.props?.class === 'story-controls');
  assert.match(JSON.stringify(controls, (key, value) => key === 'ctx' ? undefined : value), /空格/);
  const declarations = selector => {
    const values = {};
    styles.walkRules(rule => { if (rule.parent.type === 'root' && rule.selectors.includes(selector))
      rule.walkDecls(d => { values[d.prop] = d.value; }); });
    return values;
  };
  assert.equal(declarations('.story-hud')['pointer-events'], 'none');
  assert.equal(declarations('.story-bottom-stack')['grid-area'], '3 / 1');
  assert.equal(declarations('.story-controls')['flex-direction'], 'column');
  assert.equal(declarations('.story-controls')['align-self'], 'center');
  assert.equal(declarations('.player-vitals').position, undefined);
  assert.equal(declarations('.gameplay-error').position, undefined);
});
