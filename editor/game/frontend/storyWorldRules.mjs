/**
 * World rules a small world can be given by installing a fragment at the prophet.
 *
 * `game/data/fragments.json` is the authoritative catalogue; this module is its runtime
 * projection plus the per-frame engine that animates a world while a rule is installed.
 *
 * A world may carry several rules at once, and one object may be affected by all of them:
 * a `sway` rule moves it, a `spin` rule turns it. The engine therefore composes every
 * installed rule into EXACTLY ONE `actorTransform` write per actor per frame (operation 0
 * carries the composed position, operation 1 the composed rotation, scale is never
 * written), so two rules can never fight over the same actor.
 *
 * The engine deliberately runs its OWN animation frame loop instead of joining the
 * player controller's: `playerController` stops scheduling whenever the player stands
 * still (`hasWork()` is jump/dodge/keys only), which would freeze the sway, and it is a
 * file three people already share. This loop exists only while a rule is installed.
 *
 * Writing a transform every frame must stay synchronous: `api.scene.setActorTransform`
 * is an asynchronous CEF round-trip and would also persist the whole scene to disk. The
 * engine therefore uses the same fire-and-forget `coronaBridge.actorTransform` call the
 * player controller uses, and the persisted scene keeps the un-swayed base position and
 * the un-spun base rotation.
 */

import { sceneSnapshot } from './storyCharacters.mjs';
import { isMapActor } from './storyMap.mjs';

export const WORLD_RULE_VERSION = 1;

/**
 * The rotation a rule's `amplitude` is measured in: the catalogue and the saved record
 * store degrees (the number the fragment panel shows and steps), the bridge takes radians.
 * One conversion factor, used in exactly one place.
 */
export const DEGREES_TO_RADIANS = Math.PI / 180;

/**
 * Mirrors the `fragments` array of game/data/fragments.json. `initial: true` on both
 * fragments is a TEMPORARY content decision: there is no reward/ownership path that could
 * grant a fragment later (`ownedFragmentIds` below), so making `spin` initial is the only
 * way to install it today. Revisit this the moment a drop/inventory path can grant it.
 */
export const FRAGMENTS = Object.freeze([
  Object.freeze({
    id: 'sway',
    name: '浮动碎片',
    initial: true,
    description: '让小世界里的物体持续左右小范围浮动。',
    rule: Object.freeze({
      type: 'sway', axis: 'x', amplitude: 0.25, periodMs: 2400,
      excludeRoles: Object.freeze(['player']),
    }),
  }),
  Object.freeze({
    id: 'spin',
    name: '旋转碎片',
    initial: true,
    description: '让小世界里的物体持续绕 Z 轴左右小角度旋转。',
    rule: Object.freeze({
      // ±15° at 3.6 s: a peak angular speed (≈0.00046 rad/s, via DEGREES_TO_RADIANS) in the
      // same league as the sway's 0.25 m / 2.4 s (≈0.00065 m/s), so both rules read as one
      // motion when they are installed together, and 15° stays visible without looking
      // broken. amplitude is DEGREES (convert before the bridge); axis is z.
      type: 'spin', axis: 'z', amplitude: 15, periodMs: 3600,
      excludeRoles: Object.freeze(['player']),
    }),
  }),
]);

const COORD_LIMIT = 1e6;
const finiteNumber = value => typeof value === 'number' && Number.isFinite(value);
const positiveNumber = value => finiteNumber(value) && value > 0 && value <= COORD_LIMIT;
const AXES = Object.freeze(['x', 'y', 'z']);
const RULE_TYPES = Object.freeze(['sway', 'spin']);
/**
 * Which axes each type may use, mirrored from `_TYPE_AXES` in game/runtime/world_rules.py
 * (line 27): sway moves along any axis, spin only turns around z. A type/axis pair the
 * backend rejects must be rejected here too, before a frame loop can ever see it.
 */
const RULE_AXES = Object.freeze({ sway: AXES, spin: Object.freeze(['z']) });

export const fragmentIds = () => FRAGMENTS.map(fragment => fragment.id);
export const initialFragmentIds = () => FRAGMENTS.filter(fragment => fragment.initial).map(f => f.id);

export function findFragment(id) {
  return FRAGMENTS.find(fragment => fragment.id === id) || null;
}

/**
 * Which fragments the player owns. The initial ones are granted from the start, which is
 * what the requirement asks for. Wiring the main world's `inventory.worldFragment` drop
 * counter in here is the intended extension point once the item table defines which
 * fragment a drop maps to.
 */
export function ownedFragmentIds() {
  return initialFragmentIds().filter(id => Boolean(findFragment(id)));
}

/** Normalize a rule so a bad catalogue edit cannot reach the frame loop. */
export function validateRule(raw) {
  const source = raw && typeof raw === 'object' ? raw : {};
  if (!RULE_TYPES.includes(source.type)) throw new Error(`未知的世界规则类型：${String(source.type)}`);
  if (!AXES.includes(source.axis)) throw new Error(`未知的浮动轴：${String(source.axis)}`);
  // Mirrors the type/axis pairing in game/runtime/world_rules.py:51-52 (`_TYPE_AXES`, line 27).
  if (!RULE_AXES[source.type].includes(source.axis)) {
    throw new Error(`该世界规则不支持这条浮动轴：${String(source.axis)}`);
  }
  if (!positiveNumber(source.amplitude)) throw new Error('世界规则幅度无效');
  if (!positiveNumber(source.periodMs)) throw new Error('世界规则周期无效');
  const excludeRoles = (Array.isArray(source.excludeRoles) ? source.excludeRoles : [])
    .filter(role => typeof role === 'string' && role.trim()).map(role => role.trim());
  return Object.freeze({ type: source.type, axis: source.axis, amplitude: source.amplitude,
    periodMs: source.periodMs, excludeRoles: Object.freeze([...new Set(excludeRoles)]) });
}

// Fail fast on a bad catalogue rather than deep inside a frame callback.
for (const fragment of FRAGMENTS) validateRule(fragment.rule);

/** Pure: the three-vector one axis of `rule` carries `value`. */
function alongAxis(axis, value) {
  if (axis === 'x') return [value, 0, 0];
  if (axis === 'y') return [0, value, 0];
  return [0, 0, value];
}

/**
 * Pure: the POSITION offset a single rule contributes after `elapsedMs` of running.
 * Only `sway` moves an object; a `spin` rule contributes nothing to the position, which is
 * what keeps its rotation offset separate from the sway's (see `ruleRotationOffset`).
 */
export function ruleOffset(rawRule, elapsedMs) {
  const rule = validateRule(rawRule);
  if (!finiteNumber(elapsedMs) || elapsedMs < 0) throw new Error('世界规则运行时长无效');
  if (rule.type !== 'sway') return [0, 0, 0];
  const value = rule.amplitude * Math.sin((2 * Math.PI * elapsedMs) / rule.periodMs);
  return alongAxis(rule.axis, value);
}

/**
 * Pure: the ROTATION offset (radians, around the rule's axis) a single rule contributes
 * after `elapsedMs` of running, on top of the object's base rotation. Same clock and same
 * sine phase as `ruleOffset`, so two rules installed together stay in step.
 */
export function ruleRotationOffset(rawRule, elapsedMs) {
  const rule = validateRule(rawRule);
  if (!finiteNumber(elapsedMs) || elapsedMs < 0) throw new Error('世界规则运行时长无效');
  if (rule.type !== 'spin') return [0, 0, 0];
  const degrees = rule.amplitude * Math.sin((2 * Math.PI * elapsedMs) / rule.periodMs);
  return alongAxis(rule.axis, degrees * DEGREES_TO_RADIANS);
}

/** Pure: the combined offset of every installed sway rule. */
export function totalOffset(rules, elapsedMs) {
  const list = Array.isArray(rules) ? rules : [];
  const total = [0, 0, 0];
  for (const rule of list) {
    const offset = ruleOffset(rule, elapsedMs);
    total[0] += offset[0]; total[1] += offset[1]; total[2] += offset[2];
  }
  return total;
}

/** Pure: the combined rotation offset of every installed spin rule. */
export function totalRotationOffset(rules, elapsedMs) {
  const list = Array.isArray(rules) ? rules : [];
  const total = [0, 0, 0];
  for (const rule of list) {
    const offset = ruleRotationOffset(rule, elapsedMs);
    total[0] += offset[0]; total[1] += offset[1]; total[2] += offset[2];
  }
  return total;
}

const SAVED_ZERO = Object.freeze([0, 0, 0]);
const handleOf = actor => Number(actor?.handle);
const vectorOf = value => (Array.isArray(value) && value.length === 3 && value.every(finiteNumber)
  ? [...value] : null);
const positionOf = actor => vectorOf(actor?.geometry?.position);

/**
 * The roles no installed rule may animate. Every catalogue fragment is asked, not just the
 * first one: the target list is shared by every rule, so an actor excluded by ANY installed
 * rule must not enter it (otherwise the other rule would animate someone it must not).
 */
function excludedRoles(rawRule) {
  if (rawRule !== undefined && rawRule !== null) return validateRule(rawRule).excludeRoles;
  const roles = new Set();
  for (const fragment of FRAGMENTS) {
    for (const role of validateRule(fragment.rule).excludeRoles) roles.add(role);
  }
  return [...roles];
}

/**
 * The actors the rules animate, each with the base position the sway is measured from and
 * the base rotation the spin is measured from. Captured once from a snapshot: reading the
 * scene is an asynchronous CEF round-trip and must never happen inside the frame loop.
 * Called with no rule it returns the shared list for a world with several rules installed.
 *
 * The map is never a target: 地图不算物体, it is the surface everything else stands on, and its
 * transform has exactly one author (`storyMap.ensureStoryMap`). A rule that swayed it would
 * write a position and a rotation every frame and undo — or fight — that owner. The exclusion
 * is by identity (`isMapActor`), so it holds for a rule the player installed before this guard
 * existed and for a hand-edited rule record alike: a rule never names its targets, so the only
 * place a map-targeted rule can be refused is here, where the targets are resolved.
 */
export function collectSwayTargets(snapshot, rawRule = null) {
  const excluded = excludedRoles(rawRule);
  const scene = sceneSnapshot(snapshot);
  const actors = Array.isArray(scene?.actors) ? scene.actors : [];
  const targets = [];
  for (const actor of actors) {
    const role = actor?.semantic_role || actor?.runtime?.semantic_role || '';
    if (excluded.includes(role)) continue;
    if (actor?.visible === false) continue;
    if (isMapActor(actor)) continue;
    const handle = handleOf(actor);
    const base = positionOf(actor);
    if (!Number.isInteger(handle) || handle <= 0 || !base) continue;
    // A missing rotation is not fatal: yaw 0 is a usable base to spin around, and every
    // actor the engine places has one. A missing position is fatal, as before.
    const baseRotation = vectorOf(actor?.geometry?.rotation) || [...SAVED_ZERO];
    // The identity travels with the handle so the runner can re-ask `isMapActor` on its own
    // ingest path and refuse a map that a stale or hand-built target list smuggled in.
    targets.push({ guid: actor.actor_guid, role, handle, base, baseRotation,
      modelRef: actor?.model_ref || actor?.runtime?.model_ref,
      entityId: actor?.entity_id || actor?.runtime?.entity_id,
      entityType: actor?.entity_type || actor?.runtime?.entity_type });
  }
  return targets;
}

/** One rule target as `isMapActor` reads it: the same identity fields, under target names. */
const mapIdentityOf = target => ({ actor_guid: target?.guid, model_ref: target?.modelRef,
  entity_id: target?.entityId, entity_type: target?.entityType });

/** A tab that was hidden for a while must not make objects jump. */
const MAX_DELTA_MS = 100;

/**
 * The per-frame engine. Injectable `requestFrame`/`cancelFrame`/`bridge` keep it testable
 * without an engine; in the renderer they default to the real globals.
 */
export function createWorldRuleRunner({ bridge = null, rules = [], targets = [],
  requestFrame = null, cancelFrame = null, onError = () => {} } = {}) {
  const frameRequest = requestFrame || (typeof requestAnimationFrame === 'function'
    ? callback => requestAnimationFrame(callback) : null);
  const frameCancel = cancelFrame || (typeof cancelAnimationFrame === 'function'
    ? id => cancelAnimationFrame(id) : null);
  const getBridge = typeof bridge === 'function' ? bridge : () => bridge;

  let installedRules = [...rules];
  let frame = null;
  let lastTimestamp = null;
  let elapsedMs = 0;
  let running = false;

  const ZERO = Object.freeze([0, 0, 0]);
  const savedRotation = target => (Array.isArray(target.baseRotation)
    ? target.baseRotation : ZERO);
  const savedPosition = target => target.base;

  /**
   * The animated set, with the map refused — the one place a target list is accepted.
   *
   * Dropping it here as well as in `collectSwayTargets` is what makes a map-targeted rule
   * harmless rather than merely unlikely: whatever a caller hands in — the initial list, an
   * old rule that outlived this build, a list captured before the map existed, a hand-edited
   * record — the map never enters `currentTargets`, so not one frame, not `refreshBase` and
   * not `restore` can write its transform (which `storyMap.ensureStoryMap` alone owns).
   */
  const animateTargets = list => (Array.isArray(list)
    ? list.filter(target => !isMapActor(mapIdentityOf(target))).map(target => ({
      ...target, base: [...target.base],
      baseRotation: Array.isArray(target.baseRotation) ? [...target.baseRotation] : [...ZERO],
    })) : []);

  let currentTargets = animateTargets(targets);

  function writeBase(target) {
    const bridge = getBridge();
    if (bridge?.actorTransform?.(target.handle, 0, [...savedPosition(target)]) !== true
      || bridge?.actorTransform?.(target.handle, 1, [...savedRotation(target)]) !== true) {
      throw new Error('世界规则需要同步的 actorTransform 接口');
    }
  }

  /** Replace the animated set (after entering a world, installing, or moving a prop). */
  function setTargets(next) {
    currentTargets = animateTargets(next);
  }

  /**
   * Re-anchor one object after the player moved it by hand: the base is what gets
   * persisted, so the sway/spin continues around the new position and rotation instead of
   * snapping back. `rotation` is optional and only replaced when it is a usable vector.
   */
  function refreshBase(guid, position, rotation = null) {
    const target = currentTargets.find(entry => entry.guid === guid);
    if (!target || !vectorOf(position)) return false;
    target.base = [...position];
    if (vectorOf(rotation)) target.baseRotation = [...rotation];
    writeBase(target);
    return true;
  }

  /**
   * One frame of every installed rule, composed: the sway contributes the position offset,
   * the spin contributes the rotation offset, and each actor receives exactly ONE position
   * write plus ONE rotation write — never one write per rule, so two rules can never fight
   * over the same actor or lose the base the scene persisted.
   */
  function apply(positionOffset, rotationOffset) {
    const bridge = getBridge();
    for (const target of currentTargets) {
      const base = savedPosition(target), rotation = savedRotation(target);
      const position = [base[0] + positionOffset[0], base[1] + positionOffset[1],
        base[2] + positionOffset[2]];
      if (bridge?.actorTransform?.(target.handle, 0, position) !== true) {
        throw new Error('世界规则需要同步的 actorTransform 接口');
      }
      const spun = [rotation[0] + rotationOffset[0], rotation[1] + rotationOffset[1],
        rotation[2] + rotationOffset[2]];
      if (bridge?.actorTransform?.(target.handle, 1, spun) !== true) {
        throw new Error('世界规则需要同步的 actorTransform 接口');
      }
    }
  }

  function tick(timestamp) {
    frame = null;
    if (!running) return;
    if (!Number.isFinite(timestamp)) { stop(); return; }
    if (lastTimestamp === null) lastTimestamp = timestamp;
    elapsedMs += Math.min(Math.max(timestamp - lastTimestamp, 0), MAX_DELTA_MS);
    lastTimestamp = timestamp;
    try {
      // Both offsets are computed once per frame and handed to every actor together.
      apply(totalOffset(installedRules, elapsedMs),
        totalRotationOffset(installedRules, elapsedMs));
    } catch (error) {
      // The bridge is gone, so restoring through it would fail too: stop quietly and
      // report once. The persisted scene still holds the un-swayed base transform.
      halt();
      onError(error);
      return;
    }
    schedule();
  }

  function schedule() {
    if (!running || frame !== null || !frameRequest) return;
    frame = frameRequest(tick);
  }

  function setRules(next) {
    installedRules = Array.isArray(next) ? next.map(rule => ({ ...validateRule(rule) })) : [];
    elapsedMs = 0;
    lastTimestamp = null;
  }

  /** Leave every object exactly on its persisted base position and rotation. */
  function restore() {
    for (const target of currentTargets) {
      try { writeBase(target); } catch (error) { onError(error); return; }
    }
  }

  /** Stop the frame loop without touching the scene: used when the bridge itself is dead. */
  function halt() {
    running = false;
    if (frame !== null && frameCancel) frameCancel(frame);
    frame = null;
    lastTimestamp = null;
    elapsedMs = 0;
  }

  function start() {
    if (running) return true;  // idempotent: one loop, however often it is asked for
    if (!frameRequest || !currentTargets.length || !installedRules.length) return false;
    running = true;
    lastTimestamp = null;
    schedule();
    return true;
  }

  function stop() {
    if (!running) return false;
    halt();
    restore();
    return true;
  }

  return {
    setTargets, setRules, refreshBase, start, stop, restore,
    isRunning: () => running,
    targetCount: () => currentTargets.length,
    ruleCount: () => installedRules.length,
    dispose() { const wasRunning = running; halt(); return wasRunning; },
  };
}
