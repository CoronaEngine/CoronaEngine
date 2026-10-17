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
import { isMapActor } from './storyMap.mjs';
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

/**
 * 碎片的规则可以现场改数字：幅度与周期各两个方向，共四步。与 ADJUST_ACTIONS 同一种
 * 形状（{id, key, group, label}），按键在「调整碎片」子屏里有效；每一步落在哪个字段、
 * 往哪个方向走，由下面的 FRAGMENT_STEPS 决定，免得两处漂移。
 */
export const FRAGMENT_ADJUST_ACTIONS = Object.freeze([
  Object.freeze({ id: 'amplitude-up', key: '1', group: '幅度', label: '幅度增大' }),
  Object.freeze({ id: 'amplitude-down', key: '2', group: '幅度', label: '幅度减小' }),
  Object.freeze({ id: 'period-up', key: '3', group: '周期', label: '周期加长' }),
  Object.freeze({ id: 'period-down', key: '4', group: '周期', label: '周期缩短' }),
]);

const FRAGMENT_STEPS = Object.freeze({
  'amplitude-up': Object.freeze({ field: 'amplitude', direction: 1 }),
  'amplitude-down': Object.freeze({ field: 'amplitude', direction: -1 }),
  'period-up': Object.freeze({ field: 'periodMs', direction: 1 }),
  'period-down': Object.freeze({ field: 'periodMs', direction: -1 }),
});

/**
 * 镜像 game/runtime/world_rules.py 的校验，逐字对齐两处上限：
 *   - `_AMPLITUDE_LIMIT = 100.0`（world_rules.py:24）
 *   - `_PERIOD_LIMIT_MS = 600_000`（world_rules.py:25）
 * 而 `_positive()`（world_rules.py:34-36）要求 `0 < 值 <= 上限`，上限本身合法、0 不合法，
 * 所以这里的最小值必须严格大于 0。规则是：无论玩家怎么点，`adjustFragmentRule` 产出的记录
 * 都落在这些区间内，`validate_rule` / `validate_state` 不会拒绝它。
 *
 * 幅度分两种规则、两套单位，各自一套上下限与步长：
 *   - sway（world_rules.py:20 的 `_TYPES`、第 27 行 `_TYPE_AXES`）：位移，米，步长 0.05；
 *   - spin：绕 Z 轴的旋转，**度**，步长 5，上下限仍是同一份 `_AMPLITUDE_LIMIT`（100 度）；
 * 周期两边共用，恒以毫秒计、以秒显示。
 */
export const FRAGMENT_RULE_BOUNDS = Object.freeze({
  sway: Object.freeze({
    amplitude: Object.freeze({ min: 0.01, max: 100.0, step: 0.05, decimals: 2, unit: '米', scale: 1 }),
    periodMs: Object.freeze({ min: 100, max: 600_000, step: 200, decimals: 0, unit: '秒', scale: 1 / 1000 }),
  }),
  spin: Object.freeze({
    // 5 度一步：从默认 15 度按三下到下限、十几下到上限，既看得出变化也不会点得手酸。
    amplitude: Object.freeze({ min: 1, max: 100.0, step: 5, decimals: 0, unit: '度', scale: 1 }),
    periodMs: Object.freeze({ min: 100, max: 600_000, step: 200, decimals: 0, unit: '秒', scale: 1 / 1000 }),
  }),
});

/** 目录里那条规则的类型；未知碎片或未知类型时退回 sway，与旧行为一致。 */
const ruleTypeOf = fragment => (fragment?.rule?.type === 'spin' ? 'spin' : 'sway');

/** 某块碎片可调的字段及其上下限，供调整与子屏显示共用。 */
function fragmentRuleBounds(fragment) {
  return FRAGMENT_RULE_BOUNDS[ruleTypeOf(fragment)];
}

/** 每个可调字段的中文名，反馈行与子屏都用它。 */
const FRAGMENT_FIELDS = Object.freeze({
  amplitude: Object.freeze({ field: 'amplitude', label: '幅度' }),
  periodMs: Object.freeze({ field: 'periodMs', label: '周期' }),
});

const MOVE_DIRECTIONS = Object.freeze({
  'move-forward': 'forward', 'move-back': 'back', 'move-left': 'left', 'move-right': 'right',
});

/**
 * Characters are people, not scenery, so none of them may be rearranged: the player, the
 * prophet, the boss and the merchant. Matching on the entity type as well as the role means
 * a character added later is excluded without having to remember to list it here.
 */
const CHARACTER_ENTITY_TYPE = 'story_character';
const CHARACTER_ROLES = Object.freeze(['player', 'prophet', 'boss', 'merchant']);
/**
 * World fragments are collectibles the world itself placed, so they belong to the world rather
 * than to the small world and may not be adjusted. They share the `semantic_role` (`placement`),
 * the `entity_type` (`story_prop`) and the entity id prefix (`story.placement.`) with the blocks
 * the player now drops in front of themselves — which *are* small-world objects and must stay
 * adjustable. The one field that tells the two apart is the prop the placement was authored
 * from: `storyActors.syncPlacementActors` writes `entity_id: story.placement.<prop.id>`
 * (`story.placement.world-fragment` vs `story.placement.block`). Matching on that exact id
 * excludes the fragment without also excluding a placed block.
 */
const PLACEMENT_ENTITY_PREFIX = 'story.placement.';
const UNPLACEABLE_PROP_IDS = Object.freeze(['world-fragment']);

/**
 * The objects the adjust screen offers as a click-to-select list: only what lives inside the
 * small world. Listing names sidesteps viewport ray picking entirely, which is why it exists;
 * the player and the other characters are excluded, and so are the world fragments — but not
 * the blocks a player placed. The map is excluded too: 地图不算物体, so nothing in this screen
 * may resize or move it (`isMapActor` is the single owner of that decision, and the world-rule
 * target list asks the same function).
 */
export function selectableObjects(snapshot) {
  const actors = sceneSnapshot(snapshot)?.actors;
  const list = [];
  for (const actor of Array.isArray(actors) ? actors : []) {
    const role = actor?.semantic_role || actor?.runtime?.semantic_role || '';
    const entityType = actor?.entity_type || actor?.runtime?.entity_type || '';
    const entityId = String(actor?.entity_id || actor?.runtime?.entity_id || '');
    if (CHARACTER_ROLES.includes(role) || entityType === CHARACTER_ENTITY_TYPE) continue;
    if (UNPLACEABLE_PROP_IDS.some(id => entityId === `${PLACEMENT_ENTITY_PREFIX}${id}`)) continue;
    // 地图是世界的立足面，不是可调物体：不出现在列表里，就没有任何一条调整动作能写到它。
    if (isMapActor(actor)) continue;
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
export const findFragmentAdjustAction = id =>
  FRAGMENT_ADJUST_ACTIONS.find(action => action.id === id) || null;
export const fragmentAdjustActionForKey = key => keyedBy(FRAGMENT_ADJUST_ACTIONS, key);

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

/** 一条装填记录背后真正的数字：记录里的值优先，读不到才退回目录默认值。 */
const finiteOr = (value, fallback) =>
  (typeof value === 'number' && Number.isFinite(value) ? value : fallback);

function fragmentNumbers(fragment, record) {
  return {
    amplitude: finiteOr(record?.amplitude, fragment.rule.amplitude),
    periodMs: finiteOr(record?.periodMs, fragment.rule.periodMs),
  };
}

/**
 * The «碎片» screen's list: every catalogued fragment with its ownership, its装填 state and
 * the human-readable details of the rule it carries. An installed fragment shows the numbers
 * the player actually saved (the record), not the catalogue defaults — otherwise「调整碎片」
 * would look like it did nothing.
 */
export function fragmentPanel(installedRules = []) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const installed = new Map(records.filter(rule => rule?.fragmentId)
    .map(rule => [rule.fragmentId, rule]));
  const owned = new Set(ownedFragmentIds());
  return FRAGMENTS.map(fragment => {
    const record = installed.get(fragment.id) || null;
    const state = record ? RULE_STATE.installed
      : owned.has(fragment.id) ? RULE_STATE.owned : RULE_STATE.locked;
    const numbers = fragmentNumbers(fragment, record);
    return {
      id: fragment.id,
      name: fragment.name,
      description: fragment.description,
      initial: Boolean(fragment.initial),
      state: state.id,
      stateLabel: state.label,
      installed: state.id === 'installed',
      owned: state.id !== 'locked',
      amplitude: numbers.amplitude,
      periodMs: numbers.periodMs,
      amplitudeUnit: fragmentRuleBounds(fragment).amplitude.unit,
      periodUnit: fragmentRuleBounds(fragment).periodMs.unit,
      details: ruleDetails({ ...fragment.rule, ...numbers }),
    };
  });
}

const AXIS_NAMES = Object.freeze({ x: '左右', y: '上下', z: '前后' });

/**
 * Plain-language details of a rule, for the fragment list. A rotation rule (spin) is
 * described in degrees and 绕轴旋转; a sway stays in metres and 浮动.
 */
export function ruleDetails(rule) {
  const axis = AXIS_NAMES[rule?.axis] || '未知方向';
  const seconds = Number(rule?.periodMs) / 1000;
  const excludes = Array.isArray(rule?.excludeRoles) && rule.excludeRoles.length
    ? `（${rule.excludeRoles.join('、')}${rule?.type === 'spin' ? '不旋转' : '不浮动'}）` : '';
  if (rule?.type === 'spin') {
    return `绕${axis}轴旋转 ±${fixed2(rule?.amplitude)} 度，周期 ${fixed2(seconds)} 秒${excludes}`;
  }
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
 * `fragmentId` is only meaningful on the «调整碎片» sub-screen and is echoed back so the
 * caller does not have to remember which fragment it opened.
 */
export function prophetKeyAction(key, screen = null, fragmentId = null) {
  if (screen === 'adjust') {
    const action = adjustActionForKey(key);
    return action ? { type: 'adjust', id: action.id } : null;
  }
  if (screen === 'fragments') {
    const entry = fragmentForKey(key);
    return entry ? { type: 'toggle-fragment', fragmentId: entry.id } : null;
  }
  if (screen === 'fragment-adjust') {
    const action = fragmentAdjustActionForKey(key);
    return action
      ? { type: 'adjust-fragment', id: action.id, fragmentId: fragmentId || null } : null;
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

/** The installed record of one fragment, or null. Never throws, whatever it is handed. */
export function fragmentRuleOf(installedRules = [], fragmentId) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  return records.find(record => record?.fragmentId === fragmentId) || null;
}

const formatField = (field, value, fragment) => {
  const meta = FRAGMENT_FIELDS[field];
  const bounds = fragmentRuleBounds(fragment)[field];
  if (!Number.isFinite(value)) return `${meta.label} —`;
  return `${meta.label} ${fixed2(value * bounds.scale)} ${bounds.unit}`;
};

/** Integer-safe step: round first so 0.25 + 0.05 never becomes 0.30000000000000004. */
function steppedValue(bounds, current, direction) {
  const stepped = Number((current + direction * bounds.step).toFixed(bounds.decimals));
  return Math.min(Math.max(stepped, bounds.min), bounds.max);
}

/**
 * Change one number of an installed fragment. Returns the whole rule list to persist (the
 * edited record plus every other record in its original order) plus what to tell the player,
 * so the caller only has to save it. Never throws and never returns a rule the backend would
 * reject: both numbers are pushed back inside the mirrored bounds first, and nothing is
 * changed when the action is unknown, the fragment is not installed, or the value already
 * sits on the bound the step walks toward.
 */
export function adjustFragmentRule(installedRules = [], fragmentId, actionId) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const refuse = feedback => ({ changed: false, rules: records, feedback });
  const action = findFragmentAdjustAction(actionId);
  if (!action) return refuse(`未知的调整：${String(actionId)}`);
  const catalogue = findFragment(fragmentId);
  const record = fragmentRuleOf(records, fragmentId);
  if (!record) {
    return refuse(catalogue ? '先装填该碎片再调整' : `未知碎片：${String(fragmentId)}`);
  }
  const step = FRAGMENT_STEPS[action.id];
  const meta = FRAGMENT_FIELDS[step.field];
  // The bounds follow the fragment's rule type: metres for a sway, degrees for a spin.
  const boundsByField = fragmentRuleBounds(catalogue);
  const bounds = boundsByField[step.field];
  const catalogueNumbers = fragmentNumbers(catalogue, null);
  const current = finiteOr(record[step.field], catalogueNumbers[step.field]);
  if (!Number.isFinite(current)) return refuse(`${meta.label}读取不到，暂时无法调整`);
  const next = steppedValue(bounds, current, step.direction);
  if (next === current) {
    const atBound = step.direction > 0 ? current >= bounds.max : current <= bounds.min;
    if (atBound) {
      const edge = step.direction > 0 ? '上限' : '下限';
      const value = (step.direction > 0 ? bounds.max : bounds.min) * bounds.scale;
      return refuse(`${meta.label}已是${edge} ${fixed2(value)} ${bounds.unit}`);
    }
    return refuse(`${meta.label}无法再${step.direction > 0 ? '增大' : '减小'}`);
  }
  // The other number must stay what it was, but is still boxed into the mirrored bounds:
  // a hand-edited record must never be able to smuggle an out-of-range value back in.
  const otherField = step.field === 'amplitude' ? 'periodMs' : 'amplitude';
  const otherBounds = boundsByField[otherField];
  const otherCurrent = finiteOr(record[otherField], catalogueNumbers[otherField]);
  const other = Number.isFinite(otherCurrent)
    ? Math.min(Math.max(otherCurrent, otherBounds.min), otherBounds.max) : otherBounds.min;
  const amplitude = step.field === 'amplitude' ? next : other;
  const periodMs = step.field === 'periodMs' ? next : other;
  const updated = { fragmentId, type: catalogue.rule.type, axis: catalogue.rule.axis,
    amplitude, periodMs };
  const edge = next === bounds.max ? `（已达${meta.label}上限）`
    : next === bounds.min ? `（已达${meta.label}下限）` : '';
  return {
    changed: true,
    rules: records.map(entry => (entry === record ? updated : entry)),
    feedback: `${meta.label}已调到 ${fixed2(next * bounds.scale)} ${bounds.unit}${edge}`,
  };
}

/**
 * What the «调整碎片» sub-screen shows: which fragment, its current numbers, and the four
 * steps. A fragment that is not installed is still describable (catalogue defaults) so the
 * sub-screen can explain itself instead of rendering blank.
 */
export function fragmentAdjustPanel(installedRules = [], fragmentId) {
  const record = fragmentRuleOf(installedRules, fragmentId);
  const catalogue = findFragment(fragmentId);
  if (!catalogue) {
    return { id: fragmentId ?? null, name: String(fragmentId ?? '未知碎片'), description: '',
      known: false, installed: false, amplitude: null, periodMs: null,
      amplitudeLabel: '幅度 —', periodLabel: '周期 —', details: '未知碎片，无法调整',
      hint: '先装填该碎片再调整', actions: FRAGMENT_ADJUST_ACTIONS };
  }
  const numbers = fragmentNumbers(catalogue, record);
  const unitOf = field => fragmentRuleBounds(catalogue)[field].unit;
  return {
    id: catalogue.id,
    name: catalogue.name,
    description: catalogue.description,
    known: true,
    installed: Boolean(record),
    amplitude: numbers.amplitude,
    periodMs: numbers.periodMs,
    amplitudeUnit: unitOf('amplitude'),
    periodUnit: unitOf('periodMs'),
    amplitudeLabel: formatField('amplitude', numbers.amplitude, catalogue),
    periodLabel: formatField('periodMs', numbers.periodMs, catalogue),
    details: ruleDetails({ ...catalogue.rule, ...numbers }),
    hint: record ? '' : '先装填该碎片再调整',
    actions: FRAGMENT_ADJUST_ACTIONS,
  };
}
