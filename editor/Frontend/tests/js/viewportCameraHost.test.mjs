import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import { ref, reactive } from 'vue';
import { babelParse, compileScript, parse } from 'vue/compiler-sfc';
import { cameraMovementKey, createViewportCameraController } from '../../src/utils/viewportCameraController.js';
import { createStoryCameraController } from '../../src/utils/viewportStoryCamera.js';

const source = fs.readFileSync(new URL('../../src/views/layout/MainPage.vue', import.meta.url), 'utf8');
const { descriptor } = parse(source);
const declarations = babelParse(descriptor.scriptSetup.content, { sourceType: 'module' }).program.body;
// Execute the real creative camera host declarations with unrelated services stubbed.
// No duplicate handlers or movement algorithms live in this test.
const names = new Set([
  'cameraState', 'cameraBindingState', 'cameraSpeed', 'mouseSensitivity',
  'cameraMovementGestures', 'movementAxisGroups', 'mouseRotateStartForward', 'mouseRotateMoved',
  'cameraControls', 'isRealtimeCameraInputActive', 'scheduleCameraUpdate',
  'vectorDistance', 'scratchMouseButton', 'sendScratchPointerEvent', 'handleWheel',
  'handleKeyDown', 'handleKeyUp', 'isGamePreviewInputLocked', 'resetRealtimeCameraInput',
  'setEditorCameraInputLock', 'setGamePreviewInputLocked',
  'viewportCursorShape', 'handleMainViewportBlur', 'handleMainViewportVisibility',
  'onMouseDown', 'onMouseMove', 'onMouseUp', 'sendCameraUpdateFast', 'handleCameraMove',
  'coerceNumber', 'setCameraSpeedFromPanel', 'isVector3', 'sceneGridEnabledFromSnapshot', 'applySceneSnapshot',
]);
const hostSource = declarations.filter(node => node.type === 'VariableDeclaration'
  && node.declarations.some(declaration => names.has(declaration.id.name)))
  .map(node => descriptor.scriptSetup.content.slice(node.start, node.end)).join('\n');
const makeHost = new Function('deps', `
  const { ref, reactive, cameraMovementKey, createViewportCameraController, window, document,
    editorApi, cabbageContextService, viewCurrent, viewportGizmoController,
    refreshSceneCameraBinding, syncSceneCameraBinding, broadcastViewportControlsState,
    getEditorControlsState } = deps;
  const DEFAULT_SCENE_NAME = 'scene.ini';
  const tabs = ref([{id: DEFAULT_SCENE_NAME}]), activeTab = ref(0);
  const getViewportRenderRect = () => ({ left: 0, top: 0, width: 800, height: 600 });
  const indexActorsByHandle = () => new Map();
  const sceneGridEnabled = ref(true), sceneLightSettings = reactive({ direction: {} });
  const mainRenderBackend = ref('native'), mainVisionRenderMode = ref('path_tracing');
  let actorPickIndex, lastCameraViewportSignature = '', pendingMainRenderSelection = null;
  const scheduleCameraViewportSync = () => {}, syncViewportUiMode = () => {};
  ${hostSource}
  return { ${[...names].join(', ')} };
`);
const input = (props = {}) => ({ preventDefault() {}, ...props });
const snapshot = (handle = 12, position = [0, 0, 0]) => ({ status: 'success', scene: 'scene.ini', active_camera_name: 'main', cameras: [
  { handle, name: 'main', position, forward: [0, 0, 1], world_up: [0, 1, 0], fov: 60 },
] });
function mountCreative(t, { native = true } = {}) {
  let current = true, time = 0, id = 0;
  const frames = new Map(), calls = [], records = [], scratch = [];
  const window = native ? { coronaBridge: { cameraMove: (...args) => calls.push(args) } } : {};
  const document = { hidden: false, getElementById: () => null };
  const clock = {
    requestFrame: callback => { frames.set(++id, callback); return id; },
    cancelFrame: id => frames.delete(id), now: () => time,
  };
  let refreshes = 0;
  const page = makeHost({
    ref, reactive, cameraMovementKey,
    createViewportCameraController: options => createViewportCameraController({ ...options, ...clock }),
    window, document, viewCurrent: () => current,
    editorApi: { scratch: Object.fromEntries(['sendKeyEvent', 'sendKeyUpEvent', 'sendMouseEvent']
      .map(name => [name, async (...args) => scratch.push([name, ...args])])) },
    cabbageContextService: { recordEvent: async record => records.push(record) },
    viewportGizmoController: { isDragging: () => false, cancel() {} },
    refreshSceneCameraBinding: () => { refreshes++; }, syncSceneCameraBinding() {},
    broadcastViewportControlsState() {}, getEditorControlsState: () => ({}),
  });
  page.applySceneSnapshot('scene.ini', snapshot());
  function step(next) {
    time = next;
    const callbacks = [...frames.values()]; frames.clear();
    for (const callback of callbacks) callback(time);
  }
  t.after(() => page.cameraControls.dispose());
  return { page, frames, calls, records, scratch, document, step,
    invalidate: () => { current = false; }, refreshes: () => refreshes };
}

test('creative binds native snapshots with a string scene route and optional envelopes', t => {
  const h = mountCreative(t), p = h.page;
  const native = { ...snapshot(25, [1, 2, 3]), scene: 'Scene/default.scene' };
  for (const payload of [native, { data: native }, { scene: native }, { data: { scene: native } }]) {
    p.applySceneSnapshot(native.scene, payload); h.step(0);
    assert.equal(p.cameraBindingState.value.cameraHandle, 25);
    assert.equal(p.cameraBindingState.value.sceneId, native.scene);
    assert.deepEqual(p.cameraState.value.position, [1, 2, 3]);
    assert.deepEqual(h.calls.at(-1).slice(0, 2), [25, [1, 2, 3]]);
  }
});

test('creative host keeps panel controls, input refresh, analytics and script forwarding', t => {
  const h = mountCreative(t, { native: false }), p = h.page;
  p.setCameraSpeedFromPanel(0.5); p.handleCameraMove('forward');
  assert.equal(p.cameraState.value.position[2], 0.5);
  p.handleWheel(input({ deltaY: -1, shiftKey: true }));
  assert.equal(p.cameraSpeed.value, 0.52);
  p.handleWheel(input({ deltaY: -1 }));
  assert.equal(p.cameraState.value.position[2], 1.02);
  p.handleKeyDown(input({ code: 'KeyD', key: 'd' })); h.step(50);
  p.handleKeyUp(input({ code: 'KeyD', key: 'd' }));
  assert.equal(h.refreshes(), 1);
  assert.ok(h.records.some(item => item.details.interaction === 'wheel'));
  assert.ok(h.records.some(item => item.details.axisGroup === 'left_right'));
  assert.deepEqual(h.scratch.map(call => call[0]), ['sendMouseEvent', 'sendMouseEvent', 'sendKeyEvent', 'sendKeyUpEvent']);
  p.setGamePreviewInputLocked(true);
  const before = structuredClone(JSON.parse(JSON.stringify(p.cameraState.value)));
  p.handleKeyDown(input({ code: 'KeyW', key: 'w' }));
  p.handleWheel(input({ deltaY: -1 })); p.handleCameraMove('forward'); h.step(100);
  assert.deepEqual(JSON.parse(JSON.stringify(p.cameraState.value)), before);
  assert.equal(h.scratch.at(-2)[0], 'sendKeyEvent');
  assert.equal(h.scratch.at(-1)[0], 'sendMouseEvent');
  p.setCameraSpeedFromPanel(1); assert.equal(p.cameraSpeed.value, 1);
});

test('creative host rebinding, focus loss and invalidation cancel old movement', t => {
  const h = mountCreative(t), p = h.page; h.step(0); h.calls.length = 0;
  p.handleKeyDown(input({ code: 'KeyW', key: 'w' }));
  const late = [...h.frames.values()][0];
  p.applySceneSnapshot('next.ini', snapshot(25, [5, 0, 0]), { preservePose: true });
  late(50); h.step(50);
  assert.deepEqual(h.calls.map(call => call.slice(0, 2)), [[25, [5, 0, 0]]]);
  assert.equal(p.cameraControls.isInputActive(), false);
  for (const stop of [() => p.handleMainViewportBlur(), () => {
    h.document.hidden = true; p.handleMainViewportVisibility();
  }, () => h.invalidate()]) {
    h.document.hidden = false;
    p.handleKeyDown(input({ code: 'KeyW', key: 'w' }));
    p.onMouseDown(input({ button: 2, clientX: 0, clientY: 0 }));
    p.onMouseMove(input({ clientX: 10, clientY: 0, buttons: 2 }));
    const count = h.calls.length; stop(); h.step(100);
    assert.equal(h.calls.length, count); assert.equal(h.frames.size, 0);
  }
});

test('creative same-binding refresh preserves input, but invalid rebinding clears the old handle', t => {
  const h = mountCreative(t), p = h.page; h.step(0);
  p.handleKeyDown(input({ code: 'KeyW', key: 'w' }));
  p.applySceneSnapshot('scene.ini', { data: snapshot(12, [9, 9, 9]) }, { preservePose: true });
  assert.equal(p.cameraControls.isInputActive(), true);
  assert.deepEqual(p.cameraState.value.position, [0, 0, 0]);
  h.step(50);
  assert.ok(Math.abs(p.cameraState.value.position[2] - 0.6) < 1e-8);
  const late = [...h.frames.values()][0], count = h.calls.length;
  p.applySceneSnapshot('missing.ini', null);
  late(100);
  assert.equal(p.cameraBindingState.value.cameraHandle, null);
  assert.equal(p.cameraControls.isInputActive(), false);
  assert.equal(h.frames.size, 0);
  p.handleCameraMove('forward'); h.step(100);
  assert.equal(h.calls.length, count);
});

test('creative right-drag analytics and native key authority remain intact', t => {
  const h = mountCreative(t), p = h.page; h.step(0);
  p.onMouseDown(input({ button: 2, clientX: 0, clientY: 0 }));
  p.onMouseMove(input({ clientX: 100, clientY: 0, buttons: 2 }));
  assert.equal(p.viewportCursorShape(), 'grabbing');
  p.onMouseUp(input({ button: 2 }));
  assert.equal(p.viewportCursorShape(), 'arrow');
  assert.ok(h.records.some(item => item.type === 'camera_rotated'));
  p.handleKeyDown(input({ code: 'KeyW', key: 'w' })); h.step(50);
  p.handleKeyUp(input({ code: 'KeyW', key: 'w' }));
  assert.equal(h.scratch.length, 0);
});

const storySource = fs.readFileSync(new URL('../../src/views/layout/StoryWorld.vue', import.meta.url), 'utf8');
const storyDescriptor = parse(storySource).descriptor;
const compiled = compileScript(storyDescriptor, { id: 'story-camera-regression', genDefaultAs: 'StoryWorld' });
let setupSource = compiled.content;
const imports = babelParse(setupSource, { sourceType: 'module' }).program.body.filter(node => node.type === 'ImportDeclaration');
for (const node of [...imports].reverse()) {
  const bindings = node.specifiers.map(specifier => `${specifier.imported.name}: ${specifier.local.name}`).join(', ');
  setupSource = setupSource.slice(0, node.start) + `const { ${bindings} } = modules[${JSON.stringify(node.source.value)}];` + setupSource.slice(node.end);
}
const makeStory = new Function('modules', 'window', 'document', `${setupSource}; return StoryWorld;`);
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };
async function mountStory(t, { pendingInit = null, sceneSnapshot = { data: snapshot() } } = {}) {
  const mounted = [], unmounted = [], frames = new Map(), calls = [], routes = [];
  const listeners = new Map(); let id = 0, time = 0;
  const dom = () => ({
    addEventListener: (name, callback) => { listeners.set(callback, name); },
    removeEventListener: (_name, callback) => { listeners.delete(callback); },
  });
  const document = { ...dom(), hasFocus: () => true, hidden: false };
  const worldModeState = { mode: 'story', status: 'ready', revision: 1, projectPath: 'world' };
  const window = { ...dom(), devicePixelRatio: 1, alert: message => calls.push(['alert', message]),
    coronaBridge: Object.fromEntries(['cameraMove', 'setCameraViewport', 'setViewportGizmoTarget', 'setViewportUiMode', 'setViewportSystemCursorHidden']
      .map(name => [name, (...args) => { calls.push([name, ...args]); return true; }])) };
  const component = makeStory({
    vue: { ref, onMounted: fn => mounted.push(fn), onUnmounted: fn => unmounted.push(fn) },
    'vue-router': { useRouter: () => ({ replace: async path => routes.push(path) }) },
    '@/api/editorApi.js': { editorApi: {
      main: { onInit: async () => pendingInit ? pendingInit.promise : ({ scenes: [{ path: 'scene.ini' }] }) },
      scene: { getSnapshot: async () => sceneSnapshot },
    } },
    '@/services/worldModeService.js': { worldModeState },
    '@/utils/viewportStoryCamera.js': { createStoryCameraController: options => createStoryCameraController({
      ...options, now: () => time,
      requestFrame: callback => { frames.set(++id, callback); return id; }, cancelFrame: id => frames.delete(id),
    }) },
  }, window, document);
  const page = component.setup({}, { expose() {} });
  page.surface.value = { focus() {}, getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 600 }) };
  const mounting = Promise.all(mounted.map(fn => fn()));
  if (!pendingInit) await mounting;
  const cleanup = () => { for (const fn of unmounted) fn(); assert.equal(listeners.size, 0); assert.equal(frames.size, 0); };
  t.after(cleanup);
  return { page, calls, routes, frames, document, worldModeState, mounting,
    step(next) { time = next; const callbacks = [...frames.values()]; frames.clear(); for (const cb of callbacks) cb(time); } };
}

test('real story page only binds a viewport and stops on blur, hidden state, exit and revision changes', async t => {
  const h = await mountStory(t), p = h.page;
  p.onKeyDown(input({ code: 'ArrowRight' })); h.step(50);
  assert.ok(h.calls.some(call => call[0] === 'cameraMove' && call[3][0] > 0));
  p.onBlur(); assert.equal(h.frames.size, 0);
  const count = h.calls.length;
  p.camera.wheel(input({ deltaY: -1 })); h.step(100); assert.equal(h.calls.length, count);
  p.onFocus(); p.onKeyDown(input({ code: 'KeyW' }));
  h.document.hidden = true; p.onVisibilityChange(); assert.equal(h.frames.size, 0);
  h.document.hidden = false; p.onFocus(); p.onKeyDown(input({ code: 'KeyW' }));
  h.worldModeState.revision++; h.step(150); assert.equal(h.calls.length, count);
  h.worldModeState.revision--;
  p.onKeyDown(input({ code: 'Escape', stopPropagation() {} }));
  assert.deepEqual(h.routes, ['/StartScreen']);
  p.camera.wheel(input({ deltaY: -1 })); h.step(200); assert.equal(h.calls.length, count);
});

test('story Escape works during loading and late initialization cannot bind a camera', async t => {
  const pending = deferred(), h = await mountStory(t, { pendingInit: pending });
  h.page.onKeyDown(input({ code: 'Escape', stopPropagation() {} }));
  pending.resolve({ scenes: [{ path: 'scene.ini' }] }); await h.mounting;
  assert.deepEqual(h.routes, ['/StartScreen']); assert.equal(h.calls.length, 0);
});

test('story missing-camera error returns to launcher without editor actions', async t => {
  const h = await mountStory(t, { sceneSnapshot: { cameras: [] } });
  assert.deepEqual(h.routes, ['/StartScreen']);
  assert.deepEqual(h.calls, [['alert', '当前场景没有可用相机']]);
});
