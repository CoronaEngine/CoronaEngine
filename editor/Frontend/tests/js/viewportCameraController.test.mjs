import assert from 'node:assert/strict';
import test from 'node:test';
import { cameraMovementKey, createViewportCameraController } from '../../src/utils/viewportCameraController.js';
import { createStoryCameraController } from '../../src/utils/viewportStoryCamera.js';

const initialPose = () => ({ position: [0, 0, 0], forward: [0, 0, 1], up: [0, 1, 0], fov: 60 });
const event = (props = {}) => ({ preventDefault() {}, ...props });
const near = (actual, expected) => assert.ok(Math.abs(actual - expected) < 1e-8, `${actual} != ${expected}`);
function clock() {
  const frames = new Map(); let id = 0, time = 0;
  return {
    frames, now: () => time,
    requestFrame: (callback) => { frames.set(++id, callback); return id; },
    cancelFrame: (id) => frames.delete(id),
    step(next) {
      time = next;
      const callbacks = [...frames.values()]; frames.clear();
      for (const callback of callbacks) callback(time);
    },
  };
}
function fixture() {
  const timer = clock(), calls = [];
  let pose = initialPose(), speed = 0.2, current = true, locked = false;
  const controller = createViewportCameraController({
    getPose: () => pose, setPose: (next) => { pose = next; },
    getSpeed: () => speed, setSpeed: (next) => { speed = next; },
    isCurrent: () => current, isInputLocked: () => locked,
    submitPose: (next) => calls.push(structuredClone(next)), ...timer,
  });
  return { controller, calls, ...timer, pose: () => pose, speed: () => speed,
    lock: (value = true) => { locked = value; }, invalidate: () => { current = false; } };
}

test('six-axis movement and simultaneous inputs use creative-mode axes and frame time', () => {
  for (const [code, axis, sign] of [['KeyW', 2, 1], ['KeyS', 2, -1], ['KeyD', 0, 1], ['KeyA', 0, -1], ['KeyQ', 1, 1], ['KeyE', 1, -1]]) {
    const f = fixture(); f.controller.keyDown(event({ code })); f.step(50);
    near(f.pose().position[axis], 0.6 * sign);
    f.controller.keyUp({ code }); assert.equal(f.frames.size, 0);
  }
  const slow = fixture(), fast = fixture();
  for (const f of [slow, fast]) {
    for (const code of ['KeyW', 'KeyD', 'KeyQ']) f.controller.keyDown(event({ code }));
  }
  slow.step(100);
  for (let time = 10; time <= 100; time += 10) fast.step(time);
  fast.pose().position.forEach((value, i) => near(value, slow.pose().position[i]));
  const stalled = fixture(); stalled.controller.keyDown(event({ code: 'KeyW' })); stalled.step(10000);
  near(stalled.pose().position[2], 1.2);
});

test('arrow keys rotate at 120 degrees per second without translating', () => {
  for (const [code, axis, sign] of [['ArrowLeft', 0, -1], ['ArrowRight', 0, 1], ['ArrowUp', 1, 1], ['ArrowDown', 1, -1]]) {
    const f = fixture(); f.controller.keyDown(event({ code })); f.step(50);
    near(f.pose().forward[axis], sign * Math.sin(6 * Math.PI / 180));
    assert.deepEqual(f.pose().position, [0, 0, 0]);
  }
});

test('right drag uses 0.15 degrees per pixel, preserves pitch limits and coalesces updates', () => {
  const f = fixture();
  assert.equal(f.controller.pointerDown(event({ button: 0, clientX: 0, clientY: 0 })), false);
  f.controller.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
  f.controller.pointerMove({ clientX: 100, clientY: 0, buttons: 2 });
  near(f.pose().forward[0], Math.sin(15 * Math.PI / 180));
  for (let i = 1; i <= 100; i++) f.controller.pointerMove({ clientX: 100, clientY: i * 10, buttons: 2 });
  assert.ok(Math.abs(f.pose().forward[1]) <= 0.985);
  assert.equal(f.frames.size, 1); assert.equal(f.calls.length, 0);
  f.step(16); assert.equal(f.calls.length, 1);
  f.controller.pointerUp({ button: 0 }); assert.equal(f.controller.isLooking(), true);
  f.controller.pointerUp({ button: 2 }); assert.equal(f.controller.isLooking(), false);
  assert.equal(f.frames.size, 0);
});

test('wheel moves, Shift-wheel changes speed within bounds, and Ctrl no longer changes speed', () => {
  const f = fixture();
  f.controller.wheel(event({ deltaY: -100 })); near(f.pose().position[2], 0.2);
  f.controller.wheel(event({ deltaY: 100 })); near(f.pose().position[2], 0);
  f.step(16);
  f.controller.wheel(event({ deltaY: -100, shiftKey: true }));
  near(f.speed(), 0.22); near(f.pose().position[2], 0); assert.equal(f.frames.size, 0);
  f.controller.wheel(event({ deltaY: -100, ctrlKey: true }));
  near(f.pose().position[2], 0.22); near(f.speed(), 0.22);
  for (let i = 0; i < 200; i++) f.controller.wheel(event({ deltaY: -1, shiftKey: true }));
  near(f.speed(), 2);
  for (let i = 0; i < 200; i++) f.controller.wheel(event({ deltaY: 1, shiftKey: true }));
  near(f.speed(), 0.01);
  const before = structuredClone(f.pose()); f.controller.wheel(event({ deltaY: 0 }));
  assert.deepEqual(f.pose(), before);
});

test('panel moves and speed changes share the same motion implementation', () => {
  const f = fixture();
  f.controller.setSpeed(0.75); f.controller.move('forward'); near(f.pose().position[2], 0.75);
  f.controller.move('rotateRight'); near(f.pose().forward[0], Math.sin(2 * Math.PI / 180));
  assert.equal(f.controller.move('unknown'), false);
  near(f.controller.setSpeed(100), 2); near(f.controller.setSpeed(-1), 0.01);
  f.controller.setSpeed('invalid'); near(f.speed(), 0.01);
  f.lock(); near(f.controller.setSpeed(0.5), 0.5);
  assert.equal(f.controller.move('forward'), false);
});

test('key decoding supports code, legacy key fallback and text field filtering', () => {
  assert.equal(cameraMovementKey({ code: 'KeyW', key: '中' }), 'w');
  assert.equal(cameraMovementKey({ code: 'Unidentified', key: 'ArrowUp' }), 'arrowup');
  const f = fixture();
  assert.equal(f.controller.keyDown(event({ key: 'w', target: { closest: () => ({}) } })), false);
  assert.equal(f.controller.keyDown(event({ key: 'W' })), true);
  f.controller.keyUp({ key: 'w', target: { closest: () => ({}) } });
  assert.equal(f.frames.size, 0);
});

test('reset, locks, disposal and invalid sessions stop both movement and pending pose submissions', () => {
  for (const stop of [f => f.controller.resetInput(), f => f.lock(), f => f.controller.dispose(), f => f.invalidate()]) {
    const f = fixture();
    f.controller.keyDown(event({ code: 'KeyW' }));
    f.controller.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
    f.controller.pointerMove({ clientX: 20, clientY: 0, buttons: 2 });
    stop(f); f.step(100);
    assert.equal(f.calls.length, 0); assert.equal(f.frames.size, 0);
    assert.equal(f.controller.isInputActive(), false);
  }
});

test('stale frame callbacks cannot overwrite new input after reset', () => {
  const f = fixture(); f.controller.keyDown(event({ code: 'KeyW' }));
  const late = [...f.frames.values()][0];
  f.controller.resetInput(); f.controller.keyDown(event({ code: 'KeyD' }));
  late(50); assert.equal(f.calls.length, 0); assert.equal(f.frames.size, 1);
  f.step(50); near(f.pose().position[0], 0.6); assert.deepEqual(f.pose().position.slice(1), [0, 0]);
});

test('lost right button resets held keys and pending frames', () => {
  const f = fixture(); f.controller.keyDown(event({ code: 'KeyW' }));
  f.controller.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
  f.controller.pointerMove({ clientX: 20, clientY: 0, buttons: 0 });
  f.step(100); assert.equal(f.frames.size, 0); assert.equal(f.calls.length, 0);
});

test('story binding and creative host callbacks produce identical poses for identical inputs', () => {
  const creative = fixture(), timer = clock(), calls = [];
  const story = createStoryCameraController({
    getBridge: () => ({ cameraMove: (handle, position, forward, up, fov) => calls.push({ position, forward, up, fov }) }),
    getRect: () => ({ left: 0, top: 0, width: 800, height: 600 }), ...timer,
  });
  const pose = initialPose();
  story.bind({ cameras: [{ handle: 12, ...pose, world_up: pose.up }] }, 'scene.ini');
  for (const controller of [creative.controller, story]) {
    controller.keyDown(event({ code: 'KeyW' })); controller.keyDown(event({ code: 'ArrowRight' }));
    controller.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
    controller.pointerMove({ clientX: 60, clientY: -20, buttons: 2 });
    controller.wheel(event({ deltaY: -100, shiftKey: true }));
  }
  for (const time of [16, 33, 66, 1000]) { creative.step(time); timer.step(time); }
  for (const controller of [creative.controller, story]) {
    controller.keyUp({ code: 'KeyW' }); controller.keyUp({ code: 'ArrowRight' });
    controller.pointerUp({ button: 2 }); controller.wheel(event({ deltaY: -100 }));
  }
  creative.step(1016); timer.step(1016);
  assert.deepEqual(calls, creative.calls);
  story.dispose(); creative.controller.dispose();
});
