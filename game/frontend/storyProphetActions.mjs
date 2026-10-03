/**
 * What the prophet offers once the conversation is open.
 *
 * The panel is two levels: a chooser with two screens («调整物体» and «碎片»), and the
 * screen itself. Everything here is pure data and pure functions, so both screens can be
 * tested without an engine; the page only renders what these return.
 *
 * The linear script from R1 still owns Enter/Space, and Escape still closes the panel,
 * so installing this menu changes no existing behaviour.
 */

import { CUBE_PLACEMENT, cubeMoveTransform, cubeScaleTransform, cubeTransformOf } from './storyCube.mjs';
import { sceneSnapshot } from './storyCharacters.mjs';
import { FRAGMENTS, findFragment, ownedFragmentIds } from './storyWorldRules.mjs';

/** The two main options. Keys are live in the chooser only. */
export const PROPHET_SCREENS = Object.freeze([
  Object.freeze({ id: 'adjust', key: '1', label: '调整物体', detail: '进入俯瞰视角，点击物体后调整大小或位置' }),
  Object.freeze({ id: 'fragments', key: '2', label: '碎片', detail: '查看碎片的装填状态与详情，装填或拆下' }),
]);

/** Adjustment steps, live in the «调整物体» screen. They act on the selected object. */
export const ADJUST_ACTIONS = Object.freeze([
  Object.freeze({ id: 'scale-up', key: '1', group: '缩放物体', label: '放大物体' }),
  Object.freeze({ id: 'scale-down', key: '2', group: '缩放物体', label: '缩小物体' }),
  Object.freeze({ id: 'move-forward', key: '3', group: '调整位置', label: '物体前移' }),
  Object.freeze({ id: 'move-back', key: '4', group: '调整位置', label: '物体后移' }),
  Object.freeze({ id: 'move-left', key: '5', group: '调整位置', label: '物体左移' }),
  Object.freeze({ id: 'move-right', key: '6', group: '调整位置', label: '物体右移' }),
]);

const MOVE_DIRECTIONS = Object.freeze({
  'move-forward': 'forward', 'move-back': 'back', 'move-left': 'left', 'move-right': 'right',
});

/** The prophet is the quest giver and the world's own anchors are not props: neither moves. */
const UNEDITABLE_ROLES = Object.freeze(['player', 'prophet']);

/**
 * The objects the adjust screen offers as a click-to-select list.
 *
 * Listing names sidesteps viewport ray picking entirely, which is why it exists: picking a
 * label always works, whereas picking a pixel depends on the engine's readback. The scene
 * passed in is the small world's own scene, so everything listed is inside it; the player
 * and the prophet are excluded because they are not objects the player may rearrange.
 */
export function selectableObjects(snapshot) {
  const actors = sceneSnapshot(snapshot)?.actors;
  const list = [];
  for (const actor of Array.isArray(actors) ? actors : []) {
    const role = actor?.semantic_role || actor?.runtime?.semantic_role || '';
    if (UNEDITABLE_ROLES.includes(role)) continue;
    const handle = Number(actor?.handle);
    const transform = cubeTransformOf(actor);
    if (!Number.isInteger(handle) || handle <= 0 || !transform) continue;
    list.push({ guid: actor.actor_guid, name: actor.name || actor.actor_guid,
      role: role || actor.actor_type || '物体', transform });
  }
  return list.sort((a, b) => String(a.name).localeCompare(String(b.name), 'zh'));
}

/** A one-line label for the list, so a long list stays readable. */
export function objectLabel(entry) {
  return `${entry?.name || '未命名'}（${entry?.role || '物体'}）`;
}

export const findScreen = id => PROPHET_SCREENS.find(screen => screen.id === id) || null;
export const findAdjustAction = id => ADJUST_ACTIONS.find(action => action.id === id) || null;
export const isAdjustAction = id => Boolean(findAdjustAction(id));

const keyedBy = (list, key) => (typeof key === 'string' && /^[1-9]$/.test(key)
  ? list.find(entry => entry.key === key) || null : null);
export const screenForKey = key => keyedBy(PROPHET_SCREENS, key);
export const adjustActionForKey = key => keyedBy(ADJUST_ACTIONS, key);

/**
 * The transform an adjustment produces, or null when the action is not an adjustment.
 * `current` is the selected object's live transform; `reference` is the prophet, which
 * bounds how far the object may be moved; `facingYaw` makes 前/后/左/右 match what the
 * player sees.
 */
export function nextObjectTransform(actionId, { current, reference, facingYaw, placement = CUBE_PLACEMENT }) {
  if (actionId === 'scale-up') return cubeScaleTransform(current, 1, placement);
  if (actionId === 'scale-down') return cubeScaleTransform(current, -1, placement);
  const direction = MOVE_DIRECTIONS[actionId];
  if (!direction) return null;
  return cubeMoveTransform(current, direction, reference, facingYaw, placement);
}

const fixed2 = value => Number(value).toFixed(2);

export function adjustFeedback(actionId, transform, reference, placement = CUBE_PLACEMENT) {
  if (!transform) return '';
  if (actionId === 'scale-up' || actionId === 'scale-down') {
    return `物体已${actionId === 'scale-up' ? '放大' : '缩小'}到 ${fixed2(transform.scale[0])}`;
  }
  const label = findAdjustAction(actionId)?.label || '物体';
  const referencePosition = reference?.geometry?.position;
  if (!Array.isArray(referencePosition)) return `${label}完成`;
  const distance = Math.hypot(transform.position[0] - referencePosition[0],
    transform.position[2] - referencePosition[2]);
  if (distance >= placement.moveRange - 1e-6) {
    return `${label}完成（已到离先知 ${fixed2(placement.moveRange)} 米的上限）`;
  }
  return `${label}完成（距先知 ${fixed2(distance)} 米）`;
}

const RULE_STATE = Object.freeze({
  installed: Object.freeze({ id: 'installed', label: '已装填' }),
  owned: Object.freeze({ id: 'owned', label: '可装填' }),
  locked: Object.freeze({ id: 'locked', label: '未获得' }),
});

/**
 * The «碎片» screen's list: every catalogued fragment with its ownership, its装填 state and
 * the human-readable details of the rule it carries.
 */
export function fragmentPanel(installedRules = []) {
  const installed = new Set((Array.isArray(installedRules) ? installedRules : [])
    .map(rule => rule?.fragmentId).filter(Boolean));
  const owned = new Set(ownedFragmentIds());
  return FRAGMENTS.map(fragment => {
    const state = installed.has(fragment.id) ? RULE_STATE.installed
      : owned.has(fragment.id) ? RULE_STATE.owned : RULE_STATE.locked;
    return {
      id: fragment.id,
      name: fragment.name,
      description: fragment.description,
      initial: Boolean(fragment.initial),
      state: state.id,
      stateLabel: state.label,
      installed: state.id === 'installed',
      owned: state.id !== 'locked',
      details: ruleDetails(fragment.rule),
    };
  });
}

/** Plain-language details of a rule, for the fragment list. */
export function ruleDetails(rule) {
  const axis = { x: '左右', y: '上下', z: '前后' }[rule?.axis] || '未知方向';
  const seconds = Number(rule?.periodMs) / 1000;
  const excludes = Array.isArray(rule?.excludeRoles) && rule.excludeRoles.length
    ? `（${rule.excludeRoles.join('、')}不浮动）` : '';
  return `${axis}浮动 ${fixed2(rule?.amplitude)} 米，周期 ${fixed2(seconds)} 秒${excludes}`;
}

/** Number keys select a fragment in the «碎片» screen. */
export function fragmentForKey(key, installedRules = []) {
  if (typeof key !== 'string' || !/^[1-9]$/.test(key)) return null;
  return fragmentPanel(installedRules)[Number(key) - 1] || null;
}

/**
 * Resolve a number key against the screen the panel is currently on, so the page only has
 * to dispatch: with no screen open the keys open one, inside a screen they act on it.
 */
export function prophetKeyAction(key, screen = null) {
  if (screen === 'adjust') {
    const action = adjustActionForKey(key);
    return action ? { type: 'adjust', id: action.id } : null;
  }
  if (screen === 'fragments') {
    const entry = fragmentForKey(key);
    return entry ? { type: 'toggle-fragment', fragmentId: entry.id } : null;
  }
  const target = screenForKey(key);
  return target ? { type: 'open-screen', screen: target.id } : null;
}

/**
 * 装填 or 拆下. Returns the rule records to persist plus what happened, so the caller only
 * has to save them; the semantics of a rule always come from the catalogue.
 */
export function toggleFragment(installedRules = [], fragmentId) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const catalogue = findFragment(fragmentId);
  if (!catalogue) throw new Error(`未知碎片：${String(fragmentId)}`);
  const present = records.some(record => record?.fragmentId === fragmentId);
  if (present) {
    return { action: 'removed', fragmentId,
      rules: records.filter(record => record?.fragmentId !== fragmentId),
      feedback: `已拆下：${catalogue.name}` };
  }
  if (!ownedFragmentIds().includes(fragmentId)) throw new Error(`尚未获得碎片：${catalogue.name}`);
  const rule = catalogue.rule;
  return { action: 'installed', fragmentId,
    rules: [...records, { fragmentId, type: rule.type, axis: rule.axis,
      amplitude: rule.amplitude, periodMs: rule.periodMs }],
    feedback: `已装填：${catalogue.name}` };
}
