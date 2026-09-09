/**
 * 小世界资源事务：quantity 是可用数量，ownedQuantity 是已经付费的小世界库存。
 * 进入时借入的主世界资源只在保存成功后扣除；对象保存后删除仅返还到小世界。
 * 对象 cost 保存物品元数据和 pendingQuantity，确保退款及碎片逻辑可跨存档恢复。
 */
import { cloneUgcData, normalizeUgcResources } from './ugcWorldState.js';

const KEYS = ['materials', 'fragments'];

function resourceKey(type) {
  if (type === 'material' || type === 'materials') return 'materials';
  if (type === 'fragment' || type === 'fragments') return 'fragments';
  throw new Error('不支持的资源类型。');
}

function merge(list, entry, quantity, ownedQuantity = 0) {
  let item = list.find((candidate) => candidate.id === entry.id);
  if (!item) {
    item = { ...cloneUgcData(entry), quantity: 0, ownedQuantity: 0 };
    delete item.pendingQuantity;
    list.push(item);
  }
  item.quantity += quantity;
  item.ownedQuantity += ownedQuantity;
  return item;
}

/** 创建隔离资源表；任何外部查询都返回副本。 */
export function createUgcWorldResourceService({
  state = null,
  materials = [],
  fragments = [],
  spentResources = {},
} = {}) {
  let available = normalizeUgcResources({ materials, fragments });
  let spent = normalizeUgcResources(spentResources);

  function sync() {
    if (!state) return;
    state.resources = cloneUgcData(available);
    state.spentResources = cloneUgcData(spent);
  }

  function getItem(type, id) {
    const item = available[resourceKey(type)].find((entry) => entry.id === id);
    return item ? cloneUgcData(item) : null;
  }

  function has(type, id, quantity = 1) {
    return (
      Number.isSafeInteger(quantity) &&
      quantity > 0 &&
      (getItem(type, id)?.quantity ?? 0) >= quantity
    );
  }

  /** 将所有依赖合并校验后一次性扣除，避免多材料消费只成功一半。 */
  function consumeCost(request = {}) {
    const cost = normalizeUgcResources(request);
    for (const key of KEYS) {
      if (cost[key].some((item) => !has(key, item.id, item.quantity))) return null;
    }
    for (const key of KEYS) {
      cost[key] = cost[key].map((entry) => {
        const item = available[key].find((candidate) => candidate.id === entry.id);
        const ownedUsed = Math.min(item.ownedQuantity, entry.quantity);
        const pendingQuantity = entry.quantity - ownedUsed;
        const result = {
          ...cloneUgcData(item),
          quantity: entry.quantity,
          ownedQuantity: ownedUsed,
          pendingQuantity,
        };
        item.quantity -= entry.quantity;
        item.ownedQuantity -= ownedUsed;
        if (pendingQuantity) merge(spent[key], result, pendingQuantity);
        if (!item.quantity) available[key].splice(available[key].indexOf(item), 1);
        return result;
      });
    }
    sync();
    return cost;
  }

  /** 精确返还对象成本；调用方只能在对象成功删除时调用一次。 */
  function refundCost(cost = {}) {
    for (const key of KEYS) {
      for (const entry of cost[key] ?? []) {
        const pending = spent[key].find((item) => item.id === entry.id);
        if ((pending?.quantity ?? 0) < (entry.pendingQuantity ?? 0)) return false;
      }
    }
    for (const key of KEYS) {
      for (const entry of cost[key] ?? []) {
        const quantity = entry.pendingQuantity ?? 0;
        const pending = spent[key].find((item) => item.id === entry.id);
        if (quantity) {
          pending.quantity -= quantity;
          if (!pending.quantity) spent[key].splice(spent[key].indexOf(pending), 1);
        }
        merge(available[key], entry, entry.quantity, entry.quantity - quantity);
      }
    }
    sync();
    return true;
  }

  function getPendingCommit() {
    return KEYS.flatMap((type) => spent[type].map(({ id, quantity }) => ({ type, id, quantity })));
  }

  function canCommit(items) {
    return (
      Array.isArray(items) &&
      getPendingCommit().every(({ type, id, quantity }) => {
        const category = type === 'materials' ? 'material' : 'ugc';
        return (
          (items.find((item) => item.id === id && item.category === category)?.quantity ?? 0) >=
          quantity
        );
      })
    );
  }

  /** 预计算提交后的主背包。此时不修改原背包，也不清除账本。 */
  function prepareCommit(items) {
    if (!canCommit(items)) throw new Error('主世界资源不足，无法保存。');
    const next = cloneUgcData(items);
    for (const entry of getPendingCommit()) {
      const category = entry.type === 'materials' ? 'material' : 'ugc';
      const item = next.find(
        (candidate) => candidate.id === entry.id && candidate.category === category
      );

      if (!item || item.quantity < entry.quantity) {
        throw new Error(`主世界缺少可提交资源：${entry.id}。`);
      }

      item.quantity -= entry.quantity;
    }
    return next.filter((item) => item.quantity > 0);
  }

  /** 持久化成功后清空事务，并把所有对象成本标记为已付费。 */
  function markCommitted() {
    for (const object of state?.objects ?? []) {
      for (const key of KEYS) {
        for (const entry of object.cost?.[key] ?? []) {
          entry.pendingQuantity = 0;
          entry.ownedQuantity = entry.quantity;
        }
      }
      if (object.cost) object.cost.committed = true;
    }
    spent = { materials: [], fragments: [] };
    sync();
  }

  sync();
  return {
    has,
    getItem,
    consumeCost,
    refundCost,
    getPendingCommit,
    canCommit,
    prepareCommit,
    markCommitted,
    consume(type, id, quantity = 1) {
      return Boolean(consumeCost({ [resourceKey(type)]: [{ id, quantity }] }));
    },
    commitToInventory(items) {
      if (!canCommit(items)) return false;
      const next = prepareCommit(items);
      items.splice(0, items.length, ...next);
      markCommitted();
      return true;
    },
    getResources: () => cloneUgcData(available),
    getSpentResources: () => cloneUgcData(spent),
    getAvailable: (type) => cloneUgcData(available[resourceKey(type)]),
    getSpent: (type) => cloneUgcData(spent[resourceKey(type)]),
    getFragmentCatalog() {
      const all = [
        ...available.fragments,
        ...spent.fragments,
        ...(state?.objects ?? []).flatMap((object) => object.cost?.fragments ?? []),
      ];
      return cloneUgcData([...new Map(all.map((item) => [item.id, item])).values()]);
    },
    /** 借入库存不写入正式存档；重新打开时从当前主背包重新注入。 */
    getOwnedResources() {
      return Object.fromEntries(
        KEYS.map((key) => [
          key,
          available[key]
            .filter((item) => item.ownedQuantity > 0)
            .map((item) => ({
              ...cloneUgcData(item),
              quantity: item.ownedQuantity,
              ownedQuantity: item.ownedQuantity,
            })),
        ])
      );
    },
    reset(nextMaterials = [], nextFragments = []) {
      available = normalizeUgcResources({ materials: nextMaterials, fragments: nextFragments });
      spent = { materials: [], fragments: [] };
      sync();
    },
  };
}
