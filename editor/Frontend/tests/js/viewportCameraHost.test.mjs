import { NAVIGATION_KEY } from '../../../game/frontend/storyNavigation.mjs';
import { actorFixture, gameplayConfig, projectReady } from '../../../game/tests/frontend/fixtures.mjs';
import * as gameplayModule from '../../../game/frontend/storyGameplay.mjs';
import * as storyPropsModule from '../../../game/frontend/storyProps.mjs';
import * as storyLootModule from '../../../game/frontend/storyLoot.mjs';
import * as prophetDialogueModule from '../../../game/frontend/prophetDialogue.mjs';
import * as storyCubeModule from '../../../game/frontend/storyCube.mjs';
import * as storyWorldRulesModule from '../../../game/frontend/storyWorldRules.mjs';
import * as storyProphetActionsModule from '../../../game/frontend/storyProphetActions.mjs';
import * as storyMapModule from '../../../game/frontend/storyMap.mjs';
import * as storyMapHeightModule from '../../../game/frontend/storyMapHeight.mjs';
import { mapHeightUrl } from '../../../game/frontend/storyMapHeightLoader.mjs';
import { STORY_CHARACTERS, PROPHET_GUID, PLAYER_MODEL_YAW_OFFSET, isMonsterRole, resolveStoryAssetPath, sceneSnapshot } from '../../../game/frontend/storyCharacters.mjs';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import { ref, reactive, nextTick, watch, computed } from 'vue';
import { babelParse, compileScript, parse } from 'vue/compiler-sfc';
import { cameraMovementKey, createViewportCameraController } from '../../src/utils/viewportCameraController.js';
import { createStoryCameraController } from '../../src/utils/viewportStoryCamera.js';
import { createStoryWorldControls, SMALL_WORLD_VIEW_LABELS } from '../../src/utils/viewportStoryWorldCamera.js';
import { routeStoryKeydown } from '../../src/utils/storyKeyRouter.js';
import { createStoryNavigationController } from '../../../game/frontend/storyNavigation.mjs';
import { createPlayerController, VIEW_LABELS } from '../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../game/frontend/playerSave.mjs';
import { ensureStoryCharacters, syncPlacementActors } from '../../../game/frontend/storyActors.mjs';
import { sceneFixture } from '../../../game/tests/frontend/fixtures.mjs';
import { registerWorldSessionSave, flushWorldSessionSaves, trackWorldSessionWork } from '../../src/services/worldSessionLifecycle.js';
import { normalizeVisionRenderMode, visionAccumulationFromCamera, visionDenoiseFromCamera, visionStablePlanesFromCamera, visionDenoiserFromCamera } from '../../src/utils/visionRenderModes.js';

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
  'currentMainCamera', 'currentMainCameraId', 'mainVisionDenoise', 'mainVisionDenoiseError',
  'pendingMainDenoiseSelection',
  'mainVisionDenoiser', 'mainVisionDenoiserError', 'pendingMainDenoiserSelection',
  'mainVisionStablePlanes', 'mainVisionStablePlanesError', 'pendingMainStablePlanesSelection',
  'mainVisionAccumulation', 'mainVisionAccumulationError', 'pendingMainAccumulationSelection',
]);
const hostSource = declarations.filter(node => node.type === 'VariableDeclaration'
  && node.declarations.some(declaration => names.has(declaration.id.name)))
  .map(node => descriptor.scriptSetup.content.slice(node.start, node.end)).join('\n');
const makeHost = new Function('deps', `
  const { ref, reactive, cameraMovementKey, createViewportCameraController, window, document,
    editorApi, cabbageContextService, viewCurrent, viewportGizmoController,
    refreshSceneCameraBinding, syncSceneCameraBinding, broadcastViewportControlsState,
    getEditorControlsState, normalizeVisionRenderMode, visionAccumulationFromCamera, visionDenoiseFromCamera, visionStablePlanesFromCamera, visionDenoiserFromCamera } = deps;
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
const input = (props = {}) => ({ preventDefault() {}, stopPropagation() {}, ...props });
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
    ref, reactive, cameraMovementKey, normalizeVisionRenderMode, visionAccumulationFromCamera, visionDenoiseFromCamera, visionStablePlanesFromCamera, visionDenoiserFromCamera,
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
  // 样式副作用导入在测试里无需执行，直接摘除
  if (/\.css$/.test(node.source.value)) { setupSource = setupSource.slice(0, node.start) + setupSource.slice(node.end); continue; }
  // 具名导入与默认导入都要改写：`specifier.imported` 只存在于具名导入上，
  // 直接把两者都当具名导入会在 HERO_FIGURE（图片默认导入）上崩掉整个文件。
  const named = node.specifiers.filter(specifier => specifier.type === 'ImportSpecifier');
  const defaults = node.specifiers.filter(specifier => specifier.type === 'ImportDefaultSpecifier');
  const parts = [];
  if (named.length) {
    const bindings = named.map(specifier => `${JSON.stringify(specifier.imported.name)}: ${specifier.local.name}`).join(', ');
    parts.push(`const { ${bindings} } = modules[${JSON.stringify(node.source.value)}];`);
  }
  for (const specifier of defaults) {
    parts.push(`const ${specifier.local.name} = modules[${JSON.stringify(node.source.value)}].default;`);
  }
  setupSource = setupSource.slice(0, node.start) + parts.join(' ')
    + setupSource.slice(node.end);
}
const makeStory = new Function('modules', 'window', 'document', `${setupSource}; return StoryWorld;`);
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };
async function mountStory(t, { pendingInit = null, sceneSnapshot = { data: snapshot() }, role = 'main' } = {}) {
  const mounted = [], unmounted = [], frames = new Map(), calls = [], routes = [];
  const listeners = new Map(); let id = 0, time = 0;
  // The page drives both combat and exhibits over the same bridge key, so the harness
  // answers by action, and keeps a mutable scene so placement actors can be observed.
  const sceneActors = structuredClone(sceneSnapshot.data?.actors ?? sceneFixture().actors);
  const placementExhibit = { role, placements: [] };
  let gameplayOptions = null;
  const placementActor = guid => sceneActors.find(actor => actor.actor_guid === guid);
  const dom = () => ({
    addEventListener: (name, callback) => { listeners.set(callback, name); },
    removeEventListener: (_name, callback) => { listeners.delete(callback); },
  });
  const document = { ...dom(), hasFocus: () => true, hidden: false };
  const worldModeState = { mode: 'story', status: 'ready', revision: 1, projectPath: 'world' };
  const window = { ...dom(), devicePixelRatio: 1, location: { href: 'file:///D:/engine/editor/Frontend/dist/index.html' }, alert: message => calls.push(['alert', message]),
    coronaBridge: Object.fromEntries(['actorTransform', 'cameraMove', 'setCameraViewport', 'setViewportGizmoTarget', 'setViewportUiMode', 'setViewportSystemCursorHidden']
      .map(name => [name, (...args) => { calls.push([name, ...args]); return true; }])) };
  const component = makeStory({
    vue: { ref, nextTick, watch, reactive, computed, onMounted: fn => mounted.push(fn), onUnmounted: fn => unmounted.push(fn) },
    'vue-router': { onBeforeRouteLeave() {}, useRouter: () => ({ replace: async path => routes.push(path) }) },
    '@/api/editorApi.js': { editorApi: {
      project: { getProjectLoadStatus: async () => projectReady('world') },
      sceneTools: {
        setActorState: async (scene, guid, state) => {
          const actor = placementActor(guid);
          Object.assign(actor, state);
          return { status: 'success', actor };
        },
        createActor: async (scene, path, type, data) => {
          const actor = { ...actorFixture(STORY_CHARACTERS[0]), name: data.name, actor_guid: data.actor_guid,
            local_aabb: [-0.25, -0.25, -0.25, 0.25, 0.25, 0.25], route: path,
            geometry: { position: [...data.position], rotation: [...data.rotation], scale: [...data.scale] } };
          sceneActors.push(actor);
          return { status: 'success', actor };
        },
        removeActor: async (scene, guid) => {
          const index = sceneActors.findIndex(actor => actor.actor_guid === guid);
          if (index >= 0) sceneActors.splice(index, 1);
          return { status: 'success' };
        },
        setActorPhysics: async (scene, guid, physics) => { Object.assign(placementActor(guid).mechanics, physics); return { status: 'success' }; },
      },
      projectSettings: { getActiveProjectInfo: async () => ({ mode: 'story', project_path: 'world' }) },
      scratch: { sendKeyEvent: async (key, text, payload) => {
        let message = {};
        try { message = JSON.parse(payload); } catch { return { status: 'ok', role: placementExhibit.role }; }
        if (String(message.action || '').endsWith('Placements')) {
          if (message.action === 'savePlacements') placementExhibit.placements = message.placements || [];
          return { response: JSON.stringify({ status: 'ok', role: placementExhibit.role,
            state: { version: 1, placements: placementExhibit.placements } }) };
        }
        return { status: 'ok', role: placementExhibit.role,
          state: { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } },
          config: gameplayConfig };
      } },
      main: { onInit: async () => pendingInit ? pendingInit.promise : ({ scenes: [{ path: 'scene.ini' }] }) },
      scene: { getSnapshot: async () => ({ ...(sceneSnapshot.data ?? sceneSnapshot), actors: sceneActors }),
        setActorTransform: async (scene, guid, transform) => { calls.push(['playerSave', guid]);
          const actor = placementActor(guid);
          if (actor) for (const key of ['position', 'rotation', 'scale']) if (transform[key]) actor.geometry[key] = [...transform[key]];
          return { status: 'success', actor }; } },
    } },
    '@/services/worldModeService.js': { worldModeState, normalizeProjectPath: value => String(value).toLowerCase() },
    // 图片资源：测试里只当作一个 URL 字符串（默认导入经上面的改写走 .default）。
    '@/assets/hero-figure.png': { default: 'hero-figure.png' },
    '@/services/projectLauncherService.js': { projectLauncherService: {}, cancelPendingProjectOpen() {}, getProjectSelectionVersion: () => 0 },
    '@/services/worldSessionLifecycle.js': { registerWorldSessionSave, trackWorldSessionWork, notifyWorldError: error => window.alert(error.message) },
    '../../../../game/frontend/storyNavigation.mjs': { createStoryNavigationController, NAVIGATION_KEY },
    '../../../../game/frontend/storyActors.mjs': { ensureStoryCharacters, syncPlacementActors },
    '../../../../game/frontend/storyProps.mjs': storyPropsModule,
    '../../../../game/frontend/storyLoot.mjs': storyLootModule,
    '../../../../game/frontend/prophetDialogue.mjs': prophetDialogueModule,
    '../../../../game/frontend/storyCube.mjs': storyCubeModule,
    '../../../../game/frontend/storyWorldRules.mjs': storyWorldRulesModule,
    '../../../../game/frontend/storyProphetActions.mjs': storyProphetActionsModule,
    '../../../../game/frontend/playerController.mjs': { createPlayerController, VIEW_LABELS },
    '../../../../game/frontend/playerSave.mjs': { createPlayerSave },
    '../../../../game/frontend/storyGameplay.mjs': {
      ...gameplayModule,
      // Observing the real controller keeps the page's own wiring under test.
      createStoryGameplay: options => { gameplayOptions = options; return gameplayModule.createStoryGameplay(options); },
    },
    // 地图走真实模块（与 storyWorld.test.mjs 同一套桩：假时钟让「当帧确认」不必真的等 200ms）。
    '../../../../game/frontend/storyMap.mjs': { ...storyMapModule,
      ensureStoryMap: options => storyMapModule.ensureStoryMap({ ...options, wait: async () => {} }) },
    '../../../../game/frontend/storyCharacters.mjs': { STORY_CHARACTERS, PROPHET_GUID, PLAYER_MODEL_YAW_OFFSET,
      isMonsterRole, resolveStoryAssetPath, sceneSnapshot },
    // 这个夹具测的是页面/相机绑定：网格按「读不到」处理（清空并返回 null），
    // 因此玩家地面仍是固定地板；「有图贴地」那条链路由 game/tests/frontend/storyWorld.test.mjs 覆盖。
    '../../../../game/frontend/storyMapHeight.mjs': storyMapHeightModule,
    '../../../../game/frontend/storyMapHeightLoader.mjs': { mapHeightUrl,
      loadStoryMapHeight: async () => { storyMapHeightModule.clearHeightGrid(); return null; } },
    '@/utils/viewportStoryCamera.js': { createStoryCameraController: options => createStoryCameraController({
      ...options, now: () => time,
      requestFrame: callback => { frames.set(++id, callback); return id; }, cancelFrame: id => frames.delete(id),
    }) },
    // 小世界的相机（自由/俯视）也走真实模块：它由同一个 story camera controller 绑定。
    '@/utils/viewportStoryWorldCamera.js': { createStoryWorldControls, SMALL_WORLD_VIEW_LABELS },
    // 键盘路由是页面根的裁决，走真实模块，这样 `onKeyDown` 的分支在测试里与真机一致。
    '@/utils/storyKeyRouter.js': { routeStoryKeydown },
  }, window, document);
  const page = component.setup({}, { expose() {} });
  page.surface.value = { focus() {}, getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 600 }) };
  const mounting = Promise.all(mounted.map(fn => fn()));
  if (!pendingInit) await mounting;
  const cleanup = async () => { for (const fn of unmounted) fn(); await flushWorldSessionSaves(); assert.equal(listeners.size, 0); assert.equal(frames.size, 0); };
  t.after(cleanup);
  /**
   * 真机链路：浏览器把 keydown 交给**页面自己注册的那个监听器**
   * （`onMounted` 里的 `document.addEventListener('keydown', onKeyDown)`），而不是测试直接
   * 调用 `page.onKeyDown`。事件对象照浏览器给的形状造：`code`/`key`/`repeat`/`target`，并且
   * `preventDefault` / `stopPropagation` 会真的留下痕迹，于是「相机有没有吃掉这个键」可以断言。
   */
  const dispatch = (type, init = {}) => {
    const event = { type, repeat: false, ctrlKey: false, altKey: false, metaKey: false, shiftKey: false,
      isComposing: false, defaultPrevented: false, cancelBubble: false, keyCode: 0,
      preventDefault() { this.defaultPrevented = true; },
      stopPropagation() { this.cancelBubble = true; },
      stopImmediatePropagation() { this.cancelBubble = true; },
      target: { tagName: 'DIV', isContentEditable: false, closest: () => null },
      ...init };
    for (const [callback, name] of listeners) if (name === type) callback(event);
    return event;
  };
  return { page, calls, routes, frames, document, dispatch, worldModeState, mounting, sceneActors, placementExhibit,
    get gameplayOptions() { return gameplayOptions; },
    step(next) { time = next; const callbacks = [...frames.values()]; frames.clear(); for (const cb of callbacks) cb(time); } };
}

test('real story page binds player follow and stops on blur, hidden state, exit and revision changes', async t => {
  const h = await mountStory(t), p = h.page;
  p.onKeyDown(input({ code: 'KeyD' })); h.step(50);
  assert.ok(h.calls.some(call => call[0] === 'actorTransform' && call[2] === 0 && call[3][0] > 0));
  p.onBlur(); assert.equal(h.frames.size, 0);
  const count = h.calls.length;
  p.camera.wheel(input({ deltaY: -1 })); h.step(100); assert.equal(h.calls.length, count);
  p.onFocus(); p.onKeyDown(input({ code: 'KeyW' }));
  h.document.hidden = true; p.onVisibilityChange(); assert.equal(h.frames.size, 0);
  h.document.hidden = false; p.onFocus(); p.onKeyDown(input({ code: 'KeyW' }));
  h.worldModeState.revision++; h.step(150); assert.equal(h.calls.length, count);
  h.worldModeState.revision--;
  p.onKeyDown(input({ code: 'Escape', stopPropagation() {} }));
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(h.routes, ['/StartScreen']);
  p.camera.wheel(input({ deltaY: -1 })); h.step(200); assert.equal(h.calls.length, count + 1); // One final persisted player pose.
});

test('story Escape works during loading and late initialization cannot bind a camera', async t => {
  const pending = deferred(), h = await mountStory(t, { pendingInit: pending });
  h.page.onKeyDown(input({ code: 'Escape', stopPropagation() {} }));
  pending.resolve({ scenes: [{ path: 'scene.ini' }] }); await h.mounting;
  await new Promise(resolve => setImmediate(resolve));
  assert.deepEqual(h.routes, ['/StartScreen']); assert.equal(h.calls.length, 0);
});

test('story missing-camera error returns to launcher without editor actions', async t => {
  const h = await mountStory(t, { sceneSnapshot: { cameras: [] } });
  assert.deepEqual(h.routes, ['/StartScreen']);
  assert.deepEqual(h.calls, [['alert', '当前场景没有可用相机']]);
});

// ---- 占祈健四项功能：在新 UI 基线上重新接入后的回归 ----
const worldState = (overrides = {}) => ({ version: 2, revision: 0, rage: 0, boss: { hp: 200 },
  drop: null, inventory: { worldFragment: 0 }, ...overrides });
const enterWorld = (h, role, overrides) => {
  h.placementExhibit.role = role;
  h.gameplayOptions.onState({ state: worldState(overrides), config: gameplayConfig, role });
};
const hintKeys = p => p.controlHints.value.map(([key]) => key);

test('the HUD swaps world-scoped controls and re-labels the hints', async t => {
  const h = await mountStory(t), p = h.page;
  assert.equal(p.roleRef.value, 'main');
  assert.equal(p.worldLabel.value, '主世界');
  assert.equal(p.showCombatHud.value, true);
  assert.ok(hintKeys(p).includes('左键'));
  assert.ok(hintKeys(p).includes('V'), 'the view-switch key is advertised');

  enterWorld(h, 'child');
  assert.equal(p.roleRef.value, 'child');
  // The label carries the small world's own name, so the indicator and the bag agree.
  assert.ok(p.worldLabel.value.startsWith('小世界'), p.worldLabel.value);
  assert.equal(p.showCombatHud.value, false, 'the combat strip is hidden in a small world');
  const keys = hintKeys(p);
  assert.equal(keys.includes('左键'), false);
  assert.equal(keys.includes('E / R'), false);
  assert.equal(keys.includes('V'), true, 'the view switch still applies');
  assert.equal(p.controlHints.value.at(-1)[1], '保存离开');

  // A switch in flight must stay neutral instead of flipping twice.
  p.navigationPending.value = true;
  assert.equal(p.worldLabel.value, '切换中…');
  assert.equal(p.inSubworld.value, false);
  p.navigationPending.value = false;
  assert.ok(p.worldLabel.value.startsWith('小世界'));
});

test('V cycles the view and reports it in the hints', async t => {
  const h = await mountStory(t), p = h.page;
  const hint = () => p.controlHints.value.find(([key]) => key === 'V')[1];
  assert.equal(p.camera.viewMode(), 'third');
  assert.equal(hint(), '视角 · 第三人称');
  for (const [mode, label] of [['first', '降临视角'], ['top', '俯视建筑'], ['third', '第三人称']]) {
    p.onKeyDown(input({ code: 'KeyV', key: 'v' }));
    assert.equal(p.camera.viewMode(), mode);
    assert.equal(hint(), `视角 · ${label}`);
  }
  const held = p.camera.viewMode();
  p.onKeyDown(input({ code: 'KeyV', key: 'v', repeat: true }));
  assert.equal(p.camera.viewMode(), held, 'holding V must not race through the modes');
});

test('F talks to a nearby prophet and leaves the pickup path alone', async t => {
  const h = await mountStory(t), p = h.page;
  const prophet = () => h.sceneActors.find(actor => actor.actor_guid === PROPHET_GUID);
  const press = code => p.onKeyDown(input({ code }));
  const hint = () => p.controlHints.value.find(([key]) => key === 'F');

  // The main world has no prophet, so F keeps its original pickup meaning.
  enterWorld(h, 'main');
  assert.equal(p.fKeyAction.value.kind, null);
  press('KeyF');
  assert.equal(p.dialogueOpen.value, false);

  enterWorld(h, 'child');
  Object.assign(prophet().geometry, { position: [400, 0, 400] });
  p.updateProximity();
  assert.equal(p.fKeyAction.value.kind, null, 'a distant prophet is not reachable');
  assert.equal(hint(), undefined, 'no F line is promised when F would do nothing');
  press('KeyF');
  assert.equal(p.dialogueOpen.value, false);

  Object.assign(prophet().geometry, { position: [3, 0, 0] });
  p.updateProximity();
  assert.equal(p.fKeyAction.value.kind, 'prophet');
  assert.deepEqual(hint(), ['F', 'F 与先知交谈']);

  press('KeyF');
  assert.equal(p.dialogueOpen.value, true);
  assert.equal(p.dialogueLine.value, 0);
  // While talking the panel owns the keyboard.
  press('KeyW');
  assert.equal(p.camera.snapshotPlayer().movementState, 'idle');

  for (let i = 1; i < p.dialogue.lines.length; i++) {
    press('Enter');
    assert.equal(p.dialogueLine.value, i);
    assert.equal(p.dialogueOpen.value, true);
  }
  press('Enter');
  assert.equal(p.dialogueOpen.value, false, 'the last Enter ends the talk');
  assert.equal(p.fKeyAction.value.kind, 'prophet', 'reach is re-armed without moving');
  press('KeyF');
  press('Space');
  assert.equal(p.dialogueLine.value, 1);
  press('Escape');
  assert.equal(p.dialogueOpen.value, false, 'Escape closes without leaving the world');
  assert.equal(p.dialogueLine.value, 0);
});

test('only the small world places what the main world yielded', async t => {
  const h = await mountStory(t), p = h.page;
  // 主世界没有物品栏，也没有可摆放的陈列物；B 键不生效。
  enterWorld(h, 'main', { inventory: { worldFragment: 2, block: 4 } });
  assert.deepEqual(p.placements.value, []);
  assert.equal(p.hotbarVisible.value, false);
  await p.placeSelectedItem();
  assert.deepEqual(h.placementExhibit.placements, [], '主世界不许摆放');

  // 小世界：物品栏里的方块可以在身前落地，布局写入盘上并生成真实 actor。
  enterWorld(h, 'child', { inventory: { worldFragment: 2, block: 4 } });
  assert.equal(p.hotbarVisible.value, true);
  assert.equal(p.hotbarSlots.value[0].id, 'block');
  assert.equal(p.hotbarSlots.value[0].available, 4);
  await p.placeSelectedItem();
  assert.equal(p.placements.value.length, 1);
  assert.equal(p.placements.value[0].propId, 'block');
  assert.deepEqual(h.placementExhibit.placements.map(item => item.index), [0], 'the layout was persisted');
  assert.equal(p.hotbarSlots.value[0].available, 3);
  const actor = h.sceneActors.find(item => item.actor_guid === 'story.placement.0000');
  assert.ok(actor, 'the exhibit exists as a real scene actor');
  assert.equal(actor.name, '方块1');

  await p.placeSelectedItem();
  assert.equal(p.placements.value.length, 2);
  // 取回：站到方块旁边后 F 的裁决是「取回方块」，场景 actor 由同一条 reconcile 路径退场。
  const first = p.placements.value[0];
  p.camera.snapshotPlayer().position = [...first.position];
  p.updateProximity();
  assert.equal(p.fKeyAction.value.kind, 'block');
  await p.retrievePlacedBlock();
  assert.equal(p.placements.value.length, 1);
  assert.equal(h.sceneActors.some(item => item.actor_guid === 'story.placement.0000'), false);

  // Authoring is refused outside the small world.
  enterWorld(h, 'main', { inventory: { worldFragment: 2, block: 4 } });
  await p.placeSelectedItem();
  assert.equal(p.placements.value.length, 1);
});

test('小世界里空格经过页面根到达自由相机并上升，动作进行中也不被跳跃闸门吞掉', async t => {
  // 直接以 child 身份挂载：`bindPlayer` 一开始就把玩家绑到自由相机，与真机进入小世界同一条路。
  const h = await mountStory(t, { role: 'child' });
  const p = h.page;
  assert.equal(p.roleRef.value, 'child');
  assert.equal(p.camera.viewMode(), 'free', '小世界挂载即自由视角');
  const before = p.camera.snapshotPose().camera.position[1];
  // 主世界的跳跃闸门（actionBusy/busy/needsSave）挡的是「改写玩家存档」的空格；小世界的空格是
  // 相机升降、不碰存档，因此即使动作进行中也必须到达相机。这是改动前全套件都绿、真机却按不动的缺口。
  p.actionBusy.value = true;
  p.onKeyDown(input({ code: 'Space', key: ' ' }));
  h.step(50);
  const rose = p.camera.snapshotPose().camera.position[1] - before;
  assert.ok(Math.abs(rose - 0.6) < 1e-9, `空格抬 0.6 m / 50 ms，实际 ${rose}`);
  // 主世界对照：同一页面根、同一状态，空格仍走跳跃闸门（这里断言路由，不驱动跳跃侧效）。
  const mainRoute = routeStoryKeydown(input({ code: 'Space', key: ' ' }),
    { current: true, adjustMode: false, dialogueOpen: false, inSubworld: false,
      inventoryOpen: false, actionBusy: true, busy: false, needsSave: false });
  assert.equal(mainRoute, 'space-busy', '主世界忙碌时空格仍属于跳跃闸门');
});

test('小世界的空格经页面注册的真实 keydown 监听器上升（Q/E 同一条路，去掉 space 映射即红）', async t => {
  // 与真机同一条链路：`onMounted` 注册的 `document.addEventListener('keydown', onKeyDown)`
  // 是唯一入口，事件对象带 code/key/target，preventDefault 会留下痕迹。**不直接调用
  // `page.onKeyDown`** —— 上一版测试就是在这里把「页面根有没有把键交到相机手上」跳过去了。
  const h = await mountStory(t, { role: 'child' });
  const p = h.page;
  assert.equal(p.roleRef.value, 'child');
  assert.equal(p.camera.viewMode(), 'free', '小世界挂载即自由视角');
  const height = () => p.camera.snapshotPose().camera.position[1];
  // 主世界的跳跃闸门在同一页面根上开着；小世界的空格是相机升降，不能经过它。
  p.actionBusy.value = true;

  // Chromium 给空格的就是这两个值：`code: 'Space'`、`key: ' '`。
  const start = height();
  const down = h.dispatch('keydown', { code: 'Space', key: ' ' });
  h.step(50);
  const rose = height() - start;
  assert.ok(Math.abs(rose - 0.6) < 1e-9, `空格抬 0.6 m / 50 ms，实际 ${rose}`);
  assert.equal(down.defaultPrevented, true, '相机必须吃掉空格：页面不滚动、聚焦按钮不被激活');
  h.dispatch('keyup', { code: 'Space', key: ' ' });

  // 同一颗键从「只有 key/keyCode、没有 code」的宿主合成事件里进来也必须认：路由层
  // （storyKeyRouter 的 isSpace）三种写法都认，控制器不能只认 `code` 推出来的那一种。
  const beforeSynthetic = height();
  h.dispatch('keydown', { code: '', key: ' ', keyCode: 32 });
  h.step(100);
  const syntheticRise = height() - beforeSynthetic;
  assert.ok(Math.abs(syntheticRise - 0.6) < 1e-9, `没有 code 的空格同样抬 0.6 m，实际 ${syntheticRise}`);
  h.dispatch('keyup', { code: '', key: ' ', keyCode: 32 });

  // 对照：E 是 `down`，同一条页面根、同一台控制器、同一档速度。
  const beforeE = height();
  h.dispatch('keydown', { code: 'KeyE', key: 'e' });
  h.step(150);
  const dropped = height() - beforeE;
  assert.ok(Math.abs(dropped + 0.6) < 1e-9, `E 降 0.6 m，实际 ${dropped}`);
  h.dispatch('keyup', { code: 'KeyE', key: 'e' });
  assert.equal(p.camera.isInputActive(), false, '松开按键后没有键留在按下状态');
});
