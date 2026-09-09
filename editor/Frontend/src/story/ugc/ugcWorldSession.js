/**
 * 小世界会话：隔离主世界快照、可序列化世界状态和内部资源事务。
 *
 * 会话拥有唯一一份可变 world；控制器和建造器都引用这份内部状态，
 * 对外返回时才创建防御性副本，避免修改发生在脱离会话的旧快照上。
 */
import { cloneUgcData, cloneUgcWorldState, createUgcWorldState } from './ugcWorldState.js';
import { createUgcWorldResourceService } from './ugcWorldResourceService.js';

const EMPTY_RESOURCES = Object.freeze({ materials: [], fragments: [] });

function cloneValue(value) {
  return cloneUgcData(value);
}

function cloneSnapshot(snapshot) {
  if (!snapshot || typeof snapshot !== 'object') return snapshot ?? null;

  const result = {};
  for (const [key, value] of Object.entries(snapshot)) {
    // 场景、渲染器等运行时引用需要在退出时原样恢复，不能序列化。
    if (key === 'activeScene' || key === 'sceneBundle' || key === 'renderer') {
      result[key] = value;
      continue;
    }
    result[key] = cloneValue(value);
  }
  return result;
}

function cloneResources(resources) {
  return {
    materials: resources.materials.map((item) => cloneValue(item)),
    fragments: resources.fragments.map((item) => cloneValue(item)),
  };
}

function statusForLifecycle(lifecycle) {
  switch (lifecycle) {
    case 'editing':
      return 'editing';
    case 'playing':
      return 'playing';
    case 'saving':
      return 'saving';
    case 'exiting':
      return 'exiting';
    case 'error':
      return 'error';
    default:
      return lifecycle;
  }
}

/** 创建一个小世界会话。 */
export function createUgcWorldSession() {
  let lifecycle = 'created';
  let world = null;
  let snapshot = null;
  let sourceResources = cloneResources(EMPTY_RESOURCES);
  let resourceService = null;

  function updateResourceService() {
    resourceService = createUgcWorldResourceService({
      state: world,
      materials: world.resources.materials,
      fragments: world.resources.fragments,
      spentResources: world.spentResources,
    });
  }

  function ensureEntered() {
    if (!world || !resourceService) throw new Error('小世界会话尚未进入。');
  }

  const session = {
    /** 初始化或重置会话。 */
    create() {
      lifecycle = 'created';
      world = null;
      snapshot = null;
      sourceResources = cloneResources(EMPTY_RESOURCES);
      resourceService = null;
      return session;
    },

    /** 注入主世界材料和碎片；只写入会话缓存。 */
    loadResources(materials = [], fragments = []) {
      sourceResources = cloneResources({ materials, fragments });
      if (world && lifecycle !== 'disposed') {
        resourceService?.reset(materials, fragments);
      }
      return cloneResources(sourceResources);
    },

    /** 进入新世界或已保存世界。 */
    enter(mainWorldSnapshot = null, options = {}) {
      snapshot = cloneSnapshot(mainWorldSnapshot);
      lifecycle = 'preparing';

      if (options.worldData) {
        world = cloneUgcWorldState(options.worldData);
        // 存档中的剩余资源已经属于该小世界，加载后作为已付费库存恢复。
        for (const key of ['materials', 'fragments']) {
          world.resources[key].forEach((item) => {
            item.ownedQuantity = item.quantity;
          });
        }
        // 旧存档可能没有资源字段，新建资源时才使用主世界注入内容。
        if (options.worldData.resources === undefined) {
          world.resources = cloneResources(sourceResources);
        }
      } else {
        world = createUgcWorldState({
          id: options.worldId,
          name: options.name,
          spawn: options.spawn,
          resources: sourceResources,
        });
      }

      // 每次进入均从建造模式开始，试玩状态不作为输入锁定状态恢复。
      world.mode = 'build';
      world.dirty = false;
      updateResourceService();
      lifecycle = 'editing';

      return {
        state: 'entered',
        status: statusForLifecycle(lifecycle),
        resources: cloneResources(resourceService.getResources()),
        world: cloneUgcWorldState(world),
      };
    },

    /** 切换建造或试玩模式。 */
    setMode(mode) {
      ensureEntered();
      if (mode !== 'build' && mode !== 'play') return false;
      world.mode = mode;
      lifecycle = mode === 'play' ? 'playing' : 'editing';
      return true;
    },

    /** 标记保存成功。 */
    markSaved() {
      ensureEntered();
      world.dirty = false;
      lifecycle = world.mode === 'play' ? 'playing' : 'editing';
      return cloneUgcWorldState(world);
    },

    /** 设置保存生命周期状态。 */
    setSaving(value) {
      ensureEntered();
      lifecycle = value ? 'saving' : world.mode === 'play' ? 'playing' : 'editing';
    },

    /** 返回主世界快照副本。 */
    getSnapshot() {
      return cloneSnapshot(snapshot);
    },

    /** 返回小世界状态副本，供 UI 使用。 */
    getWorld() {
      return world ? cloneUgcWorldState(world) : null;
    },

    /** 返回会话唯一的可变状态，仅供内部控制器和建造器使用。 */
    getMutableWorld() {
      ensureEntered();
      return world;
    },

    /** 返回内部资源事务服务。 */
    getResourceService() {
      ensureEntered();
      return resourceService;
    },

    /** 返回生命周期和 UI 所需快照。 */
    getState() {
      return {
        state: lifecycle === 'editing' || lifecycle === 'playing' ? 'entered' : lifecycle,
        status: world ? statusForLifecycle(lifecycle) : lifecycle,
        resources: resourceService
          ? cloneResources(resourceService.getResources())
          : cloneResources(sourceResources),
        world: world ? cloneUgcWorldState(world) : null,
      };
    },

    /** 退出并返回主世界快照。 */
    exit() {
      const previousSnapshot = cloneSnapshot(snapshot);
      lifecycle = 'exited';
      return previousSnapshot;
    },

    /** 释放会话引用。 */
    dispose() {
      lifecycle = 'disposed';
      world = null;
      snapshot = null;
      resourceService = null;
      sourceResources = cloneResources(EMPTY_RESOURCES);
    },
  };

  return session;
}
