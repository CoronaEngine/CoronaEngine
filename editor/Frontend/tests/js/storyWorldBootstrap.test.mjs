import assert from 'node:assert/strict';
import test from 'node:test';

import {
  STORY_WORLD_ACTORS,
  STORY_WORLD_ASSET_METADATA,
  STORY_WORLD_DEPRECATED_ACTORS,
  STORY_WORLD_PLAN_ID,
  STORY_WORLD_SCENE_VERSION,
  STORY_WORLD_LAYOUT_REVISION,
  STORY_WORLD_TERRAIN_ACTOR,
  storyWorldExpectedSize,
  storyWorldFinalScale,
} from '../../src/config/storyWorld.js';
import {
  classifyStoryWorldScene,
  createStoryWorldMigrationActorData,
  missingStoryWorldActors,
  resolveStoryWorldAssetRoot,
  runStoryWorldBootstrap,
  storyWorldMigrationForActor,
  validateStoryWorldSnapshot,
} from '../../src/services/storyWorldBootstrapService.js';

const closeTo = (actual, expected, epsilon = 1e-6) => {
  assert.ok(Math.abs(actual - expected) <= epsilon, `${actual} is not close to ${expected}`);
};

const definitionByIdentity = (identity) => {
  const normalized = String(identity || '').toLowerCase();
  return STORY_WORLD_ACTORS.find(
    (definition) =>
      definition.name.toLowerCase() === normalized || definition.guid.toLowerCase() === normalized
  );
};

const actorScale = (actor, definition) => {
  const value = actor?.geometry?.scale ?? actor?.scale ?? storyWorldFinalScale(definition);
  return [0, 1, 2].map((index) => Number(value?.[index]) || 0);
};

const actorPosition = (actor, definition) => {
  const value = actor?.geometry?.position ?? actor?.position ?? definition.position;
  return [0, 1, 2].map((index) => Number(value?.[index]) || 0);
};

const actorRotation = (actor, definition) => {
  const value = actor?.geometry?.rotation ?? actor?.rotation ?? definition.rotation;
  return [0, 1, 2].map((index) => Number(value?.[index]) || 0);
};

const worldSizeForScale = (definition, scale) => {
  const normalizedSourceSize = definition.sourceSize.map(
    (value) => value / Math.max(definition.importScale, 1e-8)
  );
  return normalizedSourceSize.map((value, index) => Math.abs(value * scale[index]));
};

const sourceVerticalBounds = (definition) => {
  if (definition.name === 'StoryWorld_Terrain') return [-1.3, 15.102511];
  if (definition.name === 'StoryWorld_RoadNetwork') return [-1.265, 4.631842];
  return [0, Number(definition.sourceSize?.[1]) || 0];
};

const worldAabbForActor = (actor, definition) => {
  const position = actorPosition(actor, definition);
  const scale = actorScale(actor, definition);
  const size = worldSizeForScale(definition, scale);
  const [sourceMinY, sourceMaxY] = sourceVerticalBounds(definition);
  const normalizedMinY = sourceMinY / Math.max(definition.importScale, 1e-8);
  const normalizedMaxY = sourceMaxY / Math.max(definition.importScale, 1e-8);
  const scaledY0 = normalizedMinY * scale[1];
  const scaledY1 = normalizedMaxY * scale[1];
  const minY = position[1] + Math.min(scaledY0, scaledY1);
  const maxY = position[1] + Math.max(scaledY0, scaledY1);
  return [
    position[0] - size[0] * 0.5,
    minY,
    position[2] - size[2] * 0.5,
    position[0] + size[0] * 0.5,
    maxY,
    position[2] + size[2] * 0.5,
  ];
};

const storyActorSnapshot = (definition, overrides = {}) => {
  const geometry = {
    position: actorPosition(overrides, definition),
    rotation: actorRotation(overrides, definition),
    scale: actorScale(overrides, definition),
    ...(overrides.geometry || {}),
  };
  const actor = {
    name: definition.name,
    actor_guid: definition.guid,
    actor_type: 'model',
    source_plan_id: STORY_WORLD_PLAN_ID,
    source_scene_version: STORY_WORLD_SCENE_VERSION,
    source_layout_revision: STORY_WORLD_LAYOUT_REVISION,
    visible: true,
    ...overrides,
    geometry,
  };
  if (!Array.isArray(overrides.world_aabb)) {
    actor.world_aabb = worldAabbForActor(actor, definition);
  }
  return actor;
};

const unionAabbs = (actors) => {
  const aabbs = actors.map((actor) => actor.world_aabb).filter((aabb) => Array.isArray(aabb));
  if (aabbs.length === 0) return null;
  return aabbs.reduce(
    (result, aabb) => [
      Math.min(result[0], aabb[0]),
      Math.min(result[1], aabb[1]),
      Math.min(result[2], aabb[2]),
      Math.max(result[3], aabb[3]),
      Math.max(result[4], aabb[4]),
      Math.max(result[5], aabb[5]),
    ],
    [...aabbs[0]]
  );
};

function createMockApi({
  mode = 'story',
  actors = [],
  failActor = '',
  failSun = false,
  forceTinyBounds = false,
  failRebind = '',
} = {}) {
  const calls = [];
  const actorState = actors.map((actor) => {
    const definition = definitionByIdentity(actor.name || actor.actor_guid);
    return definition ? storyActorSnapshot(definition, actor) : structuredClone(actor);
  });

  const snapshotActors = () =>
    actorState.map((actor) => {
      if (!forceTinyBounds || !definitionByIdentity(actor.name || actor.actor_guid)) {
        return structuredClone(actor);
      }
      const position = actor.geometry?.position || [0, 0, 0];
      return {
        ...structuredClone(actor),
        world_aabb: [
          position[0] - 0.5,
          position[1] - 0.5,
          position[2] - 0.5,
          position[0] + 0.5,
          position[1] + 0.5,
          position[2] + 0.5,
        ],
      };
    });

  const api = {
    projectSettings: {
      getActiveProjectInfo: async () => ({
        success: true,
        data: { mode, project_path: 'D:/CoronaEngine/CoronaEngine/editor/data/world/TestWorld' },
      }),
    },
    project: {
      getDefaultProjectPath: async () => ({
        success: true,
        data: 'D:/CoronaEngine/CoronaEngine/editor/data',
      }),
    },
    scene: {
      getSnapshot: async () => {
        const currentActors = snapshotActors();
        return {
          success: true,
          data: {
            scene: 'scene.ini',
            actors: currentActors,
            scene_aabb: unionAabbs(currentActors),
          },
        };
      },
      setActorTransform: async (...args) => {
        calls.push(['setActorTransform', ...args]);
        const actorName = String(args[1] || '');
        const transform = args[2] || {};
        const actorIndex = actorState.findIndex((actor) => actor.name === actorName);
        if (actorIndex < 0) return { success: false, data: { message: 'actor missing' } };
        const definition = definitionByIdentity(actorName);
        if (!definition) return { success: false, data: { message: 'unknown actor' } };
        const current = actorState[actorIndex];
        const currentPosition = current.geometry?.position ?? definition.position;
        const nextPosition = transform.position ?? currentPosition;
        const delta = [0, 1, 2].map((index) => Number(nextPosition[index]) - Number(currentPosition[index]));
        const currentAabb = Array.isArray(current.world_aabb)
          ? current.world_aabb
          : worldAabbForActor(current, definition);
        const translatedAabb = currentAabb.map(
          (value, index) => Number(value) + delta[index % 3]
        );
        const updated = storyActorSnapshot(definition, {
          ...current,
          world_aabb: translatedAabb,
          geometry: {
            position: nextPosition,
            rotation: transform.rotation ?? current.geometry?.rotation ?? definition.rotation,
            scale: transform.scale ?? current.geometry?.scale ?? storyWorldFinalScale(definition),
          },
        });
        actorState.splice(actorIndex, 1, updated);
        return { success: true };
      },
    },
    sceneTools: {
      sunDirection: async (...args) => {
        calls.push(['sunDirection', ...args]);
        return failSun ? { success: false, data: { message: 'sun failed' } } : { success: true };
      },
      floorGrid: async (...args) => {
        calls.push(['floorGrid', ...args]);
        return { success: true };
      },
      removeActor: async (...args) => {
        calls.push(['removeActor', ...args]);
        const targetName = String(args[1] || '');
        const existingIndex = actorState.findIndex((actor) => actor.name === targetName);
        if (existingIndex >= 0) actorState.splice(existingIndex, 1);
        return { success: true };
      },
      rebindActorResource: async (...args) => {
        calls.push(['rebindActorResource', ...args]);
        const actorGuid = String(args[1] || '');
        const definition = definitionByIdentity(actorGuid);
        if (!definition || definition.name === failRebind || definition.guid === failRebind) {
          return { success: false, data: { message: 'rebind failed' } };
        }
        return { success: true, data: { ok: true, actor_guid: actorGuid, path: args[2] } };
      },
      createActor: async (...args) => {
        calls.push(['createActor', ...args]);
        const actorData = args[3];
        if (actorData.actor_name === failActor) {
          return { success: false, data: { message: 'actor failed' } };
        }

        const definition = definitionByIdentity(actorData.actor_name || actorData.actor_guid);
        if (!definition) return { success: false, data: { message: 'unknown actor' } };
        const existingIndex = actorState.findIndex(
          (actor) =>
            actor.name === definition.name || String(actor.actor_guid) === String(definition.guid)
        );
        const existing = existingIndex >= 0 ? actorState[existingIndex] : null;
        const merged = {
          ...(existing || {}),
          name: definition.name,
          actor_guid: definition.guid,
          actor_type: 'model',
          source_plan_id: actorData.source_plan_id ?? existing?.source_plan_id,
          source_scene_version:
            actorData.source_scene_version ?? existing?.source_scene_version ?? 1,
          source_layout_revision:
            actorData.source_layout_revision ??
            existing?.source_layout_revision ??
            STORY_WORLD_LAYOUT_REVISION,
          visible: existing?.visible !== false,
          world_aabb: undefined,
          geometry: {
            position: actorData.position ?? existing?.geometry?.position ?? definition.position,
            rotation: actorData.rotation ?? existing?.geometry?.rotation ?? definition.rotation,
            scale: actorData.scale ?? existing?.geometry?.scale ?? storyWorldFinalScale(definition),
          },
        };
        const snapshot = storyActorSnapshot(definition, merged);
        if (existingIndex >= 0) actorState.splice(existingIndex, 1, snapshot);
        else actorState.push(snapshot);
        return { success: true, data: { actor_name: actorData.actor_name } };
      },
      setActorPhysics: async (...args) => {
        calls.push(['setActorPhysics', ...args]);
        return { success: true };
      },
    },
  };
  return { api, calls, actorState };
}

test('resolves Story World assets from installed, development and default project paths', () => {
  assert.equal(
    resolveStoryWorldAssetRoot({
      frontendLocation:
        'file:///D:/CoronaEngine/CoronaEngine/editor/Frontend/dist/index.html#/StoryMode',
    }),
    'D:/CoronaEngine/CoronaEngine/editor/assets/story_mode'
  );
  assert.equal(
    resolveStoryWorldAssetRoot({
      frontendLocation: 'file:///C:/Program%20Files/Corona/CabbageEditor/Frontend/dist/index.html',
    }),
    'C:/Program Files/Corona/CabbageEditor/assets/story_mode'
  );
  assert.equal(
    resolveStoryWorldAssetRoot({
      frontendLocation: 'http://localhost:5173/#/StoryMode',
      defaultProjectPath: 'D:/CoronaEngine/CoronaEngine/editor/data',
    }),
    'D:/CoronaEngine/CoronaEngine/editor/assets/story_mode'
  );
});

test('classifies empty, generated and existing user scenes safely', () => {
  assert.equal(classifyStoryWorldScene({ actors: [{ actor_type: 'camera' }] }).kind, 'empty');
  assert.equal(
    classifyStoryWorldScene({ actors: [{ name: 'StoryWorld_Terrain', actor_type: 'model' }] }).kind,
    'partial'
  );
  assert.equal(
    classifyStoryWorldScene({ actors: [{ name: 'UserHouse', actor_type: 'model' }] }).kind,
    'existing'
  );
});

test('only reports missing deterministic actors by name or guid', () => {
  const first = STORY_WORLD_ACTORS[0];
  const second = STORY_WORLD_ACTORS[1];
  const missing = missingStoryWorldActors({
    actors: [
      { name: first.name },
      { actor_guid: second.guid, source_plan_id: STORY_WORLD_PLAN_ID },
    ],
  });
  assert.equal(missing.length, STORY_WORLD_ACTORS.length - 2);
  assert.ok(!missing.some((definition) => definition.name === first.name));
  assert.ok(!missing.some((definition) => definition.guid === second.guid));
  assert.ok(STORY_WORLD_ACTORS.every((definition) => definition.name.startsWith('StoryWorld_')));
  assert.equal(
    new Set(STORY_WORLD_ACTORS.map((definition) => definition.guid)).size,
    STORY_WORLD_ACTORS.length
  );
});

test('records all v5 OBJ normalization compensation values and combines variant scale', () => {
  assert.deepEqual(
    Object.fromEntries(
      Object.entries(STORY_WORLD_ASSET_METADATA).map(([asset, metadata]) => [
        asset,
        metadata.importScale,
      ])
    ),
    {
      'terrain_v5.obj': 120,
      'water_v5.obj': 43.027531,
      'road_network_v5.obj': 61.038173,
      'bridge_v5.obj': 12,
      'gate_v5.obj': 11.827528,
      'house_small_v5.obj': 10.844932,
      'house_large_v5.obj': 13.644932,
      'pavilion_v5.obj': 9.044984,
      'tree_v5_a.obj': 8.860601,
      'tree_v5_b.obj': 9.155378,
      'rock_v5.obj': 3.460254,
      'fence_v5.obj': 8.203424,
      'lantern_v5.obj': 4.2,
      'courtyard_v5.obj': 10,
      'barrels_v5.obj': 2.281324,
      'woodpile_v5.obj': 2.662435,
      'reeds_v5.obj': 4.148509,
    }
  );

  const eastHouse = STORY_WORLD_ACTORS.find(
    (definition) => definition.name === 'StoryWorld_House_East'
  );
  assert.deepEqual(storyWorldFinalScale(STORY_WORLD_TERRAIN_ACTOR), [120, 120, 120]);
  storyWorldFinalScale(eastHouse).forEach((value) => closeTo(value, 10.19423608));
  assert.deepEqual(
    storyWorldExpectedSize(eastHouse).map((value) => Number(value.toFixed(6))),
    [10.194236, 7.549363, 8.7984]
  );
});

test('builds a v5 managed-layout migration that resets transform and resource metadata', () => {
  const legacyTerrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR, {
    actor_guid: 'legacy-managed-terrain-guid',
    source_scene_version: 1,
    geometry: {
      position: [18, 2, -7],
      rotation: [0, 35, 0],
      scale: [1, 1, 1],
    },
  });
  const migration = storyWorldMigrationForActor(legacyTerrain, STORY_WORLD_TERRAIN_ACTOR);
  assert.equal(migration.repaired, true);
  assert.equal(migration.resetManagedLayout, true);
  assert.equal(migration.needsResourceRebind, true);
  assert.deepEqual(migration.scale, [120, 120, 120]);

  const actorData = createStoryWorldMigrationActorData(migration);
  assert.deepEqual(actorData.position, STORY_WORLD_TERRAIN_ACTOR.position);
  assert.deepEqual(actorData.rotation, STORY_WORLD_TERRAIN_ACTOR.rotation);
  assert.deepEqual(actorData.scale, [120, 120, 120]);
  assert.equal(actorData.actor_guid, 'legacy-managed-terrain-guid');
  assert.equal(actorData.source_scene_version, STORY_WORLD_SCENE_VERSION);
  assert.equal(actorData.source_layout_revision, STORY_WORLD_LAYOUT_REVISION);
  assert.equal(actorData.update_if_exists, true);
});

test('only migrates managed actors below v5 and never touches user actors', () => {
  const currentTerrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR);
  assert.equal(storyWorldMigrationForActor(currentTerrain, STORY_WORLD_TERRAIN_ACTOR), null);

  const ungroundedV5Terrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR, {
    source_layout_revision: 0,
  });
  const layoutOnlyMigration = storyWorldMigrationForActor(
    ungroundedV5Terrain,
    STORY_WORLD_TERRAIN_ACTOR
  );
  assert.equal(layoutOnlyMigration.needsLayoutMigration, true);
  assert.equal(layoutOnlyMigration.needsResourceRebind, false);
  assert.equal(layoutOnlyMigration.resetManagedLayout, true);

  const sizedLegacyTerrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR, {
    source_scene_version: 3,
    geometry: {
      position: [10, 4, -8],
      rotation: [0, 45, 0],
      scale: storyWorldFinalScale(STORY_WORLD_TERRAIN_ACTOR),
    },
  });
  const layoutMigration = storyWorldMigrationForActor(
    sizedLegacyTerrain,
    STORY_WORLD_TERRAIN_ACTOR
  );
  assert.equal(layoutMigration.repaired, true);
  const actorData = createStoryWorldMigrationActorData(layoutMigration);
  assert.deepEqual(actorData.position, STORY_WORLD_TERRAIN_ACTOR.position);
  assert.deepEqual(actorData.rotation, STORY_WORLD_TERRAIN_ACTOR.rotation);
  assert.deepEqual(actorData.scale, storyWorldFinalScale(STORY_WORLD_TERRAIN_ACTOR));

  const userActor = {
    name: 'UserTerrain',
    actor_type: 'model',
    source_scene_version: 1,
    geometry: { scale: [1, 1, 1] },
    world_aabb: [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5],
  };
  assert.equal(storyWorldMigrationForActor(userActor, STORY_WORLD_TERRAIN_ACTOR), null);
});

test('validates visible terrain, lake, buildings and vertical world span', () => {
  const terrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR);
  const lakeDefinition = STORY_WORLD_ACTORS.find(
    (definition) => definition.name === 'StoryWorld_YunxiLake'
  );
  const houseDefinition = STORY_WORLD_ACTORS.find((definition) =>
    definition.name.startsWith('StoryWorld_House_')
  );
  const valid = validateStoryWorldSnapshot({
    actors: [terrain, storyActorSnapshot(lakeDefinition), storyActorSnapshot(houseDefinition)],
  });
  assert.equal(valid.valid, true);
  assert.ok(valid.metrics.terrainSize[0] >= 100);
  assert.ok(valid.metrics.lakeSize[2] >= 25);
  assert.ok(valid.metrics.maximumHouseHeight >= 6);

  const invalid = validateStoryWorldSnapshot({
    actors: [
      { ...terrain, world_aabb: [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5] },
      { ...storyActorSnapshot(lakeDefinition), world_aabb: [-0.5, 0, -0.5, 0.5, 0.1, 0.5] },
      { ...storyActorSnapshot(houseDefinition), world_aabb: [-0.5, 0, -0.5, 0.5, 1, 0.5] },
    ],
  });
  assert.equal(invalid.valid, false);
  assert.ok(invalid.errors.includes('基础地形尺寸不足'));
  assert.ok(invalid.errors.includes('云溪湖尺寸不足'));
  assert.ok(invalid.errors.includes('村落建筑尺寸不足'));
  assert.ok(invalid.errors.includes('场景垂直跨度不足'));
});

test('bootstraps an empty story scene with native light, compensated actors and one spawn pose', async () => {
  const { api, calls } = createMockApi();
  const poses = [];
  const result = await runStoryWorldBootstrap({
    api,
    sceneId: 'scene.ini',
    frontendLocation: 'file:///D:/CoronaEngine/CoronaEngine/editor/Frontend/dist/index.html',
    setCameraPose: async (...args) => poses.push(args),
  });

  assert.equal(result.generated, true);
  assert.equal(result.managedWorld, true);
  assert.equal(result.terrainCreated, true);
  assert.equal(result.createdCount, STORY_WORLD_ACTORS.length);
  assert.equal(result.validation.valid, true);
  assert.equal(poses.length, 1);
  assert.equal(calls[0][0], 'sunDirection');
  const createCalls = calls.filter(([name]) => name === 'createActor');
  assert.equal(createCalls[0][4].actor_name, STORY_WORLD_TERRAIN_ACTOR.name);
  assert.equal(createCalls[0][4].source_plan_id, STORY_WORLD_PLAN_ID);
  assert.equal(createCalls[0][4].source_scene_version, STORY_WORLD_SCENE_VERSION);
  assert.deepEqual(createCalls[0][4].scale, [120, 120, 120]);
  assert.equal(createCalls[0][4].skip_if_exists, true);
  assert.ok(createCalls[0][2].endsWith('/assets/story_mode/terrain_v5.obj'));
});

test('skips creative projects and existing user worlds without changing light', async () => {
  const creative = createMockApi({ mode: 'creative' });
  const creativeResult = await runStoryWorldBootstrap({
    api: creative.api,
    sceneId: 'scene.ini',
  });
  assert.equal(creativeResult.skipReason, 'not-story');
  assert.equal(creativeResult.managedWorld, false);
  assert.equal(creative.calls.length, 0);

  const existing = createMockApi({ actors: [{ name: 'MyTerrain', actor_type: 'model' }] });
  const existingResult = await runStoryWorldBootstrap({
    api: existing.api,
    sceneId: 'scene.ini',
  });
  assert.equal(existingResult.skipReason, 'existing-world');
  assert.equal(existingResult.managedWorld, false);
  assert.equal(existing.calls.length, 0);
});

test('partial generation does not reset spawn pose when v5 terrain already exists', async () => {
  const terrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR);
  const { api, calls } = createMockApi({ actors: [terrain] });
  let poseCount = 0;
  const result = await runStoryWorldBootstrap({
    api,
    sceneId: 'scene.ini',
    setCameraPose: async () => {
      poseCount += 1;
    },
  });
  assert.equal(result.generated, true);
  assert.equal(result.terrainCreated, false);
  assert.equal(result.validation.valid, true);
  assert.equal(poseCount, 0);
  assert.equal(
    calls.some(([name]) => name === 'sunDirection'),
    false
  );
});

test('a complete v5 generated world preserves its saved lighting and camera state', async () => {
  const { api, calls } = createMockApi({
    actors: STORY_WORLD_ACTORS.map((definition) => storyActorSnapshot(definition)),
  });
  let poseCount = 0;
  const result = await runStoryWorldBootstrap({
    api,
    sceneId: 'scene.ini',
    setCameraPose: async () => {
      poseCount += 1;
    },
  });
  assert.equal(result.generated, false);
  assert.equal(result.managedWorld, true);
  assert.equal(result.createdCount, 0);
  assert.equal(result.repairedCount, 0);
  assert.equal(result.validation.valid, true);
  assert.equal(calls.length, 0);
  assert.equal(poseCount, 0);
});

test('upgrades v3 managed models once and resets the complete managed layout', async () => {
  const v3Actors = STORY_WORLD_ACTORS.map((definition) =>
    storyActorSnapshot(definition, {
      source_scene_version: 3,
      geometry: {
        position: definition.position.map((value, index) => value + (index === 2 ? 0.4 : 0)),
        rotation: definition.rotation.map((value, index) => value + (index === 1 ? 3 : 0)),
        scale: storyWorldFinalScale(definition).map((value) => value * 1.04),
      },
    })
  );
  const { api, calls, actorState } = createMockApi({ actors: v3Actors });
  const result = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });

  assert.equal(result.generated, false);
  assert.equal(result.repairedCount, STORY_WORLD_ACTORS.length);
  assert.equal(result.upgradedCount, STORY_WORLD_ACTORS.length);
  assert.equal(result.validation.valid, true);
  assert.equal(
    calls.filter(([name]) => name === 'rebindActorResource').length,
    STORY_WORLD_ACTORS.length
  );

  const terrain = actorState.find((actor) => actor.name === STORY_WORLD_TERRAIN_ACTOR.name);
  assert.deepEqual(terrain.geometry.position, STORY_WORLD_TERRAIN_ACTOR.position);
  assert.deepEqual(terrain.geometry.rotation, STORY_WORLD_TERRAIN_ACTOR.rotation);
  assert.deepEqual(terrain.geometry.scale, storyWorldFinalScale(STORY_WORLD_TERRAIN_ACTOR));
  assert.equal(terrain.source_scene_version, STORY_WORLD_SCENE_VERSION);

  const firstRebindCount = calls.filter(([name]) => name === 'rebindActorResource').length;
  await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  assert.equal(calls.filter(([name]) => name === 'rebindActorResource').length, firstRebindCount);
});

test('does not upgrade user-owned actors that resemble normal scene geometry', async () => {
  const userActor = {
    name: 'MyVillageHouse',
    actor_guid: 'user-actor-guid',
    actor_type: 'model',
    source_scene_version: 2,
    visible: true,
    geometry: { position: [2, 0, 3], rotation: [0, 30, 0], scale: [2, 2, 2] },
    world_aabb: [1, 0, 2, 3, 4, 4],
  };
  const { api, calls } = createMockApi({ actors: [userActor] });
  const result = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  assert.equal(result.skipReason, 'existing-world');
  assert.equal(
    calls.some(([name]) => name === 'rebindActorResource'),
    false
  );
});

test('migrates a complete v1 world once, restores lighting and resets managed transforms', async () => {
  const legacyActors = STORY_WORLD_ACTORS.map((definition) =>
    storyActorSnapshot(definition, {
      source_scene_version: 1,
      geometry: {
        position: definition.position.map((value, index) => value + (index === 0 ? 0.25 : 0)),
        rotation: definition.rotation,
        scale: definition.scale,
      },
    })
  );
  const { api, calls, actorState } = createMockApi({ actors: legacyActors });
  const result = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });

  assert.equal(result.generated, false);
  assert.equal(result.repairedCount, STORY_WORLD_ACTORS.length);
  assert.equal(result.upgradedCount, STORY_WORLD_ACTORS.length);
  assert.equal(result.validation.valid, true);
  assert.ok(calls.some(([name]) => name === 'sunDirection'));
  assert.equal(
    calls.filter(([name]) => name === 'rebindActorResource').length,
    STORY_WORLD_ACTORS.length
  );
  const repairedTerrain = actorState.find((actor) => actor.name === STORY_WORLD_TERRAIN_ACTOR.name);
  assert.deepEqual(repairedTerrain.geometry.position, STORY_WORLD_TERRAIN_ACTOR.position);
  assert.deepEqual(repairedTerrain.geometry.rotation, STORY_WORLD_TERRAIN_ACTOR.rotation);
  assert.deepEqual(repairedTerrain.geometry.scale, storyWorldFinalScale(STORY_WORLD_TERRAIN_ACTOR));
  assert.equal(repairedTerrain.source_scene_version, STORY_WORLD_SCENE_VERSION);

  const firstCreateCount = calls.filter(([name]) => name === 'createActor').length;
  const second = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  const secondCreateCount = calls.filter(([name]) => name === 'createActor').length;
  assert.equal(second.repairedCount, 0);
  assert.equal(secondCreateCount, firstCreateCount);
});

test('replaces five deprecated road actors once and preserves user-created actors', async () => {
  const managedV3Actors = STORY_WORLD_ACTORS.filter(
    (definition) => definition.name !== 'StoryWorld_RoadNetwork'
  ).map((definition) => storyActorSnapshot(definition, { source_scene_version: 3 }));
  const legacyRoads = STORY_WORLD_DEPRECATED_ACTORS.map((name, index) => ({
    name,
    actor_guid: `legacy-road-${index + 1}`,
    actor_type: 'model',
    source_plan_id: STORY_WORLD_PLAN_ID,
    source_scene_version: 3,
    visible: true,
    geometry: { position: [index, 0.2, index], rotation: [0, 0, 0], scale: [16, 16, 16] },
    world_aabb: [index, 0, index, index + 10, 1, index + 3],
  }));
  const userActor = {
    name: 'UserGardenStatue',
    actor_guid: 'user-garden-statue',
    actor_type: 'model',
    visible: true,
    geometry: { position: [12, 1, -7], rotation: [0, 22, 0], scale: [1.5, 1.5, 1.5] },
    world_aabb: [11, 0, -8, 13, 3, -6],
  };
  const { api, calls, actorState } = createMockApi({
    actors: [...managedV3Actors, ...legacyRoads, userActor],
  });

  const first = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  assert.equal(first.deprecatedRemovedCount, STORY_WORLD_DEPRECATED_ACTORS.length);
  assert.equal(
    calls.filter(([name]) => name === 'removeActor').length,
    STORY_WORLD_DEPRECATED_ACTORS.length
  );
  assert.ok(actorState.some((actor) => actor.name === 'StoryWorld_RoadNetwork'));
  assert.ok(
    STORY_WORLD_DEPRECATED_ACTORS.every(
      (name) => !actorState.some((actor) => actor.name === name)
    )
  );
  const preservedUserActor = actorState.find((actor) => actor.name === userActor.name);
  assert.deepEqual(preservedUserActor.geometry, userActor.geometry);

  const callCount = calls.length;
  const second = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  assert.equal(second.deprecatedRemovedCount, 0);
  assert.equal(calls.length, callCount);
});

test('lighting and terrain failures are blocking while decoration failures are warnings', async () => {
  const lighting = createMockApi({ failSun: true });
  await assert.rejects(
    runStoryWorldBootstrap({ api: lighting.api, sceneId: 'scene.ini' }),
    (error) => error.code === 'LIGHTING_FAILED'
  );

  const terrain = createMockApi({ failActor: STORY_WORLD_TERRAIN_ACTOR.name });
  await assert.rejects(
    runStoryWorldBootstrap({ api: terrain.api, sceneId: 'scene.ini' }),
    (error) => error.code === 'TERRAIN_FAILED'
  );

  const decoration = STORY_WORLD_ACTORS.find((definition) => definition.phase === 'decorations');
  const optional = createMockApi({ failActor: decoration.name });
  const result = await runStoryWorldBootstrap({ api: optional.api, sceneId: 'scene.ini' });
  assert.ok(result.warnings.some((warning) => warning.includes(decoration.name)));
  assert.equal(result.terrainCreated, true);
  assert.equal(result.validation.valid, true);
});

test('grounds floating managed actors once without touching user actors', async () => {
  const floatingHouseDefinition = STORY_WORLD_ACTORS.find(
    (definition) => definition.name === 'StoryWorld_House_Liu'
  );
  const completeWorld = STORY_WORLD_ACTORS.map((definition) => storyActorSnapshot(definition));
  const floatingHouseIndex = completeWorld.findIndex(
    (actor) => actor.name === floatingHouseDefinition.name
  );
  completeWorld[floatingHouseIndex] = storyActorSnapshot(floatingHouseDefinition, {
    world_aabb: completeWorld[floatingHouseIndex].world_aabb.map((value, index) =>
      index === 1 || index === 4 ? value + 5 : value
    ),
  });
  const userActor = {
    name: 'UserFloatingDecoration',
    actor_guid: 'user-floating-decoration',
    actor_type: 'model',
    visible: true,
    geometry: { position: [2, 20, 3], rotation: [0, 0, 0], scale: [1, 1, 1] },
    world_aabb: [1, 19, 2, 3, 21, 4],
  };
  const { api, calls, actorState } = createMockApi({ actors: [...completeWorld, userActor] });

  const first = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  assert.ok(first.groundedCount > 0);
  assert.ok(
    calls.some(
      ([name, , actorName]) =>
        name === 'setActorTransform' && actorName === floatingHouseDefinition.name
    )
  );
  assert.equal(
    calls.some(([name, , actorName]) => name === 'setActorTransform' && actorName === userActor.name),
    false
  );
  assert.deepEqual(
    actorState.find((actor) => actor.name === userActor.name).geometry,
    userActor.geometry
  );

  const transformCount = calls.filter(([name]) => name === 'setActorTransform').length;
  const second = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  assert.equal(second.groundedCount, 0);
  assert.equal(calls.filter(([name]) => name === 'setActorTransform').length, transformCount);
});

test('blocks gameplay when the refreshed snapshot still reports miniature resources', async () => {
  const { api } = createMockApi({ forceTinyBounds: true });
  await assert.rejects(runStoryWorldBootstrap({ api, sceneId: 'scene.ini' }), (error) => {
    assert.equal(error.code, 'WORLD_VALIDATION_FAILED');
    assert.equal(error.message, '剧情资源尺寸异常，世界修复未完成。');
    assert.equal(error.validation.valid, false);
    return true;
  });
});
