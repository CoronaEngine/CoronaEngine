import assert from 'node:assert/strict';
import test from 'node:test';

import {
  applyStoryCameraMovement,
  clampStoryCameraPosition,
  dotVector,
  isStoryCameraPoseUnsafe,
  normalizeStoryCameraKey,
  rotateStoryCamera,
  STORY_CAMERA_PITCH_DOT_LIMIT,
  vectorLength,
} from '../../src/utils/storyCameraControls.js';

const pose = {
  position: [0, 0, 0],
  forward: [0, 0, 1],
  worldUp: [0, 1, 0],
  moveSpeed: 12,
};

const closeTo = (actual, expected, epsilon = 1e-6) => {
  assert.ok(Math.abs(actual - expected) <= epsilon, `${actual} is not close to ${expected}`);
};

test('normalizes WASD/QE keyboard codes and rejects unrelated keys', () => {
  assert.equal(normalizeStoryCameraKey({ code: 'KeyW' }), 'KeyW');
  assert.equal(normalizeStoryCameraKey({ key: 'q' }), 'KeyQ');
  assert.equal(normalizeStoryCameraKey('E'), 'KeyE');
  assert.equal(normalizeStoryCameraKey({ code: 'Escape' }), '');
});

test('moves forward and backward along the current view direction', () => {
  const forward = applyStoryCameraMovement(pose, new Set(['KeyW']), 0.5);
  assert.equal(forward.moved, true);
  closeTo(forward.position[2], 1.2);

  const backward = applyStoryCameraMovement(pose, new Set(['KeyS']), 0.05);
  assert.deepEqual(backward.position, [0, 0, -0.6000000000000001]);
});

test('moves left/right and up/down using the left-handed camera basis', () => {
  assert.deepEqual(
    applyStoryCameraMovement(pose, new Set(['KeyD']), 0.1).position,
    [1.2000000000000002, 0, 0]
  );
  assert.deepEqual(
    applyStoryCameraMovement(pose, new Set(['KeyA']), 0.1).position,
    [-1.2000000000000002, 0, 0]
  );
  assert.deepEqual(
    applyStoryCameraMovement(pose, new Set(['KeyQ']), 0.1).position,
    [0, 1.2000000000000002, 0]
  );
  assert.deepEqual(
    applyStoryCameraMovement(pose, new Set(['KeyE']), 0.1).position,
    [0, -1.2000000000000002, 0]
  );
});

test('normalizes diagonal movement and clamps long frame deltas', () => {
  const result = applyStoryCameraMovement(pose, new Set(['KeyW', 'KeyD']), 1);
  closeTo(vectorLength(result.position), 1.2);
  closeTo(result.position[0], result.position[2]);
});

test('right mouse deltas apply yaw and pitch with editor sensitivity', () => {
  const yawed = rotateStoryCamera(pose.forward, pose.worldUp, 100, 0);
  assert.ok(yawed[0] > 0);
  closeTo(vectorLength(yawed), 1);

  const pitched = rotateStoryCamera(pose.forward, pose.worldUp, 0, -100);
  assert.ok(pitched[1] > 0);
  closeTo(vectorLength(pitched), 1);
});

test('pitch is clamped before the camera can flip over', () => {
  const pitched = rotateStoryCamera(pose.forward, pose.worldUp, 0, -5000);
  assert.ok(dotVector(pitched, pose.worldUp) <= STORY_CAMERA_PITCH_DOT_LIMIT + 1e-9);
  assert.ok(dotVector(pitched, pose.worldUp) > 0);

  const pitchedDown = rotateStoryCamera(pose.forward, pose.worldUp, 0, 5000);
  assert.ok(dotVector(pitchedDown, pose.worldUp) >= -STORY_CAMERA_PITCH_DOT_LIMIT - 1e-9);
  assert.ok(dotVector(pitchedDown, pose.worldUp) < 0);
});

test('managed Story World camera positions are clamped above terrain and below the flight ceiling', () => {
  assert.deepEqual(clampStoryCameraPosition([3, -9, 4], { minY: 1.5, maxY: 80 }), [3, 1.5, 4]);
  assert.deepEqual(clampStoryCameraPosition([3, 120, 4], { minY: 1.5, maxY: 80 }), [3, 80, 4]);

  const atGround = { ...pose, position: [0, 1.5, 0] };
  const descending = applyStoryCameraMovement(
    atGround,
    new Set(['KeyE']),
    0.1,
    atGround.moveSpeed,
    { minY: 1.5, maxY: 80 }
  );
  assert.deepEqual(descending.position, [0, 1.5, 0]);
  assert.equal(descending.moved, false);
});

test('detects invalid or underground camera poses without resetting valid positions', () => {
  const validOrientation = { forward: [0, 0, 1], worldUp: [0, 1, 0] };
  assert.equal(isStoryCameraPoseUnsafe({ ...validOrientation, position: [0, -29, 0] }, 1.5), true);
  assert.equal(
    isStoryCameraPoseUnsafe({ ...validOrientation, position: [0, Number.NaN, 0] }, 1.5),
    true
  );
  assert.equal(isStoryCameraPoseUnsafe({ ...validOrientation, position: [0, 9, 0] }, 1.5), false);
  assert.equal(
    isStoryCameraPoseUnsafe({ ...validOrientation, position: [0, 9, 0], forward: [0, 0, 0] }, 1.5),
    true
  );
});
