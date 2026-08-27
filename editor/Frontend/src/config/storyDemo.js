export const STORY_DEMO_VERSION = 1;
export const STORY_DEMO_PREFIX = 'StoryDemo_';
export const STORY_DEMO_CORE_NAME = 'StoryWorldCore_Demo';
export const STORY_DEMO_SCENE_PREFIX = 'StoryDemo_';
export const STORY_DEMO_SLOT_TYPES = Object.freeze(['terrain', 'object', 'enemy', 'objective']);
export const STORY_DEMO_COMPONENTS = Object.freeze({
  terrain: { id: 'terrain-basic', name: '基础地形', asset: 'terrain_v4.obj', scale: [0.35, 0.12, 0.35], defaultPosition: [0, 0, 0], generatedActorId: 'core-terrain' },
  object: { id: 'object-house', name: '村落房屋', asset: 'house_small_v4.obj', scale: [0.45, 0.45, 0.45], defaultPosition: [5, 0, 4], generatedActorId: 'core-object' },
  enemy: { id: 'enemy-minion', name: '夜行山怪', asset: 'monster_minion_v1.obj', scale: [0.6, 0.6, 0.6], defaultPosition: [9, 0, 8], generatedActorId: 'core-enemy' },
  objective: { id: 'objective-gate', name: '目标牌坊', asset: 'gate_v4.obj', scale: [0.35, 0.35, 0.35], defaultPosition: [0, 0, 12], generatedActorId: 'core-objective' },
});
export function storyDemoSceneName(worldBallId) { return `${STORY_DEMO_SCENE_PREFIX}${String(worldBallId || 'demo-1').replace(/[^a-z0-9_-]/gi, '_')}`; }
export function storyDemoStorageKey(projectKey, worldBallId) {
  const project = encodeURIComponent(String(projectKey || 'active-project').trim().toLowerCase().replace(/\\/g, '/'));
  return `corona.story.demo.v${STORY_DEMO_VERSION}:${project}:${String(worldBallId || 'demo-1')}`;
}
export function createEmptyStoryDemo(projectKey, worldBallId) {
  return { version: STORY_DEMO_VERSION, projectKey: String(projectKey || ''), worldBallId: String(worldBallId || 'demo-1'), sceneName: storyDemoSceneName(worldBallId), name: '我的剧情 Demo', slots: { terrain: null, object: null, enemy: null, objective: null }, actors: [], mode: 'edit', updatedAt: Date.now() };
}
export function normalizeStoryDemoDocument(source, projectKey, worldBallId) {
  const base = createEmptyStoryDemo(projectKey, worldBallId);
  if (!source || typeof source !== 'object' || Array.isArray(source)) return base;
  const slots = { ...base.slots, ...(source.slots && typeof source.slots === 'object' ? source.slots : {}) };
  return { ...base, ...source, version: STORY_DEMO_VERSION, projectKey: base.projectKey, worldBallId: base.worldBallId, sceneName: base.sceneName, slots, actors: Array.isArray(source.actors) ? source.actors : [] };
}
export function validateStoryCoreSlot(slotType, item) {
  return STORY_DEMO_SLOT_TYPES.includes(slotType)
    && String(item?.itemId || '').toLowerCase() === `enchanted_${slotType}_fragment`
    && String(item?.metadata?.enchantment?.componentType || '') === slotType;
}
export function storyDemoGeneratedActorId(slotType) {
  return `core-${String(slotType || '').trim().toLowerCase()}`;
}
export function storyDemoActorName(worldBallId, actorId) { return `${STORY_DEMO_PREFIX}${String(worldBallId || 'demo-1')}_${String(actorId || Date.now())}`; }
