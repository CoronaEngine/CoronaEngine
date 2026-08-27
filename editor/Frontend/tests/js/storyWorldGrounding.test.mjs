import assert from 'node:assert/strict';
import test from 'node:test';

import {
  calculateStoryActorGroundingCorrection,
  createGroundedActorTransform,
  normalizeStoryGroundingAabb,
} from '../../src/utils/storyWorldGrounding.js';

const closeTo = (actual, expected, epsilon = 1e-6) => {
  assert.ok(Math.abs(actual - expected) <= epsilon, `${actual} is not close to ${expected}`);
};

const actor = ({ y = 0, minY = 0, maxY = 4 } = {}) => ({
  geometry: {
    position: [8, y, -6],
    rotation: [0, 35, 0],
    scale: [10, 10, 10],
  },
  world_aabb: [3, minY, -11, 13, maxY, -1],
});

test('normalizes supported Story World AABB response shapes', () => {
  assert.deepEqual(normalizeStoryGroundingAabb([3, 4, 5, -3, -4, -5]), [-3, -4, -5, 3, 4, 5]);
  assert.deepEqual(
    normalizeStoryGroundingAabb({ min: [-1, 2, -3], max: [4, 5, 6] }),
    [-1, 2, -3, 4, 5, 6]
  );
  assert.deepEqual(
    normalizeStoryGroundingAabb({ min_x: -1, min_y: 2, min_z: -3, max_x: 4, max_y: 5, max_z: 6 }),
    [-1, 2, -3, 4, 5, 6]
  );
  assert.equal(normalizeStoryGroundingAabb({ min: [0, 0, 0] }), null);
});

test('moves a floating actor down to the sampled terrain surface', () => {
  const result = calculateStoryActorGroundingCorrection({
    actor: actor({ y: 7, minY: 7, maxY: 11 }),
    definition: { groundingMode: 'terrain' },
    terrainHeightAt: () => 2.5,
  });
  assert.equal(result.valid, true);
  closeTo(result.targetMinY, 2.5);
  closeTo(result.correctionY, -4.5);
  assert.equal(result.grounded, false);
});

test('moves an underground actor up and applies the house foundation offset', () => {
  const result = calculateStoryActorGroundingCorrection({
    actor: actor({ y: -2, minY: -2, maxY: 2 }),
    definition: { groundingMode: 'terrain', groundingOffset: -0.04 },
    terrainHeightAt: () => 1.2,
  });
  closeTo(result.targetMinY, 1.16);
  closeTo(result.correctionY, 3.16);
});

test('uses per-position terrain samples and leaves already grounded actors unchanged', () => {
  const terrainHeightAt = (x, z) => x * 0.1 + z * 0.05;
  const groundHeight = terrainHeightAt(8, -6);
  const result = calculateStoryActorGroundingCorrection({
    actor: actor({ y: groundHeight, minY: groundHeight, maxY: groundHeight + 4 }),
    definition: { groundingMode: 'terrain' },
    terrainHeightAt,
    threshold: 0.03,
  });
  closeTo(result.correctionY, 0);
  assert.equal(result.grounded, true);
});

test('uses the baked road target and fixed water level instead of local terrain height', () => {
  const road = calculateStoryActorGroundingCorrection({
    actor: actor({ y: 0, minY: -0.7, maxY: 3 }),
    definition: { groundingMode: 'road', groundingTargetHeight: -1.3 },
    terrainHeightAt: () => 9,
  });
  closeTo(road.targetMinY, -1.3);
  closeTo(road.correctionY, -0.6);

  const water = calculateStoryActorGroundingCorrection({
    actor: actor({ y: -0.6, minY: -0.6, maxY: -0.59 }),
    definition: { groundingMode: 'water' },
    terrainHeightAt: () => 9,
    waterY: -0.85,
  });
  closeTo(water.targetMinY, -0.85);
  closeTo(water.correctionY, -0.25);
});

test('creates a transform that changes only the actor Y position', () => {
  const source = actor({ y: 5, minY: 5, maxY: 9 });
  const transform = createGroundedActorTransform(source, -2.25);
  assert.deepEqual(transform.position, [8, 2.75, -6]);
  assert.deepEqual(transform.rotation, source.geometry.rotation);
  assert.deepEqual(transform.scale, source.geometry.scale);
});
