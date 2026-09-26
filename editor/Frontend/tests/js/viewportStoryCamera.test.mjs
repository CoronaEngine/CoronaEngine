import assert from 'node:assert/strict';
import test from 'node:test';
import { createStoryCameraController } from '../../src/utils/viewportStoryCamera.js';

function fixture() {
  const calls = [], frames = new Map(); let id = 0;
  let rect = { left: 0, top: 0, width: 800, height: 600 }, pixelRatio = 1;
  const bridge = Object.fromEntries(['cameraMove', 'setCameraViewport', 'setViewportGizmoTarget', 'setViewportUiMode', 'setViewportSystemCursorHidden'].map((name) => [name, (...args) => { calls.push([name, ...args]); return true; }]));
  const controller = createStoryCameraController({ getBridge: () => bridge, getRect: () => rect,
    getPixelRatio: () => pixelRatio, now: () => 0, requestFrame: (cb) => { frames.set(++id, cb); return id; }, cancelFrame: (id) => frames.delete(id) });
  const bind = (handle = 12) => controller.bind({ data: { scene: { active_camera_name: 'active', cameras: [
    { handle: 99, name: 'other' }, { handle, name: 'active', position: [0, 0, 0], forward: [0, 0, 1], world_up: [0, 1, 0], fov: 60 },
  ] } } }, 'scene.ini');
  const step = (time) => { const entry = frames.entries().next().value; if (entry) { frames.delete(entry[0]); entry[1](time); } };
  const lastMove = () => calls.filter(([name]) => name === 'cameraMove').at(-1);
  return { controller, calls, frames, bind, step, lastMove, resize: (r, p) => { rect = r; pixelRatio = p; } };
}
const event = (props) => ({ preventDefault() {}, ...props });

test('story binds the active camera, clears editor gizmos, and does not edit scene data', () => {
  const f = fixture(); f.bind();
  assert.deepEqual(f.calls[0], ['setViewportGizmoTarget', 12, 'scene.ini', '', 0]);
  assert.deepEqual(f.calls.at(-1), ['setCameraViewport', 12, 0, 0, 800, 600, 800, 600]);
  assert.throws(() => f.controller.bind({ cameras: [] }, 'scene.ini'), /没有可用相机/);
});

test('native snapshots keep the scene route separate from camera data', () => {
  const native = { status: 'success', scene: 'Scene/default.scene', active_camera_name: 'main',
    cameras: [{ handle: 99, name: 'other' }, { handle: 12, name: 'main', position: [1, 2, 3],
      forward: [0, 0, 1], world_up: [0, 1, 0], fov: 75 }] };
  for (const payload of [native, { data: native }, { scene: native }, { data: { scene: native } }]) {
    const f = fixture();
    assert.equal(f.controller.bind(payload, native.scene), true);
    assert.deepEqual(f.calls[0], ['setViewportGizmoTarget', 12, native.scene, '', 0]);
    f.controller.wheel(event({ deltaY: -1 })); f.step(16);
    assert.deepEqual(f.lastMove(), ['cameraMove', 12, [1, 2, 3.2], [0, 0, 1], [0, 1, 0], 75]);
    f.controller.dispose();
  }
});

test('resize honors DPI and caps render pixels while leaving display geometry full size', () => {
  const f = fixture(); f.bind(); f.resize({ left: 10, top: 20, width: 1920, height: 1080 }, 2);
  f.controller.syncViewport();
  assert.deepEqual(f.calls.at(-1), ['setCameraViewport', 12, 20, 40, 3840, 2160, 1920, 1080]);
  f.resize({ width: 0, height: 0 }, 1); const count = f.calls.length;
  assert.equal(f.controller.syncViewport(), false); assert.equal(f.calls.length, count);
});

test('rebind and disposal cancel animation and prevent late input from reusing camera handles', () => {
  const f = fixture(); f.bind(); f.controller.keyDown(event({ code: 'KeyW' }));
  f.bind(25); assert.equal(f.frames.size, 0);
  f.controller.wheel(event({ deltaY: -1 })); f.step(16); assert.equal(f.lastMove()[1], 25);
  f.controller.keyDown(event({ code: 'KeyW' })); f.controller.dispose();
  const count = f.calls.length;
  f.controller.keyDown(event({ code: 'KeyW' })); f.controller.wheel(event({ deltaY: -1 })); f.step(100);
  assert.equal(f.controller.syncViewport(), false); assert.equal(f.bind(), false);
  assert.equal(f.frames.size, 0); assert.equal(f.calls.length, count);
});

test('invalid rebind drops the old handle and cancels already queued callbacks', () => {
  const f = fixture(); f.bind(); f.controller.keyDown(event({ code: 'KeyW' }));
  const late = [...f.frames.values()][0];
  assert.throws(() => f.controller.bind({ cameras: [] }, 'missing.ini'), /没有可用相机/);
  const count = f.calls.length;
  late(50);
  f.controller.wheel(event({ deltaY: -1 })); f.step(100);
  assert.equal(f.calls.length, count);
  assert.equal(f.frames.size, 0);
  assert.equal(f.controller.syncViewport(), false);
});

test('late callbacks cannot move a newly bound camera', () => {
  const f = fixture(); f.bind(); f.controller.keyDown(event({ code: 'KeyW' }));
  const late = [...f.frames.values()][0];
  f.bind(25);
  f.controller.keyDown(event({ code: 'KeyD' }));
  late(50);
  assert.equal(f.lastMove(), undefined);
  assert.equal(f.frames.size, 1);
  f.step(50);
  assert.equal(f.lastMove()[1], 25);
  assert.ok(Math.abs(f.lastMove()[2][0] - 0.6) < 1e-8);
  assert.deepEqual(f.lastMove()[2].slice(1), [0, 0]);
});
