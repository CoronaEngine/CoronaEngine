import assert from 'node:assert/strict';
import test from 'node:test';
import { DEGREES_TO_RADIANS, FRAGMENTS, RESERVED_TARGET_GUIDS, WORLD_RULE_VERSION,
  collectSwayTargets, createWorldRuleRunner, findFragment, initialFragmentIds, isReservedTargetGuid,
  normalizeTarget, ownedFragmentIds, ruleOffset, ruleRotationOffset, totalOffset,
  totalRotationOffset, validateRule } from '../../frontend/storyWorldRules.mjs';
import { MAP_ENTITY_ID, MAP_ENTITY_TYPE, MAP_GUID, MAP_MODEL_REF } from '../../frontend/storyMap.mjs';
import { PROPHET_ENTITY_ID, PROPHET_GUID, PROPHET_MODEL_REF } from '../../frontend/storyCharacters.mjs';

const SWAY = findFragment('sway').rule;
const SPIN = findFragment('spin').rule;
const snapshotWith = actors => ({ data: { scene: { actors } } });
const actor = (guid, handle, position, extra = {}) => ({
  // 这个默认角色以前写的是 'prophet'（当年只是拿它当「有名字的物体」用）。先知现在由
  // `isProphetActor` 按身份排除，所以夹具里普通的可动画物体必须换一个中性的角色名，
  // 否则这些用例会变成在测「先知也被排除」，而不是在测规则本身。
  actor_guid: guid, handle, semantic_role: 'object', visible: true,
  geometry: { position: position ? [...position] : undefined, rotation: [0, 0, 0], scale: [1, 1, 1] },
  ...extra,
});

function manualScheduler() {
  let queued = null;
  const cancelled = [];
  return {
    requestFrame: callback => { queued = callback; return 7; },
    cancelFrame: id => { cancelled.push(id); queued = null; },
    fire(timestamp) { const callback = queued; queued = null; if (callback) callback(timestamp); },
    get pending() { return queued !== null; },
    cancelled,
  };
}

function recordingBridge() {
  const calls = [];
  return { calls, actorTransform: (handle, operation, vector) => {
    calls.push([handle, operation, [...vector]]); return true; } };
}

/** The last value written through one operation (0 position, 1 rotation) of one actor. */
function lastWrite(bridge, handle, operation) {
  const call = bridge.calls.filter(entry => entry[0] === handle && entry[1] === operation).at(-1);
  return call ? call[2] : null;
}

const countWrites = (bridge, handle, operation) =>
  bridge.calls.filter(entry => entry[0] === handle && entry[1] === operation).length;

test('the catalogue grants two initial fragments: a left/right sway and a z rotation', () => {
  assert.equal(WORLD_RULE_VERSION, 1);
  assert.deepEqual(FRAGMENTS.map(fragment => fragment.id), ['sway', 'spin']);
  assert.deepEqual(initialFragmentIds(), ['sway', 'spin']);
  assert.deepEqual(ownedFragmentIds(), ['sway', 'spin']);
  assert.equal(findFragment('nope'), null);
  const rule = validateRule(SWAY);
  assert.equal(rule.type, 'sway');
  assert.equal(rule.axis, 'x', 'left/right means the x axis');
  assert.deepEqual([...rule.excludeRoles], ['player'], 'the player must not be swayed');
  assert.ok(rule.amplitude > 0 && rule.amplitude <= 1, 'a small amplitude');
  assert.ok(rule.periodMs >= 500, 'a period slow enough to read as floating');

  // The spin turns objects around z; its amplitude is degrees, not metres.
  const spin = validateRule(SPIN);
  assert.equal(spin.type, 'spin');
  assert.equal(spin.axis, 'z', 'a rotation around z is the small world turn around the vertical');
  assert.deepEqual([...spin.excludeRoles], ['player'], 'the player must not be spun');
  assert.ok(spin.amplitude >= 5 && spin.amplitude <= 45, 'visible but clearly a small turn');
  assert.ok(spin.periodMs >= 1000, 'a period slow enough to read as a turn');
  // Both rules turn at a comparable pace, so installing both reads as one motion.
  const swaySpeed = SWAY.amplitude * 2 * Math.PI / SWAY.periodMs;
  const spinSpeed = SPIN.amplitude * DEGREES_TO_RADIANS * 2 * Math.PI / SPIN.periodMs;
  assert.ok(spinSpeed > swaySpeed / 2 && spinSpeed < swaySpeed * 2,
    `the two rules should feel alike: ${spinSpeed} vs ${swaySpeed}`);
});

test('rule validation rejects anything the frame loop could not survive', () => {
  for (const bad of [null, {}, { ...SWAY, type: 'orbit' }, { ...SWAY, axis: 'w' },
    // A spin may only turn around z, exactly like game/runtime/world_rules.py:51-52.
    { ...SPIN, axis: 'x' }, { ...SPIN, axis: 'y' },
    { ...SWAY, amplitude: 0 }, { ...SWAY, amplitude: -1 }, { ...SWAY, amplitude: NaN },
    { ...SWAY, amplitude: '0.5' }, { ...SWAY, amplitude: Infinity },
    { ...SWAY, periodMs: 0 }, { ...SWAY, periodMs: -10 }, { ...SWAY, periodMs: '1' },
    { ...SPIN, amplitude: 0 }, { ...SPIN, amplitude: -15 }, { ...SPIN, periodMs: 0 }]) {
    assert.throws(() => validateRule(bad), /世界规则|浮动轴|规则类型/, JSON.stringify(bad));
  }
  // Both catalogue types accept a hand-built rule of the same shape.
  assert.equal(validateRule({ type: 'spin', axis: 'z', amplitude: 15, periodMs: 3600 }).type, 'spin');
  assert.equal(validateRule({ type: 'sway', axis: 'y', amplitude: 1.5, periodMs: 900 }).axis, 'y');
  // excludeRoles is optional, trimmed and de-duplicated rather than trusted.
  assert.deepEqual([...validateRule({ ...SWAY, excludeRoles: [' player ', 'player', 7, ''] }).excludeRoles],
    ['player']);
  assert.deepEqual([...validateRule({ ...SWAY, excludeRoles: undefined }).excludeRoles], []);
});

test('ruleOffset is a pure sine on the chosen axis and repeats every period', () => {
  const period = SWAY.periodMs;
  assert.deepEqual(ruleOffset(SWAY, 0), [0, 0, 0]);
  const peak = ruleOffset(SWAY, period / 4);
  assert.ok(Math.abs(peak[0] - SWAY.amplitude) < 1e-12, 'quarter period peaks right');
  assert.deepEqual([peak[1], peak[2]], [0, 0], 'x sway must not lift or push the object');
  const trough = ruleOffset(SWAY, (3 * period) / 4);
  assert.ok(Math.abs(trough[0] + SWAY.amplitude) < 1e-12, 'three quarter period peaks left');
  const wrapped = ruleOffset(SWAY, period + period / 4);
  assert.ok(Math.abs(wrapped[0] - peak[0]) < 1e-12, 'left/right repeats continuously');
  assert.ok(Math.abs(ruleOffset(SWAY, period / 2)[0]) < 1e-12, 'half period returns to centre');
  // Other axes stay available for future rules.
  assert.deepEqual(ruleOffset({ ...SWAY, axis: 'y' }, period / 4), [0, SWAY.amplitude, 0]);
  assert.deepEqual(ruleOffset({ ...SWAY, axis: 'z' }, period / 4), [0, 0, SWAY.amplitude]);
  for (const bad of [-1, NaN, Infinity, '0']) assert.throws(() => ruleOffset(SWAY, bad), /运行时长/);
  assert.deepEqual(totalOffset([SWAY, { ...SWAY, axis: 'y' }], period / 4),
    [SWAY.amplitude, SWAY.amplitude, 0]);
  assert.deepEqual(totalOffset(null, 10), [0, 0, 0]);
  // A spin contributes no position at all: it must never double-write a swayed actor.
  assert.deepEqual(ruleOffset(SPIN, SPIN.periodMs / 4), [0, 0, 0]);
  assert.deepEqual(totalOffset([SWAY, SPIN], period / 4), [SWAY.amplitude, 0, 0]);
});

test('ruleRotationOffset is a pure sine in radians, and a sway never turns anything', () => {
  const period = SPIN.periodMs;
  assert.deepEqual(ruleRotationOffset(SPIN, 0), [0, 0, 0]);
  const peak = ruleRotationOffset(SPIN, period / 4);
  assert.ok(Math.abs(peak[2] - SPIN.amplitude * DEGREES_TO_RADIANS) < 1e-12,
    'a quarter period turns by the whole amplitude, converted to radians');
  assert.deepEqual([peak[0], peak[1]], [0, 0], 'a z rotation stays on the z euler angle');
  const trough = ruleRotationOffset(SPIN, (3 * period) / 4);
  assert.ok(Math.abs(trough[2] + SPIN.amplitude * DEGREES_TO_RADIANS) < 1e-12, 'and back the other way');
  assert.ok(Math.abs(ruleRotationOffset(SPIN, period / 2)[2]) < 1e-12, 'half period is the base rotation');
  assert.deepEqual(ruleRotationOffset(SWAY, period / 4), [0, 0, 0], 'a sway moves, it does not turn');
  for (const bad of [-1, NaN, '0']) assert.throws(() => ruleRotationOffset(SPIN, bad), /运行时长/);
  assert.deepEqual(totalRotationOffset([SPIN], period / 4), peak);
  assert.deepEqual(totalRotationOffset(null, 10), [0, 0, 0]);
});

test('collectSwayTargets keeps only movable world objects and copies their base transform', () => {
  const snapshot = snapshotWith([
    actor('a', 11, [1, 0, 2]),
    actor('player', 12, [0, 0, 0], { semantic_role: 'player' }),
    actor('hidden', 13, [3, 0, 3], { visible: false }),
    actor('no-handle', 0, [4, 0, 4]),
    actor('no-position', 15, null, { geometry: { position: undefined } }),
  ]);
  const targets = collectSwayTargets(snapshot, SWAY);
  assert.deepEqual(targets.map(target => target.guid), ['a']);
  assert.deepEqual(targets[0].base, [1, 0, 2]);
  assert.deepEqual(targets[0].baseRotation, [0, 0, 0], 'the spin needs a base rotation too');
  // The base is a snapshot: the array is a copy, so the rule keeps its own numbers.
  snapshot.data.scene.actors[0].geometry.position[0] = 99;
  snapshot.data.scene.actors[0].geometry.rotation[1] = 9;
  assert.deepEqual(targets[0].base, [1, 0, 2]);
  assert.deepEqual(targets[0].baseRotation, [0, 0, 0]);
  // A fresh read of the mutated actor proves the base really is an independent copy.
  const later = collectSwayTargets(snapshot, SWAY);
  assert.deepEqual(later[0].base, [99, 0, 2]);
  assert.deepEqual(later[0].baseRotation, [0, 9, 0]);
  assert.deepEqual(collectSwayTargets({}, SWAY), []);
  assert.deepEqual(collectSwayTargets(snapshotWith([actor('p', 1, [0, 0, 0], { semantic_role: 'player' })]), SWAY), []);
  // Called without a rule it is the shared list for a world with several rules, so the
  // player is excluded even though the exclusion comes from the other fragment.
  const shared = collectSwayTargets(snapshotWith([
    actor('a', 11, [1, 0, 2]),
    actor('player', 12, [0, 0, 0], { semantic_role: 'player' }),
  ]));
  assert.deepEqual(shared.map(target => target.guid), ['a']);
  assert.deepEqual(shared[0].base, [1, 0, 2]);
  assert.deepEqual(shared[0].baseRotation, [0, 0, 0]);
  const tilted = collectSwayTargets(snapshotWith([
    actor('t', 21, [0, 1, 0], { geometry: { position: [0, 1, 0], rotation: [0, 0.5, 0] } })]));
  assert.deepEqual(tilted[0].baseRotation, [0, 0.5, 0]);

  // 地图不算物体：它是所有东西站着的面，摆位只有一个作者，规则不许写它的 handle。它有
  // handle、有几何、角色也不是被排除的那几个，所以挡住它的只能是身份——每个身份字段都
  // 必须单独够用，否则一个丢了 guid 的老世界就会把地图交回给规则。
  const movable = actor('movable', 31, [1, 0, 1]);
  for (const identity of [{ actor_guid: MAP_GUID }, { model_ref: MAP_MODEL_REF },
    { entity_id: MAP_ENTITY_ID }, { entity_type: MAP_ENTITY_TYPE }]) {
    const map = actor('map-ish', 91, [0, 6.6477, 0],
      { semantic_role: 'world-map', geometry: { position: [0, 6.6477, 0], rotation: [0, 0, 0],
        scale: [90, 90, 90] }, ...identity });
    assert.deepEqual(collectSwayTargets(snapshotWith([map, movable]), SWAY).map(target => target.guid),
      ['movable'], `身份字段 ${Object.keys(identity)[0]} 单独出现也必须让地图退出规则目标`);
  }
  // Non-vacuity: with the identity fields blanked out the very same actor IS a target, so it is
  // the identity guard that keeps the map out — not a missing handle, role or transform.
  const anonymous = actor('anonymous', 91, [0, 6.6477, 0], { semantic_role: 'scenery',
    model_ref: '', entity_id: '', entity_type: '' });
  assert.deepEqual(collectSwayTargets(snapshotWith([anonymous, movable]), SWAY)
    .map(target => target.guid), ['anonymous', 'movable'],
  '去掉地图身份后同一个 actor 会进入规则目标，证明挡住的它的是 isMapActor');
});

test('the runner sways every target on its own frame loop and restores the base on stop', () => {
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const errors = [];
  const runner = createWorldRuleRunner({
    bridge: () => bridge, rules: [SWAY],
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [1, 0, 2] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
    onError: error => errors.push(error),
  });

  assert.equal(runner.isRunning(), false);
  assert.equal(runner.start(), true);
  assert.equal(runner.isRunning(), true);
  assert.equal(scheduler.pending, true);
  assert.equal(runner.start(), true, 'starting twice must not add a second loop');

  scheduler.fire(1000);
  assert.deepEqual(lastWrite(bridge, 11, 0), [1, 0, 2], 'the first frame sits on the base');
  // Real frames are ~16 ms; advance in small steps so the per-frame clamp never bites.
  const STEP_MS = 50;
  const steps = SWAY.periodMs / 4 / STEP_MS;
  for (let i = 1; i <= steps; i++) scheduler.fire(1000 + STEP_MS * i);
  const peak = lastWrite(bridge, 11, 0);
  assert.ok(Math.abs(peak[0] - (1 + SWAY.amplitude)) < 1e-9, 'the object floats to the right');
  assert.equal(peak[2], 2, 'and stays on its own depth');

  // A stalled tab must not teleport the object: one frame advances at most 100 ms.
  const maxStep = SWAY.amplitude * (2 * Math.PI / SWAY.periodMs) * 100;
  scheduler.fire(1000 + SWAY.periodMs / 4 + 5000);
  const stalled = lastWrite(bridge, 11, 0);
  assert.ok(Math.abs(stalled[0] - peak[0]) <= maxStep + 1e-9,
    'a five second stall advances a single clamped frame');

  // Moving the prop by hand re-anchors the sway instead of snapping it back.
  assert.equal(runner.refreshBase('a', [5, 0, 5]), true);
  assert.deepEqual(lastWrite(bridge, 11, 0), [5, 0, 5]);
  assert.equal(runner.refreshBase('missing', [0, 0, 0]), false);

  assert.equal(runner.stop(), true);
  assert.equal(runner.isRunning(), false);
  assert.deepEqual(lastWrite(bridge, 11, 0), [5, 0, 5], 'stopping leaves the object where it belongs');
  assert.equal(scheduler.cancelled.length, 1);
  assert.equal(runner.stop(), false, 'stopping twice is harmless');
  assert.deepEqual(errors, []);
});

test('sway and spin are composed into exactly one position and one rotation write per frame', () => {
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const runner = createWorldRuleRunner({
    bridge: () => bridge, rules: [SWAY, SPIN],
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [1, 0, 2], baseRotation: [0, 0.5, 0] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  assert.equal(runner.ruleCount(), 2, 'both rules are installed at once');
  assert.equal(runner.start(), true);

  const STEP_MS = 50;
  const elapsed = SWAY.periodMs / 4;   // where the sway peaks
  for (let i = 0; i * STEP_MS <= elapsed; i++) scheduler.fire(1000 + i * STEP_MS);

  // Position: the base moved by the sway only. The spin must not move anything.
  assert.deepEqual(lastWrite(bridge, 11, 0), [1 + SWAY.amplitude, 0, 2]);
  // Rotation: the base rotation turned by the spin only, in radians.
  const expectedTurn = SPIN.amplitude * DEGREES_TO_RADIANS
    * Math.sin((2 * Math.PI * elapsed) / SPIN.periodMs);
  const rotation = lastWrite(bridge, 11, 1);
  assert.ok(Math.abs(rotation[2] - expectedTurn) < 1e-9, 'the rotation follows the spin sine');
  assert.deepEqual([rotation[0], rotation[1]], [0, 0.5], 'the rest of the euler angles are the base');
  // Scale is never written by a rule.
  assert.deepEqual(bridge.calls.filter(call => call[1] === 2), []);
  // One write of each operation per actor per frame: two rules, still two calls — not four.
  const frames = 1 + Math.floor(elapsed / STEP_MS);
  assert.equal(countWrites(bridge, 11, 0), frames, 'one position write per frame');
  assert.equal(countWrites(bridge, 11, 1), frames, 'one rotation write per frame');
  assert.equal(bridge.calls.length, frames * 2, 'the two rules never write the same actor twice');

  // Two actors, two rules: still one position and one rotation write each per frame.
  const second = recordingBridge();
  const many = createWorldRuleRunner({
    bridge: () => second, rules: [SWAY, SPIN],
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [0, 0, 0] },
      { guid: 'b', role: 'object', handle: 12, base: [2, 0, 3] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  many.start();
  scheduler.fire(2000);
  assert.equal(second.calls.length, 4, 'two actors, two operations, one frame');

  // Stopping restores BOTH the base position and the base rotation.
  runner.stop();
  assert.deepEqual(lastWrite(bridge, 11, 0), [1, 0, 2]);
  assert.deepEqual(lastWrite(bridge, 11, 1), [0, 0.5, 0], 'the un-spun base is not lost');
  // Re-anchoring after a hand move re-anchors the rotation too.
  assert.equal(runner.refreshBase('a', [4, 0, 4], [0, 1.5, 0]), true);
  assert.deepEqual(lastWrite(bridge, 11, 0), [4, 0, 4]);
  assert.deepEqual(lastWrite(bridge, 11, 1), [0, 1.5, 0]);
  assert.equal(runner.refreshBase('a', [9, 0, 9], 'nope'), true, 'a bad rotation is ignored');
  assert.deepEqual(lastWrite(bridge, 11, 1), [0, 1.5, 0]);
});

/**
 * The map must be refused by the runner itself, not only by the snapshot reader: the animated
 * set is where a frame loop, `refreshBase` and `restore` all get their handles, so a target
 * list that names the map — an older rule that outlived this guard, a list captured before the
 * map existed, a hand-edited record — has to lose it on ingest.
 */
test('a rule set that names the map writes nothing to its handle, while a normal target still does', () => {
  const MAP_HANDLE = 91;
  const mapTarget = { guid: MAP_GUID, role: 'world-map', handle: MAP_HANDLE,
    base: [0, 6.6477, 0], baseRotation: [0, 0, 0],
    modelRef: MAP_MODEL_REF, entityId: MAP_ENTITY_ID, entityType: MAP_ENTITY_TYPE };
  const normalTarget = { guid: 'a', role: 'object', handle: 11, base: [1, 0, 2],
    baseRotation: [0, 0, 0] };
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const runner = createWorldRuleRunner({
    bridge: () => bridge, rules: [SWAY, SPIN],
    targets: [normalTarget, mapTarget],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });

  assert.equal(runner.targetCount(), 1, '初始目标表里的地图在装入时就被拒绝');
  assert.equal(runner.ruleCount(), 2, '规则本身照常装填：拒绝的是目标，不是碎片');
  assert.equal(runner.start(), true);
  for (let i = 0; i <= 12; i++) scheduler.fire(1000 + i * 50);
  assert.equal(countWrites(bridge, MAP_HANDLE, 0), 0, '一个位置写入都不许落到地图上');
  assert.equal(countWrites(bridge, MAP_HANDLE, 1), 0, '一个旋转写入都不许落到地图上');
  assert.ok(countWrites(bridge, 11, 0) > 0, '普通目标照常被写：否则这条用例证明不了什么');
  assert.ok(countWrites(bridge, 11, 1) > 0);

  // A stale list handed in later — before entering the world again, say — is refused the same way.
  runner.setTargets([mapTarget, normalTarget]);
  assert.equal(runner.targetCount(), 1);
  assert.equal(runner.refreshBase(MAP_GUID, [5, 5, 5]), false, '地图连手工重锚都找不到');
  assert.equal(runner.stop(), true);
  assert.equal(countWrites(bridge, MAP_HANDLE, 0), 0, '停下的回位也不许写地图');
  assert.equal(countWrites(bridge, MAP_HANDLE, 1), 0);

  // Non-vacuity: the same list without the identity fields DOES animate handle 91, so it is the
  // identity guard that keeps the map still — not a missing base, handle or rule.
  const anonymous = recordingBridge();
  const blind = createWorldRuleRunner({
    bridge: () => anonymous, rules: [SWAY],
    targets: [{ ...mapTarget, guid: 'anonymous', modelRef: '', entityId: '', entityType: '' }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  assert.equal(blind.targetCount(), 1);
  blind.start();
  scheduler.fire(1000);
  assert.ok(countWrites(anonymous, MAP_HANDLE, 0) > 0, '去掉身份后同一个 handle 会被写');
  blind.stop();
});

/**
 * 碎片不影响先知：先知是有剧本的角色，不是可以被浮动/旋转的物体。这条守卫和地图那条同一
 * 类——身份由 `isProphetActor` 一处裁决，并且在两个地方生效：目标表生成（快照）和
 * runner 自己的装入（任何人递进来的旧规则/手工记录都过不去）。
 */
test('a fragment rule never animates the prophet, while a normal object still is', () => {
  const PROPHET_HANDLE = 41, OBJECT_HANDLE = 42;
  // 三种身份写法各写一次：老世界可能只留着其中一项。名字故意写成「路人甲」，
  // 证明挡住规则的是身份、不是显示名。
  for (const identity of [{ actor_guid: PROPHET_GUID }, { model_ref: PROPHET_MODEL_REF },
    { entity_id: PROPHET_ENTITY_ID }, { semantic_role: 'prophet' }]) {
    const prophet = actor('prophet-actor', PROPHET_HANDLE, [1, 0, 2], { name: '路人甲', ...identity });
    const object = actor('object', OBJECT_HANDLE, [3, 0, 4]);
    assert.deepEqual(collectSwayTargets(snapshotWith([prophet, object]), SWAY)
      .map(target => target.guid), ['object'],
    `身份字段 ${Object.keys(identity)[0]} 单独出现也必须让先知退出规则目标`);

    const scheduler = manualScheduler(), bridge = recordingBridge();
    const runner = createWorldRuleRunner({
      bridge: () => bridge, rules: [SWAY, SPIN],
      targets: [{ guid: 'prophet-actor', role: 'prophet', handle: PROPHET_HANDLE, base: [1, 0, 2],
        baseRotation: [0, 0, 0] }, { guid: 'object', role: 'object', handle: OBJECT_HANDLE,
        base: [3, 0, 4], baseRotation: [0, 0, 0] }],
      requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
    });
    assert.equal(runner.targetCount(), 1, '装入目标表时就拒绝先知');
    runner.start();
    for (let i = 0; i <= 10; i++) scheduler.fire(1000 + i * 50);
    assert.equal(countWrites(bridge, PROPHET_HANDLE, 0), 0, '一个位置写入都不许落到先知上');
    assert.equal(countWrites(bridge, PROPHET_HANDLE, 1), 0, '一个旋转写入都不许落到先知上');
    assert.ok(countWrites(bridge, OBJECT_HANDLE, 0) > 0, '普通物体照常被写：否则这条用例证明不了什么');
    runner.stop();
    assert.equal(countWrites(bridge, PROPHET_HANDLE, 0), 0, '停下时的回位也不许写先知');
  }
  // Non-vacuity: 去掉身份后同一个 handle 会被写，所以挡住它的是 isProphetActor ——
  // 不是缺 handle、缺几何或者规则本身没跑。
  const scheduler = manualScheduler(), loose = recordingBridge();
  const blind = createWorldRuleRunner({
    bridge: () => loose, rules: [SWAY],
    targets: [{ guid: 'anonymous', role: 'object', handle: PROPHET_HANDLE, base: [1, 0, 2] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  blind.start();
  scheduler.fire(1000);
  assert.ok(countWrites(loose, PROPHET_HANDLE, 0) > 0, '去掉先知身份后同一个 handle 会被写');
  blind.stop();
});

test('the runner refuses to run without rules or targets, and stops on a dead bridge', () => {
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const errors = [];
  const make = overrides => createWorldRuleRunner({
    bridge: () => bridge, rules: [SWAY],
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [0, 0, 0] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
    onError: error => errors.push(error), ...overrides,
  });
  assert.equal(make({ rules: [] }).start(), false);
  assert.equal(make({ targets: [] }).start(), false);
  assert.equal(make({ requestFrame: null, cancelFrame: null }).start(), false,
    'no animation frame support means no silent half-working rule');

  const dead = make({});
  bridge.actorTransform = () => false;
  assert.equal(dead.start(), true);
  scheduler.fire(0);
  assert.equal(dead.isRunning(), false);
  assert.equal(errors.length, 1);
  assert.match(errors[0].message, /actorTransform/);
});

// ---- 碎片改为可作用于单独物体（target = actor guid）-------------------------------

/** 一条指向某个物体的规则：目录里的数字，目标换成 guid。 */
const aimed = (guid, overrides = {}) => ({ ...SWAY, target: guid, ...overrides });

test('两条规则各作用于一个物体：互不串味，没有规则的物体一个字节都不写', () => {
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  // A 被沿 x 摆动，B 被沿 y 摆动。同一时刻（四分之一周期）两者的峰值方向不同，
  // 所以「谁被谁的规则动了」在写入里一眼能看出来。
  const runner = createWorldRuleRunner({
    bridge: () => bridge,
    rules: [aimed('a', { axis: 'x', amplitude: 0.25 }), aimed('b', { axis: 'y', amplitude: 1.5 })],
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [1, 0, 2] },
      { guid: 'b', role: 'object', handle: 12, base: [0, 0, 0] },
      { guid: 'c', role: 'object', handle: 13, base: [5, 0, 5] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  assert.equal(runner.targetCount(), 3, '三个物体都在目标表里');
  assert.equal(runner.animatedCount(), 2, '只有真的带规则的物体进动画集合');
  assert.equal(runner.start(), true);
  const STEP = 50, elapsed = SWAY.periodMs / 4;   // 两条规则的周期相同，峰值同时到
  for (let i = 0; i * STEP <= elapsed; i++) scheduler.fire(1000 + i * STEP);

  const a = lastWrite(bridge, 11, 0), b = lastWrite(bridge, 12, 0);
  assert.deepEqual(a, [1 + 0.25, 0, 2], 'A 只被自己的规则动：x 峰、y 与 z 原样');
  // B: float 1.5 on y at the same instant -> y = 1.5, x stays 0 (A's rule never reached it).
  assert.ok(Math.abs(b[1] - 1.5) < 1e-9, 'B 按自己的规则在 y 上升');
  assert.equal(b[0], 0, 'A 的 x 浮动没有串到 B 上（cross-talk = 0）');
  assert.equal(b[2], 0);
  // A rotation is written too, but only with its own base: neither rule is a spin.
  assert.deepEqual(lastWrite(bridge, 11, 1), [0, 0, 0]);

  assert.equal(countWrites(bridge, 13, 0), 0, '没有规则的物体一次都没被写');
  assert.equal(countWrites(bridge, 13, 1), 0);
  runner.stop();
  assert.equal(countWrites(bridge, 13, 0), 0, '停下时的回位也不碰没有规则的物体');
  assert.deepEqual(lastWrite(bridge, 11, 0), [1, 0, 2], 'A 回到自己的基准位');
  assert.deepEqual(lastWrite(bridge, 12, 0), [0, 0, 0]);
});

test('同一个物体上的两条规则被合成：每帧仍只有一次位置写入与一次旋转写入', () => {
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const runner = createWorldRuleRunner({
    bridge: () => bridge,
    // sway 与 spin 都指向同一个物体：前者的位移与后者的旋转必须都落在它身上。
    rules: [{ ...SWAY, target: 'a' }, { ...SPIN, target: 'a' }],
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [1, 0, 2], baseRotation: [0, 0.5, 0] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  assert.equal(runner.animatedCount(), 1);
  runner.start();
  const STEP = 50, elapsed = SWAY.periodMs / 4;
  for (let i = 0; i * STEP <= elapsed; i++) scheduler.fire(1000 + i * STEP);
  assert.deepEqual(lastWrite(bridge, 11, 0), [1 + SWAY.amplitude, 0, 2], '位移来自 sway');
  const turn = lastWrite(bridge, 11, 1);
  assert.ok(Math.abs(turn[2] - SPIN.amplitude * DEGREES_TO_RADIANS
    * Math.sin((2 * Math.PI * elapsed) / SPIN.periodMs)) < 1e-9, '旋转来自 spin');
  assert.deepEqual([turn[0], turn[1]], [0, 0.5], '旋转的其余欧拉角仍是基准');
  const frames = 1 + Math.floor(elapsed / STEP);
  assert.equal(countWrites(bridge, 11, 0), frames, '一个物体每帧一次位置写入');
  assert.equal(countWrites(bridge, 11, 1), frames, '一个物体每帧一次旋转写入');
  runner.stop();
});

test('一份指向地图或先知的规则被拒绝：模型层与 runner 都不收，普通物体照常', () => {
  // 保留身份来自同一处：地图的 MAP_GUID、先知的 PROPHET_GUID。
  assert.deepEqual([...RESERVED_TARGET_GUIDS].sort(), [MAP_GUID, PROPHET_GUID].sort());
  assert.equal(isReservedTargetGuid(MAP_GUID), true);
  assert.equal(isReservedTargetGuid(` ${PROPHET_GUID} `), true);
  assert.equal(isReservedTargetGuid('story.placement.0003'), false);
  for (const reserved of [MAP_GUID, PROPHET_GUID]) {
    assert.throws(() => validateRule({ ...SWAY, target: reserved }), /世界规则/, reserved);
    assert.throws(() => normalizeTarget(reserved), /世界规则/);
    // `collectSwayTargets` 用同一条校验：一份指向保留身份的旧规则连目标表都生不出来。
    assert.throws(() => collectSwayTargets(snapshotWith([actor('a', 11, [1, 0, 2])]),
      { ...SWAY, target: reserved }), /世界规则/);
  }
  // 合法的目标原样保留（前后空白被裁掉），没有目标时连键都不出现。
  assert.equal(validateRule({ ...SWAY, target: ' story.placement.0003 ' }).target,
    'story.placement.0003');
  assert.equal('target' in validateRule(SWAY), false);
  for (const bad of ['', '   ', 7, {}, [], 'x'.repeat(200)]) {
    assert.throws(() => validateRule({ ...SWAY, target: bad }), /世界规则/, JSON.stringify(bad));
  }

  // runner：一份手改存档里的「指向地图」的记录被忽略，别的规则照常跑，地图零写入。
  const MAP_HANDLE = 91, OBJECT_HANDLE = 11;
  const scheduler = manualScheduler(), bridge = recordingBridge();
  const runner = createWorldRuleRunner({
    bridge: () => bridge,
    rules: [aimed(MAP_GUID), aimed('a')],
    targets: [{ guid: MAP_GUID, role: 'object', handle: MAP_HANDLE, base: [0, 0, 0] },
      { guid: 'a', role: 'object', handle: OBJECT_HANDLE, base: [1, 0, 2] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  assert.equal(runner.ruleCount(), 1, '指向地图的那条规则在装入时就被忽略');
  assert.equal(runner.start(), true);
  for (let i = 0; i <= 6; i++) scheduler.fire(1000 + i * 50);
  assert.equal(countWrites(bridge, MAP_HANDLE, 0), 0, '地图一个位置写入都没有');
  assert.equal(countWrites(bridge, MAP_HANDLE, 1), 0, '地图一个旋转写入都没有');
  assert.ok(countWrites(bridge, OBJECT_HANDLE, 0) > 0, '普通物体照常被写：否则这条用例证明不了什么');
  runner.stop();
});

test('旧存档（没有 target 的规则）仍然作用于所有合格物体，与带目标的规则可以共存', () => {
  // 改动前存下的样子：一条没有 target 的规则，版本还是 1。
  const legacySave = { version: 1, rules: [{ fragmentId: 'sway', type: 'sway', axis: 'x',
    amplitude: 0.25, periodMs: 2400 }] };
  assert.equal(legacySave.version, WORLD_RULE_VERSION, '存档版本不跳，旧文件照样能读');
  // `validateRule` 收下它，并且不凭空补一个 target：回读出来仍是「旧规则」。
  assert.equal('target' in validateRule(legacySave.rules[0]), false);

  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const runner = createWorldRuleRunner({
    bridge: () => bridge, rules: legacySave.rules,
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [1, 0, 2] },
      { guid: 'b', role: 'object', handle: 12, base: [0, 0, 0] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
  });
  assert.equal(runner.animatedCount(), 2, '旧规则照旧作用于每一个合格物体');
  runner.start();
  const STEP = 50, elapsed = SWAY.periodMs / 4;
  for (let i = 0; i * STEP <= elapsed; i++) scheduler.fire(1000 + i * STEP);
  assert.deepEqual(lastWrite(bridge, 11, 0), [1 + 0.25, 0, 2], 'A 照旧被浮动');
  assert.deepEqual(lastWrite(bridge, 12, 0), [0 + 0.25, 0, 0], 'B 也照旧被浮动');
  runner.stop();
  assert.deepEqual(lastWrite(bridge, 11, 0), [1, 0, 2], '停下时回到基准');
  assert.deepEqual(lastWrite(bridge, 12, 0), [0, 0, 0]);

  // 旧规则与新规则共存：旧规则管所有物体，新规则只管自己的那一个。
  const mixed = recordingBridge();
  const scheduler2 = manualScheduler();
  const both = createWorldRuleRunner({
    bridge: () => mixed,
    rules: [{ fragmentId: 'sway', type: 'sway', axis: 'x', amplitude: 0.25, periodMs: 2400 },
      { fragmentId: 'spin', type: 'spin', axis: 'z', amplitude: 15, periodMs: 3600, target: 'b' }],
    targets: [{ guid: 'a', role: 'object', handle: 11, base: [0, 0, 0] },
      { guid: 'b', role: 'object', handle: 12, base: [0, 0, 0] }],
    requestFrame: scheduler2.requestFrame, cancelFrame: scheduler2.cancelFrame,
  });
  assert.equal(both.animatedCount(), 2);
  both.start();
  for (let i = 0; i <= 5; i++) scheduler2.fire(1000 + i * 50);
  assert.ok(countWrites(mixed, 11, 0) > 0, '旧规则仍然浮动 A');
  assert.deepEqual(lastWrite(mixed, 11, 1), [0, 0, 0], 'B 的旋转规则没有转到 A 上');
  assert.notDeepEqual(lastWrite(mixed, 12, 1), [0, 0, 0], 'B 被自己的旋转规则转动');
  both.stop();
});
