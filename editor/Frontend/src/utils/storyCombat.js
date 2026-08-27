import { STORY_BOSS_RESPAWN_GAME_MS, STORY_PLAYER_MAX_HEALTH } from '../config/storyCombat.js';
import { normalizeStoryGameProjectKey } from './storyGameClock.js';

export const STORY_COMBAT_PROGRESS_VERSION = 1;
export const STORY_COMBAT_STORAGE_PREFIX = 'corona.story.combat.v1:';

export function storyCombatStorageKey(projectPath) {
  return `${STORY_COMBAT_STORAGE_PREFIX}${encodeURIComponent(normalizeStoryGameProjectKey(projectPath))}`;
}

export function storyMonsterActorHandle(actor) {
  const handle = Number(actor?.handle ?? actor?.actor_handle ?? 0);
  return Number.isFinite(handle) && handle > 0 ? handle : 0;
}

export function storyMonsterActorGuid(actor) {
  return String(actor?.actor_guid ?? actor?.guid ?? '')
    .trim()
    .toLowerCase();
}

export function storyMonsterActorName(actor) {
  return String(actor?.name ?? actor?.actor_name ?? '')
    .trim()
    .toLowerCase();
}

export function storyMonsterActorLoadState(actor) {
  return String(actor?.load_status ?? actor?.loadStatus ?? '')
    .trim()
    .toLowerCase();
}

export function storyMonsterActorIsRenderable(actor) {
  if (!actor || storyMonsterActorHandle(actor) <= 0) return false;
  if (
    [
      'failed',
      'error',
      'invalid',
      'missing_resource',
      'decode_failed',
      'unsupported_resource',
      'loading',
      'pending',
      'queued',
      'unloaded',
    ].includes(storyMonsterActorLoadState(actor))
  )
    return false;
  if (actor?.render_failed === true || actor?.renderFailed === true) return false;
  if (actor?.render_ready === false || actor?.renderReady === false) return false;
  return true;
}

export function resolveStoryMonsterActors(definitions = [], actors = []) {
  const candidates = Array.isArray(actors) ? actors : [];
  return (Array.isArray(definitions) ? definitions : []).map((definition) => {
    const guid = String(definition?.guid || '')
      .trim()
      .toLowerCase();
    const name = String(definition?.name || '')
      .trim()
      .toLowerCase();
    return (
      candidates.find(
        (actor) =>
          (guid && storyMonsterActorGuid(actor) === guid) ||
          (name && storyMonsterActorName(actor) === name)
      ) || null
    );
  });
}

export function storyMonsterActorVisible(actor, fallback = true) {
  if (typeof actor?.visible === 'boolean') return actor.visible;
  if (typeof actor?.optics?.visible === 'boolean') return actor.optics.visible;
  return Boolean(fallback);
}

export function storyMonsterActorDiagnostic(actor, displayName = '怪物') {
  if (!actor || storyMonsterActorHandle(actor) <= 0) return `${displayName}尚未加载完成。`;
  if (actor?.render_failed === true || actor?.renderFailed === true) {
    return `${displayName}模型渲染失败。`;
  }
  if (
    [
      'failed',
      'error',
      'invalid',
      'missing_resource',
      'decode_failed',
      'unsupported_resource',
    ].includes(storyMonsterActorLoadState(actor))
  ) {
    return `${displayName}模型加载失败。`;
  }
  if (
    actor?.render_ready === false ||
    actor?.renderReady === false ||
    ['loading', 'pending', 'queued', 'unloaded'].includes(storyMonsterActorLoadState(actor))
  ) {
    return `${displayName}资源加载中。`;
  }
  return '';
}

export function normalizeStoryCombatProgress(document) {
  const source =
    document && typeof document === 'object' && !Array.isArray(document) ? document : {};
  const bossDefeatedAtGameTimeMs = Number(source.bossDefeatedAtGameTimeMs);
  return {
    version: STORY_COMBAT_PROGRESS_VERSION,
    bossDefeatedAtGameTimeMs:
      Number.isFinite(bossDefeatedAtGameTimeMs) && bossDefeatedAtGameTimeMs >= 0
        ? bossDefeatedAtGameTimeMs
        : null,
    updatedAt: Number.isFinite(Number(source.updatedAt)) ? Number(source.updatedAt) : 0,
  };
}

export function finiteStoryVector3(value, fallback = [0, 0, 0]) {
  if (!Array.isArray(value) || value.length < 3) return [...fallback];
  const result = value.slice(0, 3).map(Number);
  return result.every(Number.isFinite) ? result : [...fallback];
}

export function storyDistance3(a, b) {
  const av = finiteStoryVector3(a);
  const bv = finiteStoryVector3(b);
  return Math.hypot(av[0] - bv[0], av[1] - bv[1], av[2] - bv[2]);
}

export function storyHorizontalDistance(a, b) {
  const av = finiteStoryVector3(a);
  const bv = finiteStoryVector3(b);
  return Math.hypot(av[0] - bv[0], av[2] - bv[2]);
}

export function clampStoryHealth(value, maximum = STORY_PLAYER_MAX_HEALTH) {
  return Math.min(Math.max(Math.round(Number(value) || 0), 0), Math.max(Number(maximum) || 0, 0));
}

export function applyStoryDamage(currentHealth, damage, maximum = STORY_PLAYER_MAX_HEALTH) {
  const before = clampStoryHealth(currentHealth, maximum);
  const amount = Math.max(Math.round(Number(damage) || 0), 0);
  const health = clampStoryHealth(before - amount, maximum);
  return { health, damage: before - health, dead: health <= 0 };
}

export function canStoryAttack(lastAttackAt, now, cooldownMs) {
  const previous = Number(lastAttackAt);
  const current = Number(now);
  return (
    Number.isFinite(current) &&
    (!Number.isFinite(previous) || current - previous >= Math.max(Number(cooldownMs) || 0, 0))
  );
}

export function canStoryReceiveDamage(lastDamageAt, protectedUntil, now, invulnerabilityMs) {
  const current = Number(now);
  if (!Number.isFinite(current) || current < (Number(protectedUntil) || 0)) return false;
  const previous = Number(lastDamageAt);
  return (
    !Number.isFinite(previous) || current - previous >= Math.max(Number(invulnerabilityMs) || 0, 0)
  );
}

export function shouldRespawnStoryBoss(defeatedAtGameTimeMs, totalGameTimeMs) {
  if (
    defeatedAtGameTimeMs === null ||
    defeatedAtGameTimeMs === undefined ||
    defeatedAtGameTimeMs === ''
  ) {
    return true;
  }
  const defeatedAt = Number(defeatedAtGameTimeMs);
  const current = Number(totalGameTimeMs);
  if (!Number.isFinite(defeatedAt) || defeatedAt < 0) return true;
  return Number.isFinite(current) && current - defeatedAt >= STORY_BOSS_RESPAWN_GAME_MS;
}

export function storyMonsterAiState({
  kind,
  alive,
  distanceToPlayer,
  distanceFromSpawn,
  attackRange,
  detectionRange,
  leashRange,
}) {
  if (!alive) return 'dead';
  if (distanceToPlayer <= attackRange) return 'attack';
  if (kind === 'boss') return 'idle';
  if (distanceFromSpawn > leashRange) return 'return';
  if (distanceToPlayer <= detectionRange) return 'chase';
  return 'wander';
}

export function moveStoryPointTowards(position, target, maximumDistance) {
  const from = finiteStoryVector3(position);
  const to = finiteStoryVector3(target, from);
  const dx = to[0] - from[0];
  const dz = to[2] - from[2];
  const distance = Math.hypot(dx, dz);
  if (distance <= 1e-6 || maximumDistance <= 0)
    return { position: from, reached: distance <= 1e-6 };
  const amount = Math.min(Math.max(Number(maximumDistance) || 0, 0), distance);
  return {
    position: [from[0] + (dx / distance) * amount, from[1], from[2] + (dz / distance) * amount],
    reached: amount >= distance - 1e-6,
  };
}

export function storyYawTowards(position, target) {
  const from = finiteStoryVector3(position);
  const to = finiteStoryVector3(target, from);
  return (Math.atan2(to[0] - from[0], to[2] - from[2]) * 180) / Math.PI;
}

export function storyWanderPoint(spawn, radius, seed) {
  const origin = finiteStoryVector3(spawn);
  let value = Math.trunc(Number(seed) || 1) >>> 0 || 1;
  value = (value * 1664525 + 1013904223) >>> 0;
  const angle = (value / 0xffffffff) * Math.PI * 2;
  value = (value * 1664525 + 1013904223) >>> 0;
  const distance = Math.sqrt(value / 0xffffffff) * Math.max(Number(radius) || 0, 0);
  return {
    point: [
      origin[0] + Math.sin(angle) * distance,
      origin[1],
      origin[2] + Math.cos(angle) * distance,
    ],
    seed: value,
  };
}
