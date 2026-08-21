import assert from 'node:assert/strict';
import test from 'node:test';

import {
  STORY_WORLD_ACTORS,
  STORY_WORLD_ASSET_METADATA,
  STORY_WORLD_PLAN_ID,
  STORY_WORLD_SCENE_VERSION,
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

const worldAabbForActor = (actor, definition) => {
  const position = actorPosition(actor, definition);
  const size = worldSizeForScale(definition, actorScale(actor, definition));
  return [
    position[0] - size[0] * 0.5,
    position[1] - size[1] * 0.5,
    position[2] - size[2] * 0.5,
    position[0] + size[0] * 0.5,
    position[1] + size[1] * 0.5,
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

test('records all OBJ normalization compensation values and combines them with variant scale', () => {
  assert.deepEqual(
    Object.fromEntries(
      Object.entries(STORY_WORLD_ASSET_METADATA).map(([asset, metadata]) => [
        asset,
        metadata.importScale,
      ])
    ),
    {
      'terrain_v3.obj': 120,
      'water_v3.obj': 50,
      'road_v3.obj': 16,
      'bridge_v3.obj': 12,
      'gate_v3.obj': 12,
      'house_small_v3.obj': 10.8,
      'house_large_v3.obj': 13.8,
      'pavilion_v3.obj': 9,
      'tree_v3_a.obj': 9.1,
      'tree_v3_b.obj': 9.1,
      'rock_v3.obj': 4.48466,
      'fence_v3.obj': 8.2,
      'lantern_v3.obj': 4.2,
      'courtyard_v3.obj': 10,
      'barrels_v3.obj': 2.3,
      'woodpile_v3.obj': 3,
      'reeds_v3.obj': 4.2,
    }
  );

  const eastHouse = STORY_WORLD_ACTORS.find(
    (definition) => definition.name === 'StoryWorld_House_East'
  );
  assert.deepEqual(storyWorldFinalScale(STORY_WORLD_TERRAIN_ACTOR), [120, 120, 120]);
  storyWorldFinalScale(eastHouse).forEach((value) => closeTo(value, 9.72));
  assert.deepEqual(storyWorldExpectedSize(eastHouse), [9.72, 7.1982, 8.3889]);
});

test('builds a scale-only v1 migration and never overwrites position or rotation', () => {
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
  assert.deepEqual(migration.scale, [120, 120, 120]);

  const actorData = createStoryWorldMigrationActorData(migration);
  assert.deepEqual(actorData.scale, [120, 120, 120]);
  assert.equal(actorData.actor_guid, 'legacy-managed-terrain-guid');
  assert.equal(actorData.source_scene_version, STORY_WORLD_SCENE_VERSION);
  assert.equal(actorData.update_if_exists, true);
  assert.equal('position' in actorData, false);
  assert.equal('rotation' in actorData, false);
});

test('does not repeatedly enlarge current, already-sized, or non-managed actors', () => {
  const currentTerrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR);
  assert.equal(storyWorldMigrationForActor(currentTerrain, STORY_WORLD_TERRAIN_ACTOR), null);

  const sizedLegacyTerrain = storyActorSnapshot(STORY_WORLD_TERRAIN_ACTOR, {
    source_scene_version: 1,
    geometry: { scale: storyWorldFinalScale(STORY_WORLD_TERRAIN_ACTOR) },
  });
  const metadataMigration = storyWorldMigrationForActor(
    sizedLegacyTerrain,
    STORY_WORLD_TERRAIN_ACTOR
  );
  assert.equal(metadataMigration.repaired, false);
  assert.equal('scale' in createStoryWorldMigrationActorData(metadataMigration), false);

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
  assert.ok(createCalls[0][2].endsWith('/assets/story_mode/terrain_v3.obj'));
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

test('partial generation does not reset spawn pose when v3 terrain already exists', async () => {
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

test('a complete v3 generated world preserves its saved lighting and camera state', async () => {
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

test('upgrades v2 managed models once without changing user transforms or scale', async () => {
  const v2Actors = STORY_WORLD_ACTORS.map((definition) =>
    storyActorSnapshot(definition, {
      source_scene_version: 2,
      geometry: {
        position: definition.position.map((value, index) => value + (index === 2 ? 0.4 : 0)),
        rotation: definition.rotation.map((value, index) => value + (index === 1 ? 3 : 0)),
        scale: storyWorldFinalScale(definition).map((value) => value * 1.04),
      },
    })
  );
  const { api, calls, actorState } = createMockApi({ actors: v2Actors });
  const result = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });

  assert.equal(result.generated, false);
  assert.equal(result.repairedCount, 0);
  assert.equal(result.upgradedCount, STORY_WORLD_ACTORS.length);
  assert.equal(result.validation.valid, true);
  assert.equal(
    calls.filter(([name]) => name === 'rebindActorResource').length,
    STORY_WORLD_ACTORS.length
  );

  const terrain = actorState.find((actor) => actor.name === STORY_WORLD_TERRAIN_ACTOR.name);
  assert.deepEqual(terrain.geometry.position, [0, 0, 0.4]);
  assert.deepEqual(terrain.geometry.rotation, [0, 3, 0]);
  terrain.geometry.scale.forEach((value) => closeTo(value, 124.8));
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

test('migrates a complete v1 world once, restores lighting and preserves transforms', async () => {
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
  assert.deepEqual(repairedTerrain.geometry.position, [0.25, 0, 0]);
  assert.deepEqual(repairedTerrain.geometry.rotation, [0, 0, 0]);
  assert.deepEqual(repairedTerrain.geometry.scale, [120, 120, 120]);
  assert.equal(repairedTerrain.source_scene_version, STORY_WORLD_SCENE_VERSION);

  const firstCreateCount = calls.filter(([name]) => name === 'createActor').length;
  const second = await runStoryWorldBootstrap({ api, sceneId: 'scene.ini' });
  const secondCreateCount = calls.filter(([name]) => name === 'createActor').length;
  assert.equal(second.repairedCount, 0);
  assert.equal(secondCreateCount, firstCreateCount);
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

test('blocks gameplay when the refreshed snapshot still reports miniature resources', async () => {
  const { api } = createMockApi({ forceTinyBounds: true });
  await assert.rejects(runStoryWorldBootstrap({ api, sceneId: 'scene.ini' }), (error) => {
    assert.equal(error.code, 'WORLD_VALIDATION_FAILED');
    assert.equal(error.message, '剧情资源尺寸异常，世界修复未完成。');
    assert.equal(error.validation.valid, false);
    return true;
  });
});
