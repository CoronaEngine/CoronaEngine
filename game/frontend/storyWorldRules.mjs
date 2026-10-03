/**
 * World rules a small world can be given by installing a fragment at the prophet.
 *
 * `game/data/fragments.json` is the authoritative catalogue; this module is its runtime
 * projection plus the per-frame engine that animates a world while a rule is installed.
 *
 * The engine deliberately runs its OWN animation frame loop instead of joining the
 * player controller's: `playerController` stops scheduling whenever the player stands
 * still (`hasWork()` is jump/dodge/keys only), which would freeze the sway, and it is a
 * file three people already share. This loop exists only while a rule is installed.
 *
 * Writing a transform every frame must stay synchronous: `api.scene.setActorTransform`
 * is an asynchronous CEF round-trip and would also persist the whole scene to disk. The
 * engine therefore uses the same fire-and-forget `coronaBridge.actorTransform` call the
 * player controller uses, and the persisted scene keeps the un-swayed base position.
 */

import { sceneSnapshot } from './storyCharacters.mjs';

export const WORLD_RULE_VERSION = 1;

/** Mirrors the `fragments` array of game/data/fragments.json. */
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
]);

const COORD_LIMIT = 1e6;
const finiteNumber = value => typeof value === 'number' && Number.isFinite(value);
const positiveNumber = value => finiteNumber(value) && value > 0 && value <= COORD_LIMIT;
const AXES = Object.freeze(['x', 'y', 'z']);
const RULE_TYPES = Object.freeze(['sway']);

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
  if (!positiveNumber(source.amplitude)) throw new Error('世界规则幅度无效');
  if (!positiveNumber(source.periodMs)) throw new Error('世界规则周期无效');
  const excludeRoles = (Array.isArray(source.excludeRoles) ? source.excludeRoles : [])
    .filter(role => typeof role === 'string' && role.trim()).map(role => role.trim());
  return Object.freeze({ type: source.type, axis: source.axis, amplitude: source.amplitude,
    periodMs: source.periodMs, excludeRoles: Object.freeze([...new Set(excludeRoles)]) });
}

// Fail fast on a bad catalogue rather than deep inside a frame callback.
for (const fragment of FRAGMENTS) validateRule(fragment.rule);

/** Pure: the offset a single rule contributes after `elapsedMs` of running. */
export function ruleOffset(rawRule, elapsedMs) {
  const rule = validateRule(rawRule);
  if (!finiteNumber(elapsedMs) || elapsedMs < 0) throw new Error('世界规则运行时长无效');
  const value = rule.amplitude * Math.sin((2 * Math.PI * elapsedMs) / rule.periodMs);
  if (rule.axis === 'x') return [value, 0, 0];
  return rule.axis === 'y' ? [0, value, 0] : [0, 0, value];
}

/** Pure: the combined offset of every installed rule. */
export function totalOffset(rules, elapsedMs) {
  const list = Array.isArray(rules) ? rules : [];
  const total = [0, 0, 0];
  for (const rule of list) {
    const offset = ruleOffset(rule, elapsedMs);
    total[0] += offset[0]; total[1] += offset[1]; total[2] += offset[2];
  }
  return total;
}

const handleOf = actor => Number(actor?.handle);
const positionOf = actor => {
  const position = actor?.geometry?.position;
  return Array.isArray(position) && position.length === 3 && position.every(finiteNumber)
    ? [...position] : null;
};

/**
 * The actors a rule animates, each with the base position the sway is measured from.
 * Captured once from a snapshot: reading the scene is an asynchronous CEF round-trip and
 * must never happen inside the frame loop.
 */
export function collectSwayTargets(snapshot, rawRule) {
  const rule = validateRule(rawRule);
  const scene = sceneSnapshot(snapshot);
  const actors = Array.isArray(scene?.actors) ? scene.actors : [];
  const targets = [];
  for (const actor of actors) {
    const role = actor?.semantic_role || actor?.runtime?.semantic_role || '';
    if (rule.excludeRoles.includes(role)) continue;
    if (actor?.visible === false) continue;
    const handle = handleOf(actor);
    const base = positionOf(actor);
    if (!Number.isInteger(handle) || handle <= 0 || !base) continue;
    targets.push({ guid: actor.actor_guid, role, handle, base });
  }
  return targets;
}

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

  let currentTargets = [...targets];
  let installedRules = [...rules];
  let frame = null;
  let lastTimestamp = null;
  let elapsedMs = 0;
  let running = false;

  function writeBase(target) {
    if (getBridge()?.actorTransform?.(target.handle, 0, [...target.base]) !== true) {
      throw new Error('世界规则需要同步的 actorTransform 接口');
    }
  }

  /** Replace the animated set (after entering a world, installing, or moving a prop). */
  function setTargets(next) {
    currentTargets = Array.isArray(next) ? next.map(target => ({ ...target, base: [...target.base] })) : [];
  }

  /**
   * Re-anchor one object after the player moved it by hand: the base is what gets
   * persisted, so the sway continues around the new position instead of snapping back.
   */
  function refreshBase(guid, position) {
    const target = currentTargets.find(entry => entry.guid === guid);
    if (!target || !Array.isArray(position) || !position.every(finiteNumber)) return false;
    target.base = [...position];
    writeBase(target);
    return true;
  }

  function apply(offset) {
    for (const target of currentTargets) {
      const position = [target.base[0] + offset[0], target.base[1] + offset[1], target.base[2] + offset[2]];
      if (getBridge()?.actorTransform?.(target.handle, 0, position) !== true) {
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
      apply(totalOffset(installedRules, elapsedMs));
    } catch (error) {
      // The bridge is gone, so restoring through it would fail too: stop quietly and
      // report once. The persisted scene still holds the un-swayed base position.
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

  /** Leave every object exactly on its persisted base position. */
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
