import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { createViewportGizmoController } from '../../src/utils/viewportGizmo.js';
import * as viewportGizmoModule from '../../src/utils/viewportGizmo.js';

const frontendRoot = join(dirname(fileURLToPath(import.meta.url)), '..', '..');

const pointerDownHandler = (source) => {
  const start = source.indexOf('const handleViewportPointerDown = (event) => {');
  const end = source.indexOf('\n};', start);
  assert.notEqual(start, -1, 'viewport pointerdown handler must exist');
  assert.notEqual(end, -1, 'viewport pointerdown handler must terminate');
  return source.slice(start, end);
};

test('both viewport surfaces capture the pointer synchronously on pointerdown', () => {
  for (const relativePath of ['src/views/layout/MainPage.vue', 'src/views/tools/CameraView.vue']) {
    const source = readFileSync(join(frontendRoot, relativePath), 'utf8');
    const handler = pointerDownHandler(source);
    assert.notEqual(
      handler.indexOf('setPointerCapture'),
      -1,
      `${relativePath} must capture pointerdown`
    );
    assert.ok(
      handler.indexOf('setPointerCapture') < handler.indexOf('viewportUiPointerController.send'),
      `${relativePath} must capture before forwarding other pointer work`
    );
    assert.ok(
      handler.indexOf('setPointerCapture') < handler.indexOf('viewportGizmoController.pointer'),
      `${relativePath} must capture before forwarding the gizmo pointerdown`
    );
  }
});

test('main viewport pointerdown does not refresh the scene camera binding', () => {
  const source = readFileSync(join(frontendRoot, 'src/views/layout/MainPage.vue'), 'utf8');
  const handler = pointerDownHandler(source);
  assert.equal(
    handler.includes('refreshSceneCameraBinding'),
    false,
    'scene camera binding must not refresh during the native pointerdown sequence'
  );
});

test('viewport pointercancel only cancels the active gizmo pointer', () => {
  for (const relativePath of ['src/views/layout/MainPage.vue', 'src/views/tools/CameraView.vue']) {
    const source = readFileSync(join(frontendRoot, relativePath), 'utf8');
    const start = source.indexOf('const handleViewportPointerCancel = (event) => {');
    const end = source.indexOf('\n};', start);
    assert.notEqual(start, -1, `${relativePath} pointercancel handler must exist`);
    assert.notEqual(end, -1, `${relativePath} pointercancel handler must terminate`);
    const handler = source.slice(start, end);
    assert.match(
      handler,
      /event\.pointerId\s*!==\s*gizmoDownPointerId/,
      `${relativePath} must ignore cancellation from another pointer`
    );
  }
});

test('main viewport pointerdown does not force a focus transition during drag start', () => {
  const source = readFileSync(join(frontendRoot, 'src/views/layout/MainPage.vue'), 'utf8');
  const handler = pointerDownHandler(source);
  assert.equal(
    handler.includes('focusViewportInput'),
    false,
    'pointerdown must not force a CEF focus transition while starting a drag'
  );
});

test('sets and clears the native gizmo target', () => {
  const calls = [];
  const bridge = {
    setViewportGizmoTarget: (...args) => calls.push(args),
  };
  const controller = createViewportGizmoController({
    getBridge: () => bridge,
    getCameraBinding: () => ({ cameraHandle: 11, sceneId: 'Scene/default.scene' }),
  });

  controller.setTarget({ handle: 22, name: 'Cube' });
  controller.clearTarget();

  assert.deepEqual(calls, [
    [11, 'Scene/default.scene', 'Cube', 22],
    [11, 'Scene/default.scene', '', 0],
  ]);
});

test('forwards viewport-local pointer coordinates and tracks consumed drag', () => {
  const calls = [];
  const bridge = {
    viewportGizmoPointer: (...args) => calls.push(args),
  };
  const controller = createViewportGizmoController({
    getBridge: () => bridge,
    getCameraBinding: () => ({ cameraHandle: 11, sceneId: 'Scene/default.scene' }),
    getHitRect: () => ({ left: 10, top: 20, width: 200, height: 100 }),
    getRenderRect: () => ({ left: 0, top: 0, width: 220, height: 140 }),
    makeRequestId: () => 'gizmo-1',
  });

  const requestId = controller.pointer(
    { clientX: 60, clientY: 70, button: 0, buttons: 1 },
    'pointerdown'
  );
  assert.equal(requestId, 'gizmo-1');
  assert.deepEqual(calls[0].slice(0, 7), [11, 'gizmo-1', 'pointerdown', 60, 70, 220, 140]);

  const result = controller.handleResult({
    requestId: 'gizmo-1',
    consumed: true,
    dragging: true,
    axis: 'x',
  });
  assert.equal(result.consumed, true);
  assert.equal(controller.isDragging(), true);
});

test('drag end is reported once for persistence', () => {
  const ended = [];
  const controller = createViewportGizmoController({
    getBridge: () => ({ viewportGizmoPointer() {} }),
    getCameraBinding: () => ({ cameraHandle: 11, sceneId: 'Scene/default.scene' }),
    getHitRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
    getRenderRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
    onDragEnd: (payload) => ended.push(payload),
    makeRequestId: () => 'gizmo-end',
  });
  controller.pointer({ clientX: 10, clientY: 10, button: 0, buttons: 1 }, 'pointerdown');
  controller.handleResult({ requestId: 'gizmo-end', consumed: true, dragging: true });
  controller.handleResult({ requestId: 'gizmo-end', consumed: true, ended: true });
  controller.handleResult({ requestId: 'gizmo-end', consumed: true, ended: true });
  assert.equal(ended.length, 1);
});

test('pointercancel forwards cancellation and clears the active drag once', () => {
  const calls = [];
  const cancelled = [];
  const controller = createViewportGizmoController({
    getBridge: () => ({ viewportGizmoPointer: (...args) => calls.push(args) }),
    getCameraBinding: () => ({ cameraHandle: 11, sceneId: 'Scene/default.scene' }),
    getHitRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
    getRenderRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
    onDragCancel: (payload) => cancelled.push(payload),
    makeRequestId: (() => {
      let index = 0;
      return () => `gizmo-cancel-${++index}`;
    })(),
  });

  const downRequest = controller.pointer(
    { clientX: 10, clientY: 10, button: 0, buttons: 1 },
    'pointerdown'
  );
  controller.handleResult({ requestId: downRequest, consumed: true, dragging: true, axis: 'x' });

  const cancelRequest = controller.cancel('pointercancel');
  assert.equal(calls.at(-1)[2], 'pointercancel');
  assert.equal(controller.isDragging(), true);

  const cancelPayload = { requestId: cancelRequest, cancelled: true, dragging: false, axis: 'x' };
  const result = controller.handleResult(cancelPayload);
  assert.equal(result.status, 'cancelled');
  assert.equal(controller.isDragging(), false);
  assert.deepEqual(cancelled, [cancelPayload]);

  assert.equal(controller.cancel('pointercancel'), false);
  assert.equal(cancelled.length, 1);
});

test('coalesces active drag moves to one animation frame', () => {
  const calls = [];
  const frames = [];
  let request = 0;
  const controller = createViewportGizmoController({
    getBridge: () => ({ viewportGizmoPointer: (...args) => calls.push(args) }),
    getCameraBinding: () => ({ cameraHandle: 11, sceneId: 'Scene/default.scene' }),
    getHitRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
    getRenderRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
    makeRequestId: () => `request-${++request}`,
    scheduleFrame: (callback) => frames.push(callback),
  });
  const down = controller.pointer(
    { clientX: 10, clientY: 10, button: 0, buttons: 1 },
    'pointerdown'
  );
  controller.handleResult({ requestId: down, consumed: true, dragging: true, axis: 'x' });
  controller.pointer({ clientX: 20, clientY: 10, buttons: 1 }, 'pointermove');
  controller.pointer({ clientX: 30, clientY: 10, buttons: 1 }, 'pointermove');

  assert.equal(calls.length, 1);
  assert.equal(frames.length, 1);
  frames[0]();
  assert.equal(calls.length, 2);
  assert.equal(calls[1][3], 30);
});

test('resolves the main viewport gizmo target from a successful pick', () => {
  assert.equal(
    typeof viewportGizmoModule.resolveViewportGizmoTarget,
    'function',
    'main viewport selection resolver must exist'
  );
  const target = viewportGizmoModule.resolveViewportGizmoTarget({
    sceneId: 'scene.ini',
    selection: { scene: 'scene.ini', actor: 'Ball', actor_type: 'model' },
    pickResult: { actor: { handle: 1176640039248, name: 'Ball', type: 'model' } },
    actorIndex: new Map(),
  });
  assert.deepEqual(target, {
    handle: 1176640039248,
    name: 'Ball',
    type: 'model',
  });
});

test('resolves a scene-tree selection through the actor index', () => {
  assert.equal(typeof viewportGizmoModule.resolveViewportGizmoTarget, 'function');
  const target = viewportGizmoModule.resolveViewportGizmoTarget({
    sceneId: 'scene.ini',
    selection: { scene: 'scene.ini', actor: 'Ball', actor_type: 'model' },
    actorIndex: new Map([[1176640039248, { name: 'Ball', type: 'model' }]]),
  });
  assert.equal(target?.handle, 1176640039248);
});

test('routes a gizmo selection only to its source viewport', () => {
  assert.equal(
    typeof viewportGizmoModule.isViewportGizmoSelectionOwner,
    'function',
    'viewport ownership resolver must exist'
  );

  const fromCameraView = {
    source_viewport: 'cameraView',
    source_camera_handle: 22,
  };
  assert.equal(
    viewportGizmoModule.isViewportGizmoSelectionOwner({
      viewportScope: 'main',
      cameraHandle: 11,
      selection: fromCameraView,
    }),
    false
  );
  assert.equal(
    viewportGizmoModule.isViewportGizmoSelectionOwner({
      viewportScope: 'cameraView',
      cameraHandle: 22,
      selection: fromCameraView,
    }),
    true
  );
  assert.equal(
    viewportGizmoModule.isViewportGizmoSelectionOwner({
      viewportScope: 'cameraView',
      cameraHandle: 33,
      selection: fromCameraView,
    }),
    false
  );

  assert.equal(
    viewportGizmoModule.isViewportGizmoSelectionOwner({
      viewportScope: 'main',
      cameraHandle: 11,
      selection: {},
    }),
    true
  );
  assert.equal(
    viewportGizmoModule.isViewportGizmoSelectionOwner({
      viewportScope: 'cameraView',
      cameraHandle: 22,
      selection: {},
    }),
    false
  );
});
