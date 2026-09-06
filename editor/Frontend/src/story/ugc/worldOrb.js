/** 世界小球模型：表示进入空白 UGC 世界的入口道具。 */
export function createWorldOrb(data = {}) { return { id: data.id || 'world-orb', name: data.name || '世界小球', type: 'world-orb', ...data }; }
