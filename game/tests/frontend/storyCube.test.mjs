import assert from 'node:assert/strict';
import test from 'node:test';
import { CUBE_GUID, CUBE_MODEL_REF, CUBE_PLACEMENT, cubeMoveTransform, cubeScaleTransform,
  cubeSpawnTransform, cubeTransformOf, ensureStoryCube, isCubeActor, isCubeReady, playerFacingYaw,
  screenAxes, validateCubePlacement } from '../../frontend/storyCube.mjs';
import { PLAYER_MODEL_YAW_OFFSET } from '../../frontend/storyCharacters.mjs';
import { ADJUST_ACTIONS, PROPHET_SCREENS, adjustActionForKey, adjustFeedback,
  fragmentForKey, fragmentPanel, isAdjustAction, nextObjectTransform, objectLabel,
  ruleDetails, screenForKey, selectableObjects, toggleFragment } from '../../frontend/storyProphetActions.mjs';

const prophet = (position = [0, 0, -4]) => ({
  actor_guid: 'f9f5c8b0-7324-4b6c-a011-000000000004', handle: 21,
  semantic_role: 'prophet', visible: true,
  geometry: { position: [...position], rotation: [0, Math.PI, 0], scale: [1, 1, 1] },
  local_aabb: [-1, 0, -1, 1, 2, 1],
});
const player = (rotationY = 0) => ({
  actor_guid: 'f9f5c8b0-7324-4b6c-a011-000000000001', handle: 11,
  geometry: { position: [0, 0, 0], rotation: [0, rotationY, 0], scale: [1, 1, 1] },
});
const transformAt = (position, scale = 1) => ({ position: [...position], rotation: [0, 0, 0], scale: [scale, scale, scale] });

test('the cube table is valid and matches the shipped data file', () => {
  assert.equal(CUBE_PLACEMENT.guid, CUBE_GUID);
  assert.equal(CUBE_PLACEMENT.modelRef, CUBE_MODEL_REF);
  assert.equal(CUBE_PLACEMENT.asset, 'cube/cube.obj');
  assert.ok(CUBE_PLACEMENT.minScale < CUBE_PLACEMENT.scale && CUBE_PLACEMENT.scale < CUBE_PLACEMENT.maxScale);
  assert.deepEqual(validateCubePlacement(CUBE_PLACEMENT), { ...CUBE_PLACEMENT });
  for (const bad of [null, {}, { ...CUBE_PLACEMENT, asset: '  ' }, { ...CUBE_PLACEMENT, guid: '' },
    { ...CUBE_PLACEMENT, modelRef: '' }, { ...CUBE_PLACEMENT, sideDistance: 0 },
    { ...CUBE_PLACEMENT, moveStep: -1 }, { ...CUBE_PLACEMENT, maxScale: NaN },
    { ...CUBE_PLACEMENT, minScale: 3, maxScale: 3 }, { ...CUBE_PLACEMENT, scale: 9 },
    { ...CUBE_PLACEMENT, scaleFactor: 1 }, { ...CUBE_PLACEMENT, scaleFactor: 0.5 },
    { ...CUBE_PLACEMENT, scaleFactor: NaN }]) {
    assert.throws(() => validateCubePlacement(bad), /正方体/, JSON.stringify(bad));
  }
});

test('screen axes follow the player so left and right mean what the player sees', () => {
  assert.deepEqual(screenAxes(0), { forward: [0, 1], right: [1, 0] });
  const quarter = screenAxes(Math.PI / 2);
  assert.ok(Math.abs(quarter.forward[0] - 1) < 1e-12 && Math.abs(quarter.forward[1]) < 1e-12);
  assert.ok(Math.abs(quarter.right[0]) < 1e-12 && Math.abs(quarter.right[1] + 1) < 1e-12);
  assert.throws(() => screenAxes(NaN), /朝向无效/);
  assert.equal(playerFacingYaw(player(0)), -PLAYER_MODEL_YAW_OFFSET);
  assert.throws(() => playerFacingYaw({}), /玩家朝向未就绪/);
});

test('the cube spawns beside the prophet, scaled from the table', () => {
  const spawn = cubeSpawnTransform(prophet([0, 0, -4]), 0);
  assert.deepEqual(spawn.position, [CUBE_PLACEMENT.sideDistance, 0, -4 + CUBE_PLACEMENT.forwardDistance]);
  assert.deepEqual(spawn.scale, [CUBE_PLACEMENT.scale, CUBE_PLACEMENT.scale, CUBE_PLACEMENT.scale]);
  assert.deepEqual(spawn.rotation, [0, Math.PI, 0], 'it turns back toward the player');
  assert.throws(() => cubeSpawnTransform({ geometry: { position: null } }, 0), /先知位置未就绪/);
});

test('scaling steps are bounded, proportional, and never disturb the position', () => {
  const factor = CUBE_PLACEMENT.scaleFactor;
  assert.ok(factor >= 1.3, 'a step must be big enough to see on screen');
  for (const [direction, expected] of [[1, 0.8 * factor], [-1, 0.8 / factor]]) {
    const next = cubeScaleTransform(transformAt([0, 0, 0], 0.8), direction);
    assert.ok(Math.abs(next.scale[0] - expected) < 1e-12);
    assert.deepEqual(next.position, [0, 0, 0]);
  }
  const top = cubeScaleTransform(transformAt([0, 0, 0], CUBE_PLACEMENT.maxScale), 1);
  assert.equal(top.scale[0], CUBE_PLACEMENT.maxScale, 'cannot grow past the ceiling');
  const bottom = cubeScaleTransform(transformAt([0, 0, 0], CUBE_PLACEMENT.minScale), -1);
  assert.equal(bottom.scale[0], CUBE_PLACEMENT.minScale, 'cannot shrink past the floor');
  assert.throws(() => cubeScaleTransform(transformAt([0, 0, 0], 1), 0), /缩放方向/);
  assert.throws(() => cubeScaleTransform({ position: [0, 0, 0], rotation: [0, 0, 0] }, 1), /当前缩放未知/);
});

test('moving steps follow the player frame and stay inside the prophet radius', () => {
  const start = transformAt([0, 0, 0], 1);
  const forward = cubeMoveTransform(start, 'forward', prophet([0, 0, 0]), 0);
  assert.ok(Math.abs(forward.position[2] - CUBE_PLACEMENT.moveStep) < 1e-12);
  const back = cubeMoveTransform(start, 'back', prophet([0, 0, 0]), 0);
  assert.ok(Math.abs(back.position[2] + CUBE_PLACEMENT.moveStep) < 1e-12);
  const right = cubeMoveTransform(start, 'right', prophet([0, 0, 0]), 0);
  assert.ok(Math.abs(right.position[0] - CUBE_PLACEMENT.moveStep) < 1e-12);
  const left = cubeMoveTransform(start, 'left', prophet([0, 0, 0]), 0);
  assert.ok(Math.abs(left.position[0] + CUBE_PLACEMENT.moveStep) < 1e-12);
  // Rotating the player rotates the movement frame.
  const turned = cubeMoveTransform(start, 'forward', prophet([0, 0, 0]), Math.PI / 2);
  assert.ok(Math.abs(turned.position[0] - CUBE_PLACEMENT.moveStep) < 1e-12);
  assert.ok(Math.abs(turned.position[2]) < 1e-12);
  // The far edge is clamped instead of running away from the prophet.
  const far = transformAt([CUBE_PLACEMENT.moveRange, 0, 0], 1);
  const clamped = cubeMoveTransform(far, 'right', prophet([0, 0, 0]), 0);
  assert.ok(Math.abs(Math.hypot(clamped.position[0], clamped.position[2]) - CUBE_PLACEMENT.moveRange) < 1e-9);
  assert.equal(clamped.scale[0], 1);
  assert.throws(() => cubeMoveTransform(start, 'up', prophet([0, 0, 0]), 0), /移动方向/);
  assert.throws(() => cubeMoveTransform(start, 'forward', { geometry: {} }, 0), /先知位置未就绪/);
});

test('the cube actor is recognized by its own markers only', () => {
  assert.equal(isCubeActor({ actor_guid: CUBE_GUID }), true);
  assert.equal(isCubeActor({ model_ref: CUBE_MODEL_REF }), true);
  assert.equal(isCubeActor({ actor_guid: 'story.placement.0000', model_ref: 'story.player.facing.v1' }), false);
  assert.equal(isCubeActor(null), false);
  assert.equal(isCubeReady({ handle: 5, load_status: 'loaded' }), true);
  assert.equal(isCubeReady({ handle: 0, load_status: 'loaded' }), false);
  const live = cubeTransformOf(prophet());
  assert.deepEqual(live.position, [0, 0, -4]);
  assert.deepEqual(live.rotation, [0, Math.PI, 0]);
  assert.deepEqual(live.scale, [1, 1, 1]);
  assert.equal(cubeTransformOf({ geometry: { position: [0, 0, 0] } }), null);
  assert.equal(cubeTransformOf({ geometry: { position: [0, NaN, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } }), null);
});

test('the prophet offers two main screens on number keys', () => {
  assert.deepEqual(PROPHET_SCREENS.map(screen => screen.key), ['1', '2']);
  assert.deepEqual(PROPHET_SCREENS.map(screen => screen.label), ['调整物体', '碎片']);
  assert.equal(screenForKey('1').id, 'adjust');
  assert.equal(screenForKey('2').id, 'fragments');
  for (const key of ['0', '3', 'a', '', null, undefined, 1]) assert.equal(screenForKey(key), null);
  // The adjust screen explains the overhead view and the click, per the requirement.
  assert.match(screenForKey('1').detail, /俯瞰视角/);
  assert.match(screenForKey('1').detail, /点击物体/);
  assert.match(screenForKey('2').detail, /装填/);
});

test('the adjust screen keeps the scale and move steps on number keys', () => {
  assert.deepEqual(ADJUST_ACTIONS.map(action => action.key), ['1', '2', '3', '4', '5', '6']);
  assert.deepEqual([...new Set(ADJUST_ACTIONS.map(action => action.group))], ['缩放物体', '调整位置']);
  assert.equal(adjustActionForKey('3').id, 'move-forward');
  assert.equal(adjustActionForKey('7'), null, 'the fragment action moved to its own screen');
  assert.equal(isAdjustAction('scale-up'), true);
  assert.equal(isAdjustAction('install-fragment'), false);
});

test('adjustment transforms drive the selected object and ignore other ids', () => {
  const current = transformAt([0, 0, 0], 1);
  const context = { current, reference: prophet([0, 0, 0]), facingYaw: 0 };
  const factor = CUBE_PLACEMENT.scaleFactor;
  assert.ok(Math.abs(nextObjectTransform('scale-up', context).scale[0] - factor) < 1e-12);
  assert.ok(Math.abs(nextObjectTransform('scale-down', context).scale[0] - 1 / factor) < 1e-12);
  assert.ok(Math.abs(nextObjectTransform('move-left', context).position[0]
    + CUBE_PLACEMENT.moveStep) < 1e-12);
  assert.equal(nextObjectTransform('install-fragment', context), null);
  assert.equal(nextObjectTransform('unknown', context), null);
  assert.deepEqual(nextObjectTransform('move-forward', context),
    cubeMoveTransform(current, 'forward', context.reference, 0));
});

test('adjustment feedback names what happened', () => {
  const factor = CUBE_PLACEMENT.scaleFactor;
  const up = cubeScaleTransform(transformAt([0, 0, 0], 1), 1);
  const down = cubeScaleTransform(transformAt([0, 0, 0], 1), -1);
  assert.ok(up.scale[0] > 1 && down.scale[0] < 1);
  assert.match(adjustFeedback('scale-up', up, prophet([0, 0, 0])), /^物体已放大到 \d+\.\d{2}$/);
  assert.match(adjustFeedback('scale-down', down, prophet([0, 0, 0])), /^物体已缩小到 \d+\.\d{2}$/);
  assert.match(adjustFeedback('scale-up', up, prophet([0, 0, 0])),
    new RegExp(`^物体已放大到 ${(1 * factor).toFixed(2)}$`));
  const moved = cubeMoveTransform(transformAt([0, 0, 0], 1), 'right', prophet([0, 0, 0]), 0);
  assert.match(adjustFeedback('move-right', moved, prophet([0, 0, 0])),
    new RegExp(`距先知 ${CUBE_PLACEMENT.moveStep.toFixed(2)} 米`));
  const far = transformAt([CUBE_PLACEMENT.moveRange, 0, 0], 1);
  assert.match(adjustFeedback('move-right', far, prophet([0, 0, 0])), /上限/);
  assert.equal(adjustFeedback('scale-up', null, prophet()), '');
});

test('the fragment screen lists state and details for every catalogued fragment', () => {
  const empty = fragmentPanel([]);
  assert.deepEqual(empty.map(entry => entry.id), ['sway']);
  assert.deepEqual(empty.map(entry => entry.state), ['owned']);
  assert.equal(empty[0].stateLabel, '可装填');
  assert.equal(empty[0].owned, true);
  assert.equal(empty[0].installed, false);
  assert.equal(empty[0].initial, true);
  assert.match(empty[0].details, /左右浮动 0\.25 米，周期 2\.40 秒/);
  assert.match(empty[0].details, /player不浮动/);

  const installed = fragmentPanel([{ fragmentId: 'sway' }]);
  assert.deepEqual(installed.map(entry => entry.state), ['installed']);
  assert.equal(installed[0].stateLabel, '已装填');
  assert.equal(installed[0].installed, true);

  // A catalogued fragment the player does not own is shown as locked, not hidden.
  const locked = fragmentPanel([]).map(entry => entry.id);
  assert.deepEqual(locked, ['sway']);
  assert.equal(ruleDetails({ axis: 'z', amplitude: 0.5, periodMs: 1000, excludeRoles: [] }),
    '前后浮动 0.50 米，周期 1.00 秒');
  assert.match(ruleDetails({}), /未知方向/);
});

test('fragment keys select an entry and toggling装填/拆下 rewrites the rule records', () => {
  assert.equal(fragmentForKey('1', []).id, 'sway');
  assert.equal(fragmentForKey('2', []), null);
  assert.equal(fragmentForKey('a', []), null);

  const loaded = toggleFragment([], 'sway');
  assert.equal(loaded.action, 'installed');
  assert.match(loaded.feedback, /已装填：浮动碎片/);
  assert.deepEqual(loaded.rules, [{ fragmentId: 'sway', type: 'sway', axis: 'x',
    amplitude: 0.25, periodMs: 2400 }]);

  const unloaded = toggleFragment(loaded.rules, 'sway');
  assert.equal(unloaded.action, 'removed');
  assert.match(unloaded.feedback, /已拆下：浮动碎片/);
  assert.deepEqual(unloaded.rules, []);
  // Toggling is idempotent in both directions.
  assert.deepEqual(toggleFragment(toggleFragment([], 'sway').rules, 'sway').rules, []);
  assert.throws(() => toggleFragment([], 'nope'), /未知碎片/);
});

/** A minimal engine stand-in: it only updates in place when told to skip-if-exists. */
function cubeApi({ actor = null } = {}) {
  const state = { actors: actor ? [{ ...actor }] : [], creates: 0, updates: 0 };
  return {
    state,
    api: {
      scene: { getSnapshot: async () => ({ data: { scene: {
        actors: state.actors.map(entry => ({ ...entry })) } } }) },
      sceneTools: {
        createActor: async (sceneId, route, type, data) => {
          // The engine only updates in place when it is told to skip-if-exists AND the
          // actor is already there; otherwise it appends. Model that, not the flags.
          const exists = Boolean(data.skip_if_exists)
            && state.actors.some(entry => entry.actor_guid === data.actor_guid);
          if (exists) state.updates++; else state.creates++;
          const created = { actor_guid: data.actor_guid, name: data.name, route,
            model_ref: data.model_ref, load_status: 'loaded', handle: 7, visible: true,
            follow_camera: false,
            geometry: { position: data.position || [0, 0, 0],
              rotation: data.rotation || [0, 0, 0], scale: data.scale || [1, 1, 1] } };
          state.actors = [created];
          return { actor: created };
        },
        setActorState: async (sceneId, guid, patch) => {
          const updated = { ...state.actors[0], ...patch };
          state.actors = [updated];
          return { actor: updated };
        },
      },
    },
  };
}

const childContext = api => ({
  api, sceneId: 'scene.ini', frontendUrl: 'file:///w/Frontend/dist/index.html', role: 'child',
  prophetActor: prophet([0, 0, -4]), playerActor: player(0),
  resolveAsset: (url, asset) => `W:/game/art/models/${asset}`,
});

test('the adjust screen lists the scene objects so a name can be clicked instead of a pixel', () => {
  const snapshot = { data: { scene: { actors: [
    { actor_guid: 'cube', name: '先知方块', handle: 7, semantic_role: 'prophet-cube', actor_type: 'model',
      geometry: { position: [1, 0, 1], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'p', name: '玩家', handle: 11, semantic_role: 'player',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'idle', name: '', handle: 0, semantic_role: 'prophet',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'bad', name: '无几何', handle: 9, semantic_role: 'prophet',
      geometry: { position: [0, NaN, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'prophet', name: '先知', handle: 12, semantic_role: 'prophet',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
  ] } } };
  const list = selectableObjects(snapshot);
  assert.deepEqual(list.map(entry => entry.guid), ['cube'],
    'only usable objects inside the small world; never the player or the prophet');
  assert.deepEqual(list[0].transform.position, [1, 0, 1]);
  assert.equal(objectLabel(list[0]), '先知方块（prophet-cube）');
  assert.equal(objectLabel({}), '未命名（物体）');
  // A missing or empty scene is an empty list, never a throw.
  assert.deepEqual(selectableObjects(null), []);
  assert.deepEqual(selectableObjects({ data: { scene: {} } }), []);
  assert.deepEqual(selectableObjects({ data: { scene: { actors: [
    { actor_guid: 'p', handle: 1, semantic_role: 'player',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
  ] } } }), [], 'the player is never adjustable');
});

test('one cube per small world: re-entering never creates a second one', async () => {
  const first = cubeApi();
  const created = await ensureStoryCube(childContext(first.api));
  assert.equal(first.state.creates, 1, 'the first entry creates the cube');
  assert.equal(first.state.updates, 0);
  assert.equal(created.actor_guid, CUBE_GUID);
  assert.equal(created.model_ref, CUBE_MODEL_REF);

  // Second entry: the world already has the cube, so nothing is created and the player's
  // own arrangement is left exactly as it was.
  const second = cubeApi({ actor: { ...created,
    geometry: { position: [9, 0, 9], rotation: [0, 1, 0], scale: [2, 2, 2] } } });
  const again = await ensureStoryCube(childContext(second.api));
  assert.equal(second.state.creates, 0, 'a second cube must never be appended');
  assert.equal(second.state.updates, 0, 'and an arranged cube must not be re-placed');
  assert.deepEqual(again.geometry.position, [9, 0, 9]);
  assert.deepEqual(again.geometry.scale, [2, 2, 2]);

  // Even when the engine reports it as not loaded yet, it is still the same cube.
  const loading = cubeApi({ actor: { ...created, load_status: 'loading' } });
  await ensureStoryCube(childContext(loading.api));
  assert.equal(loading.state.creates, 0, 'load state must not decide whether to re-create');
});

test('a cube without this version marker is refreshed in place, not duplicated', async () => {
  const stale = cubeApi({ actor: { actor_guid: CUBE_GUID, name: '先知方块',
    route: 'W:/game/art/models/cube/cube.obj', model_ref: 'story.prophet.cube.v0',
    load_status: 'loaded', handle: 7, visible: true,
    geometry: { position: [1, 0, 1], rotation: [0, 0, 0], scale: [1, 1, 1] } } });
  await ensureStoryCube(childContext(stale.api));
  assert.equal(stale.state.creates, 0, 'the engine must be asked to update, never to append');
  assert.equal(stale.state.updates, 1);
  assert.equal(stale.state.actors.length, 1);
});

test('the main world hides the cube instead of showing it', async () => {
  const world = cubeApi({ actor: { actor_guid: CUBE_GUID, model_ref: CUBE_MODEL_REF, handle: 7,
    visible: true, geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } } });
  const result = await ensureStoryCube({ ...childContext(world.api), role: 'main' });
  assert.equal(result, null);
  assert.equal(world.state.creates, 0);
  assert.equal(world.state.actors[0].visible, false);
});
