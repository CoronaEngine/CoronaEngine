import assert from 'node:assert/strict';
import test from 'node:test';
import { createStoryGameplay, GAMEPLAY_KEY, FRAGMENT, worldBounds, canHitBoss, distanceToBounds,
  contextualInteraction, INTERACTION_PROMPTS } from '../../frontend/storyGameplay.mjs';
import { PROPHET_INTERACTION } from '../../frontend/prophetDialogue.mjs';
import { PLACED_BLOCK_REACH } from '../../frontend/storyProps.mjs';
import { STORY_CHARACTERS, rotatePoint } from '../../frontend/storyCharacters.mjs';
import { actorFixture, apiFixture, deferred, url, gameplayConfig } from './fixtures.mjs';
import { ensureStoryCharacters } from '../../frontend/storyActors.mjs';

const config = gameplayConfig;
// 龙在主世界里被摆在哪儿，就从授权数据里读，而不是在用例里再抄一份坐标：
// 它一挪动，「红球落在龙倒下的地方」和「走到龙脚下」这类断言就得跟着走。
const bossSite = STORY_CHARACTERS.find(character => character.role === 'boss');
function fixture(options = {}) {
  let state = { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } };
  let role = 'main', time = 0, id = 0, fail = false, lostReply = false, visualFail = false, gate = null, replyGate = null;
  const boss = actorFixture(STORY_CHARACTERS[1]), bounds = worldBounds(boss);
  const player = { position: [0, 0, bounds[2] - 1], rotation: [0, Math.PI, 0], facingYaw: 0, grounded: true };
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
          if (!state.boss.hp) state.drop = { id: 'story.boss.world-fragment', position: request.bossPosition, collected: false };
        } else if (request.action === 'castSkill') {
          const skill = config.skills[request.skillId];
          state.rage -= skill.rageCost;
          if (request.hit) state.boss.hp = Math.max(0, state.boss.hp - skill.damage);
        } else { state.drop.collected = true; state.inventory.worldFragment++; }
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

test('ten acknowledged hits, cooldown, one drop and one pickup', async () => {
  const f = fixture(); await f.game.load();
  for (let i = 0; i < 10; i++) {
    await f.game.attack(); await f.game.attack(); f.tick();
    assert.equal(f.state.boss.hp, 200 - (i + 1) * 20);
  }
  await f.game.attack(); assert.equal(f.state.revision, 10);
  assert.deepEqual(f.state.drop.position, [bossSite.x, 0, bossSite.z]);
  await f.game.pickup(); assert.equal(f.state.inventory.worldFragment, 0);
  f.player.position = [bossSite.x, 0, bossSite.z];
  await f.game.pickup(); await f.game.pickup();
  assert.equal(f.state.inventory.worldFragment, 1);
  assert.equal(f.state.revision, 11);
  assert.equal(f.feedback.filter(v => v.startsWith('获得')).length, 1);
});

// 掉落也必须落在地面上：碎片落点读的是同一份烘好高度网格，而不是写死的 0（旧常量）。
// 没有网格时（这条文件里的其它用例）就还是 0，所以旧断言原样保留在上面那条用例里。
test('the dropped fragment lands on the grid at the boss site, and on the old constant without one', async () => {
  const ground = (x, z) => 2 + 0.5 * x - 0.25 * z;
  const f = fixture({ groundYAt: ground }); await f.game.load();
  for (let i = 0; i < 10; i++) { await f.game.attack(); f.tick(); }
  assert.equal(f.state.drop.collected, false, 'Boss 倒下时必须掉一件');
  const [x, y, z] = f.state.drop.position;
  assert.deepEqual([x, z], [bossSite.x, bossSite.z], '掉落的 x/z 仍是 Boss 倒下的地方');
  assert.equal(y, ground(x, z), '碎片必须落在 Boss 脚下那一格的地面上');
  assert.ok(Math.abs(y - 0) > 0.5, '与旧常量 0 必须差得可观测，否则这条断言是空转的');
});

test('out of range, behind and child world never dispatch damage', async () => {
  const f = fixture(); await f.game.load(); f.player.facingYaw = Math.PI;
  await f.game.attack(); f.tick(); f.player.facingYaw = 0; f.player.position[2] = -100;
  await f.game.attack(); f.tick(); f.player.position[2] = 10;
  f.role('child'); await f.game.load(); await f.game.attack(); await f.game.pickup();
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

test('dead boss is not created or resurrected; pending drop is grounded and stable', async () => {
  const f = apiFixture({ actors: STORY_CHARACTERS.map(c => actorFixture(c)) });
  const gameplay = { role: 'main', state: { boss: { hp: 0 }, drop: { id: 'x', position: [0, 0, 12], collected: false } } };
  const load = extra => ensureStoryCharacters({ api: f.api, sceneId: 'scene.ini', frontendUrl: url,
    gameplay, wait: async () => {}, ...extra });
  await load(); await load();
  assert.equal(f.get(STORY_CHARACTERS[1].guid).visible, false);
  assert.equal(f.calls.filter(c => c[0] === 'create').length, 1);
  const drop = f.get(FRAGMENT.guid), bounds = worldBounds(drop);
  assert.ok(Math.abs(bounds[1]) < 1e-9);
  assert.ok(Math.abs(Math.max(...[0, 1, 2].map(i => bounds[i + 3] - bounds[i])) - 0.5) < 1e-9);
  gameplay.state.drop.collected = true; await load({ combatOnly: true });
  assert.equal(f.get(FRAGMENT.guid).visible, false);
});

test('child hides copied boss and fragment and never creates either', async () => {
  const f = apiFixture({ actors: [...STORY_CHARACTERS, FRAGMENT].map(c => actorFixture(c)) });
  await ensureStoryCharacters({ api: f.api, sceneId: 'scene.ini', frontendUrl: url,
    gameplay: { role: 'child', state: { boss: { hp: 200 }, drop: null } } });
  assert.equal(f.get(STORY_CHARACTERS[1].guid).visible, false);
  assert.equal(f.get(FRAGMENT.guid).visible, false);
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


test('airborne attacks and pickups never mutate progress or consume the next grounded attack', async () => {
  const f = fixture(); await f.game.load(); f.player.grounded = false;
  for (let i = 0; i < 3; i++) { await f.game.attack(); await f.game.pickup(); }
  assert.equal(f.requests.length, 1); assert.equal(f.state.boss.hp, 200);
  f.player.grounded = true; await f.game.attack(); assert.equal(f.state.boss.hp, 180);
  for (let i = 0; i < 9; i++) { f.tick(); await f.game.attack(); }
  f.player.position = [bossSite.x, 1.2, bossSite.z]; f.player.grounded = false;
  await f.game.pickup(); assert.equal(f.state.inventory.worldFragment, 0);
  f.player.position[1] = 0; f.player.grounded = true;
  await f.game.pickup(); assert.equal(f.state.inventory.worldFragment, 1);
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
test('the F resolver picks the nearer of prophet and pickup and carries its own prompt', () => {
  // 两个距离都用真实配置的射程：先知 PROPHET_INTERACTION.range，碎片 config.pickupRange。
  const range = PROPHET_INTERACTION.range, pickupRange = config.pickupRange;
  assert.equal(range, 12); assert.equal(pickupRange, 2);
  const resolve = (prophet, pickup) => contextualInteraction({ prophetDistance: prophet,
    pickupDistance: pickup, range, pickupRange });
  // 只有先知 / 只有碎片在范围内。
  assert.deepEqual(resolve(3, Infinity), { kind: 'prophet', distance: 3, prompt: 'F 与先知交谈' });
  assert.deepEqual(resolve(Infinity, 1.2), { kind: 'pickup', distance: 1.2, prompt: '按 F 拾取' });
  // 都在范围内：近者胜，两种朝向都试。
  assert.equal(resolve(1.5, 2).kind, 'prophet');
  assert.deepEqual(resolve(8, 1.5), { kind: 'pickup', distance: 1.5, prompt: '按 F 拾取' });
  assert.equal(resolve(0.5, 1.9).kind, 'prophet');
  assert.equal(resolve(11.9, 0.1).kind, 'pickup');
  // 恰好等距：先知胜（确定性平局规则，与旧有的先知优先一致）。
  assert.deepEqual(resolve(1.5, 1.5), { kind: 'prophet', distance: 1.5, prompt: 'F 与先知交谈' });
  assert.equal(resolve(range, 11.5).kind, 'prophet', '两个都在射程内但先知更近');
  // 都不在范围内：没有动作，也没有提示。
  assert.deepEqual(resolve(Infinity, Infinity), { kind: null, distance: null, prompt: null });
  assert.deepEqual(resolve(range + 0.5, pickupRange + 0.5), { kind: null, distance: null, prompt: null });
  // 边界含端点：恰在射程上算命中，越界一点点就不算。
  assert.equal(resolve(range, Infinity).kind, 'prophet');
  assert.equal(resolve(range + 0.01, Infinity).kind, null);
  assert.equal(resolve(Infinity, pickupRange).kind, 'pickup');
  assert.equal(resolve(Infinity, pickupRange + 0.01).kind, null);
  // 先知射程默认取真实配置；碎片射程不传＝不设限（调用方自己已经量过距离）。
  assert.equal(contextualInteraction({ prophetDistance: 13, pickupDistance: Infinity }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: 1, pickupDistance: Infinity, range: 0 }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: 1, pickupDistance: Infinity, range: 0.5 }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: 0.4, pickupDistance: Infinity, range: 0.5 }).kind, 'prophet');
  // 缺参 / 脏数据永远退化成“什么都没有”，不会抛错。
  assert.deepEqual(contextualInteraction(), { kind: null, distance: null, prompt: null });
  assert.equal(contextualInteraction({ prophetDistance: NaN, pickupDistance: null }).kind, null);
  assert.equal(contextualInteraction({ prophetDistance: '3', pickupDistance: undefined }).kind, null);
  // 提示文本只此一份：裁决给出的字符串就是界面显示的那一行。
  assert.equal(INTERACTION_PROMPTS.prophet, 'F 与先知交谈');
  assert.equal(INTERACTION_PROMPTS.pickup, '按 F 拾取');
  assert.equal(INTERACTION_PROMPTS.block, '按 F 取回方块');
  assert.equal(Object.isFrozen(INTERACTION_PROMPTS), true);
});

// 第三个候选：小世界里玩家自己放下的方块。它和红球、先知一起进同一个裁决，
// 因此“提示说什么、F 就做什么”这条老规矩不需要第二条路径来维持。
test('the F resolver takes back the nearest placed block, and the prophet still wins a tie', () => {
  const reach = PLACED_BLOCK_REACH;
  assert.equal(reach, 2.5, '取回的射程是陈列几何给出的那个常量');
  const resolve = (prophet, pickup, block) => contextualInteraction({ prophetDistance: prophet,
    pickupDistance: pickup, blockDistance: block, range: PROPHET_INTERACTION.range,
    pickupRange: config.pickupRange, blockRange: reach });
  // 只有方块在范围内：提示就是取回。
  assert.deepEqual(resolve(Infinity, Infinity, 1), { kind: 'block', distance: 1, prompt: '按 F 取回方块' });
  assert.deepEqual(resolve(Infinity, Infinity, reach), { kind: 'block', distance: reach, prompt: '按 F 取回方块' });
  assert.deepEqual(resolve(Infinity, Infinity, reach + 0.01), { kind: null, distance: null, prompt: null });
  // 与红球都够得着：近者胜，两个方向都试。
  assert.equal(resolve(Infinity, 1.9, 0.5).kind, 'block');
  assert.equal(resolve(Infinity, 0.5, 1.9).kind, 'pickup');
  // 与先知都够得着：近者胜。
  assert.equal(resolve(1.2, Infinity, 0.4).kind, 'block');
  assert.equal(resolve(0.4, Infinity, 1.2).kind, 'prophet');
  // 平局仍然守旧：先知优先，不会被一件躺在脚边的方块抢走对话。
  assert.deepEqual(resolve(2, Infinity, 2), { kind: 'prophet', distance: 2, prompt: 'F 与先知交谈' });
  assert.deepEqual(resolve(1, Infinity, 1), { kind: 'prophet', distance: 1, prompt: 'F 与先知交谈' });
  // 红球与方块平局：拾取在先（这是把方块排在最后的直接后果）。
  assert.equal(resolve(Infinity, 1.5, 1.5).kind, 'pickup');
  // 缺参 / 脏数据照旧退化成“什么都没有”，新候选不得改变这一点。
  assert.deepEqual(contextualInteraction(), { kind: null, distance: null, prompt: null });
  assert.deepEqual(contextualInteraction({ blockDistance: 1 }), { kind: 'block', distance: 1, prompt: '按 F 取回方块' });
  assert.deepEqual(contextualInteraction({ blockDistance: reach + 0.01 }), { kind: null, distance: null, prompt: null });
  assert.equal(contextualInteraction({ blockDistance: NaN }).kind, null);
  assert.equal(contextualInteraction({ blockDistance: '1' }).kind, null);
  assert.equal(contextualInteraction({ blockDistance: 1, blockRange: 0 }).kind, null);
  // 主世界没有陈列物：不传方块距离时，原有的两条规则一字不变。
  assert.equal(resolve(3, Infinity, Infinity).kind, 'prophet');
  assert.deepEqual(resolve(Infinity, 1.2, Infinity), { kind: 'pickup', distance: 1.2, prompt: '按 F 拾取' });
});
