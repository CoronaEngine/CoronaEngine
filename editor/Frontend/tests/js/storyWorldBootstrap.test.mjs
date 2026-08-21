import assert from 'node:assert/strict';
import test from 'node:test';

import {
  STORY_WORLD_ACTORS,
  STORY_WORLD_PLAN_ID,
  STORY_WORLD_TERRAIN_ACTOR,
} from '../../src/config/storyWorld.js';
import {
  classifyStoryWorldScene,
  missingStoryWorldActors,
  resolveStoryWorldAssetRoot,
  runStoryWorldBootstrap,
} from '../../src/services/storyWorldBootstrapService.js';

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

function createMockApi({ mode = 'story', actors = [], failActor = '', failSun = false } = {}) {
  const calls = [];
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
      getSnapshot: async () => ({ success: true, data: { scene: 'scene.ini', actors } }),
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
      createActor: async (...args) => {
        calls.push(['createActor', ...args]);
        const actorData = args[3];
        return actorData.actor_name === failActor
          ? { success: false, data: { message: 'actor failed' } }
          : { success: true, data: { actor_name: actorData.actor_name } };
      },
      setActorPhysics: async (...args) => {
        calls.push(['setActorPhysics', ...args]);
        return { success: true };
      },
    },
  };
  return { api, calls };
}

test('bootstraps an empty story scene with native light, deterministic actors and one spawn pose', async () => {
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
  assert.equal(poses.length, 1);
  assert.equal(calls[0][0], 'sunDirection');
  const createCalls = calls.filter(([name]) => name === 'createActor');
  assert.equal(createCalls[0][4].actor_name, STORY_WORLD_TERRAIN_ACTOR.name);
  assert.equal(createCalls[0][4].source_plan_id, STORY_WORLD_PLAN_ID);
  assert.equal(createCalls[0][4].skip_if_exists, true);
  assert.ok(createCalls[0][2].endsWith('/assets/story_mode/terrain.obj'));
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

test('partial generation does not reset spawn pose when terrain already exists', async () => {
  const { api, calls } = createMockApi({
    actors: [{ name: STORY_WORLD_TERRAIN_ACTOR.name, actor_guid: STORY_WORLD_TERRAIN_ACTOR.guid }],
  });
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
  assert.equal(poseCount, 0);
  assert.equal(
    calls.some(([name]) => name === 'sunDirection'),
    false
  );
});

test('a complete generated world preserves its saved lighting and camera state', async () => {
  const { api, calls } = createMockApi({
    actors: STORY_WORLD_ACTORS.map((definition) => ({
      name: definition.name,
      actor_guid: definition.guid,
      source_plan_id: STORY_WORLD_PLAN_ID,
    })),
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
  assert.equal(calls.length, 0);
  assert.equal(poseCount, 0);
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
});
