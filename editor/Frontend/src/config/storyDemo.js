export const STORY_DEMO_VERSION = 2;
export const STORY_DEMO_PREFIX = 'StoryDemo_';
export const STORY_DEMO_CORE_NAME = 'StoryWorldCore_Demo';
export const STORY_DEMO_SCENE_PREFIX = 'StoryDemo_';
export const STORY_DEMO_SLOT_TYPES = Object.freeze(['terrain', 'object', 'enemy', 'objective']);
export const STORY_DEMO_GIZMO_MODES = Object.freeze(['translate', 'rotate', 'scale']);
export const STORY_DEMO_LEGACY_ASSET_MAP = Object.freeze({
  'terrain_v4.obj': 'terrain_v5.obj',
  'house_small_v4.obj': 'house_small_v5.obj',
  'house_large_v4.obj': 'house_large_v5.obj',
  'bridge_v4.obj': 'bridge_v5.obj',
  'gate_v4.obj': 'gate_v5.obj',
  'pavilion_v4.obj': 'pavilion_v5.obj',
  'tree_v4.obj': 'tree_v5_a.obj',
  'rock_v4.obj': 'rock_v5.obj',
  'fence_v4.obj': 'fence_v5.obj',
  'lantern_v4.obj': 'lantern_v5.obj',
});

const component = (value) => Object.freeze({
  category: 'object',
  scale: Object.freeze([1, 1, 1]),
  defaultPosition: Object.freeze([0, 0, 0]),
  rotation: Object.freeze([0, 0, 0]),
  gameplay: null,
  ...value,
});

export const STORY_DEMO_COMPONENT_CATALOG = Object.freeze([
  component({ id: 'terrain-village', name: '云溪地形', category: 'terrain', asset: 'terrain_v5.obj', scale: Object.freeze([0.42, 0.18, 0.42]), generatedActorId: 'core-terrain', system: true }),
  component({ id: 'house-small', name: '村落小屋', asset: 'house_small_v5.obj', scale: Object.freeze([0.8, 0.8, 0.8]) }),
  component({ id: 'house-large', name: '村落大屋', asset: 'house_large_v5.obj', scale: Object.freeze([0.72, 0.72, 0.72]) }),
  component({ id: 'bridge', name: '木桥', asset: 'bridge_v5.obj', scale: Object.freeze([0.72, 0.72, 0.72]) }),
  component({ id: 'gate', name: '牌坊', asset: 'gate_v5.obj', scale: Object.freeze([0.72, 0.72, 0.72]) }),
  component({ id: 'pavilion', name: '亭子', asset: 'pavilion_v5.obj', scale: Object.freeze([0.76, 0.76, 0.76]) }),
  component({ id: 'tree-a', name: '树木 A', asset: 'tree_v5_a.obj', scale: Object.freeze([0.9, 0.9, 0.9]) }),
  component({ id: 'tree-b', name: '树木 B', asset: 'tree_v5_b.obj', scale: Object.freeze([0.9, 0.9, 0.9]) }),
  component({ id: 'rock', name: '岩石', asset: 'rock_v5.obj', scale: Object.freeze([1, 1, 1]) }),
  component({ id: 'fence', name: '围栏', asset: 'fence_v5.obj', scale: Object.freeze([0.85, 0.85, 0.85]) }),
  component({ id: 'lantern', name: '灯笼', asset: 'lantern_v5.obj', scale: Object.freeze([0.9, 0.9, 0.9]) }),
  component({ id: 'courtyard', name: '庭院', asset: 'courtyard_v5.obj', scale: Object.freeze([0.8, 0.8, 0.8]) }),
  component({ id: 'barrels', name: '木桶', asset: 'barrels_v5.obj', scale: Object.freeze([0.9, 0.9, 0.9]) }),
  component({ id: 'woodpile', name: '柴堆', asset: 'woodpile_v5.obj', scale: Object.freeze([0.9, 0.9, 0.9]) }),
  component({ id: 'enemy-minion', name: '山怪', category: 'enemy', asset: 'monster_minion_v1.obj', scale: Object.freeze([0.8, 0.8, 0.8]), gameplay: Object.freeze({ enemyType: 'minion', maxHealth: 30, damage: 10, speed: 1.8, aggroRange: 12, attackRange: 1.8, attackCooldown: 1.2 }) }),
  component({ id: 'enemy-boss', name: '山魈王', category: 'enemy', asset: 'monster_boss_v1.obj', scale: Object.freeze([0.9, 0.9, 0.9]), gameplay: Object.freeze({ enemyType: 'boss', maxHealth: 200, damage: 20, speed: 1.1, aggroRange: 16, attackRange: 3.2, attackCooldown: 1.8 }) }),
  component({ id: 'objective-reach', name: '到达目标区', category: 'objective', marker: true, asset: 'world_ball_v1.obj', scale: Object.freeze([0.8, 0.12, 0.8]), gameplay: Object.freeze({ objectiveKind: 'reach', radius: 2.5 }) }),
  component({ id: 'objective-collectible', name: '收集物', category: 'objective', asset: 'world_ball_v1.obj', scale: Object.freeze([0.22, 0.22, 0.22]), gameplay: Object.freeze({ objectiveKind: 'collectible' }) }),
  component({ id: 'player-spawn', name: '玩家出生点', category: 'objective', marker: true, system: true, asset: 'world_ball_v1.obj', scale: Object.freeze([0.3, 0.08, 0.3]), gameplay: Object.freeze({ objectiveKind: 'spawn' }) }),
]);

const componentIndex = Object.fromEntries(
  STORY_DEMO_COMPONENT_CATALOG.map((entry) => [entry.id, entry]),
);
export const STORY_DEMO_COMPONENTS = Object.freeze({
  ...componentIndex,
  terrain: componentIndex['terrain-village'],
  object: componentIndex['house-small'],
  enemy: componentIndex['enemy-minion'],
  objective: componentIndex['objective-reach'],
});

export const STORY_DEMO_DEFAULT_COMPONENT_BY_SLOT = Object.freeze({
  terrain: 'terrain-village',
  object: 'house-small',
  enemy: 'enemy-minion',
  objective: 'objective-reach',
});

export function storyDemoSceneName(worldBallId) {
  return `${STORY_DEMO_SCENE_PREFIX}${String(worldBallId || 'demo-1').replace(/[^a-z0-9_-]/gi, '_')}`;
}

function normalizedProjectKey(projectKey) {
  return encodeURIComponent(String(projectKey || 'active-project').trim().toLowerCase().replace(/\\/g, '/'));
}

export function storyDemoStorageKey(projectKey, worldBallId) {
  return `corona.story.demo.v${STORY_DEMO_VERSION}:${normalizedProjectKey(projectKey)}:${String(worldBallId || 'demo-1')}`;
}

export function legacyStoryDemoStorageKey(projectKey, worldBallId) {
  return `corona.story.demo.v1:${normalizedProjectKey(projectKey)}:${String(worldBallId || 'demo-1')}`;
}

export function createEmptyStoryDemo(projectKey, worldBallId, options = {}) {
  const now = Number(options.now) || Date.now();
  return {
    version: STORY_DEMO_VERSION,
    projectKey: String(projectKey || ''),
    worldBallId: String(worldBallId || 'demo-1'),
    sceneName: storyDemoSceneName(worldBallId),
    name: String(options.name || '我的剧情 Demo'),
    slots: { terrain: null, object: null, enemy: null, objective: null },
    actors: [],
    spawn: { position: [0, 1.6, -8], forward: [0, -0.08, 1] },
    gameplay: {
      objectiveType: 'reach',
      targetActorIds: [],
      targetCount: 1,
      reachPosition: [0, 0, 10],
      reachRadius: 2.5,
      playerMaxHealth: 100,
    },
    editor: {
      gridSize: 0.5,
      rotationSnap: 15,
      scaleSnap: 0.1,
      snapEnabled: true,
      gizmoMode: 'translate',
      cameraPose: null,
    },
    customAssets: [],
    mode: 'edit',
    status: 'empty',
    validation: [],
    createdAt: now,
    updatedAt: now,
  };
}

const vec3 = (value, fallback) => Array.isArray(value) && value.length >= 3
  ? value.slice(0, 3).map((entry, index) => Number.isFinite(Number(entry)) ? Number(entry) : fallback[index])
  : [...fallback];

export function migrateStoryDemoAsset(asset, custom = false) {
  const normalized = String(asset || '');
  return custom ? normalized : (STORY_DEMO_LEGACY_ASSET_MAP[normalized] || normalized);
}

export function normalizeStoryDemoDocument(source, projectKey, worldBallId) {
  const base = createEmptyStoryDemo(projectKey, worldBallId, {
    name: source?.name,
    now: source?.createdAt,
  });
  if (!source || typeof source !== 'object' || Array.isArray(source)) return base;
  const slots = { ...base.slots, ...(source.slots && typeof source.slots === 'object' ? source.slots : {}) };
  const actors = Array.isArray(source.actors) ? source.actors.map((actor) => ({
    ...actor,
    asset: migrateStoryDemoAsset(actor?.asset, Boolean(actor?.customAsset)),
    position: vec3(actor?.position, [0, 0, 0]),
    rotation: vec3(actor?.rotation, [0, 0, 0]),
    scale: vec3(actor?.scale, [1, 1, 1]).map((value) => Math.max(0.01, value)),
  })) : [];
  const editor = { ...base.editor, ...(source.editor && typeof source.editor === 'object' ? source.editor : {}) };
  if (!STORY_DEMO_GIZMO_MODES.includes(editor.gizmoMode)) editor.gizmoMode = 'translate';
  const gameplay = { ...base.gameplay, ...(source.gameplay && typeof source.gameplay === 'object' ? source.gameplay : {}) };
  const spawn = {
    ...base.spawn,
    ...(source.spawn && typeof source.spawn === 'object' ? source.spawn : {}),
    position: vec3(source.spawn?.position, base.spawn.position),
    forward: vec3(source.spawn?.forward, base.spawn.forward),
  };
  return {
    ...base,
    ...source,
    version: STORY_DEMO_VERSION,
    projectKey: base.projectKey,
    worldBallId: base.worldBallId,
    sceneName: base.sceneName,
    slots,
    actors,
    spawn,
    gameplay,
    editor,
    customAssets: Array.isArray(source.customAssets) ? source.customAssets.filter(Boolean) : [],
    status: ['empty', 'editing', 'playable'].includes(source.status) ? source.status : (actors.length ? 'editing' : 'empty'),
    validation: Array.isArray(source.validation) ? source.validation.map(String) : [],
    createdAt: Number(source.createdAt) || base.createdAt,
    updatedAt: Number(source.updatedAt) || base.updatedAt,
  };
}

export function validateStoryCoreSlot(slotType, item) {
  return STORY_DEMO_SLOT_TYPES.includes(slotType)
    && String(item?.itemId || '').toLowerCase() === `enchanted_${slotType}_fragment`
    && String(item?.metadata?.enchantment?.componentType || slotType) === slotType;
}

export function storyDemoGeneratedActorId(slotType) {
  return `core-${String(slotType || '').trim().toLowerCase()}`;
}

export function storyDemoActorName(worldBallId, actorId) {
  return `${STORY_DEMO_PREFIX}${String(worldBallId || 'demo-1')}_${String(actorId || Date.now())}`;
}

export function storyDemoComponent(componentId, customAssets = []) {
  if (STORY_DEMO_COMPONENTS[componentId]) return STORY_DEMO_COMPONENTS[componentId];
  const custom = customAssets.find((entry) => String(entry?.id) === String(componentId));
  return custom ? { ...custom, category: 'object', customAsset: true } : null;
}
