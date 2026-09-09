/**
 * 小世界协调器：组合建造、独立试玩、资源事务、场景和项目存档。
 * 建造数据始终留在会话中；试玩使用副本；主世界只在磁盘保存成功后扣费。
 */
import { createUgcWorldBuilder } from './ugcWorldBuilder.js';
import { createUgcWorldLogicRunner } from './ugcWorldLogicRunner.js';
import { createUgcWorldMode } from './ugcWorldMode.js';
import { createUgcWorldPersistence } from './ugcWorldPersistence.js';
import { createUgcWorldScene } from './ugcWorldScene.js';
import { createUgcWorldSession } from './ugcWorldSession.js';
import { cloneUgcData, cloneUgcWorldState, createUgcId } from './ugcWorldState.js';

const emptyResources = () => ({ materials: [], fragments: [] });

function borrowedResources(items) {
  const copy = (item) => ({ ...cloneUgcData(item), ownedQuantity: 0 });
  return {
    materials: items.filter((item) => item.category === 'material' && item.quantity > 0).map(copy),
    fragments: items
      .filter((item) => item.category === 'ugc' && item.logic && item.quantity > 0)
      .map(copy),
  };
}

/** 创建独立控制器；可注入持久化和场景工厂进行无渲染器单元测试。 */
export function createUgcWorldController({
  persistence = createUgcWorldPersistence(),
  mainItems = [],
  onChanged,
  onMessage,
  sceneFactory = createUgcWorldScene,
} = {}) {
  const session = createUgcWorldSession();
  let sceneBundle = null;
  let builder = null;
  let mode = null;
  let logicRunner = null;
  let world = null;
  let playWorld = null;
  let playItems = [];
  let committedItems = [];
  let inventory = mainItems;
  let active = false;
  let entering = false;
  let saving = false;
  let disposed = false;
  let lastError = '';
  const enteredAreas = new Set();

  function activeWorld() {
    return playWorld || world;
  }

  function getViewState() {
    return {
      world: activeWorld() ? cloneUgcWorldState(activeWorld()) : null,
      resources: active ? session.getResourceService().getResources() : emptyResources(),
      playItems: cloneUgcData(playItems),
      mode: world?.mode || 'build',
      dirty: Boolean(world?.dirty),
      active,
      saving,
      error: lastError,
    };
  }

  function emitChanged() {
    if (!disposed) onChanged?.(getViewState());
  }

  function syncScene() {
    sceneBundle?.syncFromState(activeWorld());
    emitChanged();
  }

  function fail(error) {
    lastError = error?.message || String(error || '小世界操作失败。');
    emitChanged();
    return { ok: false, error: lastError };
  }

  function clearRuntime() {
    sceneBundle?.dispose?.();
    sceneBundle = null;
    builder = null;
    mode = null;
    logicRunner = null;
    world = null;
    playWorld = null;
    active = false;
    saving = false;
    enteredAreas.clear();
  }

  function hasPlayItem(id, quantity) {
    return playItems.some((item) => item.id === id && item.quantity >= quantity);
  }

  function startPlay() {
    // 每次试玩均从建造数据重置，目标生命、逻辑变量和奖励不会写回设计稿。
    playWorld = cloneUgcWorldState(world);
    playWorld.logicState = { completedTriggers: [], activeGoals: [], variables: {} };
    playWorld.playState = { started: true, completed: false, result: null };
    playItems = cloneUgcData(committedItems);
    enteredAreas.clear();
    const bindings = new Map();
    for (const object of playWorld.objects) {
      for (const id of object.fragmentIds) bindings.set(id, object.id);
    }
    const fragments = session
      .getResourceService()
      .getFragmentCatalog()
      .filter((fragment) => bindings.has(fragment.id));
    logicRunner = createUgcWorldLogicRunner({
      state: playWorld,
      fragments,
      fragmentBindings: bindings,
      onAddItem(itemId, quantity) {
        const item = playItems.find((entry) => entry.id === itemId);
        if (item) item.quantity += quantity;
        else playItems.push({ id: itemId, name: itemId, category: 'material', quantity });
        return true;
      },
      onRemoveItem(itemId, quantity) {
        const item = playItems.find((entry) => entry.id === itemId);
        if (!item || item.quantity < quantity) return false;
        item.quantity -= quantity;
        if (!item.quantity) playItems.splice(playItems.indexOf(item), 1);
        return true;
      },
      onMessage: (message) => onMessage?.({ type: 'message', message }),
    });
    runTrigger('onStart');
  }

  /** 只有试玩副本可以执行碎片逻辑，未绑定的碎片不会自动运行。 */
  function runTrigger(type, context = {}) {
    if (!active || saving || !playWorld || world.mode !== 'play')
      return { executed: 0, errors: [] };
    const result = logicRunner.runTrigger(type, {
      ...context,
      hasItem: context.hasItem || hasPlayItem,
    });
    if (result.executed || result.errors.length) syncScene();
    return result;
  }

  function savePayload() {
    const data = cloneUgcWorldState(world);
    data.mode = 'build';
    data.dirty = false;
    // 仅保存已付费且未放置的小世界库存；主世界借入的剩余材料不得复制进存档。
    data.resources = session.getResourceService().getOwnedResources();
    data.spentResources = emptyResources();
    for (const object of data.objects) {
      for (const key of ['materials', 'fragments']) {
        for (const item of object.cost[key]) {
          item.pendingQuantity = 0;
          item.ownedQuantity = item.quantity;
        }
      }
      object.cost.committed = true;
    }
    return data;
  }

  /** 显式保存：先校验扣费结果和完整存档，再写文件，最后提交内存背包。 */
  async function save() {
    if (!active || !world) return fail(new Error('小世界未进入。'));
    if (saving) return { ok: false, error: '小世界正在保存，请稍候。' };
    const service = session.getResourceService();
    let nextItems;
    let payload;
    try {
      nextItems = service.prepareCommit(inventory);
      payload = savePayload();
    } catch (error) {
      return fail(error);
    }
    saving = true;
    session.setSaving(true);
    lastError = '';
    emitChanged();
    try {
      const receipt = await persistence.saveWorld(world.id, payload);
      // 保存期间建造/模式/退出均被锁定；主世界模拟暂停，不会产生并发背包修改。
      inventory.splice(0, inventory.length, ...nextItems);
      committedItems = cloneUgcData(nextItems);
      service.markCommitted();
      session.markSaved();
      return { ok: true, receipt };
    } catch (error) {
      return fail(error);
    } finally {
      saving = false;
      session.setSaving(false);
      emitChanged();
      if (disposed) {
        clearRuntime();
        session.dispose();
      }
    }
  }

  function edit(operation) {
    if (!active || disposed) return { ok: false, error: '小世界未进入。' };
    if (saving) return { ok: false, error: '保存中不能修改小世界。' };
    const result = operation();
    lastError = result.ok ? '' : result.error;
    emitChanged();
    return result;
  }

  return {
    /** 创建或加载指定世界；进入失败会销毁局部运行时，主世界保持原样。 */
    async enter(options = {}) {
      if (active || entering || disposed) throw new Error('小世界会话已激活或已销毁。');
      entering = true;
      try {
        inventory = options.mainItems || mainItems;
        if (
          options.requireWorldOrb !== false &&
          !inventory.some((item) => item.id === 'world-orb-demo' && item.quantity > 0)
        ) {
          throw new Error('需要世界小球才能进入小世界。');
        }
        committedItems = cloneUgcData(inventory);
        const source = borrowedResources(inventory);
        let loaded = options.worldData || null;
        if (!loaded && options.worldId && options.loadExisting === true) {
          // 损坏/不存在的旧存档不可静默替换成同名空世界。
          loaded = await persistence.loadWorld(options.worldId);
        }
        if (disposed) throw new Error('小世界会话已销毁。');
        session.create();
        session.loadResources(source.materials, source.fragments);
        session.enter(options.mainWorldSnapshot || null, {
          worldId: options.worldId || createUgcId(),
          name: options.name,
          worldData: loaded,
        });
        world = session.getMutableWorld();
        if (loaded) {
          const service = session.getResourceService();
          const owned = service.getOwnedResources();
          const bound = new Set(world.objects.flatMap((object) => object.fragmentIds));
          const ownedFragmentIds = new Set(owned.fragments.map((item) => item.id));
          service.reset(
            [...owned.materials, ...source.materials],
            [
              ...owned.fragments,
              ...source.fragments.filter(
                (item) => !bound.has(item.id) && !ownedFragmentIds.has(item.id)
              ),
            ]
          );
        }
        world.dirty = !loaded;
        sceneBundle = sceneFactory(world, {
          onInteract: (target, context) =>
            runTrigger('onInteract', { ...context, targetId: target.userData.objectId }),
        });
        mode = createUgcWorldMode({ state: world });
        builder = createUgcWorldBuilder({
          state: world,
          resources: session.getResourceService(),
          onChanged: syncScene,
        });
        active = true;
        lastError = '';
        sceneBundle.setMode?.('build');
        syncScene();
        return getViewState();
      } catch (error) {
        clearRuntime();
        session.dispose();
        throw error;
      } finally {
        entering = false;
      }
    },
    /** 读取当前项目的小世界索引；路径解析由引擎侧完成。 */
    listWorlds: () => persistence.listWorlds(),
    getSceneBundle: () => sceneBundle,
    getWorld: () => (world ? cloneUgcWorldState(world) : null),
    getResources: () => (active ? session.getResourceService().getResources() : emptyResources()),
    getPlayItems: () => cloneUgcData(playItems),
    getMode: () => world?.mode || 'build',
    isDirty: () => Boolean(world?.dirty),
    getViewState,
    /** 切回建造时直接丢弃试玩副本，不保存目标伤害、胜利状态或试玩奖励。 */
    setMode(nextMode) {
      if (!active || saving || disposed || world.mode === nextMode || !mode.setMode(nextMode))
        return false;
      session.setMode(nextMode);
      if (nextMode === 'play') startPlay();
      else {
        playWorld = null;
        logicRunner = null;
        playItems = [];
      }
      sceneBundle.setMode?.(nextMode);
      syncScene();
      return true;
    },
    placeObject: (options) => edit(() => builder.placeObject(options)),
    removeObject: (id) => edit(() => builder.removeObject(id)),
    bindFragment: (objectId, fragmentId) => edit(() => builder.bindFragment(objectId, fragmentId)),
    runTrigger,
    /** 接收命中结果并触发攻击/击败事件，不暴露可变对象给 Vue。 */
    recordAttack(objectId, health) {
      const target = playWorld?.objects.find(
        (object) => object.id === objectId && object.type === 'target'
      );
      if (!target || saving || target.health <= 0) return false;
      target.health = Math.max(0, Math.min(100, health));
      runTrigger('onAttack', { targetId: objectId });
      if (target.health === 0) {
        target.targetState = 'defeated';
        runTrigger('onTargetDefeated', { targetId: objectId });
      }
      syncScene();
      return true;
    },
    /** 简单区域触发：进入目标周围 1.5 米时触发，离开后允许再次进入。 */
    updatePlayerPosition(position) {
      if (!playWorld || saving) return;
      for (const object of playWorld.objects) {
        if (object.type !== 'target') continue;
        const inside =
          Math.hypot(position.x - object.position[0], position.z - object.position[2]) <= 1.5;
        if (inside && !enteredAreas.has(object.id)) {
          enteredAreas.add(object.id);
          runTrigger('onEnterArea', { targetId: object.id });
        } else if (!inside) enteredAreas.delete(object.id);
      }
    },
    save,
    /** 未保存退出需要确认；已保存的扣费不会被进入时的旧背包快照回滚。 */
    async exit({ discard = false, save: shouldSave = false } = {}) {
      if (!active) return { ok: true, snapshot: null };
      if (saving) return { ok: false, error: '小世界正在保存，请稍候。' };
      if (world.dirty && !discard && !shouldSave) return { ok: false, requiresConfirmation: true };
      if (shouldSave) {
        const result = await save();
        if (!result.ok) return result;
      }
      const restored = session.exit();
      if (restored) {
        if ('items' in restored) restored.items = cloneUgcData(committedItems);
        if ('inventory' in restored) restored.inventory = cloneUgcData(committedItems);
      }
      clearRuntime();
      session.dispose();
      emitChanged();
      return { ok: true, snapshot: restored };
    },
    /** 保存中的请求不能取消：等待其完成提交后再释放会话，防止落盘成功却漏扣费。 */
    dispose() {
      disposed = true;
      if (!saving) {
        clearRuntime();
        session.dispose();
      }
    },
  };
}
