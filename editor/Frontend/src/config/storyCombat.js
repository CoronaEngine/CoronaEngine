import { storyWorldTerrainHeight } from './storyWorld.js';

export const STORY_MONSTER_PLAN_ID = 'story-combat-v1';
export const STORY_MONSTER_SCENE_VERSION = 2;
export const STORY_MONSTER_PREFIX = 'StoryMonster_';

export const STORY_PLAYER_MAX_HEALTH = 100;
export const STORY_PLAYER_DAMAGE_INVULNERABILITY_MS = 450;
export const STORY_PLAYER_RESPAWN_PROTECTION_MS = 3000;
export const STORY_PLAYER_ATTACK_DAMAGE = 10;
export const STORY_PLAYER_ATTACK_RANGE = 3;
export const STORY_PLAYER_ATTACK_COOLDOWN_MS = 600;
export const STORY_MINION_FRAGMENT_DROP_CHANCE = 0.35;
export const STORY_BOSS_RESPAWN_GAME_MS = 48 * 60 * 60 * 1000;

const groundSpawn = (x, z) => [x, storyWorldTerrainHeight(x, z), z];
const minionSpawns = [
  groundSpawn(-34, 10),
  groundSpawn(-23, 36),
  groundSpawn(5, 38),
  groundSpawn(25, -16),
  groundSpawn(36, -18),
  groundSpawn(-18, -28),
];

const guidFor = (index) => `f45c1001-71b4-4d34-9a31-${String(index).padStart(12, '0')}`;

const common = Object.freeze({
  actorType: 'model',
  entityType: 'monster',
  rotation: [0, 0, 0],
  physicsEnabled: false,
});

export const STORY_MONSTER_DEFINITIONS = Object.freeze([
  ...minionSpawns.map((position, index) =>
    Object.freeze({
      ...common,
      id: `minion-${index + 1}`,
      guid: guidFor(index + 1),
      name: `StoryMonster_Minion_${String(index + 1).padStart(2, '0')}`,
      displayName: `夜行山怪 ${index + 1}`,
      kind: 'minion',
      asset: 'monster_minion_v1.obj',
      importScale: 2.528661,
      position: Object.freeze(position),
      semanticRole: 'enemy_minion',
      maxHealth: 30,
      damage: 10,
      moveSpeed: 1.8,
      detectionRange: 12,
      attackRange: 1.8,
      attackCooldownMs: 1200,
      leashRange: 18,
      wanderRadius: 6,
    })
  ),
  Object.freeze({
    ...common,
    id: 'boss',
    guid: guidFor(99),
    name: 'StoryMonster_Boss',
    displayName: '山魈王',
    kind: 'boss',
    asset: 'monster_boss_v1.obj',
    importScale: 4.677942,
    position: Object.freeze(groundSpawn(30, -30)),
    semanticRole: 'enemy_boss',
    maxHealth: 200,
    damage: 20,
    moveSpeed: 0,
    detectionRange: 10,
    attackRange: 3.2,
    attackCooldownMs: 1800,
    leashRange: 0,
    wanderRadius: 0,
  }),
]);

export function createStoryMonsterActorData(definition) {
  return {
    actor_name: definition.name,
    name: definition.name,
    actor_guid: definition.guid,
    position: [...definition.position],
    rotation: [...definition.rotation],
    scale: [definition.importScale, definition.importScale, definition.importScale],
    semantic_role: definition.semanticRole,
    entity_type: definition.entityType,
    source_plan_id: STORY_MONSTER_PLAN_ID,
    source_scene_version: STORY_MONSTER_SCENE_VERSION,
    skip_if_exists: true,
    update_if_exists: true,
    physics_enabled: false,
  };
}
