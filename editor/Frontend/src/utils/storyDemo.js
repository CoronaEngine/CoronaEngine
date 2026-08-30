import {
  STORY_DEMO_SLOT_TYPES,
  migrateStoryDemoAsset,
  validateStoryCoreSlot,
} from '../config/storyDemo.js';

const vec3 = (value, fallback) => Array.isArray(value) && value.length >= 3
  ? value.slice(0, 3).map((entry, index) => Number.isFinite(Number(entry)) ? Number(entry) : fallback[index])
  : [...fallback];

export function normalizeDemoActor(actor = {}) {
  const componentType = STORY_DEMO_SLOT_TYPES.includes(String(actor.componentType))
    ? String(actor.componentType)
    : 'object';
  const customAsset = Boolean(actor.customAsset);
  return {
    ...actor,
    id: String(actor.id || Date.now()),
    guid: String(actor.guid || actor.actorGuid || `story-demo-${actor.id || Date.now()}`),
    name: String(actor.name || ''),
    asset: migrateStoryDemoAsset(actor.asset, customAsset),
    position: vec3(actor.position, [0, 0, 0]),
    rotation: vec3(actor.rotation, [0, 0, 0]),
    scale: vec3(actor.scale, [1, 1, 1]).map((value) => Math.max(0.01, value)),
    componentId: String(actor.componentId || ''),
    componentType,
    generatedBySlot: actor.generatedBySlot ? String(actor.generatedBySlot) : '',
    customAsset,
    system: Boolean(actor.system),
    paused: Boolean(actor.paused),
    visible: actor.visible !== false,
    gameplay: actor.gameplay && typeof actor.gameplay === 'object' ? { ...actor.gameplay } : null,
  };
}

export function addDemoActor(document, actor) {
  return { ...document, actors: [...(Array.isArray(document?.actors) ? document.actors : []), normalizeDemoActor(actor)] };
}

export function removeDemoActor(document, actorId) {
  return { ...document, actors: (document?.actors || []).filter((actor) => String(actor.id) !== String(actorId)) };
}

export function setDemoCoreSlot(document, slotType, item) {
  if (!STORY_DEMO_SLOT_TYPES.includes(slotType) || !validateStoryCoreSlot(slotType, item)) return { document, changed: false };
  return {
    document: {
      ...document,
      slots: { ...(document.slots || {}), [slotType]: item },
      actors: (document.actors || []).map((actor) => actor.componentType === slotType ? { ...actor, paused: false } : actor),
      updatedAt: Date.now(),
    },
    changed: true,
  };
}

export function removeDemoCoreSlot(document, slotType) {
  if (!STORY_DEMO_SLOT_TYPES.includes(slotType) || !document?.slots?.[slotType]) return { document, item: null, changed: false };
  const item = document.slots[slotType];
  return {
    document: {
      ...document,
      slots: { ...document.slots, [slotType]: null },
      actors: (document.actors || []).map((actor) => actor.componentType === slotType ? { ...actor, paused: true } : actor),
      updatedAt: Date.now(),
    },
    item,
    changed: true,
  };
}

export function validateStoryDemoForPlay(document = {}) {
  const errors = [];
  const actors = Array.isArray(document.actors) ? document.actors.filter((actor) => !actor.paused) : [];
  if (!document.slots?.terrain) errors.push('需要安装地形核心。');
  if (!actors.some((actor) => actor.componentType === 'terrain')) errors.push('需要放置有效地形。');
  if (!Array.isArray(document.spawn?.position) || document.spawn.position.length < 3) errors.push('需要设置玩家出生点。');
  const type = String(document.gameplay?.objectiveType || '');
  if (!['reach', 'kill', 'collect'].includes(type)) errors.push('需要选择试玩目标。');
  if (type === 'reach') {
    if (!Array.isArray(document.gameplay?.reachPosition) || !(Number(document.gameplay?.reachRadius) > 0)) errors.push('需要设置有效到达区域。');
  }
  if (type === 'kill' && !actors.some((actor) => actor.componentType === 'enemy')) errors.push('击杀目标需要至少一个敌人。');
  if (type === 'collect') {
    const count = actors.filter((actor) => actor.gameplay?.objectiveKind === 'collectible').length;
    if (count < Math.max(1, Number(document.gameplay?.targetCount) || 1)) errors.push('收集物数量不足。');
  }
  return { valid: errors.length === 0, errors };
}

export function buildPlayableDemoManifest(document = {}) {
  const validation = validateStoryDemoForPlay(document);
  const actors = Array.isArray(document.actors) ? document.actors.map(normalizeDemoActor) : [];
  const resources = [...new Set([
    ...actors.map((actor) => actor.asset).filter(Boolean),
    ...(Array.isArray(document.customAssets) ? document.customAssets.map((asset) => asset.path || asset.asset).filter(Boolean) : []),
  ])];
  return {
    format: 'corona-story-demo',
    version: 2,
    demoName: String(document.name || '未命名 Demo'),
    worldBallId: String(document.worldBallId || ''),
    sceneName: String(document.sceneName || ''),
    readOnly: true,
    packageOnly: true,
    launchSupported: false,
    coreSlots: document.slots || {},
    actors,
    playerSpawn: document.spawn || null,
    gameplay: document.gameplay || {},
    editor: document.editor || {},
    customAssets: Array.isArray(document.customAssets) ? document.customAssets : [],
    resourceDependencies: resources,
    playable: validation.valid,
    validationErrors: validation.errors,
    generatedAt: new Date().toISOString(),
  };
}

export function cloneStoryDemoDocument(document, targetWorldBallId, targetName) {
  const now = Date.now();
  const sourceActors = Array.isArray(document?.actors) ? document.actors : [];
  const actors = sourceActors.map((source, index) => {
    const id = `actor-${now}-${index + 1}`;
    return normalizeDemoActor({
      ...source,
      id,
      guid: `story-demo-${targetWorldBallId}-${now}-${index + 1}`,
      name: `StoryDemo_${targetWorldBallId}_${id}`,
    });
  });
  return {
    ...JSON.parse(JSON.stringify(document || {})),
    version: 2,
    worldBallId: String(targetWorldBallId),
    sceneName: `StoryDemo_${String(targetWorldBallId).replace(/[^a-z0-9_-]/gi, '_')}`,
    name: String(targetName || `${document?.name || 'Demo'} 副本`),
    actors,
    mode: 'edit',
    createdAt: now,
    updatedAt: now,
  };
}
