/**
 * 小世界数据边界：校验、复制和序列化纯数据，不持有 Three.js、DOM 或主世界引用。
 * 存档版本升级在此集中处理；资源的 ownedQuantity 表示已付费的小世界库存。
 */
export const UGC_WORLD_FORMAT = 'corona-ugc-world';
export const UGC_WORLD_VERSION = 1;
export const UGC_MAX_OBJECTS = 128;
export const UGC_OBJECT_TYPES = Object.freeze(['block', 'spawn', 'target']);
export const UGC_WORLD_MODES = Object.freeze(['build', 'play']);
export const UGC_WORLD_ID_PATTERN = /^[a-zA-Z0-9_-]{1,64}$/;
export const UGC_DEFAULT_SPAWN = Object.freeze({
  position: Object.freeze([0, 1.7, 4]),
  yaw: 0,
  pitch: 0,
});
const UNSAFE_KEYS = new Set(['__proto__', 'constructor', 'prototype']);

/** 复制纯 JSON 数据，兼容 Vue 代理；拒绝循环引用、函数和原型污染字段。 */
export function cloneUgcData(value) {
  const seen = new Set();
  let nodes = 0;
  function visit(entry, depth) {
    nodes += 1;
    if (depth > 24 || nodes > 100000) throw new Error('小世界数据超出大小或嵌套限制。');
    if (entry === null || typeof entry === 'string' || typeof entry === 'boolean') return entry;
    if (typeof entry === 'number' && Number.isFinite(entry)) return entry;
    if (typeof entry !== 'object' || seen.has(entry))
      throw new Error('小世界数据必须是无循环的 JSON 数据。');
    seen.add(entry);
    let result;
    if (Array.isArray(entry)) {
      if (entry.length > 4096) throw new Error('小世界数据数组过长。');
      result = entry.map((item) => visit(item, depth + 1));
    } else {
      if (
        Object.getPrototypeOf(entry) !== Object.prototype &&
        Object.getPrototypeOf(entry) !== null
      ) {
        throw new Error('小世界数据不能包含运行时对象。');
      }
      result = {};
      for (const [key, item] of Object.entries(entry)) {
        if (UNSAFE_KEYS.has(key)) throw new Error(`不允许的字段：${key}。`);
        result[key] = visit(item, depth + 1);
      }
    }
    seen.delete(entry);
    return result;
  }
  return visit(value, 0);
}

/** 生成安全的世界或对象 ID。 */
export function createUgcId(prefix = 'ugc-world') {
  return `${prefix}-${globalThis.crypto?.randomUUID?.() || `${Date.now()}-${Math.random().toString(36).slice(2, 12)}`}`;
}

/** 校验跨平台文件名，包含 Windows 保留设备名限制。 */
export function isValidUgcWorldId(value) {
  return (
    typeof value === 'string' &&
    UGC_WORLD_ID_PATTERN.test(value) &&
    !/^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i.test(value)
  );
}

function assertObject(value, label) {
  if (!value || typeof value !== 'object' || Array.isArray(value))
    throw new Error(`${label}必须是对象。`);
}

/** 校验三维数值，不把错误坐标静默变成原点。 */
export function normalizeUgcVector(value, fallback, label = '坐标', positive = false) {
  const source = value === undefined ? fallback : value;
  if (
    !Array.isArray(source) ||
    source.length !== 3 ||
    source.some(
      (n) =>
        typeof n !== 'number' || !Number.isFinite(n) || Math.abs(n) > 10000 || (positive && n <= 0)
    )
  ) {
    throw new Error(`${label}必须是有效的三维数字数组。`);
  }
  return [...source];
}

/** 规范化资源表，保留物品描述及碎片逻辑；碎片 ID 唯一，不堆叠。 */
export function normalizeUgcResources(value = {}) {
  assertObject(value, '资源');
  const result = { materials: [], fragments: [] };
  for (const key of ['materials', 'fragments']) {
    const list = value[key] ?? [];
    if (!Array.isArray(list) || list.length > 1024) throw new Error('资源列表无效。');
    const merged = new Map();
    for (const entry of list) {
      assertObject(entry, '资源');
      if (typeof entry.id !== 'string' || !entry.id || entry.id.length > 128)
        throw new Error('资源 ID 无效。');
      const quantity = entry.quantity ?? 1;
      const ownedQuantity = entry.ownedQuantity ?? 0;
      if (
        !Number.isSafeInteger(quantity) ||
        quantity < 0 ||
        quantity > 1000000 ||
        !Number.isSafeInteger(ownedQuantity) ||
        ownedQuantity < 0 ||
        ownedQuantity > quantity
      ) {
        throw new Error('资源数量无效。');
      }
      const item = { ...cloneUgcData(entry), quantity, ownedQuantity };
      if (key === 'fragments' && (quantity > 1 || merged.has(item.id)))
        throw new Error('世界碎片不能重复或堆叠。');
      if (merged.has(item.id)) {
        item.quantity += merged.get(item.id).quantity;
        item.ownedQuantity += merged.get(item.id).ownedQuantity;
      }
      if (item.quantity > 1000000) throw new Error('资源数量超出上限。');
      if (item.quantity) merged.set(item.id, item);
    }
    result[key] = [...merged.values()];
  }
  return result;
}

function normalizeCost(cost = {}) {
  const result = normalizeUgcResources(cost);
  for (const key of ['materials', 'fragments']) {
    result[key] = result[key].map((entry) => {
      const pendingQuantity = entry.pendingQuantity ?? (cost.committed ? 0 : entry.quantity);
      if (
        !Number.isSafeInteger(pendingQuantity) ||
        pendingQuantity < 0 ||
        pendingQuantity > entry.quantity
      ) {
        throw new Error('对象待提交成本无效。');
      }
      return { ...entry, pendingQuantity };
    });
  }
  result.committed = [...result.materials, ...result.fragments].every(
    (item) => item.pendingQuantity === 0
  );
  return result;
}

/** 创建小世界；只复制声明的数据字段，允许 options 中省略字段而不复制运行时参数。 */
export function createUgcWorldState(options = {}) {
  assertObject(options, '小世界');
  const id = options.id ?? createUgcId();
  if (!isValidUgcWorldId(id)) throw new Error('小世界 worldId 无效。');
  if (options.version !== undefined && options.version !== UGC_WORLD_VERSION)
    throw new Error('不支持的小世界版本。');
  if (options.mode !== undefined && !UGC_WORLD_MODES.includes(options.mode))
    throw new Error('小世界模式无效。');
  const source = options.objects ?? [];
  if (!Array.isArray(source) || source.length > UGC_MAX_OBJECTS)
    throw new Error(`小世界最多支持 ${UGC_MAX_OBJECTS} 个对象。`);
  const ids = new Set();
  const boundFragments = new Set();
  const objects = source.map((entry) => {
    assertObject(entry, '对象');
    if (!isValidUgcWorldId(entry.id) || ids.has(entry.id)) throw new Error('对象 ID 无效或重复。');
    if (!UGC_OBJECT_TYPES.includes(entry.type)) throw new Error('不支持的对象类型。');
    ids.add(entry.id);
    const fragments = entry.fragmentIds ?? [];
    if (
      !Array.isArray(fragments) ||
      fragments.length > 32 ||
      fragments.some(
        (item) => typeof item !== 'string' || !item || item.length > 128 || boundFragments.has(item)
      ) ||
      new Set(fragments).size !== fragments.length
    )
      throw new Error('对象碎片绑定无效或重复。');
    fragments.forEach((item) => boundFragments.add(item));
    const health = entry.health ?? 100;
    if (!Number.isFinite(health) || health < 0 || health > 100) throw new Error('目标生命值无效。');
    if (
      entry.materialId != null &&
      (typeof entry.materialId !== 'string' || entry.materialId.length > 128)
    )
      throw new Error('材料 ID 无效。');
    return {
      id: entry.id,
      type: entry.type,
      position: normalizeUgcVector(entry.position, [0, 0.5, 0], '对象位置'),
      rotation: normalizeUgcVector(entry.rotation, [0, 0, 0], '对象旋转'),
      scale: normalizeUgcVector(entry.scale, [1, 1, 1], '对象缩放', true),
      materialId: entry.materialId ?? null,
      fragmentIds: [...fragments],
      cost: normalizeCost(entry.cost),
      health,
      targetState: typeof entry.targetState === 'string' ? entry.targetState.slice(0, 64) : 'idle',
    };
  });
  if (objects.filter((item) => item.type === 'spawn').length > 1)
    throw new Error('只能有一个出生点。');
  const spawn = options.spawn ?? UGC_DEFAULT_SPAWN;
  assertObject(spawn, '出生点');
  if (!Number.isFinite(spawn.yaw ?? 0) || !Number.isFinite(spawn.pitch ?? 0))
    throw new Error('出生点角度无效。');
  const logic = options.logicState ?? {};
  assertObject(logic, '逻辑状态');
  const stringList = (list = []) => {
    if (
      !Array.isArray(list) ||
      list.length > 4096 ||
      list.some((item) => typeof item !== 'string' || item.length > 256)
    )
      throw new Error('逻辑状态列表无效。');
    return [...new Set(list)];
  };
  assertObject(logic.variables ?? {}, '逻辑变量');
  return {
    id,
    version: UGC_WORLD_VERSION,
    name:
      typeof options.name === 'string' && options.name.trim()
        ? options.name.trim().slice(0, 80)
        : '未命名小世界',
    mode: options.mode ?? 'build',
    dirty: Boolean(options.dirty),
    spawn: {
      position: normalizeUgcVector(spawn.position, UGC_DEFAULT_SPAWN.position, '出生点位置'),
      yaw: spawn.yaw ?? 0,
      pitch: Math.max(-1.45, Math.min(1.45, spawn.pitch ?? 0)),
    },
    objects,
    resources: normalizeUgcResources(options.resources),
    spentResources: normalizeUgcResources(options.spentResources),
    logicState: {
      completedTriggers: stringList(logic.completedTriggers),
      activeGoals: stringList(logic.activeGoals),
      variables: cloneUgcData(logic.variables ?? {}),
    },
    playState: {
      started: Boolean(options.playState?.started),
      completed: Boolean(options.playState?.completed),
      result: cloneUgcData(options.playState?.result ?? null),
    },
  };
}

/** 加载必须有原始 ID，不能静默生成新 ID。 */
export function normalizeUgcWorldState(input) {
  if (!input?.id) throw new Error('小世界存档缺少 id。');
  cloneUgcData(input);
  return createUgcWorldState(input);
}

/** 序列化存档，运行模式和 dirty 不作为下一次进入时的活动状态。 */
export function serializeUgcWorld(state) {
  const world = normalizeUgcWorldState(state);
  world.mode = 'build';
  world.dirty = false;
  return { format: UGC_WORLD_FORMAT, version: UGC_WORLD_VERSION, world };
}

/** 校验存档外层格式及版本，再恢复状态。 */
export function deserializeUgcWorld(payload) {
  if (payload?.format !== UGC_WORLD_FORMAT) throw new Error('不是有效的 Corona UGC 小世界存档。');
  if (payload.version !== UGC_WORLD_VERSION) throw new Error('不支持的小世界存档版本。');
  return normalizeUgcWorldState(payload.world);
}

/** 返回防御性状态副本。 */
export function cloneUgcWorldState(state) {
  return normalizeUgcWorldState(state);
}

/** 仅建造数据变化标记未保存。 */
export function setUgcWorldDirty(state, dirty = true) {
  state.dirty = Boolean(dirty);
  return state.dirty;
}
