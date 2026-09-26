import assert from 'node:assert/strict';
import test from 'node:test';
import { createPlayerController, PLAYER_CONTROLS } from '../../frontend/playerController.mjs';
import { createPlayerSave } from '../../frontend/playerSave.mjs';
import { actorFixture, apiFixture, deferred } from './fixtures.mjs';
export function controlFixture(options = {}) {
  const calls = [], frames = new Map(), errors = []; let id = 0, time = 0, current = true, locked = false;
  let pose = { handle: 12, position: [0,0,0], forward: [0,0,1], up: [0,1,0], fov: 60 };
  const bridge = { actorTransform: (...args) => { calls.push(['actor', ...args]); return true; } };
  const controller = createPlayerController({ getPose: () => pose, setPose: p => { pose = p; },
    submitPose: p => calls.push(['camera', structuredClone(p)]),
    getBridge: () => ({ ...bridge, cameraMove() {} }), onError: e => errors.push(e),
    isCurrent: () => current, isInputLocked: () => locked, ...options,
    now: () => time, requestFrame: cb => { frames.set(++id, cb); return id; }, cancelFrame: id => frames.delete(id) });
  const actor = actorFixture(); controller.bindPlayer(actor, 0.45);
  return { controller, calls, frames, actor, errors, bridge, get pose() { return pose; },
    current(value) { current = value; }, lock(value) { locked = value; },
    step(ms) { time += ms; const callbacks = [...frames.values()]; frames.clear(); callbacks.forEach(cb => cb(time)); } };
}
const key = code => ({ code, preventDefault() {} });
const near = (a,b) => assert.ok(Math.abs(a-b) < 1e-8, `${a} != ${b}`);
const length = value => Math.hypot(...value);
test('third-person starts behind player and movement/facing is camera relative', () => {
  const f = controlFixture();
  assert.ok(f.pose.position[2] < 0); assert.ok(f.pose.position[1] > f.actor.geometry.position[1]);
  f.controller.keyDown(key('KeyW')); f.step(50);
  const p = f.controller.snapshotPlayer(); near(p.position[2], 0.15); near(p.rotation[1], 0);
  near(p.position[1], f.actor.geometry.position[1]);
  assert.equal(f.calls.filter(c => c[0] === 'actor').length, 2);
  near(f.pose.position[2], 0.15 - Math.cos(PLAYER_CONTROLS.pitch) * 4.5);
  f.controller.dispose(); assert.equal(f.frames.size, 0);
});
test('diagonal movement is normalized and a stalled frame does not teleport', () => {
  const f = controlFixture(); f.controller.keyDown(key('KeyW')); f.controller.keyDown(key('KeyD'));
  f.step(5000); const p = f.controller.snapshotPlayer(); near(Math.hypot(p.position[0],p.position[2]), 0.15);
  near(p.rotation[1], Math.PI/4); f.controller.dispose();
});
test('right drag rotates the camera, clamps pitch, and determines WASD direction', () => {
  const f = controlFixture();
  assert.equal(f.controller.pointerDown({ button: 0 }), false);
  f.controller.pointerDown({ button: 2, pointerId: 1, clientX: 0, clientY: 0 });
  f.controller.pointerMove({ pointerId: 1, buttons: 2, clientX: Math.PI / 2 / 0.005, clientY: 10000 });
  f.step(16); near(f.pose.forward[1], -Math.sin(PLAYER_CONTROLS.maxPitch));
  f.controller.pointerUp({ pointerId: 1 }); f.controller.keyDown(key('KeyW')); f.step(50);
  const p = f.controller.snapshotPlayer(); near(p.position[0],0.15); near(p.position[2],0);
  f.controller.dispose();
});
test('wheel distance clamps without moving the actor', () => {
  const f = controlFixture();
  for (let i=0;i<20;i++) { f.controller.wheel({ deltaY: 10000 }); f.step(16); }
  const target = [0,f.actor.geometry.position[1]+0.45,0];
  near(length(f.pose.position.map((v,i)=>v-target[i])), 10);
  for (let i=0;i<20;i++) { f.controller.wheel({ deltaY: -10000 }); f.step(16); }
  near(length(f.pose.position.map((v,i)=>v-target[i])), 2.5);
  assert.equal(f.calls.filter(c=>c[0]==='actor').length,0); f.controller.dispose();
});
test('keyup, blur/reset, lock, session invalidation and disposal stop pending work', () => {
  for (const stop of [f=>f.controller.keyUp(key('KeyW')),f=>f.controller.resetInput(),f=>f.lock(true),f=>f.current(false),f=>f.controller.dispose()]) {
    const f=controlFixture(); f.controller.keyDown(key('KeyW')); const stale=[...f.frames.values()][0];
    stop(f); const count=f.calls.length; f.step(50); stale(100);
    assert.equal(f.calls.length,count); assert.equal(f.frames.size,0); f.controller.dispose();
  }
});
test('rebind fences late animation callbacks and editable targets do not move', () => {
  const f=controlFixture(); f.controller.keyDown(key('KeyW')); const stale=[...f.frames.values()][0];
  f.controller.bindPlayer(f.actor,0.45); const count=f.calls.length; stale(100);
  assert.equal(f.calls.length,count);
  assert.equal(f.controller.keyDown({code:'KeyW',target:{isContentEditable:true}}),false);
  assert.equal(f.controller.keyDown({code:'KeyW',ctrlKey:true}),false); f.controller.dispose();
});
test('bridge failure stops input with a visible error', () => {
  const f=controlFixture(); f.bridge.actorTransform=()=>false; f.controller.keyDown(key('KeyW')); f.step(50);
  assert.equal(f.errors.length,1); assert.equal(f.frames.size,0); f.controller.dispose();
});
test('movement uses no persistent API; save after disposal persists only final player pose', async () => {
  const f=controlFixture(), api=apiFixture({actors:[f.actor]});
  const saver=createPlayerSave({api:api.api,sceneId:'scene.ini',readPlayer:f.controller.snapshotPlayer,stopInput:f.controller.resetInput});
  await saver.save(); assert.equal(api.calls.length,0);
  f.controller.keyDown(key('KeyD')); f.step(50); f.controller.dispose();
  assert.equal(api.calls.length,0); await saver.save(); await saver.save();
  assert.equal(api.calls.length,1); near(api.state.actors[0].geometry.position[0],0.15);
});
test('failed save remains dirty; concurrent save calls share an in-flight write', async () => {
  const f=controlFixture(); f.controller.keyDown(key('KeyW')); f.step(50);
  let attempts=0, fail=true; const gate=deferred();
  const saver=createPlayerSave({api:{scene:{setActorTransform:async()=>{attempts++; await gate.promise;
    if(fail) throw new Error('disk full'); return {data:{status:'success'}};}}},
    sceneId:'old',readPlayer:f.controller.snapshotPlayer,stopInput:f.controller.resetInput});
  const first=saver.save(), second=saver.save(); gate.resolve();
  await assert.rejects(first,/disk full/); await assert.rejects(second,/disk full/); assert.equal(attempts,1);
  fail=false; await saver.save(); assert.equal(attempts,2); await saver.save(); assert.equal(attempts,2); f.controller.dispose();
});

test('a partially accepted movement frame remains saveable when rotation fails', async () => {
  const f = controlFixture();
  f.bridge.actorTransform = (_handle, operation) => operation === 0;
  f.controller.keyDown(key('KeyD')); f.step(50);
  near(f.controller.snapshotPlayer().position[0], 0.15);
  assert.equal(f.controller.snapshotPlayer().version, 1);
  assert.equal(f.frames.size, 0); assert.equal(f.errors.length, 1);
  const writes = [];
  const saver = createPlayerSave({ api: { scene: { setActorTransform: async (...args) => {
    writes.push(args); return { status: 'success' };
  } } }, sceneId: 'old.ini', readPlayer: f.controller.snapshotPlayer, stopInput: f.controller.resetInput });
  await saver.save(); near(writes[0][2].position[0], 0.15);
});

test('mouse turns without buttons; first sample and reset never jump', () => {
  const f = controlFixture(); const before = structuredClone(f.pose);
  f.controller.pointerMove({ clientX: 400, clientY: 300, buttons: 0 }); f.step(16);
  assert.deepEqual(f.pose, before);
  f.controller.pointerMove({ clientX: 450, clientY: 320, buttons: 0 }); f.step(16);
  assert.notDeepEqual(f.pose.forward, before.forward);
  f.controller.resetInput(); const saved = structuredClone(f.pose);
  f.controller.pointerMove({ clientX: 100, clientY: 100, buttons: 0 }); f.step(16);
  assert.deepEqual(f.pose, saved); f.controller.dispose();
});
test('edge turning continues without pointer events and stops on leave/lock', () => {
  const f = controlFixture({ getRect: () => ({ left: 0, top: 0, width: 800, height: 600 }) });
  f.controller.pointerMove({ clientX: 799, clientY: 300, buttons: 0 });
  f.step(50); const first = [...f.pose.forward]; f.step(50);
  assert.notDeepEqual(f.pose.forward, first); assert.equal(f.frames.size, 1);
  f.controller.keyDown(key('KeyW')); f.controller.keyUp(key('KeyW'));
  assert.equal(f.frames.size, 1);
  f.controller.pointerLeave(); assert.equal(f.frames.size, 0);
  const saved = structuredClone(f.pose);
  f.controller.pointerMove({ clientX: 400, clientY: 300, buttons: 0 }); f.step(16);
  assert.deepEqual(f.pose, saved);
  f.controller.pointerMove({ clientX: 799, clientY: 300, buttons: 0 }); f.lock(true); f.step(16);
  assert.equal(f.frames.size, 0); f.controller.dispose();
});
test('outside viewport cancels edge turning and detached callbacks', () => {
  const f = controlFixture({ getRect: () => ({ left: 0, top: 0, width: 800, height: 600 }) });
  f.controller.pointerMove({ clientX: 10, clientY: 10 });
  const stale = [...f.frames.values()][0];
  f.controller.pointerMove({ clientX: -1, clientY: 200 });
  const count = f.calls.length; stale(100);
  assert.equal(f.calls.length, count); assert.equal(f.frames.size, 0);
  f.controller.dispose();
});
