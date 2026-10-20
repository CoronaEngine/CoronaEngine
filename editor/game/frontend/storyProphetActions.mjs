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
import { isProphetActor, sceneSnapshot } from './storyCharacters.mjs';
import { isMapActor } from './storyMap.mjs';
import { FRAGMENTS, RULE_AXES, findFragment, normalizeTarget, ownedFragmentIds }
  from './storyWorldRules.mjs';

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
 * 碎片的规则可以现场改数字：幅度、周期各两个方向，加一个换轴，共五步。与 ADJUST_ACTIONS 同
 * 一种形状（{id, key, group, label}），按键在「调整碎片」子屏里有效；每一步落在哪个字段、
 * 往哪个方向走，由下面的 FRAGMENT_STEPS 决定，免得两处漂移。
 *
 * 「目标物体」不在这里：它由子屏的物体列表先选（`selectFragmentTarget`），动作只作用于
 * 「这块碎片 × 这个物体」那一条记录。
 */
export const FRAGMENT_ADJUST_ACTIONS = Object.freeze([
  Object.freeze({ id: 'amplitude-up', key: '1', group: '幅度', label: '幅度增大' }),
  Object.freeze({ id: 'amplitude-down', key: '2', group: '幅度', label: '幅度减小' }),
  Object.freeze({ id: 'period-up', key: '3', group: '周期', label: '周期加长' }),
  Object.freeze({ id: 'period-down', key: '4', group: '周期', label: '周期缩短' }),
  // 轴由规则类型决定：sway 可以在 x/y/z 之间换，spin 只能绕 z（换了也会被后端拒绝）。
  Object.freeze({ id: 'axis-next', key: '5', group: '方向', label: '换一条浮动轴' }),
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
    // 先知永远不是可调物体：身份由 `isProphetActor` 一处裁决，角色名或改名都不影响。
    if (isProphetActor(actor)) continue;
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
 * 一条装填记录作用于哪个物体：`target` 是 actor 的 guid。
 *
 * 没有 `target` 的记录是**旧规则**（这份改动之前存下的世界）：它仍然作用于整个小世界，语义
 * 与改动前逐字一致，所以老世界不需要迁移也能照样动。判定一律用 guid，绝不用显示名——名字
 * 可以随时改，guid 才是 actor 的身份。
 */
const recordTarget = record => (typeof record?.target === 'string' && record.target
  ? record.target : null);
/** 读目标的宽松版本：只用来查一条记录，遇到什么都返回 null，绝不抛异常。 */
const readTarget = raw => (typeof raw === 'string' && raw.trim() ? raw.trim() : null);
const sameTarget = (a, b) => (a || null) === (b || null);
/** 目标的一句话名字：页面给了显示名就用它，没有就退回 guid；没有目标就是整个小世界。 */
const targetLabelOf = (target, label) => (target ? (label || target) : '整个小世界');
/** 类型的中文名，子屏标题与换轴反馈共用。 */
const typeLabelOf = type => (type === 'spin' ? '旋转规则' : '浮动规则');

/** 这块碎片此刻装在哪个物体上（guid）；没装填返回 null，装的是旧规则也返回 null。 */
export function installedTargetOf(installedRules = [], fragmentId) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const record = records.find(entry => entry?.fragmentId === fragmentId) || null;
  return record ? recordTarget(record) : null;
}
/** 这块碎片是不是一条旧规则（整世界生效）——子屏据此说明「作用于整个小世界」。 */
export function isLegacyRule(record) {
  return Boolean(record) && recordTarget(record) === null;
}

/**
 * The «碎片» screen's list: every catalogued fragment with its ownership, its装填 state, the
 * human-readable details of the rule it carries and the object it acts on.
 *
 * `names` maps guid → display name so the row can say 作用于：方块（放置物） instead of a guid;
 * it is optional, and a missing name falls back to the guid rather than hiding the target.
 */
export function fragmentPanel(installedRules = [], names = {}) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const installed = new Map(records.filter(rule => rule?.fragmentId)
    .map(rule => [rule.fragmentId, rule]));
  const owned = new Set(ownedFragmentIds());
  const nameOf = guid => (guid ? names?.[guid] || null : null);
  return FRAGMENTS.map(fragment => {
    const record = installed.get(fragment.id) || null;
    const state = record ? RULE_STATE.installed
      : owned.has(fragment.id) ? RULE_STATE.owned : RULE_STATE.locked;
    const numbers = fragmentNumbers(fragment, record);
    const target = recordTarget(record);
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
      // 作用对象：null = 旧规则（整个小世界）。guid 是身份，label 只用来显示。
      target,
      targetLabel: record ? targetLabelOf(target, nameOf(target)) : null,
      legacy: isLegacyRule(record),
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
    // 数字键打开「装填 / 调整」子屏，而不是立刻装填：一块碎片必须先选中要作用于哪个物体，
    // 没有目标就装填等于回到「影响所有物体」，那正是本次要改掉的行为。
    return entry ? { type: 'open-fragment-adjust', fragmentId: entry.id } : null;
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
 * 装填 / 拆下 / 换目标 — 一块碎片只作用于一个物体。
 *
 * `target` 是被选中物体的 actor guid（`label` 只是它的显示名）。三种结果：
 *   - 这块碎片已经装在这个物体上 → 拆下；
 *   - 已经装在别的物体上（或是一条没有目标的旧规则）→ 换目标，数字与轴原样保留；
 *   - 还没装 → 用目录里的数字装到这个物体上。
 * 没有 `target` 时保持改动前的行为（一条整世界生效的记录），这条路径只给旧存档与既有调用用；
 * 新界面一定先选物体。返回要落盘的记录与发生了什么，写入由调用方负责。
 */
export function toggleFragment(installedRules = [], fragmentId, { target = null, label = null } = {}) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const catalogue = findFragment(fragmentId);
  if (!catalogue) throw new Error(`未知碎片：${String(fragmentId)}`);
  // 目标只有两种合法写法：null（旧规则/整世界），或一个非空 guid。地图与先知在这里就被
  // `normalizeTarget` 拒绝（`isReservedTargetGuid`），所以它们既装不上也换不过去。
  const guid = normalizeTarget(target);
  const at = records.find(record => record?.fragmentId === fragmentId) || null;
  if (at && sameTarget(recordTarget(at), guid)) {
    return { action: 'removed', fragmentId, target: guid, label: targetLabelOf(guid, label),
      rules: records.filter(record => record !== at),
      feedback: `已拆下：${catalogue.name}` };
  }
  if (at) {
    const moved = { ...at };
    if (guid === null) delete moved.target; else moved.target = guid;
    return { action: 'retargeted', fragmentId, target: guid, label: targetLabelOf(guid, label),
      rules: records.map(record => (record === at ? moved : record)),
      feedback: `已改目标：${catalogue.name} → ${targetLabelOf(guid, label)}` };
  }
  if (!ownedFragmentIds().includes(fragmentId)) throw new Error(`尚未获得碎片：${catalogue.name}`);
  const rule = catalogue.rule;
  const record = { fragmentId, type: rule.type, axis: rule.axis,
    amplitude: rule.amplitude, periodMs: rule.periodMs };
  // 「旧规则」那一份记录不带 target 键：两种形状在深比较里也分得开。
  if (guid !== null) record.target = guid;
  return { action: 'installed', fragmentId, target: guid, label: targetLabelOf(guid, label),
    rules: [...records, record],
    feedback: `已装填：${catalogue.name}${guid ? `（作用于 ${targetLabelOf(guid, label)}）` : ''}` };
}

/**
 * The installed record of one fragment ON ONE OBJECT, or null. Never throws, whatever it is
 * handed. Without `target` it answers for the legacy record (a rule with no target), which is
 * what an old world has and what the old 2-argument calls asked for.
 */
export function fragmentRuleOf(installedRules = [], fragmentId, { target = null } = {}) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const guid = readTarget(target);
  return records.find(record => record?.fragmentId === fragmentId
    && sameTarget(recordTarget(record), guid)) || null;
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
 * Change one number (or the axis) of an installed fragment ON ONE OBJECT.
 *
 * `target` selects which record: the guid of the object the player picked. Without it the
 * legacy record (no target) is edited, which is what an old world has; a record that names a
 * different object is never touched by an action that does not name it — that is what stops
 * two objects from cross-talking in the UI either.
 *
 * Returns the whole rule list to persist plus what to tell the player, so the caller only has
 * to save it. Never throws and never returns a rule the backend would reject: numbers are
 * pushed back inside the mirrored bounds first, the axis is only ever one of the type's, and
 * nothing changes when the action is unknown, the fragment is not installed, or the value
 * already sits on the bound the step walks toward.
 */
export function adjustFragmentRule(installedRules = [], fragmentId, actionId,
  { target = null, label = null } = {}) {
  const records = Array.isArray(installedRules) ? installedRules : [];
  const guid = readTarget(target);
  const refuse = feedback => ({ changed: false, rules: records, feedback, target: guid });
  const action = findFragmentAdjustAction(actionId);
  if (!action) return refuse(`未知的调整：${String(actionId)}`);
  const catalogue = findFragment(fragmentId);
  const record = fragmentRuleOf(records, fragmentId, { target: guid });
  if (!record) {
    if (!catalogue) return refuse(`未知碎片：${String(fragmentId)}`);
    return refuse(guid
      ? `先在该物体上装填${catalogue.name}再调整`
      : '先装填该碎片再调整');
  }
  const axes = RULE_AXES[catalogue.rule.type] || [];
  const axis = axes.includes(record.axis) ? record.axis : catalogue.rule.axis;
  // 「换一条浮动轴」：轴必须属于规则类型（spin 只有 z，按了也只会得到一句说明），
  // 类型本身来自碎片——一条规则的类型就是它装的那块碎片，不在子屏里改。
  if (action.id === 'axis-next') {
    if (axes.length <= 1) {
      return refuse(`${typeLabelOf(catalogue.rule.type)}只能绕 ${axes[0] || 'z'} 轴，方向不能改`);
    }
    const nextAxis = axes[(axes.indexOf(axis) + 1) % axes.length];
    const updated = { ...record, axis: nextAxis };
    return {
      changed: true,
      rules: records.map(entry => (entry === record ? updated : entry)),
      target: guid,
      feedback: `方向已改为 ${AXIS_NAMES[nextAxis] || nextAxis}（${nextAxis} 轴）`,
    };
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
  // `...record` keeps the target (and the axis the player may have changed) — an edit must not
  // quietly move the rule back to the whole world or to the catalogue's axis.
  const updated = { ...record, fragmentId, type: catalogue.rule.type, axis, amplitude, periodMs };
  const edge = next === bounds.max ? `（已达${meta.label}上限）`
    : next === bounds.min ? `（已达${meta.label}下限）` : '';
  return {
    changed: true,
    rules: records.map(entry => (entry === record ? updated : entry)),
    target: guid,
    feedback: `${meta.label}已调到 ${fixed2(next * bounds.scale)} ${bounds.unit}${edge}`,
  };
}

/**
 * What the «调整碎片» sub-screen shows: the chosen object, this fragment's rule for THAT object
 * (or, with no object chosen, the legacy whole-world record), its numbers, axis and type, and
 * the adjustment steps. A fragment that is not installed is still describable (catalogue
 * defaults) so the sub-screen can explain itself instead of rendering blank.
 */
export function fragmentAdjustPanel(installedRules = [], fragmentId, { target = null, label = null } = {}) {
  const guid = readTarget(target);
  const record = fragmentRuleOf(installedRules, fragmentId, { target: guid });
  const catalogue = findFragment(fragmentId);
  const targetLabel = targetLabelOf(guid, label);
  if (!catalogue) {
    return { id: fragmentId ?? null, name: String(fragmentId ?? '未知碎片'), description: '',
      known: false, installed: false, amplitude: null, periodMs: null,
      target: guid, targetLabel, legacy: false, type: null, typeLabel: '—', axis: null,
      axisLabel: '方向 —',
      amplitudeLabel: '幅度 —', periodLabel: '周期 —', details: '未知碎片，无法调整',
      hint: '先装填该碎片再调整', actions: FRAGMENT_ADJUST_ACTIONS };
  }
  const numbers = fragmentNumbers(catalogue, record);
  const unitOf = field => fragmentRuleBounds(catalogue)[field].unit;
  const axes = RULE_AXES[catalogue.rule.type] || [];
  const axis = record && axes.includes(record.axis) ? record.axis : catalogue.rule.axis;
  return {
    id: catalogue.id,
    name: catalogue.name,
    description: catalogue.description,
    known: true,
    installed: Boolean(record),
    target: guid,
    targetLabel,
    // 旧规则（没有目标）照旧作用于整个小世界，子屏必须把这一点说出来，而不是假装它只作用
    // 于某个物体。
    legacy: isLegacyRule(record),
    type: catalogue.rule.type,
    typeLabel: catalogue.rule.type === 'spin' ? '旋转 ±度' : '浮动（米）',
    axis,
    axisLabel: `方向 ${AXIS_NAMES[axis] || axis}（${axis} 轴）`,
    amplitude: numbers.amplitude,
    periodMs: numbers.periodMs,
    amplitudeUnit: unitOf('amplitude'),
    periodUnit: unitOf('periodMs'),
    amplitudeLabel: formatField('amplitude', numbers.amplitude, catalogue),
    periodLabel: formatField('periodMs', numbers.periodMs, catalogue),
    details: ruleDetails({ ...catalogue.rule, axis, amplitude: numbers.amplitude,
      periodMs: numbers.periodMs }),
    hint: record ? '' : (guid ? `先在该物体上装填${catalogue.name}再调整` : '先装填该碎片再调整'),
    actions: FRAGMENT_ADJUST_ACTIONS,
  };
}
