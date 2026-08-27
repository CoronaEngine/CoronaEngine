export const STORY_NPC_PLAN_ID = 'story-npc-v1';
export const STORY_NPC_SCENE_VERSION = 1;
export const STORY_NPC_PREFIX = 'StoryNpc_';
export const STORY_WORLD_BALL_PREFIX = 'StoryWorldBall_';
export const STORY_WORLD_CORE_PREFIX = 'StoryWorldCore_';

const fixed = Object.freeze([
  Object.freeze({
    id: 'quest', name: 'StoryNpc_Quest', displayName: '村口委托人',
    semanticRole: 'story_npc_quest', asset: 'npc_quest_v1.obj', position: [-8, 0.02, 3],
  }),
  Object.freeze({
    id: 'creator', name: 'StoryNpc_Creator', displayName: '创作师',
    semanticRole: 'story_npc_creator', asset: 'npc_creator_v1.obj', position: [4, 0.02, 16],
  }),
]);

export const STORY_NPC_DEFINITIONS = fixed;
export const STORY_MERCHANT_CANDIDATES = Object.freeze([
  Object.freeze([-30, 0.02, -18]), Object.freeze([18, 0.02, 18]), Object.freeze([0, 0.02, 34]),
]);
export const STORY_NPC_INTERACTION_RANGE = 4.5;
export const STORY_MERCHANT_DAILY_CHANCE = 0.3;
export const STORY_NPC_ASSET_FALLBACK = 'rock_v4.obj';

export const STORY_QUEST_DEFINITIONS = Object.freeze([
  Object.freeze({ id: 'slay-minions', title: '清理山怪', description: '击败 3 只小怪。', type: 'minions', target: 3, reward: { itemId: 'world_fragment', quantity: 2 } }),
  Object.freeze({ id: 'collect-fragments', title: '寻找世界碎片', description: '收集 3 个世界碎片。', type: 'fragments', target: 3, reward: { itemId: 'world_fragment', quantity: 1 } }),
  Object.freeze({ id: 'slay-boss', title: '村外的威胁', description: '击败山魈王。', type: 'boss', target: 1, reward: { itemId: 'world_ball', quantity: 1 } }),
]);

export const STORY_MERCHANT_STOCK = Object.freeze([
  Object.freeze({ itemId: 'world_fragment', quantity: 1, price: 2, currency: 'blue_crystal' }),
  Object.freeze({ itemId: 'enchanted_object_fragment', quantity: 1, price: 6, currency: 'blue_crystal' }),
]);

export function createStoryNpcActorData(definition, overrides = {}) {
  return {
    actor_name: definition.name, name: definition.name,
    actor_guid: overrides.guid || `story-npc-${definition.id}`,
    position: [...definition.position], rotation: [0, 0, 0], scale: [1, 1, 1],
    semantic_role: definition.semanticRole, entity_type: 'story_npc',
    source_plan_id: STORY_NPC_PLAN_ID, source_scene_version: STORY_NPC_SCENE_VERSION,
    skip_if_exists: true, update_if_exists: false, physics_enabled: false,
    ...overrides,
  };
}

export function createStoryWorldBallActorData(worldBallId = 'demo-1') {
  const id = String(worldBallId).trim() || 'demo-1';
  return {
    actor_name: `${STORY_WORLD_BALL_PREFIX}${id}`, name: `${STORY_WORLD_BALL_PREFIX}${id}`,
    actor_guid: `story-world-ball-${id}`, position: [0, 0.6, 8], rotation: [0, 0, 0], scale: [1, 1, 1],
    semantic_role: 'story_world_ball', entity_type: 'story_world_ball', source_plan_id: 'story-npc-v1',
    source_scene_version: 1, skip_if_exists: true, update_if_exists: false, physics_enabled: false,
  };
}
