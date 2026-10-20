import assert from 'node:assert/strict';
import test from 'node:test';
import {
  PLAYER_ANIMATION_BLEND_SECONDS, PLAYER_ATTACK_CLIP, PLAYER_CLIP_DURATIONS,
  PLAYER_MOVEMENT_CLIPS, PLAYER_SKILL_CLIP, createPlayerAnimationSwitcher, locomotionClip,
} from '../../frontend/storyPlayerAnimation.mjs';

/** A fake engine: records every clip switch and lets a test advance the one-shot timer. */
function fixture() {
  const calls = [];
  const timers = new Map();
  let id = 0;
  const api = { sceneTools: { setActorAnimation: async (scene, guid, clip, blend) => {
    calls.push([scene, guid, clip, blend]);
    return { data: { status: 'accepted' } };
  } } };
  const switcher = createPlayerAnimationSwitcher({
    api, sceneId: 'scene.ini', guid: 'actor-guid',
    schedule: (fn, ms) => { const key = ++id; timers.set(key, { fn, ms }); return key; },
    cancel: key => timers.delete(key),
    onError: error => calls.push(['error', error.message]),
  });
  return { calls, timers, switcher, clips: () => calls.filter(c => c[0] !== 'error').map(c => c[2]) };
}

test('movement states map to the shipped clips, unknown states fall back to idle', () => {
  assert.equal(locomotionClip('idle'), PLAYER_MOVEMENT_CLIPS.idle);
  assert.equal(locomotionClip('walking'), PLAYER_MOVEMENT_CLIPS.walking);
  assert.equal(locomotionClip('sprinting'), 'Run');
  assert.equal(locomotionClip('jumping'), PLAYER_MOVEMENT_CLIPS.jumping);
  assert.equal(locomotionClip('dodging'), PLAYER_MOVEMENT_CLIPS.dodging);
  assert.equal(locomotionClip('nonsense'), PLAYER_MOVEMENT_CLIPS.idle);
  assert.equal(locomotionClip(undefined), PLAYER_MOVEMENT_CLIPS.idle);
  // Every clip the mapping names must have a measured duration, or a one-shot would time out on
  // the fallback instead of on the real clip.
  for (const clip of Object.values(PLAYER_MOVEMENT_CLIPS)) {
    assert.equal(typeof PLAYER_CLIP_DURATIONS[clip], 'number', clip);
  }
  for (const clip of [PLAYER_ATTACK_CLIP, PLAYER_SKILL_CLIP]) {
    assert.equal(typeof PLAYER_CLIP_DURATIONS[clip], 'number', clip);
  }
});

test('repeated states do not re-submit the clip, and a change does', async () => {
  const f = fixture();
  await f.switcher.update('idle');
  await f.switcher.update('idle');
  await f.switcher.update('idle');
  assert.deepEqual(f.clips(), [PLAYER_MOVEMENT_CLIPS.idle], 'the controller reports state many times per second');
  await f.switcher.update('walking');
  await f.switcher.update('sprinting');
  assert.deepEqual(f.clips(), [PLAYER_MOVEMENT_CLIPS.idle, PLAYER_MOVEMENT_CLIPS.walking, 'Run']);
  // The blend is passed through so the engine can cross-fade instead of snapping.
  assert.equal(f.calls[0][3], PLAYER_ANIMATION_BLEND_SECONDS);
  f.switcher.dispose();
});

test('a one-shot owns the actor, then hands it back to the current locomotion', async () => {
  const f = fixture();
  await f.switcher.update('walking');
  await f.switcher.playOnce(PLAYER_ATTACK_CLIP);
  assert.equal(f.switcher.isOneShot(), true);
  assert.equal(f.switcher.currentClip(), PLAYER_ATTACK_CLIP);
  // A walking update arriving mid-swing must be ignored, not cut the attack short.
  await f.switcher.update('idle');
  assert.deepEqual(f.clips(), [PLAYER_MOVEMENT_CLIPS.walking, PLAYER_ATTACK_CLIP]);
  // The timer is armed for the clip's own duration.
  const [timer] = [...f.timers.values()];
  assert.equal(timer.ms, Math.round(PLAYER_CLIP_DURATIONS[PLAYER_ATTACK_CLIP] * 1000));
  timer.fn();
  assert.equal(f.switcher.isOneShot(), false);
  assert.equal(f.switcher.currentClip(), PLAYER_MOVEMENT_CLIPS.idle, 'returns to the state the player is in now');
  f.switcher.dispose();
});

test('an explicit duration overrides the catalogue and a second one-shot restarts it', async () => {
  const f = fixture();
  await f.switcher.playOnce(PLAYER_ATTACK_CLIP, 0.25);
  assert.equal([...f.timers.values()][0].ms, 250);
  await f.switcher.playOnce(PLAYER_SKILL_CLIP, 0.5);
  assert.equal(f.timers.size, 1, 'the first timer is cancelled, not left to fire');
  assert.equal([...f.timers.values()][0].ms, 500);
  f.switcher.dispose();
});

test('a failed round-trip is retried instead of pinning the actor', async () => {
  const calls = [];
  let fail = true;
  const switcher = createPlayerAnimationSwitcher({
    api: { sceneTools: { setActorAnimation: async (scene, guid, clip) => {
      calls.push(clip);
      if (fail) throw new Error('bridge down');
    } } },
    sceneId: 'scene.ini', guid: 'g',
    schedule: () => 1, cancel: () => {},
  });
  await switcher.update('walking');
  assert.deepEqual(calls, [PLAYER_MOVEMENT_CLIPS.walking]);
  fail = false;
  await switcher.update('walking');
  assert.deepEqual(calls, [PLAYER_MOVEMENT_CLIPS.walking, PLAYER_MOVEMENT_CLIPS.walking],
    'the same clip is submitted again after a failure');
  switcher.dispose();
});

test('dispose stops the timer and ignores later updates', async () => {
  const f = fixture();
  await f.switcher.playOnce(PLAYER_ATTACK_CLIP);
  assert.equal(f.timers.size, 1);
  assert.equal(f.switcher.dispose(), true);
  assert.equal(f.timers.size, 0, 'no timer may fire into a replaced world');
  assert.equal(f.switcher.dispose(), false, 'idempotent');
  await f.switcher.update('walking');
  assert.deepEqual(f.clips(), [PLAYER_ATTACK_CLIP]);
});

test('the constructor refuses an engine without the animation route', () => {
  assert.throws(() => createPlayerAnimationSwitcher({ api: { sceneTools: {} }, sceneId: 's', guid: 'g' }),
    /setActorAnimation/);
  assert.throws(() => createPlayerAnimationSwitcher({ api: { sceneTools: { setActorAnimation() {} } }, sceneId: '', guid: 'g' }),
    /scene id/);
  assert.throws(() => createPlayerAnimationSwitcher({ api: { sceneTools: { setActorAnimation() {} } }, sceneId: 's', guid: '' }),
    /actor guid/);
});
