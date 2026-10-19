import assert from 'node:assert/strict';
import test from 'node:test';
import { CUBE_GUID, CUBE_MODEL_REF, CUBE_PLACEMENT, cubeMoveTransform, cubeScaleTransform,
  cubeSpawnTransform, cubeTransformOf, ensureStoryCube, isCubeActor, isCubeReady, playerFacingYaw,
  screenAxes, validateCubePlacement } from '../../frontend/storyCube.mjs';
import { PLAYER_MODEL_YAW_OFFSET } from '../../frontend/storyCharacters.mjs';
import { MAP_ENTITY_ID, MAP_ENTITY_TYPE, MAP_GUID, MAP_MODEL_REF } from '../../frontend/storyMap.mjs';
import { ADJUST_ACTIONS, FRAGMENT_ADJUST_ACTIONS, FRAGMENT_RULE_BOUNDS, PROPHET_SCREENS,
  adjustActionForKey, adjustFeedback, adjustFragmentRule, fragmentAdjustActionForKey,
  fragmentAdjustPanel, fragmentForKey, fragmentPanel, fragmentRuleOf, installedTargetOf,
  isAdjustAction, isLegacyRule, nextObjectTransform, objectLabel, prophetKeyAction,
  ruleDetails, screenForKey, selectableObjects, toggleFragment } from '../../frontend/storyProphetActions.mjs';
import { PROPHET_GUID } from '../../frontend/storyCharacters.mjs';

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

test('the cube also spawns onto the ground grid when the page supplies one', () => {
  const ground = (x, z) => 3 + 0.5 * x + 0.25 * z;
  const spawn = cubeSpawnTransform(prophet([0, 4, -4]), 0, CUBE_PLACEMENT, ground);
  assert.equal(spawn.position[1], ground(spawn.position[0], spawn.position[2]),
    '有网格时方块的原点按它自己那一格的地面来（贴地由既有的原点离地偏移负责）');
  assert.ok(Math.abs(spawn.position[1] - 4) > 0.5, '与先知自己的 y 必须差得可观测，否则这条断言是空转的');
  // 没有网格（没给 / 采样不是有限数）时仍然是先知自己的高度，旧行为一步不变。
  for (const none of [undefined, null, 0, () => null, () => NaN]) {
    assert.deepEqual(cubeSpawnTransform(prophet([0, 0, -4]), 0, CUBE_PLACEMENT, none).position,
      [CUBE_PLACEMENT.sideDistance, 0, -4 + CUBE_PLACEMENT.forwardDistance],
      `没有可用地面时必须退回旧行为（${String(none)}）`);
  }
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
  assert.deepEqual(empty.map(entry => entry.id), ['sway', 'spin']);
  assert.deepEqual(empty.map(entry => entry.state), ['owned', 'owned']);
  assert.equal(empty[0].stateLabel, '可装填');
  assert.equal(empty[0].owned, true);
  assert.equal(empty[0].installed, false);
  assert.equal(empty[0].initial, true);
  assert.match(empty[0].details, /左右浮动 0\.25 米，周期 2\.40 秒/);
  assert.match(empty[0].details, /player不浮动/);
  assert.equal(empty[0].amplitudeUnit, '米');
  // The rotation fragment is described in degrees and says 不旋转, not 不浮动.
  assert.equal(empty[1].stateLabel, '可装填');
  assert.equal(empty[1].initial, true);
  assert.match(empty[1].details, /绕前后轴旋转 ±15\.00 度，周期 3\.60 秒/);
  assert.match(empty[1].details, /player不旋转/);
  assert.equal(empty[1].amplitudeUnit, '度');
  assert.equal(empty[1].amplitude, 15);
  assert.equal(empty[1].periodMs, 3600);

  const installed = fragmentPanel([{ fragmentId: 'sway' }, { fragmentId: 'spin' }]);
  assert.deepEqual(installed.map(entry => entry.state), ['installed', 'installed']);
  assert.equal(installed[0].stateLabel, '已装填');
  assert.equal(installed[0].installed, true);
  assert.equal(installed[1].stateLabel, '已装填');

  // A catalogued fragment the player does not own is shown as locked, not hidden.
  const locked = fragmentPanel([]).map(entry => entry.id);
  assert.deepEqual(locked, ['sway', 'spin']);
  assert.equal(ruleDetails({ axis: 'z', amplitude: 0.5, periodMs: 1000, excludeRoles: [] }),
    '前后浮动 0.50 米，周期 1.00 秒');
  assert.match(ruleDetails({}), /未知方向/);
  assert.equal(ruleDetails({ type: 'spin', axis: 'z', amplitude: 20, periodMs: 2000, excludeRoles: [] }),
    '绕前后轴旋转 ±20.00 度，周期 2.00 秒');
  assert.equal(ruleDetails({ type: 'spin', axis: 'z', amplitude: 20, periodMs: 2000,
    excludeRoles: ['player', 'prophet'] }), '绕前后轴旋转 ±20.00 度，周期 2.00 秒（player、prophet不旋转）');
});

test('fragment keys select an entry and toggling装填/拆下 rewrites the rule records', () => {
  assert.equal(fragmentForKey('1', []).id, 'sway');
  assert.equal(fragmentForKey('2', []).id, 'spin');
  assert.equal(fragmentForKey('3', []), null);
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

  // The rotation fragment installs as its own record, beside the sway.
  const spun = toggleFragment(loaded.rules, 'spin');
  assert.equal(spun.action, 'installed');
  assert.match(spun.feedback, /已装填：旋转碎片/);
  assert.deepEqual(spun.rules, [{ fragmentId: 'sway', type: 'sway', axis: 'x',
    amplitude: 0.25, periodMs: 2400 },
  { fragmentId: 'spin', type: 'spin', axis: 'z', amplitude: 15, periodMs: 3600 }]);
});

/** 装填一块碎片后得到的那份记录，作为「调整碎片」各条测试的共同起点。 */
const swayRecord = (amplitude = 0.25, periodMs = 2400) =>
  ({ fragmentId: 'sway', type: 'sway', axis: 'x', amplitude, periodMs });
const spinRecord = (amplitude = 15, periodMs = 3600) =>
  ({ fragmentId: 'spin', type: 'spin', axis: 'z', amplitude, periodMs });

test('「调整碎片」走一步只改一个数字，且始终贴着后端的上下限', () => {
  // 镜像自 game/runtime/world_rules.py：_AMPLITUDE_LIMIT = 100.0（第 24 行）、
  // _PERIOD_LIMIT_MS = 600_000（第 25 行），而 _positive()（第 34-36 行）要求
  // 0 < 值 <= 上限 —— 上限合法、0 不合法，所以前端的最小值必须严格大于 0。
  assert.equal(FRAGMENT_RULE_BOUNDS.sway.amplitude.max, 100.0);
  assert.equal(FRAGMENT_RULE_BOUNDS.sway.periodMs.max, 600_000);
  assert.ok(FRAGMENT_RULE_BOUNDS.sway.amplitude.min > 0, '0 会被 _positive() 拒绝');
  assert.ok(FRAGMENT_RULE_BOUNDS.sway.periodMs.min > 0);
  // spin 的幅度以「度」计，共用同一个 100 的上限，但步长与单位都是它自己的。
  assert.equal(FRAGMENT_RULE_BOUNDS.spin.amplitude.max, 100.0);
  assert.equal(FRAGMENT_RULE_BOUNDS.spin.amplitude.unit, '度');
  assert.equal(FRAGMENT_RULE_BOUNDS.spin.amplitude.step, 5);
  assert.ok(FRAGMENT_RULE_BOUNDS.spin.amplitude.min > 0);
  assert.equal(FRAGMENT_RULE_BOUNDS.spin.periodMs.max, 600_000);
  assert.equal(FRAGMENT_RULE_BOUNDS.spin.periodMs.unit, '秒');
  assert.notEqual(FRAGMENT_RULE_BOUNDS.spin.amplitude.step, FRAGMENT_RULE_BOUNDS.sway.amplitude.step,
    '旋转按 5 度一步，浮动按 0.05 米一步');
  assert.deepEqual(FRAGMENT_ADJUST_ACTIONS.map(action =>
    [action.id, action.key, action.group, action.label]), [
    ['amplitude-up', '1', '幅度', '幅度增大'],
    ['amplitude-down', '2', '幅度', '幅度减小'],
    ['period-up', '3', '周期', '周期加长'],
    ['period-down', '4', '周期', '周期缩短'],
    // 第 5 步是方向：规则的类型由碎片决定，轴由这里换（sway 可在 x/y/z 之间换）。
    ['axis-next', '5', '方向', '换一条浮动轴'],
  ]);
  assert.equal(fragmentAdjustActionForKey('1').id, 'amplitude-up');
  assert.equal(fragmentAdjustActionForKey('5').id, 'axis-next');

  const start = [swayRecord()];
  const up = adjustFragmentRule(start, 'sway', 'amplitude-up');
  assert.equal(up.changed, true);
  assert.equal(up.rules[0].amplitude, 0.3, '四舍五入，不是 0.30000000000000004');
  assert.equal(up.rules[0].periodMs, 2400, '周期没被碰过');
  assert.match(up.feedback, /幅度已调到 0\.30 米/);
  const back = adjustFragmentRule(up.rules, 'sway', 'amplitude-down');
  assert.equal(back.rules[0].amplitude, 0.25, '往回一步就回到原处');
  assert.deepEqual(back.rules, start);

  const longer = adjustFragmentRule(start, 'sway', 'period-up');
  assert.equal(longer.rules[0].periodMs, 2400 + FRAGMENT_RULE_BOUNDS.sway.periodMs.step);
  assert.equal(longer.rules[0].amplitude, 0.25);
  assert.match(longer.feedback, /周期已调到 2\.60 秒/);
  assert.equal(adjustFragmentRule(longer.rules, 'sway', 'period-down').rules[0].periodMs, 2400);
});

test('旋转碎片的幅度按「度」走 5 度一步，并且在两端都被夹住', () => {
  const start = [spinRecord()];
  const up = adjustFragmentRule(start, 'spin', 'amplitude-up');
  assert.equal(up.changed, true);
  assert.equal(up.rules[0].amplitude, 20, '15 度 + 5 度');
  assert.equal(up.rules[0].periodMs, 3600, '周期没被碰过');
  assert.equal(up.rules[0].type, 'spin', '类型与轴在调整后不变');
  assert.equal(up.rules[0].axis, 'z');
  assert.match(up.feedback, /幅度已调到 20\.00 度/, '反馈用的是度，不是米');
  const back = adjustFragmentRule(up.rules, 'spin', 'amplitude-down');
  assert.equal(back.rules[0].amplitude, 15);
  assert.deepEqual(back.rules, start);

  // 步长是 5 度一档，一路点到上限就停住，不会越过去。
  const climb = adjustFragmentRule([spinRecord(95)], 'spin', 'amplitude-up');
  assert.equal(climb.rules[0].amplitude, FRAGMENT_RULE_BOUNDS.spin.amplitude.max);
  assert.match(climb.feedback, /已达幅度上限/);
  const atMax = adjustFragmentRule(climb.rules, 'spin', 'amplitude-up');
  assert.equal(atMax.changed, false);
  assert.match(atMax.feedback, /幅度已是上限 100\.00 度/);
  // 下限同样：1 度是最小值，再往下就拒绝。
  const atMin = adjustFragmentRule([spinRecord(FRAGMENT_RULE_BOUNDS.spin.amplitude.min)], 'spin', 'amplitude-down');
  assert.equal(atMin.changed, false);
  assert.match(atMin.feedback, /幅度已是下限 1\.00 度/);
  // 周期仍然按毫秒步进、以秒显示，与浮动碎片同一条路径。
  const slower = adjustFragmentRule(start, 'spin', 'period-up');
  assert.equal(slower.rules[0].periodMs, 3600 + FRAGMENT_RULE_BOUNDS.spin.periodMs.step);
  assert.match(slower.feedback, /周期已调到 3\.80 秒/);
});

test('无论怎么点，产出的规则都落在 validate_rule / _positive() 接受的区间里', () => {
  // This is the mirror of validate_rule (world_rules.py:39-58): the same checks（类型、
  // 轴、类型与轴配套、幅度、周期，加上 fragmentId）。
  const backendValid = (rule, fragmentId) => rule.fragmentId === fragmentId
    && ['sway', 'spin'].includes(rule.type)
    && (rule.type === 'sway' ? ['x', 'y', 'z'] : ['z']).includes(rule.axis)
    && typeof rule.amplitude === 'number' && rule.amplitude > 0
    && rule.amplitude <= FRAGMENT_RULE_BOUNDS[rule.type].amplitude.max
    && typeof rule.periodMs === 'number' && rule.periodMs > 0
    && rule.periodMs <= FRAGMENT_RULE_BOUNDS[rule.type].periodMs.max;
  for (const [fragmentId, record, bounds] of [
    ['sway', swayRecord(50, 3000), FRAGMENT_RULE_BOUNDS.sway],
    ['spin', spinRecord(50, 3000), FRAGMENT_RULE_BOUNDS.spin],
  ]) {
    for (const [actionId, field, edge] of [
      ['amplitude-up', 'amplitude', 'max'], ['amplitude-down', 'amplitude', 'min'],
      ['period-up', 'periodMs', 'max'], ['period-down', 'periodMs', 'min'],
    ]) {
      let rules = [record];
      let feedback = '';
      let steps = 0;
      for (; steps < 4000; steps++) {
        const result = adjustFragmentRule(rules, fragmentId, actionId);
        for (const rule of result.rules) {
          assert.equal(backendValid(rule, fragmentId), true, JSON.stringify(rule));
        }
        if (!result.changed) { feedback = result.feedback; break; }
        rules = result.rules;
      }
      assert.equal(rules[0][field], bounds[field][edge], '停在边界上，不会越过去');
      assert.match(feedback, edge === 'max' ? /已是上限/ : /已是下限/);
      assert.ok(steps > 0 && steps < 4000, '先真的走了若干步，再被边界拦住');
    }
  }
  // 手改过的存档值也拉回区间内，绝不原样写回去。
  const repaired = adjustFragmentRule([swayRecord(100, 9e9)], 'sway', 'amplitude-down');
  assert.equal(repaired.changed, true);
  assert.equal(repaired.rules[0].periodMs, FRAGMENT_RULE_BOUNDS.sway.periodMs.max);
  assert.equal(backendValid(repaired.rules[0], 'sway'), true);
  const repairedSpin = adjustFragmentRule([spinRecord(1e6, 9e9)], 'spin', 'amplitude-down');
  assert.equal(repairedSpin.changed, true);
  assert.equal(repairedSpin.rules[0].periodMs, FRAGMENT_RULE_BOUNDS.spin.periodMs.max,
    '手改过的周期被拉回上限');
  assert.equal(repairedSpin.rules[0].amplitude, FRAGMENT_RULE_BOUNDS.spin.amplitude.max,
    '手改过的幅度被夹在上限，而不是原样写回 1e6');
  assert.equal(backendValid(repairedSpin.rules[0], 'spin'), true);
});

test('调整碎片：未知动作、未知碎片、未装填一律拒绝，且从不抛异常', () => {
  const installed = [swayRecord()];
  for (const [rules, id, action] of [
    [installed, 'sway', 'nope'], [installed, 'nope', 'amplitude-up'],
    [[], 'sway', 'amplitude-up'], [null, 'sway', 'amplitude-up'],
    [installed, 'sway', null], [installed, null, 'amplitude-up'],
  ]) {
    const result = adjustFragmentRule(rules, id, action);
    assert.equal(result.changed, false, `${String(id)} / ${String(action)}`);
    assert.ok(result.feedback.length > 0);
  }
  assert.match(adjustFragmentRule([], 'sway', 'amplitude-up').feedback, /先装填该碎片再调整/);
  assert.deepEqual(adjustFragmentRule([], 'sway', 'amplitude-up').rules, [], '一条也没写');
  assert.match(adjustFragmentRule(installed, 'nope', 'amplitude-up').feedback, /未知碎片/);
  assert.match(adjustFragmentRule(installed, 'sway', 'nope').feedback, /未知的调整/);
  // 记录里读不到数字时退回目录默认值，产出的仍是合法数字。
  const bare = adjustFragmentRule([{ fragmentId: 'sway' }], 'sway', 'amplitude-up');
  assert.equal(bare.changed, true);
  assert.equal(bare.rules[0].amplitude, 0.3);
  assert.equal(bare.rules[0].periodMs, 2400);
  assert.equal(fragmentRuleOf(null, 'sway'), null);
  assert.equal(fragmentRuleOf(installed, 'nope'), null);
  assert.equal(fragmentRuleOf(installed, 'sway').periodMs, 2400);
});

test('调整一块碎片不动其它记录，也不改它们的顺序', () => {
  const other = { fragmentId: 'other', type: 'sway', axis: 'y', amplitude: 0.5, periodMs: 1000 };
  const third = { fragmentId: 'third', type: 'sway', axis: 'z', amplitude: 1.5, periodMs: 5000 };
  const rules = [other, swayRecord(), third];
  const result = adjustFragmentRule(rules, 'sway', 'period-up');
  assert.equal(result.changed, true);
  assert.deepEqual(result.rules.map(rule => rule.fragmentId), ['other', 'sway', 'third']);
  assert.equal(result.rules[0], other, '没被改的记录还是原来那一份');
  assert.equal(result.rules[2], third);
  assert.equal(result.rules[1].periodMs, 2600);
  assert.equal(rules[1].periodMs, 2400, '传入的列表本身不被就地修改');
});

test('「调整碎片」子屏：按键分发、当前数字，以及未装填时的说法', () => {
  assert.equal(prophetKeyAction('2').screen, 'fragments', '主选项没被改动');
  assert.equal(prophetKeyAction('1', 'fragments').fragmentId, 'sway');
  assert.equal(prophetKeyAction('9', 'fragments'), null);
  assert.equal(prophetKeyAction('1', 'fragment-adjust').type, 'adjust-fragment');
  assert.equal(prophetKeyAction('1', 'fragment-adjust').id, 'amplitude-up');
  assert.equal(prophetKeyAction('4', 'fragment-adjust', 'sway').fragmentId, 'sway');
  // 5 是「换一条浮动轴」，与子屏里的第五个按钮同一条分发。
  assert.equal(prophetKeyAction('5', 'fragment-adjust').id, 'axis-next');
  assert.equal(prophetKeyAction('1', 'fragment-adjust').type, 'adjust-fragment');

  const installed = [swayRecord(0.6, 1200)];
  const panel = fragmentAdjustPanel(installed, 'sway');
  assert.equal(panel.name, '浮动碎片');
  assert.equal(panel.installed, true);
  assert.equal(panel.amplitude, 0.6);
  assert.equal(panel.amplitudeLabel, '幅度 0.60 米');
  assert.equal(panel.periodLabel, '周期 1.20 秒');
  assert.equal(panel.actions, FRAGMENT_ADJUST_ACTIONS);
  assert.equal(panel.hint, '');
  // 「碎片」屏那一行显示的也必须是装填进去的数字，而不是目录默认值。
  assert.match(fragmentPanel(installed)[0].details, /左右浮动 0\.60 米，周期 1\.20 秒/);
  assert.match(fragmentPanel(installed)[0].details, /player不浮动/);

  const missing = fragmentAdjustPanel([], 'sway');
  assert.equal(missing.installed, false);
  assert.equal(missing.amplitude, 0.25, '先显示目录默认值，子屏不至于是空白');
  assert.match(missing.hint, /先装填该碎片再调整/);
  assert.equal(fragmentAdjustPanel([], 'nope').known, false);
  assert.match(fragmentAdjustPanel([], 'nope').hint, /先装填该碎片再调整/);

  // 旋转碎片的子屏：相同的四个按键，数字按「度」显示，标题行说的是绕轴旋转。
  assert.equal(prophetKeyAction('2', 'fragments').fragmentId, 'spin', '第二块碎片用 2 键');
  const spinPanel = fragmentAdjustPanel([spinRecord(20, 1800)], 'spin');
  assert.equal(spinPanel.name, '旋转碎片');
  assert.equal(spinPanel.installed, true);
  assert.equal(spinPanel.amplitude, 20);
  assert.equal(spinPanel.amplitudeLabel, '幅度 20.00 度');
  assert.equal(spinPanel.periodLabel, '周期 1.80 秒');
  assert.equal(spinPanel.amplitudeUnit, '度');
  assert.match(spinPanel.details, /绕前后轴旋转 ±20\.00 度/);
  assert.match(fragmentPanel([spinRecord(20, 1800)])[1].details, /绕前后轴旋转 ±20\.00 度，周期 1\.80 秒/);
  assert.match(fragmentPanel([spinRecord(20, 1800)])[1].details, /player不旋转/);
  const spinMissing = fragmentAdjustPanel([], 'spin');
  assert.equal(spinMissing.installed, false);
  assert.equal(spinMissing.amplitude, 15, '先显示目录默认值');
  assert.equal(spinMissing.amplitudeLabel, '幅度 15.00 度');
  assert.equal(spinMissing.periodLabel, '周期 3.60 秒');
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
      entity_type: 'story_character', entity_id: 'story.prophet',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'boss', name: 'Boss', handle: 13, semantic_role: 'boss',
      entity_type: 'story_character', entity_id: 'story.boss',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'merchant', name: '商人', handle: 14, semantic_role: 'merchant',
      entity_type: 'story_character', entity_id: 'story.merchant',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'placed-fragment', name: '世界碎片1', handle: 15, semantic_role: 'placement',
      entity_type: 'story_prop', entity_id: 'story.placement.world-fragment',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'placed-block', name: '方块1', handle: 16, semantic_role: 'placement',
      entity_type: 'story_prop', entity_id: 'story.placement.block',
      geometry: { position: [2, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    // 地图：handle 与几何都和上面的方块一样齐全，唯一不同的是身份。它必须被身份挡掉，
    // 而不是碰巧因为缺字段缺席——否则这条守卫什么都没证明。
    { actor_guid: MAP_GUID, name: '剧情世界地图', handle: 18, semantic_role: 'world-map',
      entity_type: MAP_ENTITY_TYPE, entity_id: MAP_ENTITY_ID, model_ref: MAP_MODEL_REF,
      geometry: { position: [0, 6.6477, 0], rotation: [0, 0, 0], scale: [90, 90, 90] } },
  ] } } };
  const list = selectableObjects(snapshot);
  assert.deepEqual(list.map(entry => entry.guid), ['placed-block', 'cube'],
    'only small-world objects; never a character (player/prophet/boss/merchant), '
    + 'never a world fragment — but a block the player placed IS a small-world object');
  assert.ok(!list.some(entry => entry.guid === MAP_GUID), '地图不算物体：调整列表里没有它');
  assert.ok(!list.some(entry => entry.transform?.scale?.[0] === 90), '地图的 90 倍缩放不可调');
  // Non-vacuity: the same actor with the map's identity fields removed IS listed, so it is the
  // identity guard that excludes it — not a missing handle, name, role or transform.
  const anonymousMap = { actor_guid: 'scenery-1', name: '布景', handle: 19, semantic_role: 'scenery',
    model_ref: '', entity_id: '', entity_type: '',
    geometry: { position: [0, 6.6477, 0], rotation: [0, 0, 0], scale: [90, 90, 90] } };
  assert.deepEqual(selectableObjects({ data: { scene: { actors: [anonymousMap] } } })
    .map(entry => entry.guid), ['scenery-1'],
  '去掉地图身份后同一个 actor 会被列出，证明挡住它的是 isMapActor');
  // Each identity field has to be enough on its own: a world whose record lost its guid (or its
  // version marker) must still keep the map out of the list instead of offering it for resizing.
  for (const identity of [{ actor_guid: MAP_GUID }, { model_ref: MAP_MODEL_REF },
    { entity_id: MAP_ENTITY_ID }, { entity_type: MAP_ENTITY_TYPE }]) {
    const partial = { ...anonymousMap, ...identity };
    assert.deepEqual(selectableObjects({ data: { scene: { actors: [partial] } } }), [],
      `身份字段 ${Object.keys(identity)[0]} 单独出现也必须挡住地图`);
  }
  assert.deepEqual(list.find(entry => entry.guid === 'cube').transform.position, [1, 0, 1]);
  assert.deepEqual(list.find(entry => entry.guid === 'placed-block').transform.position, [2, 0, 0]);
  assert.equal(objectLabel(list.find(entry => entry.guid === 'cube')), '先知方块（prophet-cube）');
  assert.equal(objectLabel({}), '未命名（物体）');
  // Another collectible prop under the same prefix is still a collectible, not scenery.
  assert.deepEqual(selectableObjects({ data: { scene: { actors: [
    { actor_guid: 'frag-2', name: '世界碎片2', handle: 16, semantic_role: 'placement',
      entity_type: 'story_prop', entity_id: 'story.placement.world-fragment',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    { actor_guid: 'placed-block-2', name: '方块2', handle: 17, semantic_role: 'placement',
      entity_type: 'story_prop', entity_id: 'story.placement.block',
      geometry: { position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
  ] } } }).map(entry => entry.guid), ['placed-block-2'],
  '只排除碎片这一件，同角色的方块仍然可调');
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

// ---- 碎片改为可作用于单独物体：目标先选，再装填/调整 ------------------------------

const BLOCK_A = 'story.placement.0003';
const BLOCK_B = 'story.placement.0007';

test('装填到选中的物体上：一块碎片只有一个目标，换目标只改那一条记录', () => {
  const installed = toggleFragment([], 'sway', { target: BLOCK_A, label: '方块A（放置物）' });
  assert.equal(installed.action, 'installed');
  assert.equal(installed.target, BLOCK_A);
  assert.deepEqual(installed.rules, [{ fragmentId: 'sway', type: 'sway', axis: 'x',
    amplitude: 0.25, periodMs: 2400, target: BLOCK_A }], 'target 就是 actor 的 guid');
  assert.match(installed.feedback, /方块A/);
  // 换到另一个物体：改目标，不新增第二条记录（一块碎片只有一条规则、一个目标）。
  const moved = toggleFragment(installed.rules, 'sway', { target: BLOCK_B });
  assert.equal(moved.action, 'retargeted');
  assert.equal(moved.rules.length, 1);
  assert.equal(moved.rules[0].target, BLOCK_B);
  assert.equal(moved.rules[0].amplitude, 0.25, '换目标不动数字');
  assert.match(moved.feedback, /已改目标/);
  // 同一个目标再点一次 = 拆下。
  const removed = toggleFragment(moved.rules, 'sway', { target: BLOCK_B });
  assert.equal(removed.action, 'removed');
  assert.deepEqual(removed.rules, []);
  // 另一块碎片可以同时装在另一个物体上：两条记录各带各的目标。
  const both = toggleFragment(installed.rules, 'spin', { target: BLOCK_B });
  assert.deepEqual(both.rules.map(rule => [rule.fragmentId, rule.target]),
    [['sway', BLOCK_A], ['spin', BLOCK_B]]);
  assert.equal(installedTargetOf(both.rules, 'sway'), BLOCK_A);
  assert.equal(installedTargetOf(both.rules, 'spin'), BLOCK_B);
  assert.equal(installedTargetOf(both.rules, 'nope'), null);
});

test('调整只改「这块碎片 × 这个物体」那一条记录，别的记录一个数都不动', () => {
  const rules = toggleFragment(toggleFragment([], 'sway', { target: BLOCK_A }).rules,
    'spin', { target: BLOCK_B }).rules;
  const up = adjustFragmentRule(rules, 'sway', 'amplitude-up', { target: BLOCK_A });
  assert.equal(up.changed, true);
  assert.equal(fragmentRuleOf(up.rules, 'sway', { target: BLOCK_A }).amplitude, 0.3);
  assert.equal(fragmentRuleOf(up.rules, 'spin', { target: BLOCK_B }).amplitude, 15,
    '另一件物体上的记录没被碰');
  assert.equal(up.rules.find(rule => rule.fragmentId === 'sway').target, BLOCK_A,
    '调整不会把规则悄悄放回整个小世界');
  // 目标不对就不动：拿 B 的 guid 去调只装在 A 上的碎片，什么也不会发生。
  const wrong = adjustFragmentRule(rules, 'sway', 'amplitude-up', { target: BLOCK_B });
  assert.equal(wrong.changed, false);
  assert.match(wrong.feedback, /先在该物体上装填浮动碎片/);
  assert.equal(wrong.rules, rules, '被拒绝的调整连列表都不换');
  // 换轴同样只落在那一条记录上，并且保留它的目标。
  const axis = adjustFragmentRule(rules, 'sway', 'axis-next', { target: BLOCK_A });
  assert.equal(axis.changed, true);
  const swayed = axis.rules.find(rule => rule.fragmentId === 'sway');
  assert.equal(swayed.axis, 'y', 'x → y');
  assert.equal(swayed.target, BLOCK_A);
  assert.equal(axis.rules.find(rule => rule.fragmentId === 'spin').axis, 'z', '旋转规则没被换轴');
  // 旋转只能绕 z：按 5 只得到一句说明，不产生新记录。
  const noAxis = adjustFragmentRule(rules, 'spin', 'axis-next', { target: BLOCK_B });
  assert.equal(noAxis.changed, false);
  assert.match(noAxis.feedback, /只能绕 z 轴/);
});

test('「调整碎片」子屏先选目标：没有目标时只说清楚该先做什么，不写任何记录', () => {
  const rules = toggleFragment([], 'sway', { target: BLOCK_A }).rules;
  // 选了 B，但 B 上没有这条碎片：子屏要说明，动作全部拒绝。
  const atB = fragmentAdjustPanel(rules, 'sway', { target: BLOCK_B, label: '方块B' });
  assert.equal(atB.target, BLOCK_B);
  assert.equal(atB.targetLabel, '方块B');
  assert.equal(atB.installed, false);
  assert.match(atB.hint, /先在该物体上装填/);
  assert.equal(adjustFragmentRule(rules, 'sway', 'amplitude-up', { target: BLOCK_B }).changed, false);
  // 选了 A：显示的就是 A 上那条记录。
  const atA = fragmentAdjustPanel(rules, 'sway', { target: BLOCK_A, label: '方块A' });
  assert.equal(atA.installed, true);
  assert.equal(atA.targetLabel, '方块A');
  assert.equal(atA.hint, '');
  assert.equal(atA.axis, 'x');
  // 没有目标的老记录：子屏说的是「整个小世界」，并且认得出它是旧规则。
  const legacyPanel = fragmentAdjustPanel([{ fragmentId: 'sway' }], 'sway');
  assert.equal(legacyPanel.target, null);
  assert.equal(legacyPanel.targetLabel, '整个小世界');
  assert.equal(legacyPanel.legacy, true);
  assert.equal(isLegacyRule({ fragmentId: 'sway' }), true);
  assert.equal(isLegacyRule({ fragmentId: 'sway', target: BLOCK_A }), false);
  // 「碎片」屏那一行写的是作用对象，而不是一串 guid。
  const listed = fragmentPanel(rules, { [BLOCK_A]: '方块A' }).find(entry => entry.id === 'sway');
  assert.equal(listed.target, BLOCK_A);
  assert.equal(listed.targetLabel, '方块A');
  assert.equal(listed.legacy, false);
});

test('地图与先知既不在目标列表里，也装不上：UI 与模型层都拒绝', () => {
  // 目标列表 = 调整物体那份列表：只有小世界里的物体，地图与先知按身份排除。
  const snapshot = { data: { scene: { actors: [
    { actor_guid: BLOCK_A, name: '方块A', handle: 16, semantic_role: 'placement',
      entity_type: 'story_prop', entity_id: 'story.placement.block',
      geometry: { position: [2, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] } },
    prophet([0, 0, -4]),
    { actor_guid: MAP_GUID, name: '剧情世界地图', handle: 18, semantic_role: 'world-map',
      entity_type: MAP_ENTITY_TYPE, entity_id: MAP_ENTITY_ID, model_ref: MAP_MODEL_REF,
      geometry: { position: [0, 6.6477, 0], rotation: [0, 0, 0], scale: [90, 90, 90] } },
  ] } } };
  const targets = selectableObjects(snapshot).map(entry => entry.guid);
  assert.deepEqual(targets, [BLOCK_A], '只有方块是候选目标');
  assert.ok(!targets.includes(MAP_GUID));
  assert.ok(!targets.includes(PROPHET_GUID));
  // 即便绕过列表直接点名，模型层也拒绝：既装不上，也不会生成一条记录。
  for (const reserved of [MAP_GUID, PROPHET_GUID]) {
    assert.throws(() => toggleFragment([], 'sway', { target: reserved }), /世界规则目标不能是地图或先知/);
  }
  const rules = toggleFragment([], 'sway', { target: BLOCK_A }).rules;
  const refused = adjustFragmentRule(rules, 'sway', 'amplitude-up', { target: MAP_GUID });
  assert.equal(refused.changed, false);
  assert.deepEqual(refused.rules, rules, '一条指向地图的调整写不出任何东西');
});

