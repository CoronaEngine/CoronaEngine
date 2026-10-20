import assert from 'node:assert/strict';
import test from 'node:test';
import { createStoryGameplay, GAMEPLAY_KEY, FRAGMENT_GUID, worldBounds, canHitBoss,
  distanceToBounds, contextualInteraction, nearestAttackTarget,
  INTERACTION_PROMPTS } from '../../frontend/storyGameplay.mjs';
import { PROPHET_INTERACTION } from '../../frontend/prophetDialogue.mjs';
import { PLACED_BLOCK_REACH } from '../../frontend/storyProps.mjs';
import { STORY_CHARACTERS, rotatePoint } from '../../frontend/storyCharacters.mjs';
import { actorFixture, apiFixture, deferred, url, gameplayConfig } from './fixtures.mjs';
import { ensureStoryCharacters } from '../../frontend/storyActors.mjs';

const config = gameplayConfig;
// A legacy actor id: old saves can still contain the red ball this flow used to create.
const LEGACY_FRAGMENT = { role: 'fragment', name: '世界碎片', guid: FRAGMENT_GUID,
  x: 0, z: 12, size: 0.5, rotation: [0, 0, 0] };
function fixture(options = {}) {
  let state = { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null,
    inventory: { worldFragment: 0, block: 0, ball: 0 } };
  let role = 'main', time = 0, id = 0, fail = false, lostReply = false, visualFail = false, gate = null, replyGate = null;
  const boss = actorFixture(STORY_CHARACTERS[1]), bounds = worldBounds(boss);
  // 玩家摆在 Boss 正前方：x 用 Boss 自己的中线，而不是写死的 0——
  // Boss 一旦横向挪位，写死 0 会让玩家站到十几米开外，命中类断言全部失真。
  const bossCentreX = (bounds[0] + bounds[3]) / 2;
  const player = { position: [bossCentreX, 0, bounds[2] - 1], rotation: [0, Math.PI, 0], facingYaw: 0, grounded: true };
  const requests = [], reads = [], feedback = [], states = [], visuals = [], operations = new Set();
  // 小世界的自有文档（陈列 / 世界规则 / 名册）与战斗版本号无关，进入世界时各读一次；
  // `requests` 只记战斗请求与 `load`，文档读取记在 `reads` 里，两者互不混淆。
  let placements = { version: 1, placements: [] };
  let rules = { version: 1, rules: [] };
  let roster = { version: 1, subworlds: [] };
  const response = () => ({ status: 'ok', role, state: structuredClone(state), config });
  const document_ = (payload) => ({ status: 'ok', role, state: structuredClone(payload) });
  const game = createStoryGameplay({ projectPath: 'D:/story', readPlayer: () => player, readBoss: () => boss,
    // 地面网格与页面同一个传法：给了就按它落脚，没给（undefined）就是旧行为。
    groundYAt: options.groundYAt,
    now: () => time, newId: () => `operation-${++id}`, onFeedback: v => feedback.push(v), onState: v => states.push(v),
    reconcile: async data => { visuals.push(structuredClone(data)); if (visualFail) throw new Error('model failed'); },
    api: { scratch: { sendKeyEvent: async (key, mods, json) => {
      assert.equal(key, GAMEPLAY_KEY); assert.equal(mods, '');
      const request = JSON.parse(json);
      if (request.action === 'loadPlacements') { reads.push(request); return document_(placements); }
      if (request.action === 'loadWorldRules') { reads.push(request); return document_(rules); }
      if (request.action === 'loadSubworlds') { reads.push(request); return document_(roster); }
      requests.push(request);
      if (request.action === 'load') return response();
      if (gate) await gate.promise;
      if (fail) throw new Error('disk full');
      if (!operations.has(request.operationId)) {
        if (request.expectedRevision !== state.revision) return { ...response(), status: 'error', code: 'REVISION_CONFLICT', message: 'conflict' };
        operations.add(request.operationId);
        state.revision++;
        if (request.action === 'hitBoss') {
          state.boss.hp -= 20;
          state.rage = Math.min(config.rageMax, state.rage + config.ragePerHit);
        } else if (request.action === 'castSkill') {
          const skill = config.skills[request.skillId];
          state.rage -= skill.rageCost;
          if (request.hit) state.boss.hp = Math.max(0, state.boss.hp - skill.damage);
        }
        // The killing blow grants the fragment, 64 blocks and 64 balls; no drop is written.
        if (!state.boss.hp) {
          state.inventory.worldFragment = Math.max(1, state.inventory.worldFragment);
          state.inventory.block = (state.inventory.block || 0) + 64;
          state.inventory.ball = (state.inventory.ball || 0) + 64;
        }
      }
      if (lostReply) { lostReply = false; throw new Error('reply lost'); }
      const reply = { data: response() };
      if (replyGate) await replyGate.promise;
      return reply;
    } } } });
  return { game, player, boss, requests, reads, feedback, states, visuals,
    tick: (ms = 400) => { time += ms; }, fail: value => { fail = value; }, loseReply: () => { lostReply = true; },
    failVisual: value => { visualFail = value; }, gate: value => { gate = value; }, role: value => { role = value; },
    delayReply: value => { replyGate = value; },
    // 世界重新打开时磁盘上已有的自有文档。
    setPlacements: value => { placements = { version: 1, placements: structuredClone(value) }; },
    setRules: value => { rules = { version: 1, rules: structuredClone(value) }; },
    setSubworlds: value => { roster = { version: 1, subworlds: structuredClone(value) }; },
    get state() { return state; }, conflict: () => { state.revision++; state.boss.hp -= 20; } };
}

test('world bounds include scale and rotation; range uses the model edge, not its pivot', () => {
  const boss = actorFixture(STORY_CHARACTERS[1]);
  boss.geometry.scale = [2, 3, 4]; boss.geometry.rotation = [0.4, 0.6, 0.8];
  const bounds = worldBounds(boss);
  for (let i = 0; i < 8; i++) {
    const p = rotatePoint([0, 1, 2].map(a => boss.local_aabb[a + ((i & (1 << a)) ? 3 : 0)] * boss.geometry.scale[a]), boss.geometry.rotation)
      .map((v, a) => v + boss.geometry.position[a]);
    p.forEach((v, a) => assert.ok(v >= bounds[a] - 1e-9 && v <= bounds[a + 3] + 1e-9));
  }
  // The ground point is 2 m in front of the bounds' own -z face: the enemy's authored site sits
  // on one side of the model's pivot (the dragon is not centred on it), so the point has to be
  // derived from the bounds rather than written down.
  const ground = [(bounds[0] + bounds[3]) / 2, 0, bounds[2] - 2];
  assert.equal(distanceToBounds(ground, bounds), 2);
  assert.ok(canHitBoss({ position: ground, rotation: [0, Math.PI, 0], facingYaw: 0, grounded: true }, bounds, config));
  assert.equal(canHitBoss({ position: ground, rotation: [0, 0, 0], facingYaw: Math.PI, grounded: true }, bounds, config), false);
  assert.equal(canHitBoss({ position: [ground[0], 0, bounds[2] - 2.51], rotation: [0, Math.PI, 0], facingYaw: 0, grounded: true }, bounds, config), false);
  assert.equal(canHitBoss(null, null, config), false);
});

test('the nearest living target in front wins the swing, boss or monster alike', () => {
  const config = { meleeRange: 2.5, meleeHalfAngle: Math.PI / 3 };
  const player = { position: [0, 0, 0], facingYaw: 0, grounded: true };
  const near = { id: 'monster-near', bounds: [-1, 0, 0.5, 1, 2, 1.5] };
  const far = { id: 'monster-far', bounds: [-1, 0, 2, 1, 2, 3] };
  const behind = { id: 'monster-behind', bounds: [-1, 0, -2, 1, 2, -1] };
  assert.equal(nearestAttackTarget(player, [far, near, behind], config).id, 'monster-near');
  assert.equal(nearestAttackTarget(player, [behind], config), null);
  assert.equal(nearestAttackTarget(player, [], config), null);
  assert.equal(nearestAttackTarget({ ...player, grounded: false }, [near], config), null);
});

test('ten acknowledged hits grant the fragment immediately, with no red ball and no pickup', async () => {
  const f = fixture(); await f.game.load();
  for (let i = 0; i < 10; i++) {
    await f.game.attack(); await f.game.attack(); f.tick();
    assert.equal(f.state.boss.hp, 200 - (i + 1) * 20);
  }
  await f.game.attack(); assert.equal(f.state.revision, 10);
  // 击杀本身就把凭证交给背包：世界里没有红球，F 也没有「拾取」这一步。
  assert.equal(f.state.inventory.worldFragment, 1);
  assert.equal(f.state.inventory.block, 64, '击杀同时把 64 个方块交给行囊');
  assert.equal(f.state.inventory.ball, 64, '击杀同时把 64 个球交给行囊');
  assert.equal(f.state.drop, null);
  assert.equal(f.game.pickup, undefined, 'pickup 不再是玩法 API 的一部分');
  assert.deepEqual(f.requests.map(request => request.action).includes('pickupDrop'), false);
  assert.deepEqual(f.feedback, ['Boss 已击败 · 小世界已开启']);
});

test('a monster swing dispatches its role and a kill reports both dropped items', async () => {
  const monster = STORY_CHARACTERS.find(character => character.role === 'monster-mantis');
  const boss = STORY_CHARACTERS.find(character => character.role === 'boss');
  const monsterActor = actorFixture(monster), bossActor = actorFixture(boss);
  const bounds = worldBounds(monsterActor);
  const player = { position: [(bounds[0] + bounds[3]) / 2, 0, bounds[2] - 1],
    rotation: [0, Math.PI, 0], facingYaw: 0, grounded: true };
  const monsterState = Object.fromEntries(STORY_CHARACTERS
    .filter(character => character.role.startsWith('monster'))
    .map(character => [character.role, { hp: character.role === monster.role ? 20 : 120 }]));
  const state = { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null,
    monsters: monsterState,
    inventory: { worldFragment: 0, block: 0, ball: 0, 'hunter-blade': 0,
      'hunter-helm': 0, 'hunter-chest': 0, 'hunter-boots': 0 } };
  const requests = [], feedback = [];
  const documents = { loadPlacements: { version: 1, placements: [] },
    loadWorldRules: { version: 1, rules: [] }, loadSubworlds: { version: 1, subworlds: [] } };
  const game = createStoryGameplay({ projectPath: 'D:/story', readPlayer: () => player,
    readBoss: () => bossActor,
    readMonsters: () => [{ id: monster.role, name: monster.name, actor: monsterActor }],
    onFeedback: value => feedback.push(value),
    api: { scratch: { sendKeyEvent: async (key, mods, json) => {
      assert.equal(key, GAMEPLAY_KEY);
      const request = JSON.parse(json);
      if (documents[request.action]) return { status: 'ok', role: 'main', state: documents[request.action] };
      if (request.action === 'load') return { status: 'ok', role: 'main', config: gameplayConfig,
        state: structuredClone(state) };
      requests.push(request);
      state.revision++;
      if (request.action === 'hitMonster') {
        state.monsters[request.monsterId].hp = 0;
        state.inventory.ball = 7;
        state.inventory['hunter-blade'] = 1;
      }
      return { status: 'ok', role: 'main', config: gameplayConfig, state: structuredClone(state) };
    } } } });
  await game.load();
  await game.attack();
  assert.equal(requests.at(-1).action, 'hitMonster');
  assert.equal(requests.at(-1).monsterId, monster.role);
  assert.equal(game.data.state.monsters[monster.role].hp, 0);
  assert.deepEqual(feedback, ['击败怪物 · 获得 球 ×7、猎人长刃 ×1']);
});

test('out of range, behind and child world never dispatch damage', async () => {
  const f = fixture(); await f.game.load(); f.player.facingYaw = Math.PI;
  await f.game.attack(); f.tick(); f.player.facingYaw = 0; f.player.position[2] = -100;
  await f.game.attack(); f.tick(); f.player.position[2] = 10;
  f.role('child'); await f.game.load(); await f.game.attack();
  assert.ok(f.requests.every(v => v.action === 'load'));
});

// 回归：`load()` 一度用对象字面量里的兄弟方法名直接调用 `loadPlacements()` /
// `loadWorldRules()` / `loadSubworlds()`。对象字面量的方法不在彼此的词法作用域里，因此每一次
// 都是 ReferenceError，又被空 `catch` 吞掉 —— 世界重新打开时三份自有文档一份也没读回来：
// 陈列归零（方块数量退回 64）、碎片规则归零（「调整」子屏仍显示未装填，按 1-4 毫无反应）。
test('load reads every small-world document a reopened world already has on disk', async () => {
  const f = fixture();
  f.setPlacements([{ propId: 'block', index: 0, siteId: null, position: [1, 0, 0], rotation: [0, 0, 0], scale: 1 }]);
  f.setRules([{ fragmentId: 'sway', type: 'sway', axis: 'x', amplitude: 0.3, periodMs: 2600 }]);
  f.setSubworlds([{ id: 'subworld-1', name: '第一界' }]);
  await f.game.load();
  assert.deepEqual(f.reads.map(v => v.action), ['loadPlacements', 'loadWorldRules', 'loadSubworlds'],
    '三份自有文档必须各读一次，而不是静默失败');
  assert.deepEqual(f.game.loadFailures, [], '读失败必须留下原因，不能再被空 catch 吞掉');
  assert.deepEqual(f.game.placements.placements.map(v => v.propId), ['block']);
  assert.deepEqual(f.game.worldRules.rules.map(v => [v.amplitude, v.periodMs]), [[0.3, 2600]]);
  assert.deepEqual(f.game.subworlds.subworlds.map(v => v.name), ['第一界']);
  // 再次打开世界（新的页面、新的 gameplay 对象）拿到的是同一份盘上数据。
  const reopened = fixture();
  reopened.setPlacements(f.game.placements.placements);
  reopened.setRules(f.game.worldRules.rules);
  await reopened.game.load();
  assert.deepEqual(reopened.game.placements, f.game.placements);
  assert.deepEqual(reopened.game.worldRules, f.game.worldRules);
  assert.deepEqual(reopened.game.loadFailures, []);
});

// A broken side document must not block entry, but it must not vanish either.
test('a side document that cannot be read is reported instead of being swallowed', async () => {
  const state = { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } };
  const seen = [];
  const game = createStoryGameplay({ projectPath: 'D:/story', readPlayer: () => null, readBoss: () => null,
    api: { scratch: { sendKeyEvent: async (key, mods, json) => {
      const request = JSON.parse(json); seen.push(request.action);
      if (request.action === 'load') return { status: 'ok', role: 'main', config, state: structuredClone(state) };
      if (request.action === 'loadPlacements') return { status: 'error', message: '陈列存档损坏' };
      if (request.action === 'loadWorldRules') return { status: 'ok', role: 'main', state: { version: 1, rules: [] } };
      return { status: 'ok', role: 'main', state: { version: 1, subworlds: [] } };
    } } } });
  const loaded = await game.load();
  assert.deepEqual(seen, ['load', 'loadPlacements', 'loadWorldRules', 'loadSubworlds'],
    '一份文档坏掉不能挡住另外两份，也不能一份都不读');
  assert.deepEqual(game.loadFailures, ['陈列存档损坏'], '读失败必须留下原因');
  assert.equal(game.placements, null);
  assert.equal(loaded.state.boss.hp, 200, '战斗状态照旧可用');
});

test('failed saves retain the identical command; repeated input shares in-flight work', async () => {
  const f = fixture(); await f.game.load(); f.fail(true);
  await assert.rejects(f.game.attack(), /disk full/);
  assert.equal(f.game.data.state.boss.hp, 200); assert.ok(f.game.needsSave);
  const gate = deferred(); f.gate(gate); f.fail(false);
  const retry = f.game.flush(); const again = f.game.attack(); assert.equal(retry, again);
  gate.resolve(); await retry;
  assert.deepEqual(f.requests[1], f.requests[2]);
  assert.equal(f.state.boss.hp, 180); assert.equal(f.game.needsSave, false);
});

test('lost replies and visual failure do not duplicate committed damage', async () => {
  const f = fixture(); await f.game.load(); f.loseReply();
  await assert.rejects(f.game.attack(), /reply lost/); assert.equal(f.state.boss.hp, 180);
  await f.game.flush(); assert.equal(f.state.boss.hp, 180);
  f.tick(); f.failVisual(true);
  await assert.rejects(f.game.attack(), /model failed/);
  const count = f.requests.length; assert.equal(f.state.boss.hp, 160);
  f.failVisual(false); await f.game.flush(); assert.equal(f.requests.length, count);
});

test('revision conflict reconciles authoritative state and clears only the obsolete command', async () => {
  const f = fixture(); await f.game.load(); f.conflict();
  await assert.rejects(f.game.attack(), /conflict/);
  assert.equal(f.game.data.state.boss.hp, 180); assert.equal(f.game.needsSave, false);
  f.tick(); await f.game.attack(); assert.equal(f.state.boss.hp, 160);
});

test('a dead boss is hidden and the old red-ball actor is retired, never created', async () => {
  const f = apiFixture({ actors: [...STORY_CHARACTERS.map(c => actorFixture(c)),
    actorFixture(LEGACY_FRAGMENT)] });
  const gameplay = { role: 'main', state: { boss: { hp: 0 }, drop: null } };
  await ensureStoryCharacters({ api: f.api, sceneId: 'scene.ini', frontendUrl: url,
    gameplay, wait: async () => {} });
  assert.equal(f.get(STORY_CHARACTERS[1].guid).visible, false);
  assert.equal(f.get(FRAGMENT_GUID).visible, false, '旧存档里的红球必须退场');
  assert.equal(f.calls.filter(c => c[0] === 'create').length, 0);
  // A world that does not contain the legacy actor is not given one either.
  const fresh = apiFixture();
  await ensureStoryCharacters({ api: fresh.api, sceneId: 'scene.ini', frontendUrl: url,
    gameplay, wait: async () => {} });
  assert.equal(fresh.get(FRAGMENT_GUID), undefined, '主世界不再创建世界碎片 actor');
});

test('child hides copied boss and legacy fragment and never creates either', async () => {
  const f = apiFixture({ actors: [...STORY_CHARACTERS.map(c => actorFixture(c)),
    actorFixture(LEGACY_FRAGMENT)] });
  await ensureStoryCharacters({ api: f.api, sceneId: 'scene.ini', frontendUrl: url,
    gameplay: { role: 'child', state: { boss: { hp: 200 }, drop: null } } });
  assert.equal(f.get(STORY_CHARACTERS[1].guid).visible, false);
  assert.equal(f.get(FRAGMENT_GUID).visible, false);
  assert.equal(f.calls.filter(c => c[0] === 'create').length, 0);
  const empty = apiFixture();
  await ensureStoryCharacters({ api: empty.api, sceneId: 'scene.ini', frontendUrl: url,
    gameplay: { role: 'child', state: { boss: { hp: 0 }, drop: null } } });
  // A small world is the player and the prophet. The boss and the merchant belong to the main
  // world, so neither is created here.
  const childCast = STORY_CHARACTERS.filter(character => ['player', 'prophet'].includes(character.role));
  assert.equal(empty.calls.filter(c => c[0] === 'create' && !c[4].skip_if_exists).length, childCast.length);
  assert.equal(empty.state.actors.length, childCast.length);
  assert.equal(empty.get(STORY_CHARACTERS[1].guid), undefined);
});

test('source guard prevents any stale actor mutation', async () => {
  const f = apiFixture();
  await assert.rejects(ensureStoryCharacters({ api: f.api, sceneId: 'scene.ini', frontendUrl: url,
    assertSource: async () => { throw new Error('changed source'); } }), /changed source/);
  assert.equal(f.calls.length, 0);
});


test('a missing CEF reply times out; an identical retry commits once and ignores stale replies', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const f = fixture(); await f.game.load();
  const reply = deferred(); f.delayReply(reply);
  const rejected = assert.rejects(f.game.attack(), /玩法请求超时/);
  assert.equal(f.state.boss.hp, 180); // Native commit succeeded, but its reply never arrived.
  assert.equal(f.game.data.state.boss.hp, 200);
  t.mock.timers.tick(15_001); await rejected;
  assert.equal(f.game.busy, false);
  assert.equal(f.game.needsSave, true);

  f.delayReply(null); await f.game.flush();
  assert.deepEqual(f.requests[1], f.requests[2]);
  assert.equal(f.state.boss.hp, 180);
  assert.equal(f.game.needsSave, false);
  f.tick(); await f.game.attack();
  assert.equal(f.game.data.state.boss.hp, 160);
  const accepted = f.states.length, feedback = f.feedback.length;
  reply.resolve(); await new Promise(resolve => setImmediate(resolve));
  assert.equal(f.game.data.state.boss.hp, 160);
  assert.equal(f.states.length, accepted);
  assert.equal(f.feedback.length, feedback);
});

test('a missing initial load reply fails visibly without accepting its late state', async t => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const reply = deferred(), accepted = [];
  const game = createStoryGameplay({ projectPath: 'D:/story', onState: state => accepted.push(state),
    api: { scratch: { sendKeyEvent: () => reply.promise } } });
  const rejected = assert.rejects(game.load(), /玩法请求超时/);
  t.mock.timers.tick(15_001); await rejected;
  assert.equal(game.data, null);
  reply.resolve({ status: 'ok', role: 'main', config,
    state: { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } } });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(game.data, null);
  assert.deepEqual(accepted, []);
});


test('airborne attacks never mutate progress or consume the next grounded attack', async () => {
  const f = fixture(); await f.game.load(); f.player.grounded = false;
  for (let i = 0; i < 3; i++) await f.game.attack();
  assert.equal(f.requests.length, 1); assert.equal(f.state.boss.hp, 200);
  f.player.grounded = true; await f.game.attack(); assert.equal(f.state.boss.hp, 180);
  for (let i = 0; i < 9; i++) { f.tick(); await f.game.attack(); }
  assert.equal(f.state.inventory.worldFragment, 1, '最后一刀落地就把凭证交给背包');
});

test('normal hits and misses keep the shared action interval without showing skill cooldowns', async () => {
  for (const hit of [true, false]) {
    const f = fixture(); f.state.rage = 100; await f.game.load();
    if (!hit) f.player.facingYaw = Math.PI;
    await f.game.attack();
    for (const id of ['heavy', 'sweep']) {
      assert.equal(f.game.skillStatus(id).remainingMs, 0);
      assert.equal(f.game.skillStatus(id).available, false);
    }
    const commands = f.requests.length;
    await f.game.castSkill('heavy'); assert.equal(f.requests.length, commands);
    f.tick(399); assert.equal(f.game.skillStatus('heavy').available, false);
    f.tick(1); assert.equal(f.game.skillStatus('heavy').available, true);
  }
});
test('E and R count only their own cooldowns, with unchanged rage costs and durations', async () => {
  const f = fixture(); f.state.rage = 100; await f.game.load();
  await f.game.castSkill('heavy');
  assert.equal(f.game.skillStatus('heavy').remainingMs, 1200);
  assert.equal(f.game.skillStatus('sweep').remainingMs, 0);
  assert.equal(f.state.rage, 70); assert.equal(f.state.boss.hp, 150);
  f.tick(); await f.game.castSkill('sweep');
  assert.equal(f.game.skillStatus('heavy').remainingMs, 800);
  assert.equal(f.game.skillStatus('sweep').remainingMs, 3000);
  assert.equal(f.state.rage, 20); assert.equal(f.state.boss.hp, 70);
  f.tick(800); await f.game.attack();
  assert.equal(f.game.skillStatus('heavy').remainingMs, 0);
  assert.equal(f.game.skillStatus('sweep').remainingMs, 2200);
});
test('insufficient rage, dodging and pending acknowledgements cannot start another skill cooldown', async () => {
  const f = fixture(); await f.game.load();
  await f.game.castSkill('heavy'); await f.game.castSkill('sweep');
  assert.equal(f.requests.length, 1);
  assert.equal(f.game.skillStatus('heavy').remainingMs, 0);
  f.state.rage = 100; await f.game.load(); f.player.movementState = 'dodging';
  await f.game.castSkill('heavy'); assert.equal(f.game.skillStatus('heavy').remainingMs, 0);
  f.player.movementState = 'idle'; const gate = deferred(); f.gate(gate);
  const casting = f.game.castSkill('heavy'); f.tick(); const duplicate = f.game.castSkill('sweep');
  assert.equal(f.game.skillStatus('sweep').remainingMs, 0);
  assert.equal(f.game.skillStatus('sweep').available, false);
  gate.resolve(); await Promise.all([casting, duplicate]);
});

// F 的裁决：提示文本和按键动作同源，这里只钉住纯函数本身（不碰引擎、不碰 DOM）。
// 拾取红球已从流程里删掉，因此裁决只剩先知与小世界里的方块两项。
test('the F resolver no longer knows pickup, and still answers the prophet alone', () => {
  const range = PROPHET_INTERACTION.range;
  assert.equal(range, 12);
  const resolve = prophet => contextualInteraction({ prophetDistance: prophet, range });
  // 只有先知在范围内 / 不在范围内。
  assert.deepEqual(resolve(3), { kind: 'prophet', distance: 3, prompt: 'F 与先知交谈' });
  assert.deepEqual(resolve(Infinity), { kind: null, distance: null, prompt: null });
  // 边界含端点：恰在射程上算命中，越界一点点就不算。
  assert.equal(resolve(range).kind, 'prophet');
  assert.equal(resolve(range + 0.01).kind, null);
  // 先知射程默认取真实配置。
  assert.equal(contextualInteraction({ prophetDistance: 13 }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: 1, range: 0 }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: 1, range: 0.5 }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: 0.4, range: 0.5 }).kind, 'prophet');
  // 缺参 / 脏数据永远退化成“什么都没有”，不会抛错。
  assert.deepEqual(contextualInteraction(), { kind: null, distance: null, prompt: null });
  assert.equal(contextualInteraction({ prophetDistance: NaN }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: '3' }).kind, null);
  // 提示文本只此一份：裁决给出的字符串就是界面显示的那一行，且不再有拾取这一项。
  assert.equal(INTERACTION_PROMPTS.prophet, 'F 与先知交谈');
  assert.equal(INTERACTION_PROMPTS.block, '按 F 取回方块');
  assert.equal('pickup' in INTERACTION_PROMPTS, false);
  assert.equal(Object.isFrozen(INTERACTION_PROMPTS), true);
});

// 小世界里玩家自己放下的方块：与先知进同一个裁决，
// 因此“提示说什么、F 就做什么”这条老规矩不需要第二条路径来维持。
test('the F resolver takes back the nearest placed block, and the prophet still wins a tie', () => {
  const reach = PLACED_BLOCK_REACH;
  assert.equal(reach, 2.5, '取回的射程是陈列几何给出的那个常量');
  const resolve = (prophet, block) => contextualInteraction({ prophetDistance: prophet,
    blockDistance: block, range: PROPHET_INTERACTION.range, blockRange: reach });
  // 只有方块在范围内：提示就是取回。
  assert.deepEqual(resolve(Infinity, 1), { kind: 'block', distance: 1, prompt: '按 F 取回方块' });
  assert.deepEqual(resolve(Infinity, reach), { kind: 'block', distance: reach, prompt: '按 F 取回方块' });
  assert.deepEqual(resolve(Infinity, reach + 0.01), { kind: null, distance: null, prompt: null });
  // 与先知都够得着：近者胜。
  assert.equal(resolve(1.2, 0.4).kind, 'block');
  assert.equal(resolve(0.4, 1.2).kind, 'prophet');
  // 平局仍然守旧：先知优先，不会被一件躺在脚边的方块抢走对话。
  assert.deepEqual(resolve(2, 2), { kind: 'prophet', distance: 2, prompt: 'F 与先知交谈' });
  assert.deepEqual(resolve(1, 1), { kind: 'prophet', distance: 1, prompt: 'F 与先知交谈' });
  // 缺参 / 脏数据照旧退化成“什么都没有”，新候选不得改变这一点。
  assert.deepEqual(contextualInteraction(), { kind: null, distance: null, prompt: null });
  assert.deepEqual(contextualInteraction({ blockDistance: 1 }), { kind: 'block', distance: 1, prompt: '按 F 取回方块' });
  assert.deepEqual(contextualInteraction({ blockDistance: reach + 0.01 }), { kind: null, distance: null, prompt: null });
  assert.equal(contextualInteraction({ blockDistance: NaN }).kind, null);
  assert.equal(contextualInteraction({ blockDistance: '1' }).kind, null);
  assert.equal(contextualInteraction({ blockDistance: 1, blockRange: 0 }).kind, null);
  // 主世界没有陈列物：不传方块距离时只回答先知。
  assert.equal(resolve(3, Infinity).kind, 'prophet');
  assert.deepEqual(resolve(Infinity, reach + 1), { kind: null, distance: null, prompt: null });
});
