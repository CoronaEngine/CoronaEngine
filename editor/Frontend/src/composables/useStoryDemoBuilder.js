import { computed, ref } from 'vue';
import { editorApi } from '@/api/editorApi.js';
import {
  STORY_DEMO_COMPONENTS,
  STORY_DEMO_CORE_NAME,
  STORY_DEMO_SLOT_TYPES,
  storyDemoActorName,
  storyDemoGeneratedActorId,
  storyDemoSceneName,
  storyDemoStorageKey,
  createEmptyStoryDemo,
  validateStoryCoreSlot,
} from '@/config/storyDemo.js';
import {
  addDemoActor,
  normalizeDemoActor,
  removeDemoActor,
} from '@/utils/storyDemo.js';

function browserStorage() {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

function resultData(value) {
  return value?.data?.data || value?.data || value || {};
}

function clone(value) {
  return JSON.parse(JSON.stringify(value));
}

function activeProjectPath(projectKey) {
  const value = String(projectKey || '').trim();
  return value && value.toLowerCase() !== 'active-project' ? value : '';
}

function assetRoot() {
  const location = String(window.location?.href || '')
    .split('#')[0]
    .split('?')[0]
    .replace(/^file:\/\//i, '');
  const normalized = location.replace(/\\/g, '/').replace(/^\//, '');
  const marker = normalized.toLowerCase().lastIndexOf('/frontend/');
  return marker >= 0 ? `${normalized.slice(0, marker)}/assets/story_mode` : '';
}

function actorName(actor) {
  return String(actor?.name || actor?.actor_name || '').trim();
}

function actorTransform(actor) {
  return {
    position: [...actor.position],
    rotation: [...actor.rotation],
    scale: [...actor.scale],
  };
}

export function useStoryDemoBuilder({ projectKey, worldBallId } = {}) {
  const key = storyDemoStorageKey(projectKey, worldBallId);
  const document = ref(createEmptyStoryDemo(projectKey, worldBallId));
  const selectedComponent = ref('object');
  const selectedActorId = ref('');
  const editMode = ref(true);
  const error = ref('');
  const notice = ref('');
  const history = ref([]);
  const sceneReady = ref(false);
  const syncing = ref(false);

  const load = () => {
    let source = null;
    try {
      const raw = browserStorage()?.getItem(key);
      if (raw) source = JSON.parse(raw);
    } catch {
      error.value = 'Demo 存档损坏，已使用空白 Demo。';
    }
    const base = createEmptyStoryDemo(projectKey, worldBallId);
    document.value = {
      ...base,
      ...(source && typeof source === 'object' ? source : {}),
      projectKey: base.projectKey,
      worldBallId: base.worldBallId,
      sceneName: base.sceneName,
      slots: { ...base.slots, ...(source?.slots || {}) },
      actors: Array.isArray(source?.actors) ? source.actors.map(normalizeDemoActor) : [],
    };
    editMode.value = document.value.mode !== 'play';
    return document.value;
  };

  const save = () => {
    try {
      const payload = { ...document.value, updatedAt: Date.now() };
      browserStorage()?.setItem(key, JSON.stringify(payload));
      document.value = payload;
      void Promise.resolve(editorApi.main.sceneSave(document.value.sceneName)).catch(() => {});
      notice.value = 'Demo 已保存。';
      return true;
    } catch {
      error.value = 'Demo 保存失败。';
      return false;
    }
  };

  const ensureCoreActor = async () => {
    const sceneName = document.value.sceneName;
    const root = assetRoot();
    try {
      const snapshot = resultData(await editorApi.scene.getSnapshot(sceneName));
      const actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
      if (actors.some((actor) => actorName(actor) === STORY_DEMO_CORE_NAME)) return false;
    } catch (_) {
      // A newly created scene can briefly have no snapshot. The idempotent create
      // call below is still safe because skip_if_exists is enabled.
    }
    await editorApi.sceneTools.createActor(
      sceneName,
      root ? `${root}/world_core_v1.obj` : 'world_core_v1.obj',
      'model',
      {
        actor_name: STORY_DEMO_CORE_NAME,
        name: STORY_DEMO_CORE_NAME,
        actor_guid: `story-demo-core-${document.value.worldBallId}`,
        position: [0, 0, 0],
        rotation: [0, 0, 0],
        scale: [1, 1, 1],
        semantic_role: 'story_world_core',
        entity_type: 'story_world_core',
        source_plan_id: 'story-demo-v1',
        source_scene_version: 1,
        skip_if_exists: true,
        update_if_exists: false,
        physics_enabled: false,
      },
    );
    return true;
  };

  const createComponentActor = async (slotType, item, actorOverride = {}) => {
    const component = STORY_DEMO_COMPONENTS[slotType];
    if (!component || !validateStoryCoreSlot(slotType, item)) return null;
    const actor = normalizeDemoActor({
      id: storyDemoGeneratedActorId(slotType),
      name: storyDemoActorName(worldBallId, storyDemoGeneratedActorId(slotType)),
      asset: component.asset,
      position: component.defaultPosition,
      rotation: [0, 0, 0],
      scale: component.scale,
      componentType: slotType,
      generatedBySlot: slotType,
      ...actorOverride,
    });
    const root = assetRoot();
    await editorApi.sceneTools.createActor(
      document.value.sceneName,
      root ? `${root}/${actor.asset}` : actor.asset,
      'model',
      {
        actor_name: actor.name,
        name: actor.name,
        actor_guid: `story-demo-${worldBallId}-${actor.id}`,
        position: actor.position,
        rotation: actor.rotation,
        scale: actor.scale,
        semantic_role: 'story_demo_actor',
        entity_type: 'story_demo_actor',
        component_type: slotType,
        generated_by_slot: slotType,
        source_plan_id: 'story-demo-v1',
        source_scene_version: 1,
        skip_if_exists: true,
        update_if_exists: false,
      },
    );
    return actor;
  };

  const reconcileInstalledSlots = async () => {
    let snapshot = {};
    try { snapshot = resultData(await editorApi.scene.getSnapshot(document.value.sceneName)); } catch (_) { /* create is still idempotent */ }
    const actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
    const existingNames = new Set(actors.map(actorName));
    let changed = false;
    const nextActors = Array.isArray(document.value.actors) ? [...document.value.actors] : [];
    for (const slotType of STORY_DEMO_SLOT_TYPES) {
      const item = document.value.slots?.[slotType];
      if (!validateStoryCoreSlot(slotType, item)) continue;
      const generatedId = storyDemoGeneratedActorId(slotType);
      const existingDocumentActor = nextActors.find((actor) => actor.id === generatedId || actor.generatedBySlot === slotType);
      const expectedName = storyDemoActorName(worldBallId, generatedId);
      if (!existingNames.has(expectedName)) {
        const actor = await createComponentActor(slotType, item, existingDocumentActor || {});
        if (actor && !existingDocumentActor) { nextActors.push(actor); changed = true; }
      } else if (!existingDocumentActor) {
        nextActors.push(normalizeDemoActor({
          id: generatedId,
          name: expectedName,
          asset: STORY_DEMO_COMPONENTS[slotType].asset,
          position: STORY_DEMO_COMPONENTS[slotType].defaultPosition,
          rotation: [0, 0, 0],
          scale: STORY_DEMO_COMPONENTS[slotType].scale,
          componentType: slotType,
          generatedBySlot: slotType,
        }));
        changed = true;
      }
    }
    if (changed) { document.value = { ...document.value, actors: nextActors }; save(); }
    return changed;
  };

  const ensureScene = async () => {
    const sceneName = document.value.sceneName;
    syncing.value = true;
    try {
      const projectPath = activeProjectPath(projectKey);
      let sceneExists = false;
      try {
        // Loading first makes repeated entries idempotent. The native create_scene
        // endpoint intentionally chooses a suffixed filename when the requested
        // scene already exists, so create-before-load would leak duplicate files.
        await editorApi.sceneTools.reloadScene(sceneName, projectPath);
        sceneExists = true;
      } catch (_) {
        // The first entry has no scene yet; create it below.
      }
      if (!sceneExists) {
        await editorApi.main.createScene(sceneName);
        await editorApi.sceneTools.reloadScene(sceneName, projectPath);
      }
      await ensureCoreActor();
      await reconcileInstalledSlots();
      sceneReady.value = true;
      error.value = '';
      return sceneName;
    } catch (cause) {
      sceneReady.value = false;
      error.value = cause?.message || '小世界场景加载失败。';
      throw cause;
    } finally {
      syncing.value = false;
    }
  };

  const chooseComponent = (type) => {
    if (STORY_DEMO_COMPONENTS[type]) selectedComponent.value = type;
  };

  const placeSelected = async () => {
    if (!editMode.value || syncing.value) return false;
    const component = STORY_DEMO_COMPONENTS[selectedComponent.value];
    if (!component) return false;
    const actor = normalizeDemoActor({
      id: `${Date.now()}-${Math.random().toString(36).slice(2, 7)}`,
      name: storyDemoActorName(worldBallId, Date.now()),
      asset: component.asset,
      position: [0, 0, 0],
      rotation: [0, 0, 0],
      scale: component.scale,
      componentType: selectedComponent.value,
    });
    const previous = clone(document.value);
    history.value.push(previous);
    document.value = addDemoActor(document.value, actor);
    selectedActorId.value = actor.id;
    try {
      const root = assetRoot();
      await editorApi.sceneTools.createActor(
        document.value.sceneName,
        root ? `${root}/${component.asset}` : component.asset,
        'model',
        {
          actor_name: actor.name,
          name: actor.name,
          actor_guid: `story-demo-${worldBallId}-${actor.id}`,
          position: actor.position,
          rotation: actor.rotation,
          scale: actor.scale,
          semantic_role: 'story_demo_actor',
          entity_type: 'story_demo_actor',
          component_type: selectedComponent.value,
          source_plan_id: 'story-demo-v1',
          source_scene_version: 1,
          skip_if_exists: true,
        },
      );
      save();
      notice.value = `${component.name} 已放置。`;
      return true;
    } catch (cause) {
      document.value = previous;
      history.value.pop();
      notice.value = `放置失败：${cause?.message || '引擎对象创建失败。'}`;
      return false;
    }
  };

  const updateActor = async (actorId, transform = {}) => {
    if (!editMode.value || syncing.value) return false;
    const index = document.value.actors.findIndex((actor) => String(actor.id) === String(actorId));
    if (index < 0) return false;
    const actor = document.value.actors[index];
    const next = normalizeDemoActor({
      ...actor,
      ...transform,
      id: actor.id,
      name: actor.name,
    });
    history.value.push(clone(document.value));
    document.value.actors.splice(index, 1, next);
    try {
      await editorApi.scene.setActorTransform(document.value.sceneName, actor.name, actorTransform(next));
      save();
      return true;
    } catch (cause) {
      document.value.actors.splice(index, 1, actor);
      history.value.pop();
      notice.value = `变换保存失败：${cause?.message || '未知错误'}`;
      return false;
    }
  };

  const deleteActor = async (actorId) => {
    if (!editMode.value || syncing.value) return false;
    const actor = document.value.actors.find((item) => String(item.id) === String(actorId));
    if (!actor) return false;
    history.value.push(clone(document.value));
    document.value = removeDemoActor(document.value, actorId);
    selectedActorId.value = '';
    try {
      await editorApi.sceneTools.removeActor(document.value.sceneName, actor.name);
      save();
      notice.value = '组件已删除。';
      return true;
    } catch (cause) {
      document.value = history.value[history.value.length - 1];
      history.value.pop();
      notice.value = `删除失败：${cause?.message || '未知错误'}`;
      return false;
    }
  };

  const installSlot = async (slotType, item) => {
    if (!editMode.value || syncing.value || !validateStoryCoreSlot(slotType, item)) return false;
    if (!STORY_DEMO_SLOT_TYPES.includes(slotType) || document.value.slots?.[slotType]) {
      notice.value = '该核心槽位已经安装组件。';
      return false;
    }
    const previous = clone(document.value);
    history.value.push(previous);
    try {
      const actor = await createComponentActor(slotType, item);
      if (!actor) throw new Error('组件定义不可用。');
      document.value = {
        ...document.value,
        slots: { ...document.value.slots, [slotType]: clone(item) },
        actors: [...document.value.actors, actor],
      };
      save();
      notice.value = `已安装${STORY_DEMO_COMPONENTS[slotType].name}组件。`;
      return true;
    } catch (cause) {
      document.value = previous;
      history.value.pop();
      notice.value = `安装失败：${cause?.message || '引擎对象创建失败。'}`;
      return false;
    }
  };

  const undo = async () => {
    if (!editMode.value || syncing.value) return false;
    const previous = history.value.pop();
    if (!previous) return false;
    const currentNames = new Set(document.value.actors.map(actorName));
    const previousNames = new Set(previous.actors.map(actorName));
    for (const actor of document.value.actors) {
      if (!previousNames.has(actorName(actor))) {
        try { await editorApi.sceneTools.removeActor(document.value.sceneName, actor.name); } catch (_) { /* best effort */ }
      }
    }
    const root = assetRoot();
    for (const actor of previous.actors) {
      if (!currentNames.has(actorName(actor))) {
        const component = STORY_DEMO_COMPONENTS[actor.componentType] || STORY_DEMO_COMPONENTS.object;
        try {
          await editorApi.sceneTools.createActor(
            document.value.sceneName,
            root ? `${root}/${actor.asset || component.asset}` : actor.asset || component.asset,
            'model',
            {
              actor_name: actor.name,
              name: actor.name,
              position: actor.position,
              rotation: actor.rotation,
              scale: actor.scale,
              semantic_role: 'story_demo_actor',
              source_plan_id: 'story-demo-v1',
              skip_if_exists: true,
            },
          );
        } catch (_) { /* the persisted document is still restored */ }
      } else {
        const current = document.value.actors.find((item) => actorName(item) === actorName(actor));
        if (current && JSON.stringify(actorTransform(current)) !== JSON.stringify(actorTransform(actor))) {
          try { await editorApi.scene.setActorTransform(document.value.sceneName, actor.name, actorTransform(actor)); } catch (_) { /* best effort */ }
        }
      }
    }
    document.value = clone(previous);
    selectedActorId.value = '';
    save();
    notice.value = '已撤销最近一次操作。';
    return true;
  };

  const toggleMode = () => {
    editMode.value = !editMode.value;
    document.value = { ...document.value, mode: editMode.value ? 'edit' : 'play' };
    save();
  };

  const componentList = computed(() =>
    Object.entries(STORY_DEMO_COMPONENTS).map(([id, value]) => ({ id, ...value })),
  );

  load();
  return {
    document,
    selectedComponent,
    selectedActorId,
    editMode,
    error,
    notice,
    history,
    sceneReady,
    syncing,
    componentList,
    load,
    save,
    ensureScene,
    reconcileInstalledSlots,
    chooseComponent,
    placeSelected,
    updateActor,
    deleteActor,
    installSlot,
    undo,
    toggleMode,
    sceneName: storyDemoSceneName(worldBallId),
  };
}
