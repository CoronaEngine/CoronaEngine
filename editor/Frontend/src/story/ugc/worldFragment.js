/**
 * 世界碎片模型：使用受控数据表达可校验的游戏逻辑，不执行任意脚本。
 *
 * 这里不接受旧版字符串脚本或外部伪造的 validation 结果。所有碎片逻辑都
 * 必须经过 ugcWorldLogicRunner.js 的白名单校验，方便后续交接和扩展。
 */
import { validateUgcLogic } from './ugcWorldLogicRunner.js';

const DEMO_LOGIC = Object.freeze({
  triggers: [{ type: 'onTargetDefeated' }],
  conditions: [],
  actions: [
    { type: 'showMessage', message: '目标已完成。' },
    { type: 'completeGoal', goalId: 'demo-complete' },
  ],
});

const EMPTY_LOGIC = Object.freeze({
  triggers: [],
  conditions: [],
  actions: [],
});

/**
 * 规范化碎片逻辑输入。
 *
 * 缺少逻辑时创建一个空的结构化逻辑对象；如果调用方传入了错误类型，
 * 则原样交给校验器，由 validation.valid=false 明确反馈错误。
 *
 * @param {unknown} value 原始逻辑数据。
 * @returns {unknown} 待校验逻辑数据。
 */
function normalizeLogic(value) {
  if (value === undefined) {
    return {
      triggers: [...EMPTY_LOGIC.triggers],
      conditions: [...EMPTY_LOGIC.conditions],
      actions: [...EMPTY_LOGIC.actions],
    };
  }

  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    return value;
  }

  return {
    ...EMPTY_LOGIC,
    ...value,
  };
}

/**
 * 创建世界碎片。
 *
 * @param {object} data 世界碎片字段。
 * @returns {object} 可序列化的世界碎片。
 */
export function createWorldFragment(data = {}) {
  const source = data && typeof data === 'object' && !Array.isArray(data) ? data : {};
  const logic = normalizeLogic(source.logic);
  const validation = validateUgcLogic(logic);

  return {
    ...source,
    id: source.id || globalThis.crypto?.randomUUID?.() || `fragment-${Date.now()}`,
    name: source.name || '世界碎片',
    description: source.description || '承载受控游戏逻辑的世界碎片。',
    category: source.category || 'ugc',
    quantity: source.quantity ?? 1,
    version: source.version ?? 1,
    sourceWorld: source.sourceWorld || 'main-world',
    creator: source.creator || 'system',
    requiredMaterials: source.requiredMaterials || [],
    validation,
    logic,
  };
}

/** 创建击败主世界 Boss 后掉落的首个世界碎片。 */
export function createDemoWorldFragment() {
  return createWorldFragment({
    id: 'world-fragment-demo',
    name: '世界碎片',
    description: '承载受控游戏逻辑，可用于制作 Demo。',
    sourceWorld: 'main-world',
    creator: 'system',
    requiredMaterials: [{ itemId: 'material-wood', quantity: 2 }],
    logic: DEMO_LOGIC,
  });
}
