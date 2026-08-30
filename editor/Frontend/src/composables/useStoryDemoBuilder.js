import { computed, ref } from 'vue';
import { editorApi } from '@/api/editorApi.js';
import {
  STORY_DEMO_COMPONENT_CATALOG,
  STORY_DEMO_CORE_NAME,
  STORY_DEMO_GIZMO_MODES,
  STORY_DEMO_SLOT_TYPES,
  createEmptyStoryDemo,
  legacyStoryDemoStorageKey,
  normalizeStoryDemoDocument,
  storyDemoActorName,
  storyDemoComponent,
  storyDemoSceneName,
  storyDemoStorageKey,
  validateStoryCoreSlot,
} from '@/config/storyDemo.js';
import {
  normalizeDemoActor,
  removeDemoActor,
  removeDemoCoreSlot,
  setDemoCoreSlot,
  validateStoryDemoForPlay,
} from '@/utils/storyDemo.js';

function browserStorage() { try { return window.localStorage; } catch { return null; } }
function resultData(value) { return value?.data?.data || value?.data || value || {}; }
function clone(value) { return JSON.parse(JSON.stringify(value)); }
function activeProjectPath(projectKey) { const value = String(projectKey || '').trim(); return value && value.toLowerCase() !== 'active-project' ? value : ''; }
function assetRoot() {
  const location = String(window.location?.href || '').split('#')[0].split('?')[0].replace(/^file:\/\//i, '');
  const normalized = location.replace(/\\/g, '/').replace(/^\//, '');
  const marker = normalized.toLowerCase().lastIndexOf('/frontend/');
  return marker >= 0 ? `${normalized.slice(0, marker)}/assets/story_mode` : '';
}
function actorName(actor) { return String(actor?.name || actor?.actor_name || '').trim(); }
function actorTransform(actor) { return { position: [...actor.position], rotation: [...actor.rotation], scale: [...actor.scale] }; }
function absoluteAsset(actor) {
  if (actor.customAsset || /^[a-z]:[\\/]/i.test(actor.asset) || String(actor.asset).startsWith('/')) return actor.asset;
  const root = assetRoot();
  return root ? `${root}/${actor.asset}` : actor.asset;
}
function uniqueId(prefix = 'actor') { return `${prefix}-${Date.now()}-${Math.random().toString(36).slice(2, 8)}`; }

export function useStoryDemoBuilder({ projectKey, worldBallId, onDocumentChanged } = {}) {
  const key = storyDemoStorageKey(projectKey, worldBallId);
  const legacyKey = legacyStoryDemoStorageKey(projectKey, worldBallId);
  const document = ref(createEmptyStoryDemo(projectKey, worldBallId));
  const selectedComponent = ref('house-small');
  const selectedActorId = ref('');
  const editMode = ref(true);
  const error = ref('');
  const notice = ref('');
  const history = ref([]);
  const redoHistory = ref([]);
  const sceneReady = ref(false);
  const syncing = ref(false);

  const notifyDocumentChanged = () => {
    const validation = validateStoryDemoForPlay(document.value);
    const nextStatus = validation.valid ? 'playable' : (document.value.actors.length ? 'editing' : 'empty');
    document.value = { ...document.value, status: nextStatus, validation: validation.errors, updatedAt: Date.now() };
    onDocumentChanged?.(clone(document.value));
  };

  const load = () => {
    let source = null;
    const store = browserStorage();
    try {
      const raw = store?.getItem(key) || store?.getItem(legacyKey);
      if (raw) source = JSON.parse(raw);
    } catch {
      error.value = 'Demo 存档损坏，已使用空白 Demo。';
    }
    document.value = normalizeStoryDemoDocument(source, projectKey, worldBallId);
    editMode.value = document.value.mode !== 'play';
    if (source && Number(source.version) < 2) save({ quiet: true });
    return document.value;
  };

  const save = ({ quiet = false } = {}) => {
    try {
      notifyDocumentChanged();
      browserStorage()?.setItem(key, JSON.stringify(document.value));
      void Promise.resolve(editorApi.main.sceneSave(document.value.sceneName)).catch(() => {});
      if (!quiet) notice.value = 'Demo 已保存。';
      return true;
    } catch {
      error.value = 'Demo 保存失败。';
      return false;
    }
  };

  const pushHistory = () => {
    history.value.push(clone(document.value));
    if (history.value.length > 100) history.value.shift();
    redoHistory.value = [];
  };

  const ensureCoreActor = async () => {
    const sceneName = document.value.sceneName;
    try {
      const snapshot = resultData(await editorApi.scene.getSnapshot(sceneName));
      if ((snapshot.actors || []).some((actor) => actorName(actor) === STORY_DEMO_CORE_NAME)) return false;
    } catch (_) { /* newly created scene */ }
    const root = assetRoot();
    await editorApi.sceneTools.createActor(sceneName, root ? `${root}/world_core_v1.obj` : 'world_core_v1.obj', 'model', {
      actor_name: STORY_DEMO_CORE_NAME,
      name: STORY_DEMO_CORE_NAME,
      actor_guid: `story-demo-core-${document.value.worldBallId}`,
      position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1],
      semantic_role: 'story_world_core', entity_type: 'story_world_core',
      source_plan_id: 'story-demo-v2', source_scene_version: 2,
      skip_if_exists: true, update_if_exists: false, physics_enabled: false,
    });
    return true;
  };

  const createNativeActor = async (actor) => editorApi.sceneTools.createActor(
    document.value.sceneName,
    absoluteAsset(actor),
    'model',
    {
      actor_name: actor.name,
      name: actor.name,
      actor_guid: actor.guid,
      position: actor.position,
      rotation: actor.rotation,
      scale: actor.scale,
      semantic_role: actor.system ? 'story_demo_system_marker' : 'story_demo_actor',
      entity_type: 'story_demo_actor',
      component_type: actor.componentType,
      component_id: actor.componentId,
      source_plan_id: 'story-demo-v2',
      source_scene_version: 2,
      skip_if_exists: true,
      update_if_exists: false,
      physics_enabled: actor.componentType === 'terrain',
    },
  );

  const reconcileActors = async () => {
    let snapshotActors = [];
    try { snapshotActors = resultData(await editorApi.scene.getSnapshot(document.value.sceneName)).actors || []; } catch (_) { /* best effort */ }
    const existing = new Map(snapshotActors.map((actor) => [actorName(actor), actor]));
    for (const actor of document.value.actors) {
      if (!existing.has(actor.name)) await createNativeActor(actor);
      else {
        const snapshot = existing.get(actor.name);
        const resource = String(snapshot?.resource_path || snapshot?.resource || snapshot?.asset || '');
        if (actor.asset && !actor.customAsset && resource && !resource.replace(/\\/g, '/').endsWith(actor.asset)) {
          try { await editorApi.sceneTools.rebindActorResource(document.value.sceneName, actor.name, absoluteAsset(actor), 'model'); } catch (_) { /* transform still restored below */ }
        }
        try { await editorApi.scene.setActorTransform(document.value.sceneName, actor.name, actorTransform(actor)); } catch (_) { /* best effort */ }
      }
      try {
        const handle = Number(existing.get(actor.name)?.handle || 0);
        if (handle > 0 && window.coronaBridge?.setProperty) window.coronaBridge.setProperty(handle, 3, !actor.paused && actor.visible !== false);
      } catch (_) { /* optional fast bridge */ }
    }
  };

  const ensureScene = async () => {
    syncing.value = true;
    try {
      const sceneName = document.value.sceneName;
      const projectPath = activeProjectPath(projectKey);
      let exists = false;
      try { await editorApi.sceneTools.reloadScene(sceneName, projectPath); exists = true; } catch (_) { /* create below */ }
      if (!exists) {
        await editorApi.main.createScene(sceneName);
        await editorApi.sceneTools.reloadScene(sceneName, projectPath);
      }
      await ensureCoreActor();
      await reconcileActors();
      sceneReady.value = true;
      error.value = '';
      return sceneName;
    } catch (cause) {
      sceneReady.value = false;
      error.value = cause?.message || '小世界场景加载失败。';
      throw cause;
    } finally { syncing.value = false; }
  };

  const componentList = computed(() => [
    ...STORY_DEMO_COMPONENT_CATALOG,
    ...(document.value.customAssets || []).map((entry) => ({ ...entry, category: 'object', customAsset: true })),
  ]);
  const availableComponents = computed(() => componentList.value.map((entry) => ({
    ...entry,
    unlocked: Boolean(document.value.slots?.[entry.category]),
  })));

  const chooseComponent = (id) => {
    if (storyDemoComponent(id, document.value.customAssets)) selectedComponent.value = id;
  };

  const placeComponent = async (componentId = selectedComponent.value, position = [0, 0, 0]) => {
    if (!editMode.value || syncing.value) return false;
    const component = storyDemoComponent(componentId, document.value.customAssets);
    if (!component) return false;
    if (!document.value.slots?.[component.category]) {
      notice.value = `请先在世界核心安装${component.category}碎片。`;
      return false;
    }
    const id = uniqueId(component.category);
    const actor = normalizeDemoActor({
      id,
      guid: `story-demo-${worldBallId}-${id}`,
      name: storyDemoActorName(worldBallId, id),
      asset: component.asset || component.path,
      position,
      rotation: component.rotation || [0, 0, 0],
      scale: component.scale || [1, 1, 1],
      componentId: component.id,
      componentType: component.category,
      customAsset: Boolean(component.customAsset),
      system: Boolean(component.system),
      gameplay: component.gameplay ? clone(component.gameplay) : null,
    });
    pushHistory();
    document.value.actors.push(actor);
    selectedActorId.value = actor.id;
    try {
      await createNativeActor(actor);
      save({ quiet: true });
      notice.value = `${component.name} 已放置。`;
      return actor;
    } catch (cause) {
      document.value.actors = document.value.actors.filter((entry) => entry.id !== actor.id);
      history.value.pop();
      notice.value = `放置失败：${cause?.message || '引擎对象创建失败。'}`;
      return false;
    }
  };

  const placeSelected = (position = [0, 0, 0]) => placeComponent(selectedComponent.value, position);

  const updateActor = async (actorId, transform = {}, { recordHistory = true, quiet = false } = {}) => {
    if (!editMode.value || syncing.value) return false;
    const index = document.value.actors.findIndex((actor) => String(actor.id) === String(actorId));
    if (index < 0) return false;
    const actor = document.value.actors[index];
    if (actor.paused) { notice.value = '该类别核心已移除，对象当前暂停编辑。'; return false; }
    const next = normalizeDemoActor({ ...actor, ...transform, id: actor.id, name: actor.name, guid: actor.guid });
    if (recordHistory) pushHistory();
    document.value.actors.splice(index, 1, next);
    try {
      await editorApi.scene.setActorTransform(document.value.sceneName, actor.name, actorTransform(next));
      save({ quiet: true });
      if (!quiet) notice.value = '变换已保存。';
      return true;
    } catch (cause) {
      document.value.actors.splice(index, 1, actor);
      if (recordHistory) history.value.pop();
      notice.value = `变换保存失败：${cause?.message || '未知错误'}`;
      return false;
    }
  };

  const deleteActor = async (actorId) => {
    if (!editMode.value || syncing.value) return false;
    const actor = document.value.actors.find((item) => String(item.id) === String(actorId));
    if (!actor || actor.system) { notice.value = '系统标记不能删除。'; return false; }
    pushHistory();
    document.value = removeDemoActor(document.value, actorId);
    selectedActorId.value = '';
    try {
      await editorApi.sceneTools.removeActor(document.value.sceneName, actor.name);
      save({ quiet: true }); notice.value = '组件已删除。'; return true;
    } catch (cause) {
      document.value = history.value.pop();
      notice.value = `删除失败：${cause?.message || '未知错误'}`;
      return false;
    }
  };

  const duplicateActor = async (actorId) => {
    const source = document.value.actors.find((actor) => String(actor.id) === String(actorId));
    if (!source || source.system || source.paused) return false;
    const id = uniqueId(source.componentType);
    const actor = normalizeDemoActor({
      ...clone(source), id, guid: `story-demo-${worldBallId}-${id}`,
      name: storyDemoActorName(worldBallId, id),
      position: [source.position[0] + 1, source.position[1], source.position[2] + 1],
    });
    pushHistory(); document.value.actors.push(actor);
    try { await createNativeActor(actor); selectedActorId.value = actor.id; save({ quiet: true }); notice.value = '对象已复制。'; return actor; }
    catch (cause) { document.value.actors.pop(); history.value.pop(); notice.value = `复制失败：${cause?.message || '未知错误'}`; return false; }
  };

  const installSlot = async (slotType, item) => {
    if (!editMode.value || syncing.value || !validateStoryCoreSlot(slotType, item)) return false;
    if (document.value.slots?.[slotType]) { notice.value = '该核心槽位已经安装。'; return false; }
    pushHistory();
    const result = setDemoCoreSlot(document.value, slotType, clone(item));
    document.value = result.document;
    save({ quiet: true });
    notice.value = `已解锁${slotType}组件。`;
    return true;
  };

  const uninstallSlot = (slotType) => {
    if (!editMode.value || syncing.value) return null;
    const result = removeDemoCoreSlot(document.value, slotType);
    if (!result.changed) return null;
    pushHistory(); document.value = result.document; save({ quiet: true });
    notice.value = `已移除${slotType}核心，同类对象已暂停。`;
    return result.item;
  };

  const importModel = async () => {
    if (!document.value.slots.object) { notice.value = '请先安装物体核心。'; return null; }
    try {
      const data = resultData(await editorApi.main.importResourceFile(document.value.sceneName, 'model'));
      const path = String(data.path || data.resource_path || data.resourcePath || data.file || '').trim();
      if (!path) throw new Error('未选择模型文件。');
      const id = `custom-${Math.abs([...path].reduce((hash, char) => ((hash * 31) + char.charCodeAt(0)) | 0, 7)).toString(36)}`;
      if (!document.value.customAssets.some((entry) => entry.id === id)) {
        document.value.customAssets.push({ id, name: String(data.name || path.split(/[\\/]/).pop() || '导入模型'), asset: path, path, category: 'object', scale: [1, 1, 1], customAsset: true });
        save({ quiet: true });
      }
      selectedComponent.value = id;
      notice.value = '模型已加入项目组件库。';
      return id;
    } catch (cause) { notice.value = cause?.message || '模型导入失败。'; return null; }
  };

  const applySnapshot = async (target) => {
    const current = clone(document.value);
    const targetDoc = normalizeStoryDemoDocument(target, projectKey, worldBallId);
    const currentNames = new Set(current.actors.map(actorName));
    const targetNames = new Set(targetDoc.actors.map(actorName));
    for (const actor of current.actors) if (!targetNames.has(actor.name)) { try { await editorApi.sceneTools.removeActor(current.sceneName, actor.name); } catch (_) {} }
    for (const actor of targetDoc.actors) {
      if (!currentNames.has(actor.name)) { try { await createNativeActor(actor); } catch (_) {} }
      else { try { await editorApi.scene.setActorTransform(current.sceneName, actor.name, actorTransform(actor)); } catch (_) {} }
    }
    document.value = targetDoc; selectedActorId.value = ''; save({ quiet: true });
  };

  const undo = async () => {
    if (!editMode.value || syncing.value || !history.value.length) return false;
    const previous = history.value.pop(); redoHistory.value.push(clone(document.value));
    await applySnapshot(previous); notice.value = '已撤销。'; return true;
  };
  const redo = async () => {
    if (!editMode.value || syncing.value || !redoHistory.value.length) return false;
    const next = redoHistory.value.pop(); history.value.push(clone(document.value));
    await applySnapshot(next); notice.value = '已重做。'; return true;
  };

  const setGizmoMode = (mode) => {
    if (!STORY_DEMO_GIZMO_MODES.includes(mode)) return false;
    document.value.editor.gizmoMode = mode; save({ quiet: true }); return true;
  };
  const setGameplay = (patch = {}) => { document.value.gameplay = { ...document.value.gameplay, ...patch }; save({ quiet: true }); };
  const setSpawn = (spawn = {}) => { document.value.spawn = { ...document.value.spawn, ...spawn }; save({ quiet: true }); };
  const setName = (name) => { document.value.name = String(name || '').trim() || document.value.name; save({ quiet: true }); };
  const setMode = (mode) => { editMode.value = mode !== 'play'; document.value.mode = editMode.value ? 'edit' : 'play'; save({ quiet: true }); };

  load();
  return {
    document, selectedComponent, selectedActorId, editMode, error, notice, history, redoHistory,
    sceneReady, syncing, componentList, availableComponents,
    load, save, ensureScene, reconcileInstalledSlots: reconcileActors, chooseComponent,
    placeComponent, placeSelected, updateActor, deleteActor, duplicateActor,
    installSlot, uninstallSlot, importModel, undo, redo, setGizmoMode, setGameplay, setSpawn, setName, setMode,
    validateForPlay: () => validateStoryDemoForPlay(document.value),
    sceneName: storyDemoSceneName(worldBallId),
  };
}
