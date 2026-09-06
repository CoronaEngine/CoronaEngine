/**
 * 世界碎片模型：使用受控数据表达可校验的游戏逻辑，不执行任意脚本。
 */
export function createWorldFragment(data = {}) {
  const logic = {
    triggers: [],
    conditions: [],
    actions: [],
    ...data.logic,
  };

  return {
    id: data.id || globalThis.crypto?.randomUUID?.() || `fragment-${Date.now()}`,
    name: data.name || '世界碎片',
    description: data.description || '承载受控游戏逻辑的世界碎片。',
    version: data.version || 1,
    sourceWorld: data.sourceWorld || 'main-world',
    creator: data.creator || 'system',
    requiredMaterials: data.requiredMaterials || [],
    validation: {
      valid: true,
      errors: [],
      ...data.validation,
    },
    ...data,
    logic,
  };
}

