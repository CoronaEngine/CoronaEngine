import assert from 'node:assert/strict';
import test from 'node:test';

import {
  createStoryMonsterActorData,
  STORY_BOSS_RESPAWN_GAME_MS,
  STORY_MONSTER_DEFINITIONS,
  STORY_MONSTER_PLAN_ID,
  STORY_PLAYER_ATTACK_COOLDOWN_MS,
  STORY_PLAYER_ATTACK_DAMAGE,
  STORY_PLAYER_DAMAGE_INVULNERABILITY_MS,
  STORY_PLAYER_MAX_HEALTH,
} from '../../src/config/storyCombat.js';
import { storyWorldTerrainHeight } from '../../src/config/storyWorld.js';
import {
  applyStoryDamage,
  canStoryAttack,
  canStoryReceiveDamage,
  clampStoryHealth,
  moveStoryPointTowards,
  normalizeStoryCombatProgress,
  shouldRespawnStoryBoss,
  storyCombatStorageKey,
  storyDistance3,
  storyHorizontalDistance,
  storyMonsterAiState,
  storyWanderPoint,
  storyYawTowards,
} from '../../src/utils/storyCombat.js';

test('combat definitions provide six minions and one fixed boss with unique identities', () => {
  const minions = STORY_MONSTER_DEFINITIONS.filter((definition) => definition.kind === 'minion');
  const bosses = STORY_MONSTER_DEFINITIONS.filter((definition) => definition.kind === 'boss');
  assert.equal(minions.length, 6);
  assert.equal(bosses.length, 1);
  assert.equal(new Set(STORY_MONSTER_DEFINITIONS.map((definition) => definition.guid)).size, 7);
  assert.equal(new Set(STORY_MONSTER_DEFINITIONS.map((definition) => definition.name)).size, 7);
  assert.ok(minions.every((definition) => definition.damage === 10 && definition.maxHealth === 30));
  assert.equal(bosses[0].damage, 20);
  assert.equal(bosses[0].maxHealth, 200);
  assert.equal(bosses[0].moveSpeed, 0);
});

test('monster actor data is idempotent, scaled and isolated from Story World actors', () => {
  for (const definition of STORY_MONSTER_DEFINITIONS) {
    const actor = createStoryMonsterActorData(definition);
    assert.equal(actor.source_plan_id, STORY_MONSTER_PLAN_ID);
    assert.equal(actor.skip_if_exists, true);
    assert.equal(actor.update_if_exists, true);
    assert.deepEqual(actor.scale, [
      definition.importScale,
      definition.importScale,
      definition.importScale,
    ]);
    assert.deepEqual(actor.position, definition.position);
    assert.ok(
      Math.abs(actor.position[1] - storyWorldTerrainHeight(actor.position[0], actor.position[2])) <=
        1e-9
    );
    assert.equal(actor.physics_enabled, false);
  }
});

test('health, player attack damage and monster damage clamp correctly', () => {
  assert.equal(clampStoryHealth(150), STORY_PLAYER_MAX_HEALTH);
  assert.equal(clampStoryHealth(-20), 0);
  assert.deepEqual(applyStoryDamage(100, STORY_PLAYER_ATTACK_DAMAGE, 30), {
    health: 20,
    damage: 10,
    dead: false,
  });
  assert.deepEqual(applyStoryDamage(10, 20, 100), { health: 0, damage: 10, dead: true });
  assert.equal(applyStoryDamage(100, 10, 100).health, 90);
  assert.equal(applyStoryDamage(100, 20, 100).health, 80);
});

test('attack cooldown, damage invulnerability and respawn protection use time edges', () => {
  assert.equal(
    canStoryAttack(Number.NEGATIVE_INFINITY, 1000, STORY_PLAYER_ATTACK_COOLDOWN_MS),
    true
  );
  assert.equal(canStoryAttack(1000, 1599, STORY_PLAYER_ATTACK_COOLDOWN_MS), false);
  assert.equal(canStoryAttack(1000, 1600, STORY_PLAYER_ATTACK_COOLDOWN_MS), true);
  assert.equal(canStoryReceiveDamage(1000, 0, 1200, STORY_PLAYER_DAMAGE_INVULNERABILITY_MS), false);
  assert.equal(canStoryReceiveDamage(1000, 0, 1450, STORY_PLAYER_DAMAGE_INVULNERABILITY_MS), true);
  assert.equal(
    canStoryReceiveDamage(1000, 2000, 1999, STORY_PLAYER_DAMAGE_INVULNERABILITY_MS),
    false
  );
});

test('boss progress survives valid saves and respawns after 48 game hours', () => {
  const defeatedAt = 6 * 60 * 60 * 1000;
  assert.equal(shouldRespawnStoryBoss(null, defeatedAt), true);
  assert.equal(
    shouldRespawnStoryBoss(defeatedAt, defeatedAt + STORY_BOSS_RESPAWN_GAME_MS - 1),
    false
  );
  assert.equal(shouldRespawnStoryBoss(defeatedAt, defeatedAt + STORY_BOSS_RESPAWN_GAME_MS), true);
  assert.equal(
    normalizeStoryCombatProgress({ bossDefeatedAtGameTimeMs: defeatedAt }).bossDefeatedAtGameTimeMs,
    defeatedAt
  );
  assert.equal(
    normalizeStoryCombatProgress({ bossDefeatedAtGameTimeMs: -1 }).bossDefeatedAtGameTimeMs,
    null
  );
  assert.equal(storyCombatStorageKey('D:\\Worlds\\A'), storyCombatStorageKey('d:/worlds/a/'));
});

test('monster state machine keeps minions active all day and covers movement states', () => {
  const base = {
    kind: 'minion',
    alive: true,
    distanceToPlayer: 20,
    distanceFromSpawn: 0,
    attackRange: 1.8,
    detectionRange: 12,
    leashRange: 18,
  };
  assert.equal(storyMonsterAiState({ ...base, alive: false }), 'dead');
  assert.equal(storyMonsterAiState(base), 'wander');
  assert.equal(storyMonsterAiState({ ...base, distanceToPlayer: 8 }), 'chase');
  assert.equal(storyMonsterAiState({ ...base, distanceToPlayer: 1.5 }), 'attack');
  assert.equal(storyMonsterAiState({ ...base, distanceFromSpawn: 19 }), 'return');
  assert.equal(storyMonsterAiState({ ...base, kind: 'boss', distanceToPlayer: 8 }), 'idle');
  assert.equal(storyMonsterAiState({ ...base, kind: 'boss', distanceToPlayer: 3 }), 'idle');
  assert.equal(storyMonsterAiState({ ...base, kind: 'boss', distanceToPlayer: 1.5 }), 'attack');
});

test('monster movement remains horizontal, bounded and deterministic', () => {
  const moved = moveStoryPointTowards([0, 2, 0], [10, 7, 0], 1.5);
  assert.deepEqual(moved.position, [1.5, 2, 0]);
  assert.equal(moved.reached, false);
  assert.equal(storyDistance3([0, 0, 0], [1, 2, 2]), 3);
  assert.equal(storyHorizontalDistance([0, 5, 0], [3, -8, 4]), 5);
  assert.equal(storyYawTowards([0, 0, 0], [1, 0, 0]), 90);

  const first = storyWanderPoint([4, 2, -3], 6, 1234);
  const second = storyWanderPoint([4, 2, -3], 6, 1234);
  assert.deepEqual(first, second);
  assert.equal(first.point[1], 2);
  assert.ok(storyHorizontalDistance(first.point, [4, 2, -3]) <= 6 + 1e-9);
});
