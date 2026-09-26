import assert from 'node:assert/strict';
import test from 'node:test';
import { createStoryGameplay, GAMEPLAY_KEY, FRAGMENT, worldBounds, canHitBoss, distanceToBounds } from '../../frontend/storyGameplay.mjs';
import { STORY_CHARACTERS, rotatePoint } from '../../frontend/storyCharacters.mjs';
import { actorFixture, apiFixture, deferred, url } from './fixtures.mjs';
import { ensureStoryCharacters } from '../../frontend/storyActors.mjs';

const config = { playerHp: 100, playerMp: 100, bossHp: 200, damage: 20, cooldownMs: 400,
  bossBarRadius: 10, meleeRange: 2.5, meleeHalfAngle: Math.PI / 3, pickupRange: 2 };
function fixture() {
  let state = { version: 1, revision: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } };
  let role = 'main', time = 0, id = 0, fail = false, lostReply = false, visualFail = false, gate = null, replyGate = null;
  const boss = actorFixture(STORY_CHARACTERS[1]), bounds = worldBounds(boss);
  const player = { position: [0, 0, bounds[2] - 1], rotation: [0, 0, 0] };
  const requests = [], feedback = [], states = [], visuals = [], operations = new Set();
  const response = () => ({ status: 'ok', role, state: structuredClone(state), config });
  const game = createStoryGameplay({ projectPath: 'D:/story', readPlayer: () => player, readBoss: () => boss,
    now: () => time, newId: () => `operation-${++id}`, onFeedback: v => feedback.push(v), onState: v => states.push(v),
    reconcile: async data => { visuals.push(structuredClone(data)); if (visualFail) throw new Error('model failed'); },
    api: { scratch: { sendKeyEvent: async (key, mods, json) => {
      assert.equal(key, GAMEPLAY_KEY); assert.equal(mods, '');
      const request = JSON.parse(json); requests.push(request);
      if (request.action === 'load') return response();
      if (gate) await gate.promise;
      if (fail) throw new Error('disk full');
      if (!operations.has(request.operationId)) {
        if (request.expectedRevision !== state.revision) return { ...response(), status: 'error', code: 'REVISION_CONFLICT', message: 'conflict' };
        operations.add(request.operationId);
        state.revision++;
        if (request.action === 'hitBoss') {
          state.boss.hp -= 20;
          if (!state.boss.hp) state.drop = { id: 'story.boss.world-fragment', position: request.bossPosition, collected: false };
        } else { state.drop.collected = true; state.inventory.worldFragment++; }
      }
      if (lostReply) { lostReply = false; throw new Error('reply lost'); }
      const reply = { data: response() };
      if (replyGate) await replyGate.promise;
      return reply;
    } } } });
  return { game, player, boss, requests, feedback, states, visuals,
    tick: () => { time += 400; }, fail: value => { fail = value; }, loseReply: () => { lostReply = true; },
    failVisual: value => { visualFail = value; }, gate: value => { gate = value; }, role: value => { role = value; },
    delayReply: value => { replyGate = value; },
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
  const ground = [0, 0, bounds[2] - 2];
  assert.equal(distanceToBounds(ground, bounds), 2);
  assert.ok(canHitBoss({ position: ground, rotation: [0, 0, 0] }, bounds, config));
  assert.equal(canHitBoss({ position: ground, rotation: [0, Math.PI, 0] }, bounds, config), false);
  assert.equal(canHitBoss({ position: [0, 0, bounds[2] - 2.51], rotation: [0, 0, 0] }, bounds, config), false);
  assert.equal(canHitBoss(null, null, config), false);
});

test('ten acknowledged hits, cooldown, one drop and one pickup', async () => {
  const f = fixture(); await f.game.load();
  for (let i = 0; i < 10; i++) {
    await f.game.attack(); await f.game.attack(); f.tick();
    assert.equal(f.state.boss.hp, 200 - (i + 1) * 20);
  }
  await f.game.attack(); assert.equal(f.state.revision, 10);
  assert.deepEqual(f.state.drop.position, [0, 0, 12]);
  await f.game.pickup(); assert.equal(f.state.inventory.worldFragment, 0);
  f.player.position = [0, 0, 12];
  await f.game.pickup(); await f.game.pickup();
  assert.equal(f.state.inventory.worldFragment, 1);
  assert.equal(f.state.revision, 11);
  assert.equal(f.feedback.filter(v => v.startsWith('获得')).length, 1);
});

test('out of range, behind and child world never dispatch damage', async () => {
  const f = fixture(); await f.game.load(); f.player.rotation[1] = Math.PI;
  await f.game.attack(); f.tick(); f.player.rotation[1] = 0; f.player.position[2] = -100;
  await f.game.attack(); f.tick(); f.player.position[2] = 10;
  f.role('child'); await f.game.load(); await f.game.attack(); await f.game.pickup();
  assert.ok(f.requests.every(v => v.action === 'load'));
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
  assert.equal(empty.calls.filter(c => c[0] === 'create').length, 3);
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
    state: { version: 1, revision: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } } });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(game.data, null);
  assert.deepEqual(accepted, []);
});
