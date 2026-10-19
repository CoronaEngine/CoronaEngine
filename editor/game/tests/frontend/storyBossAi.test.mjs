import assert from 'node:assert/strict';
import test from 'node:test';
import {
  BOSS_CLIPS, BOSS_CLIP_SECONDS, BOSS_TUNING, createBossDirector, nextBossState,
} from '../../frontend/storyBossAi.mjs';

function fixture(base = {}) {
  const calls = [];
  const attacks = [];
  let clock = 0;
  const api = { sceneTools: { setActorAnimation: async (scene, guid, clip) => { calls.push(clip); } } };
  const director = createBossDirector({
    api, sceneId: 'scene.ini', guid: 'boss-guid', now: () => clock,
    onError: error => calls.push(`error:${error.message}`),
    onAttack: info => attacks.push(info.clip),
  });
  return {
    calls, attacks, director,
    advance: ms => { clock += ms; },
    state: () => director.state(),
  };
}

test('death wins over everything else and is terminal', () => {
  assert.deepEqual(nextBossState({ state: 'idle', hp: 0, distance: 1, cooldownReady: true, wasHit: true }),
    { state: 'dead', clip: BOSS_CLIPS.dead, once: true });
  assert.deepEqual(nextBossState({ state: 'dead', hp: 0, distance: 1, cooldownReady: false, wasHit: false }),
    { state: 'dead', clip: BOSS_CLIPS.dead, once: true });
});

test('a hit makes the boss flinch before it swings', () => {
  const flinch = nextBossState({ state: 'idle', hp: 100, distance: 1, cooldownReady: true, wasHit: true });
  assert.equal(flinch.state, 'hurt');
  assert.equal(flinch.clip, BOSS_CLIPS.hurt);
});

test('the boss swings inside range on cooldown and idles outside it', () => {
  assert.equal(nextBossState({ state: 'idle', hp: 100, distance: 5, cooldownReady: true, wasHit: false }).state, 'attack1');
  assert.equal(nextBossState({ state: 'attack1', hp: 100, distance: 5, cooldownReady: true, wasHit: false }).state, 'attack2',
    'a second swing alternates, so the fight does not read as one repeated animation');
  assert.equal(nextBossState({ state: 'idle', hp: 100, distance: 30, cooldownReady: true, wasHit: false }).state, 'idle');
  assert.equal(nextBossState({ state: 'idle', hp: 100, distance: 5, cooldownReady: false, wasHit: false }).state, 'idle',
    'a cooling-down boss waits inside range rather than spamming');
});

test('the director plays idle, swings when the player closes in, and alternates', async () => {
  const f = fixture();
  await f.director.tick({ hp: 200, distance: 30 });
  assert.equal(f.state(), 'idle');
  assert.deepEqual(f.calls, [BOSS_CLIPS.idle]);

  await f.director.tick({ hp: 200, distance: 6 });
  assert.equal(f.state(), 'attack1');
  assert.deepEqual(f.attacks, [BOSS_CLIPS.attack1]);

  f.advance(BOSS_TUNING.attackCooldownSeconds * 1000 + 50);
  await f.director.tick({ hp: 200, distance: 6 });
  assert.equal(f.state(), 'attack2', 'the follow-up uses the other attack clip');
  assert.deepEqual(f.attacks, [BOSS_CLIPS.attack1, BOSS_CLIPS.attack2]);
  f.director.dispose();
});

test('a hit interrupts into the flinch and it is not re-triggered every tick', async () => {
  const f = fixture();
  await f.director.tick({ hp: 200, distance: 30 });
  await f.director.tick({ hp: 180, distance: 30, wasHit: true });
  assert.equal(f.state(), 'hurt');
  const hurtCalls = f.calls.filter(clip => clip === BOSS_CLIPS.hurt).length;
  assert.equal(hurtCalls, 1);
  // Still flinching: another hit does not restart the clip.
  f.advance(100);
  await f.director.tick({ hp: 160, distance: 30, wasHit: true });
  assert.equal(f.calls.filter(clip => clip === BOSS_CLIPS.hurt).length, 1);
  f.director.dispose();
});

test('zero health stops the boss on its death clip and never swings again', async () => {
  const f = fixture();
  await f.director.tick({ hp: 20, distance: 30 });
  await f.director.tick({ hp: 0, distance: 4 });
  assert.equal(f.state(), 'dead');
  assert.equal(f.calls.at(-1), BOSS_CLIPS.dead);
  await f.director.tick({ hp: 0, distance: 4 });
  await f.director.tick({ hp: 0, distance: 4 });
  assert.deepEqual(f.attacks, [], 'a dead boss never swings');
  assert.equal(f.calls.filter(clip => clip === BOSS_CLIPS.dead).length, 1, 'the death clip is not re-sent');
  f.director.dispose();
});

test('every clip the machine names has a measured duration', () => {
  for (const clip of [BOSS_CLIPS.attack1, BOSS_CLIPS.attack2, BOSS_CLIPS.hurt, BOSS_CLIPS.dead]) {
    assert.equal(typeof BOSS_CLIP_SECONDS[clip], 'number', clip);
  }
});

test('the constructor refuses an engine without the animation route', () => {
  assert.throws(() => createBossDirector({ api: {}, sceneId: 's', guid: 'g' }), /setActorAnimation/);
  assert.throws(() => createBossDirector({ api: { sceneTools: { setActorAnimation() {} } }, sceneId: '', guid: 'g' }),
    /scene id/);
  assert.throws(() => createBossDirector({ api: { sceneTools: { setActorAnimation() {} } }, sceneId: 's', guid: '' }),
    /actor guid/);
});
