import assert from 'node:assert/strict';

import {
  createViewportUiCursorController,
  createViewportUiModeStore,
  createViewportUiPointerController,
  DEFAULT_NATIVE_VIEWPORT_CURSOR_ENABLED,
  isNativeViewportCursorEnabled,
  normalizeViewportUiCursorShape,
  normalizeFeatureSwitch,
  normalizeViewportUiMode,
  NATIVE_VIEWPORT_CURSOR_STORAGE_KEY,
} from './viewportUiMode.js';

const storage = new Map();
const store = createViewportUiModeStore({
  storage: {
    getItem: (key) => storage.get(key) ?? null,
    setItem: (key, value) => storage.set(key, value),
  },
});

assert.equal(normalizeViewportUiMode('stereo3d'), 'stereo3d');
assert.equal(normalizeViewportUiMode('unknown'), 'flat2d');
assert.equal(DEFAULT_NATIVE_VIEWPORT_CURSOR_ENABLED, false);
assert.equal(normalizeFeatureSwitch('on'), true);
assert.equal(normalizeFeatureSwitch('off'), false);
assert.equal(isNativeViewportCursorEnabled({
  storage: { getItem: () => null },
  globalObject: {},
}), false);
assert.equal(isNativeViewportCursorEnabled({
  storage: { getItem: (key) => (key === NATIVE_VIEWPORT_CURSOR_STORAGE_KEY ? 'on' : null) },
  globalObject: {},
}), true);
assert.equal(isNativeViewportCursorEnabled({
  storage: { getItem: () => null },
  globalObject: { __CORONA_ENABLE_NATIVE_VIEWPORT_CURSOR__: true },
}), true);

const mainKey = store.keyFor({ scope: 'main', sceneId: 'demo', cameraHandle: 101 });
const cameraKey = store.keyFor({ scope: 'camera', sceneId: 'demo', cameraId: 'shotA' });
assert.notEqual(mainKey, cameraKey);

assert.equal(store.get({ scope: 'main', sceneId: 'demo', cameraHandle: 101 }), 'flat2d');
store.set({ scope: 'main', sceneId: 'demo', cameraHandle: 101 }, 'stereo3d');
store.set({ scope: 'camera', sceneId: 'demo', cameraId: 'shotA' }, 'flat2d');
assert.equal(store.get({ scope: 'main', sceneId: 'demo', cameraHandle: 101 }), 'stereo3d');
assert.equal(store.get({ scope: 'camera', sceneId: 'demo', cameraId: 'shotA' }), 'flat2d');

const bridgeCalls = [];
store.applyToBridge({
  bridge: { setViewportUiMode: (...args) => bridgeCalls.push(args) },
  cameraHandle: 101,
  mode: 'stereo3d',
});
assert.deepEqual(bridgeCalls, [[101, 'stereo3d']]);

const bodyStyle = { cursor: 'default' };
const cursor = createViewportUiCursorController({
  getTarget: () => ({ style: bodyStyle }),
});
cursor.set('grab');
assert.equal(bodyStyle.cursor, 'grab');
cursor.set('grabbing');
assert.equal(bodyStyle.cursor, 'grabbing');
cursor.restore();
assert.equal(bodyStyle.cursor, 'default');
cursor.set('crosshair');
cursor.cancel();
assert.equal(bodyStyle.cursor, 'default');

assert.equal(normalizeViewportUiCursorShape('pointer'), 'hand');
assert.equal(normalizeViewportUiCursorShape('none'), 'hidden');
assert.equal(normalizeViewportUiCursorShape('bogus'), 'arrow');

const pointerCalls = [];
const pointerController = createViewportUiPointerController({
  getBridge: () => ({ viewportUiPointer: (...args) => pointerCalls.push(args) }),
  getCameraHandle: () => 777,
  getNativeCursorEnabled: () => true,
  getHitRect: () => ({ left: 10, top: 20, width: 200, height: 100 }),
  getRenderRect: () => ({
    left: 10,
    top: 20,
    width: 200,
    height: 100,
    renderWidth: 800,
    renderHeight: 400,
  }),
});
assert.equal(pointerController.send({
  type: 'pointermove',
  clientX: 60,
  clientY: 70,
  buttons: 1,
  shiftKey: true,
}, undefined, 'pointer'), true);
assert.deepEqual(pointerCalls.at(-1), [777, 'pointermove', 200, 200, 1, 2, 'hand']);

pointerController.hide();
assert.deepEqual(pointerCalls.at(-1), [777, 'pointerleave', 200, 200, 0, 0, 'hidden']);

assert.equal(pointerController.send({
  type: 'pointermove',
  clientX: 0,
  clientY: 0,
}, undefined, 'crosshair'), false);
assert.deepEqual(pointerCalls.at(-1), [777, 'pointerleave', 200, 200, 0, 0, 'hidden']);

const gatedPointerCalls = [];
const gatedCursorCalls = [];
let pointerEnabled = true;
const gatedPointerController = createViewportUiPointerController({
  getBridge: () => ({
    viewportUiPointer: (...args) => gatedPointerCalls.push(args),
    setViewportSystemCursorHidden: (...args) => gatedCursorCalls.push(args),
  }),
  getCameraHandle: () => 888,
  getEnabled: () => pointerEnabled,
  getNativeCursorEnabled: () => true,
  getHitRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
  getRenderRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
});
assert.equal(gatedPointerController.send({
  type: 'pointermove',
  clientX: 20,
  clientY: 30,
}, undefined, 'arrow'), true);
assert.deepEqual(gatedPointerCalls.at(-1), [888, 'pointermove', 20, 30, 0, 0, 'arrow']);
assert.deepEqual(gatedCursorCalls, [[true, true]]);
pointerEnabled = false;
assert.equal(gatedPointerController.send({
  type: 'pointermove',
  clientX: 21,
  clientY: 31,
}, undefined, 'arrow'), false);
assert.deepEqual(gatedPointerCalls.at(-1), [888, 'pointerleave', 20, 30, 0, 0, 'hidden']);
assert.deepEqual(gatedCursorCalls, [[true, true], [false, false]]);
const callsAfterDisableHide = gatedPointerCalls.length;
assert.equal(gatedPointerController.hide(), false);
assert.deepEqual(gatedPointerCalls.at(-1), [888, 'pointerleave', 20, 30, 0, 0, 'hidden']);
assert.deepEqual(gatedCursorCalls, [[true, true], [false, false]]);
const callsAfterExplicitHide = gatedPointerCalls.length;
assert.equal(gatedPointerController.send({
  type: 'pointermove',
  clientX: 22,
  clientY: 32,
}, undefined, 'arrow'), false);
assert.equal(gatedPointerCalls.length, callsAfterExplicitHide);
assert.equal(callsAfterExplicitHide, callsAfterDisableHide);
assert.deepEqual(gatedCursorCalls, [[true, true], [false, false]]);

const disabledPointerCalls = [];
const disabledCursorCalls = [];
const disabledPointerController = createViewportUiPointerController({
  getBridge: () => ({
    viewportUiPointer: (...args) => disabledPointerCalls.push(args),
    setViewportSystemCursorHidden: (...args) => disabledCursorCalls.push(args),
  }),
  getCameraHandle: () => 999,
  getHitRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
  getRenderRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
});
assert.equal(disabledPointerController.send({
  type: 'pointermove',
  clientX: 10,
  clientY: 20,
}, undefined, 'arrow'), false);
assert.deepEqual(disabledPointerCalls, []);
assert.deepEqual(disabledCursorCalls, []);
console.log('viewport UI mode tests passed');
