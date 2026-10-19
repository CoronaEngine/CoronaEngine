/**
 * What the player is doing → which animation clip plays.
 *
 * The engine gained a per-actor `AnimationPlayer` (`corona/resource/types/animation_player.h`):
 * an actor's clips are catalogued from its own model plus any `animation_sources`, and
 * `sceneTools.setActorAnimation(scene, actor, clip, blendSeconds)` cross-fades to one of them.
 * Before that the engine always looped `animations[0]`, so a character could only ever have one
 * motion.
 *
 * This module owns the policy — the mapping and the one-shot bookkeeping — and nothing else, so
 * it stays pure enough to unit test without an engine. The caller supplies the `api`.
 *
 * Clip names come from the seven clips embedded in `game/art/models/player/E3D-Assassin.glb`.
 * They were renamed per action at export on purpose: every file in the source package carried the
 * same authored action name (`Bip001|Take 001|BaseLayer`), which `request_switch` rejects as
 * `AMBIGUOUS_ANIMATION` when switching by name.
 */

/**
 * Clip durations in seconds, measured from the shipped GLB's accessor maxima. Used for one-shot
 * timing, so they must track the art: a one-shot that ends early snaps back mid-swing.
 */
export const PLAYER_CLIP_DURATIONS = Object.freeze({
  IdleB: 11.0,
  Walk: 1.5,
  Run: 0.8,
  AttackA: 0.833,
  AttackB: 0.933,
  Dead: 1.933,
});

/**
 * The locomotion clip for each `movementState` the player controller reports
 * (`snapshotPlayer().movementState`: 'idle' | 'walking' | 'sprinting' | 'jumping' | 'dodging').
 * Jumping and dodging share `JumpA` because the package has no dodge clip; both are short
 * airborne motions, so the reuse reads correctly.
 */
export const PLAYER_MOVEMENT_CLIPS = Object.freeze({
  idle: 'IdleB',
  walking: 'Walk',
  sprinting: 'Run',
  // 没有跳跃动作：跳与闪避都回到待机，避免请求不存在的 clip
  jumping: 'IdleB',
  dodging: 'IdleB',
});

/** The clip a melee swing plays. */
export const PLAYER_ATTACK_CLIP = 'AttackA';
/** The clip a skill plays (heavy slash / sweep). */
export const PLAYER_SKILL_CLIP = 'AttackB';
/** Cross-fade used when no caller asks for one, matching the engine's own default. */
export const PLAYER_ANIMATION_BLEND_SECONDS = 0.2;

/** The locomotion clip for a movement state; anything unknown falls back to idle. */
export function locomotionClip(movementState) {
  return PLAYER_MOVEMENT_CLIPS[movementState] || PLAYER_MOVEMENT_CLIPS.idle;
}

/**
 * Drive one actor's clip from the player's state.
 *
 * Two rules keep this from fighting the engine's per-call async bridge:
 *  - a clip is only submitted when it differs from the one already requested, because
 *    `setActorAnimation` is a round-trip and the controller reports state many times per second;
 *  - a one-shot (attack / skill) owns the actor until its clip has played out, then hands the
 *    actor back to whatever locomotion state is current — so a swing is not cut off by the
 *    walking update that arrives a frame later.
 */
export function createPlayerAnimationSwitcher({
  api, sceneId, guid, blendSeconds = PLAYER_ANIMATION_BLEND_SECONDS,
  schedule = (fn, ms) => setTimeout(fn, ms),
  cancel = id => clearTimeout(id),
  onError = () => {},
}) {
  if (!api?.sceneTools?.setActorAnimation) throw new Error('engine does not expose setActorAnimation');
  if (typeof sceneId !== 'string' || !sceneId) throw new Error('missing scene id');
  if (typeof guid !== 'string' || !guid) throw new Error('missing actor guid');

  let desired = null;      // clip the page wants on screen
  let submitted = null;    // clip last handed to the engine (for de-duplication)
  let submits = 0;         // how many round-trips actually left the page
  let lastResult = '';     // engine's answer to the latest one, for diagnostics
  let locomotion = 'idle'; // last movement state seen
  let oneShot = null;      // clip currently owning the actor, if any
  let timer = null;
  let disposed = false;

  function request(clip) {
    if (disposed || !clip) return Promise.resolve(false);
    desired = clip;
    if (clip === submitted) return Promise.resolve(false);
    submitted = clip;
    submits += 1;
    return Promise.resolve()
      .then(() => api.sceneTools.setActorAnimation(sceneId, guid, clip, blendSeconds))
      .then(() => { lastResult = `ok:${clip}`; return true; })
      .catch(error => {
        // Let the next update retry: a failed round-trip must not pin the actor forever.
        lastResult = `err:${error?.message || error}`;
        if (submitted === clip) submitted = null;
        if (!disposed) onError(error);
        return false;
      });
  }

  function cancelTimer() {
    if (timer !== null) { cancel(timer); timer = null; }
  }

  return {
    /** Follow the controller's movement state. Ignored while a one-shot owns the actor. */
    update(movementState) {
      locomotion = movementState || 'idle';
      if (disposed || oneShot) return Promise.resolve(false);
      return request(locomotionClip(locomotion));
    },
    /** Play `clip` once, then return to the current locomotion clip. */
    playOnce(clip, seconds = PLAYER_CLIP_DURATIONS[clip] ?? 0.8) {
      if (disposed || !clip) return Promise.resolve(false);
      cancelTimer();
      oneShot = clip;
      const played = request(clip);
      timer = schedule(() => {
        timer = null;
        if (disposed) return;
        oneShot = null;
        request(locomotionClip(locomotion));
      }, Math.max(0, Math.round(seconds * 1000)));
      return played;
    },
    /** Clip currently requested, for tests and diagnostics. */
    currentClip: () => desired,
    /** Whether a one-shot is still running. */
    isOneShot: () => oneShot !== null,
    /** How many round-trips left the page, and what the engine said to the last one. */
    diagnostics: () => ({ submits, lastResult }),
    /** Stop the timers and ignore further updates. Idempotent. */
    dispose() {
      const was = !disposed;
      disposed = true;
      cancelTimer();
      return was;
    },
  };
}
