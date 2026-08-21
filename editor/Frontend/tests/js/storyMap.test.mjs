import assert from 'node:assert/strict';
import test from 'node:test';

import {
  buildStoryMapSnapshot,
  createStoryLocalMapBounds,
  normalizeStoryMapAabb,
  projectStoryWorldToMap,
  storyActorWorldPosition,
  storyMapMarkerFromActor,
  storyPlayerHeadingDegrees,
} from '../../src/utils/storyMap.js';

test('normalizes scene AABBs and computes actor world centers', () => {
  assert.deepEqual(normalizeStoryMapAabb([5, 2, 3, -1, -2, 9]), {
    min: [-1, -2, 3],
    max: [5, 2, 9],
  });
  assert.deepEqual(storyActorWorldPosition({ world_aabb: [0, 0, 2, 4, 6, 10] }), [2, 3, 6]);
  assert.deepEqual(storyActorWorldPosition({ geometry: { position: [7, 8, 9] } }), [7, 8, 9]);
});

test('projects X/Z coordinates with positive Z facing map north', () => {
  const bounds = { min: [0, 0, 0], max: [100, 10, 100] };
  assert.deepEqual(projectStoryWorldToMap([0, 0, 100], bounds), {
    x: 0,
    y: 0,
    outOfBounds: false,
  });
  assert.deepEqual(projectStoryWorldToMap([100, 0, 0], bounds), {
    x: 100,
    y: 100,
    outOfBounds: false,
  });
  assert.equal(storyPlayerHeadingDegrees([0, 0, 1]), 0);
  assert.equal(storyPlayerHeadingDegrees([1, 0, 0]), 90);
});

test('clamps an out-of-bounds player marker without changing map bounds', () => {
  const bounds = { min: [0, 0, 0], max: [10, 2, 10] };
  assert.deepEqual(projectStoryWorldToMap([20, 0, 5], bounds, { clamp: true }), {
    x: 100,
    y: 50,
    outOfBounds: true,
  });
});

test('filters unsupported actors and assigns semantic marker kinds', () => {
  assert.equal(storyMapMarkerFromActor({ actor_type: 'audio', geometry: { position: [0, 0, 0] } }), null);
  assert.equal(storyMapMarkerFromActor({ follow_camera: true, geometry: { position: [0, 0, 0] } }), null);
  assert.equal(
    storyMapMarkerFromActor({ name: 'Goal', semantic_role: 'quest_target', geometry: { position: [1, 0, 2] } }).kind,
    'quest'
  );
});

test('builds a map from native scene bounds and actor snapshots', () => {
  const result = buildStoryMapSnapshot({
    scene: 'scene.ini',
    scene_name: 'Story World',
    scene_aabb: [0, 0, 0, 10, 5, 20],
    actors: [
      { name: 'Rock', actor_guid: 'rock-1', load_status: 'loaded', world_aabb: [1, 0, 2, 3, 2, 4] },
    ],
  });
  assert.equal(result.sceneName, 'Story World');
  assert.equal(result.boundsReady, true);
  assert.equal(result.markers.length, 1);
  assert.deepEqual(result.markers[0].position, [2, 1, 3]);
  assert.ok(result.bounds.min[0] < 0);
  assert.ok(result.bounds.max[2] > 20);
});

test('uses actor and player positions when native bounds are missing', () => {
  const result = buildStoryMapSnapshot(
    { actors: [{ name: 'OnlyActor', geometry: { position: [10, 0, 10] } }] },
    [20, 4, 30]
  );
  assert.equal(result.boundsReady, false);
  assert.ok(result.bounds.min[0] < 10);
  assert.ok(result.bounds.max[2] > 30);

  assert.deepEqual(createStoryLocalMapBounds([5, 2, 8], 40), {
    min: [-35, -38, -32],
    max: [45, 42, 48],
  });
});
