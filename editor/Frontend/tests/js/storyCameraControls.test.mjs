import assert from 'node:assert/strict';
import test from 'node:test';

import {
  applyStoryCameraGravity,
  applyStoryCameraMovement,
  clampStoryCameraPosition,
  dotVector,
  groundStoryCameraPose,
  isStoryCameraPoseUnsafe,
  normalizeStoryCameraKey,
  rotateStoryCamera,
  storyCameraGroundY,
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

test('detects upward-looking, over-height and outward-facing camera poses', () => {
  const worldBounds = [-60, -8, -60, 60, 20, 60];
  const options = { minimumY: 1.5, maximumY: 80, worldBounds };

  assert.equal(
    isStoryCameraPoseUnsafe(
      { position: [0, 9, 0], forward: [0, 0.7, 0.7], worldUp: [0, 1, 0] },
      options
    ),
    true
  );
  assert.equal(
    isStoryCameraPoseUnsafe(
      { position: [0, 81, 0], forward: [0, 0, 1], worldUp: [0, 1, 0] },
      options
    ),
    true
  );
  assert.equal(
    isStoryCameraPoseUnsafe(
      { position: [90, 9, 0], forward: [1, 0, 0], worldUp: [0, 1, 0] },
      options
    ),
    true
  );
  assert.equal(
    isStoryCameraPoseUnsafe(
      { position: [90, 9, 0], forward: [-1, -0.1, 0], worldUp: [0, 1, 0] },
      options
    ),
    false
  );
});

test('keeps the Yunxi Village spawn pose when it is in bounds and looks slightly downward', () => {
  assert.equal(
    isStoryCameraPoseUnsafe(
      {
        position: [-45, 9, -34],
        forward: [0.737, -0.123, 0.667],
        worldUp: [0, 1, 0],
      },
      {
        minimumY: 1.5,
        maximumY: 80,
        worldBounds: [-60, -8, -60, 60, 20, 60],
      }
    ),
    false
  );
});


test('applies frame-rate independent gravity until the camera reaches terrain height', () => {
  const falling = applyStoryCameraGravity(
    [0, 10, 0],
    0.1,
    0,
    new Set(),
    () => 0,
    1.6,
    null,
    9.8
  );
  closeTo(falling.position[1], 9.951);
  closeTo(falling.verticalVelocity, -0.98);
  assert.equal(falling.grounded, false);
  assert.equal(falling.moved, true);

  const landed = applyStoryCameraGravity(
    [0, 1.62, 0],
    0.1,
    -1,
    new Set(),
    () => 0,
    1.6,
    null,
    9.8
  );
  closeTo(landed.position[1], 1.6);
  assert.equal(landed.verticalVelocity, 0);
  assert.equal(landed.grounded, true);
});

test('suppresses gravity during manual Q/E movement and follows rising terrain', () => {
  const manual = applyStoryCameraGravity(
    [0, 6, 0],
    0.1,
    -3,
    new Set(['KeyQ']),
    () => 0,
    1.6,
    null,
    9.8
  );
  assert.deepEqual(manual.position, [0, 6, 0]);
  assert.equal(manual.verticalVelocity, 0);
  assert.equal(manual.moved, false);

  const risingGround = applyStoryCameraGravity(
    [0, 1.6, 0],
    0.016,
    0,
    new Set(),
    () => 4,
    1.6,
    null,
    9.8
  );
  assert.deepEqual(risingGround.position, [0, 5.6, 0]);
  assert.equal(risingGround.grounded, true);
});

test('uses the safe minimum Y fallback when no terrain sampler is available', () => {
  const result = applyStoryCameraGravity(
    [0, 6, 0],
    0.1,
    0,
    new Set(),
    null,
    1.6,
    { minY: 2 },
    9.8
  );
  closeTo(result.position[1], 5.951);
  assert.equal(result.grounded, false);
});


test('computes the grounded camera eye height from terrain plus 1.6 units', () => {
  closeTo(storyCameraGroundY([-42, 99, -27], () => 2.9918387566780678, 1.6), 4.591838756678068);
  closeTo(storyCameraGroundY([0, 99, 0], null, 1.6, 2.4), 2.4);
});

test('grounds hovering and underground camera poses while preserving valid orientation', () => {
  const source = {
    position: [4, 20, -3],
    forward: [0.3, -0.1, 0.95],
    worldUp: [0, 1, 0],
    fov: 52,
  };
  const hovering = groundStoryCameraPose(source, {
    terrainHeightAt: () => 2,
    groundOffset: 1.6,
    maximumHoverHeight: 3,
  });
  assert.deepEqual(hovering.pose.position, [4, 3.6, -3]);
  assert.deepEqual(hovering.pose.forward, source.forward.map((value) => value / vectorLength(source.forward)));
  assert.deepEqual(hovering.pose.worldUp, source.worldUp);
  assert.equal(hovering.pose.fov, 52);
  assert.deepEqual(hovering.reasons, ['hovering']);

  const underground = groundStoryCameraPose({ ...source, position: [4, -8, -3] }, {
    terrainHeightAt: () => 2,
    groundOffset: 1.6,
  });
  assert.deepEqual(underground.pose.position, [4, 3.6, -3]);
  assert.ok(underground.reasons.includes('below-ground'));
});

test('keeps a reasonable saved camera height and falls back from invalid pose vectors', () => {
  const reasonable = groundStoryCameraPose(
    { position: [1, 4.5, 2], forward: [0, 0, 1], worldUp: [0, 1, 0], fov: 45 },
    { terrainHeightAt: () => 2, groundOffset: 1.6, maximumHoverHeight: 3 }
  );
  assert.equal(reasonable.changed, false);
  assert.deepEqual(reasonable.pose.position, [1, 4.5, 2]);

  const fallback = groundStoryCameraPose(
    { position: [1, Number.NaN, 2], forward: [0, 0, 0], worldUp: [0, 0, 0] },
    {
      terrainHeightAt: () => 3,
      groundOffset: 1.6,
      fallbackPose: {
        position: [-42, 4.6, -27],
        forward: [0.7, -0.1, 0.7],
        worldUp: [0, 1, 0],
        fov: 48,
      },
    }
  );
  assert.deepEqual(fallback.pose.position, [-42, 4.6, -27]);
  assert.ok(fallback.reasons.includes('invalid-position'));
  assert.ok(fallback.reasons.includes('invalid-forward'));
  assert.ok(fallback.reasons.includes('invalid-world-up'));
});

test('manual E descent is clamped to the local terrain eye height', () => {
  const result = applyStoryCameraGravity(
    [5, -20, 7],
    0.1,
    -8,
    new Set(['KeyE']),
    () => 3,
    1.6,
    null,
    9.8
  );
  assert.deepEqual(result.position, [5, 4.6, 7]);
  assert.equal(result.verticalVelocity, 0);
  assert.equal(result.grounded, true);
});
