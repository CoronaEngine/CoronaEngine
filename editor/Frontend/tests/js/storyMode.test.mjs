import test from 'node:test';
import assert from 'node:assert/strict';
import { createStoryPhysicsSystem } from '../../src/story/storyPhysicsSystem.js';
import { createWorldFragment } from '../../src/story/ugc/worldFragment.js';

test('story physics applies gravity and permits grounded jump only', () => {
  const player = { position: { x: 0, y: 1.7, z: 0 }, velocityY: 0, grounded: true, height: 1.7 };
  const physics = createStoryPhysicsSystem({ player });
  physics.jump();
  assert.equal(player.grounded, false);
  const velocityAfterJump = player.velocityY;
  physics.jump();
  assert.equal(player.velocityY, velocityAfterJump);
  physics.update(1);
  assert.equal(player.position.y, 1.7);
  assert.equal(player.grounded, true);
});

test('world fragment uses controlled logic data', () => {
  const fragment = createWorldFragment({ id: 'fragment-1', logic: { triggers: ['boss-defeated'], actions: ['unlock-demo'] } });
  assert.equal(fragment.id, 'fragment-1');
  assert.deepEqual(fragment.logic.actions, ['unlock-demo']);
  assert.equal(fragment.validation.valid, true);
});

import { calculateViewRelativeMovement, clampPitch } from '../../src/story/storyRuntime.js';

test('story movement follows current yaw and remains normalized', () => {
  const forward = calculateViewRelativeMovement({ x: 0, z: -1 }, 0);
  assert.deepEqual(forward, { x: 0, z: -1 });

  const turnedForward = calculateViewRelativeMovement({ x: 0, z: -1 }, Math.PI / 2);
  assert.ok(Math.abs(turnedForward.x - -1) < 1e-10);
  assert.ok(Math.abs(turnedForward.z) < 1e-10);

  const diagonal = calculateViewRelativeMovement({ x: 1, z: -1 }, 0);
  assert.ok(Math.abs(Math.hypot(diagonal.x, diagonal.z) - 1) < 1e-10);
});

test('story movement does not move for an empty axis', () => {
  assert.deepEqual(calculateViewRelativeMovement({ x: 0, z: 0 }, 1.2), { x: 0, z: 0 });
});

test('story camera pitch is clamped', () => {
  assert.equal(clampPitch(2), 1.45);
  assert.equal(clampPitch(-2), -1.45);
  assert.equal(clampPitch(0.5), 0.5);
});

import { createUgcWorldSession } from '../../src/story/ugc/ugcWorldSession.js';
import { createStoryPlayer } from '../../src/story/storyPlayer.js';

test('UGC world session isolates injected resources and restores main-world snapshot', () => {
  const session = createUgcWorldSession();
  const materials = [{ id: 'wood', quantity: 2 }];
  const fragments = [{ id: 'fragment-1' }];
  session.loadResources(materials, fragments);
  const result = session.enter({ position: { x: 1, z: 2 } });

  materials[0].quantity = 99;
  assert.equal(result.resources.materials[0].quantity, 2);
  assert.equal(session.getState().state, 'entered');
  assert.deepEqual(session.exit(), { position: { x: 1, z: 2 } });
});

test('story player can reset to its spawn state', () => {
  const { player, resetToSpawn } = createStoryPlayer({ spawn: [1, 1.7, 2] });
  player.position.set(9, 4, -3);
  player.velocityY = 5;
  player.grounded = false;
  resetToSpawn();
  assert.deepEqual(player.position.toArray(), [1, 1.7, 2]);
  assert.equal(player.velocityY, 0);
  assert.equal(player.grounded, true);
});
