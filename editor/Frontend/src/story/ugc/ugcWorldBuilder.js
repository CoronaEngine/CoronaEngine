/** 小世界建造器：网格放置、精确退款和碎片绑定；不操作 DOM、Three.js 或文件。 */
import {
  UGC_MAX_OBJECTS,
  UGC_OBJECT_TYPES,
  UGC_DEFAULT_SPAWN,
  createUgcId,
  cloneUgcData,
  normalizeUgcVector,
} from './ugcWorldState.js';
import { validateUgcLogic } from './ugcWorldLogicRunner.js';

export const UGC_GRID_SIZE = 1;

/** 创建建造器。资源变更只发生在会话内部。 */
export function createUgcWorldBuilder({ state, resources, onChanged } = {}) {
  function changed() {
    state.dirty = true;
    onChanged?.(state);
  }

  function requirements(fragmentIds) {
    if (!Array.isArray(fragmentIds) || new Set(fragmentIds).size !== fragmentIds.length) {
      throw new Error('世界碎片绑定重复或无效。');
    }
    const cost = { materials: [], fragments: [] };
    for (const id of fragmentIds) {
      if (state.objects.some((object) => object.fragmentIds.includes(id))) {
        throw new Error('一个碎片只能绑定一个目标。');
      }
      const fragment = resources.getItem('fragments', id);
      if (!fragment) throw new Error('世界碎片不可用。');
      const validation = validateUgcLogic(fragment.logic);
      if (!validation.valid) throw new Error(validation.errors.join(' '));
      if (!Array.isArray(fragment.requiredMaterials ?? [])) throw new Error('碎片材料依赖无效。');
      cost.fragments.push({ id, quantity: 1 });
      for (const entry of fragment.requiredMaterials ?? []) {
        cost.materials.push({ id: entry.itemId ?? entry.id, quantity: entry.quantity });
      }
    }
    return cost;
  }

  function perform(operation) {
    if (state.mode !== 'build') return { ok: false, error: '试玩模式不能修改场景。' };
    try {
      return operation();
    } catch (error) {
      return { ok: false, error: error.message };
    }
  }

  return {
    /** 坐标吸附到 1 米网格；方块中心和出生点眼睛高度分别为 0.5/1.7 米。 */
    placeObject({
      type = 'block',
      position = [0, 0, 0],
      materialId = null,
      fragmentIds = [],
    } = {}) {
      return perform(() => {
        if (!UGC_OBJECT_TYPES.includes(type)) throw new Error('不支持的对象类型。');
        if (state.objects.length >= UGC_MAX_OBJECTS) throw new Error('最多放置 128 个对象。');
        if (type === 'spawn' && state.objects.some((object) => object.type === 'spawn')) {
          throw new Error('只能放置一个出生点，请先删除旧出生点。');
        }
        const point = normalizeUgcVector(position, [0, 0, 0]).map(Math.round);
        if (Math.abs(point[0]) > 30 || Math.abs(point[2]) > 30 || point[1] < 0 || point[1] > 30) {
          throw new Error('对象超出小世界建造范围。');
        }
        point[1] += type === 'spawn' ? 1.7 : type === 'block' ? 0.5 : 1;
        if (
          state.objects.some((object) =>
            object.position.every((n, index) => Math.abs(n - point[index]) < 0.01)
          )
        ) {
          throw new Error('此网格位置已经有对象。');
        }
        if (fragmentIds.length && type !== 'target') throw new Error('首版只允许给目标绑定碎片。');
        const request = requirements(fragmentIds);
        if (type === 'block') {
          if (!materialId) throw new Error('请选择材料。');
          request.materials.push({ id: materialId, quantity: 1 });
        }
        const cost = resources.consumeCost(request);
        if (!cost) throw new Error('材料或世界碎片不足。');
        const object = {
          id: createUgcId('object'),
          type,
          position: point,
          rotation: [0, 0, 0],
          scale: [1, 1, 1],
          materialId: type === 'block' ? materialId : null,
          fragmentIds: [...fragmentIds],
          cost,
          health: 100,
          targetState: 'idle',
        };
        state.objects.push(object);
        if (type === 'spawn') state.spawn.position = [...point];
        changed();
        return { ok: true, object: cloneUgcData(object) };
      });
    },
    /** 删除已存在的对象后返还它的精确成本；重复删除不会再次退款。 */
    removeObject(id) {
      return perform(() => {
        const index = state.objects.findIndex((object) => object.id === id);
        if (index < 0) throw new Error('对象不存在。');
        const object = state.objects[index];
        if (!resources.refundCost(object.cost)) throw new Error('资源账本不一致，无法删除。');
        state.objects.splice(index, 1);
        if (object.type === 'spawn') state.spawn = cloneUgcData(UGC_DEFAULT_SPAWN);
        changed();
        return { ok: true, object: cloneUgcData(object) };
      });
    },
    /** 只允许绑定通过白名单校验的逻辑；完整碎片数据保存在对象成本中。 */
    bindFragment(objectId, fragmentId) {
      return perform(() => {
        const object = state.objects.find((entry) => entry.id === objectId);
        if (object?.type !== 'target') throw new Error('请选择一个目标。');
        const cost = resources.consumeCost(requirements([fragmentId]));
        if (!cost) throw new Error('绑定碎片所需材料不足。');
        object.fragmentIds.push(fragmentId);
        for (const key of ['materials', 'fragments']) {
          for (const entry of cost[key]) {
            const existing = object.cost[key].find((item) => item.id === entry.id);
            if (existing) {
              existing.quantity += entry.quantity;
              existing.ownedQuantity += entry.ownedQuantity;
              existing.pendingQuantity += entry.pendingQuantity;
            } else object.cost[key].push(entry);
          }
        }
        changed();
        return { ok: true, object: cloneUgcData(object) };
      });
    },
  };
}
