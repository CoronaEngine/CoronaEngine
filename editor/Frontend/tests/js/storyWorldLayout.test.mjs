import assert from 'node:assert/strict';
import test from 'node:test';

import { STORY_MONSTER_DEFINITIONS } from '../../src/config/storyCombat.js';
import {
  isStoryWorldLakePoint,
  STORY_WORLD_ACTORS,
  STORY_WORLD_DEPRECATED_ACTORS,
  STORY_WORLD_LAKE,
  storyWorldDistanceToRoad,
  storyWorldFootprintDistance,
  storyWorldFootprintDistanceToRoad,
  storyWorldTerrainHeight,
} from '../../src/config/storyWorld.js';

const closeTo = (actual, expected, epsilon = 1e-6) => {
  assert.ok(Math.abs(actual - expected) <= epsilon, `${actual} is not close to ${expected}`);
};

test('v5 village uses one road network and keeps the lake east of the courtyard groups', () => {
  const roads = STORY_WORLD_ACTORS.filter((definition) => definition.entityType === 'road');
  assert.deepEqual(roads.map((definition) => definition.name), ['StoryWorld_RoadNetwork']);
  assert.equal(roads[0].asset, 'road_network_v5.obj');
  assert.ok(
    STORY_WORLD_DEPRECATED_ACTORS.every(
      (name) => !STORY_WORLD_ACTORS.some((definition) => definition.name === name)
    )
  );

  const lake = STORY_WORLD_ACTORS.find((definition) => definition.entityType === 'water');
  assert.deepEqual([lake.position[0], lake.position[2]], STORY_WORLD_LAKE.center);
  assert.ok(lake.position[0] >= 30, 'the lake should stay in the east basin');

  const houses = STORY_WORLD_ACTORS.filter((definition) => definition.entityType === 'building');
  assert.ok(houses.length >= 7);
  assert.ok(houses.every((house) => house.position[0] >= -30 && house.position[0] <= 12));
  assert.ok(houses.every((house) => house.position[2] >= -12 && house.position[2] <= 40));
});

test('building footprints are separated from each other, the road edge and the lake', () => {
  const houses = STORY_WORLD_ACTORS.filter((definition) => definition.entityType === 'building');
  for (let firstIndex = 0; firstIndex < houses.length; firstIndex += 1) {
    const first = houses[firstIndex];
    assert.ok(
      storyWorldFootprintDistanceToRoad(first) >= 1.5,
      `${first.name} is too close to the road edge`
    );
    const waterMargin = Math.max(...first.footprint) * 0.5;
    assert.equal(
      isStoryWorldLakePoint(first.position[0], first.position[2], waterMargin),
      false,
      `${first.name} overlaps the lake basin`
    );
    for (let secondIndex = firstIndex + 1; secondIndex < houses.length; secondIndex += 1) {
      const second = houses[secondIndex];
      assert.ok(
        storyWorldFootprintDistance(first, second) >= 3,
        `${first.name} overlaps or crowds ${second.name}`
      );
    }
  }
});

test('trees and monsters stay off roads, buildings and open water', () => {
  const buildings = STORY_WORLD_ACTORS.filter((definition) => definition.entityType === 'building');
  const trees = STORY_WORLD_ACTORS.filter(
    (definition) => definition.semanticRole === 'vegetation_tree'
  );
  for (const tree of trees) {
    assert.ok(
      storyWorldFootprintDistanceToRoad(tree) >= 2,
      `${tree.name} is too close to a road`
    );
    assert.equal(
      isStoryWorldLakePoint(tree.position[0], tree.position[2], 2),
      false,
      `${tree.name} is inside the lake`
    );
    for (const building of buildings) {
      assert.ok(
        storyWorldFootprintDistance(tree, building) >= 2,
        `${tree.name} is too close to ${building.name}`
      );
    }
  }

  for (const monster of STORY_MONSTER_DEFINITIONS) {
    assert.equal(isStoryWorldLakePoint(monster.position[0], monster.position[2], 2), false);
    assert.ok(storyWorldDistanceToRoad(monster.position[0], monster.position[2]) >= 4);
    closeTo(
      monster.position[1],
      storyWorldTerrainHeight(monster.position[0], monster.position[2])
    );
  }
});

test('all placed v5 assets share the terrain height function and grounded contact offsets', () => {
  const explicitWaterActors = new Set([
    'StoryWorld_YunxiLake',
    'StoryWorld_Reeds_West',
    'StoryWorld_Reeds_East',
  ]);
  for (const definition of STORY_WORLD_ACTORS) {
    if (
      definition.name === 'StoryWorld_Terrain' ||
      definition.name === 'StoryWorld_RoadNetwork'
    ) {
      continue;
    }
    if (explicitWaterActors.has(definition.name)) {
      const expected =
        definition.name === 'StoryWorld_YunxiLake'
          ? STORY_WORLD_LAKE.waterY
          : STORY_WORLD_LAKE.waterY + 0.01;
      closeTo(definition.position[1], expected);
      continue;
    }
    const groundHeight = storyWorldTerrainHeight(definition.position[0], definition.position[2]);
    assert.ok(
      Math.abs(definition.position[1] - groundHeight) <= 0.051,
      `${definition.name} is not grounded by the shared terrain sampler`
    );
  }
});
