import assert from 'node:assert/strict';
import test from 'node:test';
import {
  createIkEditorController,
  createIkEditorState,
  makeIkFoot,
  ikFootStatus,
  isAutomaticIkFoot,
  serializeIkFeet,
  ikBoneOptions,
  ikBoneError,
} from '../../src/utils/ikEditorController.js';

test('runtime labels distinguish support, release and fading from a retained target', () => {
  const foot = makeIkFoot();
  assert.equal(ikFootStatus(foot), '等待地面');
  Object.assign(foot.runtime, { has_target: true, weight: 0.5, grounded: true });
  assert.equal(ikFootStatus(foot), '支撑中');
  Object.assign(foot.runtime, { grounded: false, releasing: true });
  assert.equal(ikFootStatus(foot), '释放支撑中');
  foot.runtime.releasing = false;
  assert.equal(ikFootStatus(foot), '淡出中');
  foot.mode = 'contact';
  foot.runtime.grounded = true;
  assert.equal(ikFootStatus(foot), '接触中');
  foot.mode = 'look_at';
  assert.equal(isAutomaticIkFoot(foot), false);
  assert.equal(ikFootStatus(foot), '手动位置目标');
  foot.enabled = false;
  assert.equal(ikFootStatus(foot), '已禁用');
});

const deferred = () => {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => {
    resolve = yes;
    reject = no;
  });
  return { promise, resolve, reject };
};

const chain = (bone = 'Foot', overrides = {}) => ({
  id: `chain-${bone}`,
  bone_name: bone,
  mode: 'foot_plant',
  weight: 1,
  runtime: { has_target: true, weight: 0.3, target: [1, 2, 3] },
  ...overrides,
});
const config = (chains = [chain()], revision = 4) => ({
  ready: true,
  is_skinned: true,
  revision,
  chains,
});

function setup(overrides = {}) {
  const api = {
    getActorSkeletonLeaves: async () => ({ is_skinned: true, leaves: ['Foot'] }),
    getActorIkChains: async () => config(),
    setActorIkChains: async (_scene, _actor, chains, revision) => config(chains, revision + 1),
    ...overrides,
  };
  const state = createIkEditorState();
  return { state, controller: createIkEditorController(api, state) };
}

test('payload preserves zero weight/damping, sanitizes nonfinite values and excludes runtime', () => {
  const foot = makeIkFoot(chain());
  Object.assign(foot, {
    weight: 0,
    damping: 0,
    targetX: Infinity,
    targetY: '',
    targetZ: -3,
    chainLength: 1,
    maxIterations: NaN,
    probeMaxDrop: 0,
    footHeight: 0,
  });
  const [payload] = serializeIkFeet([foot, makeIkFoot()]);
  assert.equal(payload.weight, 0);
  assert.equal(payload.damping, 0);
  assert.equal(payload.chain_length, 2);
  assert.equal(payload.max_iterations, 10);
  assert.equal(payload.probe_max_drop, 0);
  assert.deepEqual(payload.target, [0, 0, -3]);
  assert.equal('runtime' in payload, false);
  assert.equal(payload.id, foot.id);
  assert.deepEqual(serializeIkFeet([]), []);
});

test('old actor responses cannot replace a newer actor, even with the same name in another scene', async () => {
  const oldConfig = deferred();
  const oldSkeleton = deferred();
  const { controller, state } = setup({
    getActorSkeletonLeaves: (scene) =>
      scene === 'A' ? oldSkeleton.promise : Promise.resolve({ leaves: ['NewFoot'] }),
    getActorIkChains: (scene) =>
      scene === 'A' ? oldConfig.promise : Promise.resolve(config([chain('NewFoot')], 7)),
  });
  const oldLoad = controller.select('A', 'Character');
  await controller.select('B', 'Character');
  oldConfig.resolve(config([chain('OldFoot')], 1));
  oldSkeleton.resolve({ leaves: ['OldFoot'] });
  await oldLoad;
  assert.deepEqual(state.boneNames, ['NewFoot']);
  assert.equal(state.feet[0].boneName, 'NewFoot');
  assert.equal(state.revision, 7);
});

test('selection immediately clears drafts and stale rejected loads leave current actor alone', async () => {
  const first = deferred();
  const { controller, state } = setup({
    getActorIkChains: (_scene, actor) =>
      actor === 'A' ? first.promise : Promise.resolve(config([])),
  });
  const oldLoad = controller.select('Scene', 'A');
  state.feet.push(makeIkFoot(chain('OldDraft')));
  const next = controller.select('Scene', 'B');
  assert.deepEqual(state.feet, []);
  await next;
  first.reject(new Error('Old request failed'));
  await oldLoad;
  assert.equal(state.error, '');
  assert.equal(state.ready, true);
});

test('writes serialize and coalesce latest edits using the acknowledged revision', async () => {
  const requests = [];
  const replies = [deferred(), deferred()];
  const { controller, state } = setup({
    setActorIkChains: (...args) => {
      requests.push(args);
      return replies[requests.length - 1].promise;
    },
  });
  await controller.select('Scene', 'Actor');
  state.feet[0].weight = 0.2;
  const completed = controller.apply();
  state.feet[0].weight = 0.4;
  void controller.apply();
  state.feet[0].weight = 0;
  void controller.apply();
  assert.equal(requests.length, 1);
  assert.equal(requests[0][2][0].weight, 0.2);
  replies[0].resolve(config([chain()], 5));
  await Promise.resolve();
  assert.equal(requests.length, 2);
  assert.equal(requests[1][3], 5);
  assert.equal(requests[1][2][0].weight, 0);
  assert.equal(state.feet[0].weight, 0);
  replies[1].resolve(
    config(
      [
        chain('Foot', {
          runtime: { has_target: true, weight: 0, target: [2, 3, 4] },
        }),
      ],
      6
    )
  );
  await completed;
  assert.equal(state.revision, 6);
  assert.equal(state.saving, false);
  assert.deepEqual(state.feet[0].runtime.target, [2, 3, 4]);
});

test('switching actor discards queued writes and ignores the in-flight old response', async () => {
  const oldReply = deferred();
  const requests = [];
  const { controller, state } = setup({
    getActorSkeletonLeaves: async () => ({ leaves: ['A', 'B'] }),
    getActorIkChains: async (_scene, actor) => config([chain(actor)], actor === 'B' ? 20 : 4),
    setActorIkChains: (...args) => {
      requests.push(args);
      return oldReply.promise;
    },
  });
  await controller.select('Scene', 'A');
  const first = controller.apply();
  state.feet[0].weight = 0.1;
  void controller.apply();
  await controller.select('Scene', 'B');
  oldReply.resolve(config([chain('A')], 5));
  await first;
  assert.equal(requests.length, 1);
  assert.equal(requests[0][1], 'A');
  assert.equal(state.feet[0].boneName, 'B');
  assert.equal(state.revision, 20);
  assert.equal(state.saving, false);
});

test('removing the final foot sends an empty replacement and retains an empty editor', async () => {
  const requests = [];
  const { controller, state } = setup({
    setActorIkChains: async (...args) => {
      requests.push(args);
      return config([], args[3] + 1);
    },
  });
  await controller.select('Scene', 'Actor');
  state.feet.splice(0, 1);
  await controller.apply();
  assert.deepEqual(requests[0][2], []);
  assert.deepEqual(state.feet, []);
});

test('failed updates stop the queue until explicit refresh acquires a current revision', async () => {
  let attempts = 0;
  let revision = 1;
  const { controller, state } = setup({
    getActorIkChains: async () => config([chain()], revision),
    setActorIkChains: async () => {
      attempts += 1;
      throw new Error('409 revision conflict');
    },
  });
  await controller.select('Scene', 'Actor');
  await controller.apply();
  await controller.apply();
  assert.equal(attempts, 1);
  assert.equal(state.ready, false);
  assert.match(state.error, /409 revision conflict/);
  revision = 10;
  await controller.refresh();
  assert.equal(state.ready, true);
  assert.equal(state.revision, 10);
  assert.equal(state.error, '');
});

test('a resource readiness failure cannot expose a writable empty draft', async () => {
  let writes = 0;
  const { controller, state } = setup({
    getActorIkChains: async () => ({ ready: false, is_skinned: true }),
    setActorIkChains: async () => {
      writes += 1;
    },
  });
  await controller.select('Scene', 'Actor');
  await controller.apply();
  assert.equal(state.ready, false);
  assert.equal(writes, 0);
  assert.match(state.error, /尚未就绪/);
});

test('FootPlant only offers leaf children; other modes retain non-root joints', async () => {
  const { controller, state } = setup({
    getActorSkeletonLeaves: async () => ({
      leaves: ['Toe_End'],
      nodes: [
        { name: 'Root', parent: -1, leaf: false },
        { name: 'Ankle', parent: 1, leaf: false },
        { name: 'Toe_End', parent: 2, leaf: true },
      ],
    }),
  });
  await controller.select('Scene', 'Actor');
  assert.deepEqual(state.boneNames, ['Ankle', 'Toe_End']);
  assert.deepEqual(state.footBoneNames, ['Toe_End']);
  assert.deepEqual(ikBoneOptions(makeIkFoot(), state), ['Toe_End']);
  for (const mode of ['contact', 'look_at', 'weapon_aim']) {
    assert.deepEqual(ikBoneOptions(makeIkFoot({ mode }), state), ['Ankle', 'Toe_End']);
  }
});

test('legacy non-leaf Foot configuration stays visible and editable until explicitly corrected', async () => {
  const writes = [];
  const { controller, state } = setup({
    getActorSkeletonLeaves: async () => ({ nodes: [
      { name: 'Root', parent: -1, leaf: false },
      { name: 'Ankle', parent: 0, leaf: false },
      { name: 'Toe', parent: 1, leaf: true },
    ] }),
    getActorIkChains: async () => config([chain('Ankle')]),
    setActorIkChains: async (_scene, _actor, chains, revision) => {
      writes.push(chains);
      return config(chains, revision + 1);
    },
  });
  await controller.select('Scene', 'Actor');
  assert.equal(state.feet[0].boneName, 'Ankle');
  assert.match(ikBoneError(state.feet[0], state), /叶子节点/);
  await controller.apply();
  assert.equal(writes.length, 0);
  assert.equal(state.ready, true);
  assert.equal(state.feet[0].boneName, 'Ankle');
  state.feet[0].boneName = 'Toe';
  await controller.apply();
  assert.equal(writes[0][0].bone_name, 'Toe');
  assert.equal(state.error, '');
  state.feet[0].mode = 'contact';
  state.feet[0].boneName = 'Ankle';
  await controller.apply();
  assert.equal(writes[1][0].bone_name, 'Ankle');
});

test('node metadata excludes roots, ambiguous names and unproven leaves', async () => {
  const { controller, state } = setup({
    getActorSkeletonLeaves: async () => ({ leaves: ['Unknown'], nodes: [
      { name: 'Root', parent: -1, leaf: true },
      { name: 'Ambiguous', parent: -1, leaf: false },
      { name: 'Ambiguous', parent: 0, leaf: true },
      { name: 'Unknown', parent: 0 },
      { name: 'Leaf', parent: 0, leaf: true },
      { name: '', parent: 0, leaf: true },
      null,
    ] }),
  });
  await controller.select('Scene', 'Actor');
  assert.deepEqual(state.boneNames, ['Unknown', 'Leaf']);
  assert.deepEqual(state.footBoneNames, ['Leaf']);
});

test('legacy leaves fallback excludes malformed and ambiguous names without inventing a selection', async () => {
  const { controller, state } = setup({
    getActorSkeletonLeaves: async () => ({ leaves: ['Leaf', '', null, 1, 'Duplicate', 'Duplicate', '  '] }),
    getActorIkChains: async () => config([]),
  });
  await controller.select('Scene', 'Actor');
  assert.deepEqual(state.boneNames, ['Leaf']);
  assert.deepEqual(state.footBoneNames, ['Leaf']);
  assert.deepEqual(state.feet, []);
});
