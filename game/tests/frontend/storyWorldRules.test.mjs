import assert from 'node:assert/strict';
import test from 'node:test';
import { FRAGMENTS, WORLD_RULE_VERSION, collectSwayTargets, createWorldRuleRunner,
  findFragment, initialFragmentIds, ownedFragmentIds, ruleOffset, totalOffset,
  validateRule } from '../../frontend/storyWorldRules.mjs';

const SWAY = findFragment('sway').rule;
const snapshotWith = actors => ({ data: { scene: { actors } } });
const actor = (guid, handle, position, extra = {}) => ({
  actor_guid: guid, handle, semantic_role: 'prophet', visible: true,
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

test('the catalogue grants exactly one initial fragment whose rule sways left and right', () => {
  assert.equal(WORLD_RULE_VERSION, 1);
  assert.deepEqual(FRAGMENTS.map(fragment => fragment.id), ['sway']);
  assert.deepEqual(initialFragmentIds(), ['sway']);
  assert.deepEqual(ownedFragmentIds(), ['sway']);
  assert.equal(findFragment('nope'), null);
  const rule = validateRule(SWAY);
  assert.equal(rule.type, 'sway');
  assert.equal(rule.axis, 'x', 'left/right means the x axis');
  assert.deepEqual([...rule.excludeRoles], ['player'], 'the player must not be swayed');
  assert.ok(rule.amplitude > 0 && rule.amplitude <= 1, 'a small amplitude');
  assert.ok(rule.periodMs >= 500, 'a period slow enough to read as floating');
});

test('rule validation rejects anything the frame loop could not survive', () => {
  for (const bad of [null, {}, { ...SWAY, type: 'spin' }, { ...SWAY, axis: 'w' },
    { ...SWAY, amplitude: 0 }, { ...SWAY, amplitude: -1 }, { ...SWAY, amplitude: NaN },
    { ...SWAY, amplitude: '0.5' }, { ...SWAY, amplitude: Infinity },
    { ...SWAY, periodMs: 0 }, { ...SWAY, periodMs: -10 }, { ...SWAY, periodMs: '1' }]) {
    assert.throws(() => validateRule(bad), /世界规则|浮动轴|规则类型/, JSON.stringify(bad));
  }
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
});

test('collectSwayTargets keeps only movable world objects and copies their base position', () => {
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
  // The base is a snapshot: later engine reads cannot mutate the running rule.
  snapshot.data.scene.actors[0].geometry.position[0] = 99;
  assert.deepEqual(targets[0].base, [1, 0, 2]);
  assert.deepEqual(collectSwayTargets({}, SWAY), []);
  assert.deepEqual(collectSwayTargets(snapshotWith([actor('p', 1, [0, 0, 0], { semantic_role: 'player' })]), SWAY), []);
});

test('the runner sways every target on its own frame loop and restores the base on stop', () => {
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const errors = [];
  const runner = createWorldRuleRunner({
    bridge: () => bridge, rules: [SWAY],
    targets: [{ guid: 'a', role: 'prophet', handle: 11, base: [1, 0, 2] }],
    requestFrame: scheduler.requestFrame, cancelFrame: scheduler.cancelFrame,
    onError: error => errors.push(error),
  });

  assert.equal(runner.isRunning(), false);
  assert.equal(runner.start(), true);
  assert.equal(runner.isRunning(), true);
  assert.equal(scheduler.pending, true);
  assert.equal(runner.start(), true, 'starting twice must not add a second loop');

  scheduler.fire(1000);
  assert.deepEqual(bridge.calls.at(-1), [11, 0, [1, 0, 2]], 'the first frame sits on the base');
  // Real frames are ~16 ms; advance in small steps so the per-frame clamp never bites.
  const STEP_MS = 50;
  const steps = SWAY.periodMs / 4 / STEP_MS;
  for (let i = 1; i <= steps; i++) scheduler.fire(1000 + STEP_MS * i);
  const peak = bridge.calls.at(-1)[2];
  assert.ok(Math.abs(peak[0] - (1 + SWAY.amplitude)) < 1e-9, 'the object floats to the right');
  assert.equal(peak[2], 2, 'and stays on its own depth');

  // A stalled tab must not teleport the object: one frame advances at most 100 ms.
  const maxStep = SWAY.amplitude * (2 * Math.PI / SWAY.periodMs) * 100;
  scheduler.fire(1000 + SWAY.periodMs / 4 + 5000);
  const stalled = bridge.calls.at(-1)[2];
  assert.ok(Math.abs(stalled[0] - peak[0]) <= maxStep + 1e-9,
    'a five second stall advances a single clamped frame');

  // Moving the prop by hand re-anchors the sway instead of snapping it back.
  assert.equal(runner.refreshBase('a', [5, 0, 5]), true);
  assert.deepEqual(bridge.calls.at(-1), [11, 0, [5, 0, 5]]);
  assert.equal(runner.refreshBase('missing', [0, 0, 0]), false);

  assert.equal(runner.stop(), true);
  assert.equal(runner.isRunning(), false);
  assert.deepEqual(bridge.calls.at(-1), [11, 0, [5, 0, 5]], 'stopping leaves the object where it belongs');
  assert.equal(scheduler.cancelled.length, 1);
  assert.equal(runner.stop(), false, 'stopping twice is harmless');
  assert.deepEqual(errors, []);
});

test('the runner refuses to run without rules or targets, and stops on a dead bridge', () => {
  const scheduler = manualScheduler();
  const bridge = recordingBridge();
  const errors = [];
  const make = overrides => createWorldRuleRunner({
    bridge: () => bridge, rules: [SWAY],
    targets: [{ guid: 'a', role: 'prophet', handle: 11, base: [0, 0, 0] }],
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
