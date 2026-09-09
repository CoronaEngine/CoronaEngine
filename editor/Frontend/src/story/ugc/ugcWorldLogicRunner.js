/** 小世界碎片解释器：校验并执行有限的白名单数据，不支持任意脚本或动态代码。 */
import { cloneUgcData } from './ugcWorldState.js';

export const UGC_TRIGGER_TYPES = Object.freeze([
  'onStart',
  'onInteract',
  'onAttack',
  'onTargetDefeated',
  'onEnterArea',
]);
export const UGC_CONDITION_TYPES = Object.freeze(['hasItem', 'variableEquals', 'targetState']);
export const UGC_ACTION_TYPES = Object.freeze([
  'setVariable',
  'addItem',
  'removeItem',
  'showMessage',
  'completeGoal',
  'changeTargetState',
]);
const UNSAFE_KEYS = new Set(['__proto__', 'constructor', 'prototype']);
const validName = (value) =>
  typeof value === 'string' && value.length > 0 && value.length <= 128 && !UNSAFE_KEYS.has(value);
const scalar = (value) =>
  value === null ||
  ['string', 'boolean'].includes(typeof value) ||
  (typeof value === 'number' && Number.isFinite(value));

/** 校验触发器、条件及动作参数和预算。缺少的数组按空数组处理。 */
export function validateUgcLogic(logic = {}) {
  const errors = [];
  try {
    cloneUgcData(logic);
    if (!logic || typeof logic !== 'object' || Array.isArray(logic))
      throw new Error('逻辑必须是对象。');
    for (const [key, allowed] of [
      ['triggers', UGC_TRIGGER_TYPES],
      ['conditions', UGC_CONDITION_TYPES],
      ['actions', UGC_ACTION_TYPES],
    ]) {
      const entries = logic[key] ?? [];
      if (!Array.isArray(entries) || entries.length > 64)
        throw new Error(`${key} 必须是最多 64 项的数组。`);
      for (const entry of entries) {
        if (!entry || !allowed.includes(entry.type)) throw new Error(`不支持的 ${key} 类型。`);
        if (entry.targetId !== undefined && !validName(entry.targetId))
          throw new Error('目标 ID 无效。');
        if (entry.once !== undefined && typeof entry.once !== 'boolean')
          throw new Error('once 必须是布尔值。');
        if (['hasItem', 'addItem', 'removeItem'].includes(entry.type)) {
          if (
            !validName(entry.itemId) ||
            !Number.isSafeInteger(entry.quantity ?? 1) ||
            (entry.quantity ?? 1) < 1 ||
            (entry.quantity ?? 1) > 1000000
          )
            throw new Error('物品参数无效。');
        }
        if (
          ['setVariable', 'variableEquals'].includes(entry.type) &&
          (!validName(entry.key) || !scalar(entry.value))
        )
          throw new Error('变量名或变量值无效。');
        if (['targetState', 'changeTargetState'].includes(entry.type) && !validName(entry.state))
          throw new Error('目标状态无效。');
        if (
          entry.type === 'showMessage' &&
          (typeof entry.message !== 'string' || !entry.message || entry.message.length > 240)
        )
          throw new Error('消息无效。');
        if (entry.type === 'completeGoal' && !validName(entry.goalId))
          throw new Error('目标 ID 无效。');
      }
    }
  } catch (error) {
    errors.push(error.message);
  }
  return { valid: errors.length === 0, errors };
}

/** 创建运行器；state 必须是独立的试玩副本，不得传入建造数据或主世界状态。 */
export function createUgcWorldLogicRunner({
  state,
  fragments = [],
  fragmentBindings = new Map(),
  onAddItem,
  onRemoveItem,
  onMessage,
} = {}) {
  function targetFor(context, action = {}) {
    const id = action.targetId || context.targetId;
    return state.objects.find((object) => object.id === id);
  }

  function matches(conditions, context) {
    return conditions.every((condition) => {
      if (condition.type === 'hasItem')
        return Boolean(context.hasItem?.(condition.itemId, condition.quantity ?? 1));
      if (condition.type === 'variableEquals')
        return state.logicState.variables[condition.key] === condition.value;
      if (condition.type === 'targetState')
        return targetFor(context, condition)?.targetState === condition.state;
      return false;
    });
  }

  function execute(action, context) {
    switch (action.type) {
      case 'setVariable':
        state.logicState.variables[action.key] = cloneUgcData(action.value);
        return true;
      case 'addItem':
        return (
          typeof onAddItem === 'function' &&
          onAddItem(action.itemId, action.quantity ?? 1) !== false
        );
      case 'removeItem':
        return (
          typeof onRemoveItem === 'function' &&
          onRemoveItem(action.itemId, action.quantity ?? 1) !== false
        );
      case 'showMessage':
        onMessage?.(action.message);
        return true;
      case 'completeGoal':
        if (!state.logicState.activeGoals.includes(action.goalId))
          state.logicState.activeGoals.push(action.goalId);
        state.playState.completed = true;
        state.playState.result = { goalId: action.goalId, completed: true };
        return true;
      case 'changeTargetState': {
        const target = targetFor(context, action);
        if (!target) return false;
        target.targetState = action.state;
        return true;
      }
      default:
        return false;
    }
  }

  return {
    validate: (fragment) => validateUgcLogic(fragment?.logic),
    /** 每个碎片单独容错；动作失败的触发器不记录为已完成。 */
    runTrigger(type, context = {}) {
      const result = { executed: 0, errors: [] };
      if (!UGC_TRIGGER_TYPES.includes(type)) return { executed: 0, errors: ['不支持的触发器。'] };
      for (const fragment of fragments) {
        try {
          const validation = validateUgcLogic(fragment.logic);
          if (!validation.valid) throw new Error(validation.errors.join(' '));
          const boundId = fragmentBindings.get(fragment.id);
          if (boundId && type !== 'onStart' && boundId !== context.targetId) continue;
          const logic = fragment.logic ?? {};
          for (const [index, trigger] of (logic.triggers ?? []).entries()) {
            if (trigger.type !== type) continue;
            if (!boundId && trigger.targetId && trigger.targetId !== context.targetId) continue;
            const currentContext = {
              ...context,
              targetId: context.targetId || boundId || trigger.targetId,
            };
            const key = `${fragment.id}:${index}`;
            if (trigger.once !== false && state.logicState.completedTriggers.includes(key))
              continue;
            if (!matches(logic.conditions ?? [], currentContext)) continue;
            let succeeded = true;
            for (const action of logic.actions ?? []) {
              if (!execute(action, currentContext)) {
                succeeded = false;
                break;
              }
            }
            if (!succeeded) {
              result.errors.push(`${fragment.name || fragment.id}：动作执行失败。`);
              continue;
            }
            if (trigger.once !== false) state.logicState.completedTriggers.push(key);
            result.executed += 1;
          }
        } catch (error) {
          result.errors.push(`${fragment.id}：${error.message}`);
        }
      }
      return result;
    },
  };
}
