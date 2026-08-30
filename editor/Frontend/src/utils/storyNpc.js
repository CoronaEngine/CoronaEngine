import { STORY_MERCHANT_DAILY_CHANCE } from '../config/storyNpc.js';

export const STORY_PROGRESS_VERSION = 2;

export function normalizeStoryNpcActors(actors) {
  return (Array.isArray(actors) ? actors : []).filter(Boolean);
}

export function findStoryNpcByRole(actors, role) {
  return normalizeStoryNpcActors(actors).find((actor) => String(actor.semantic_role || actor.semanticRole || '').toLowerCase() === role) || null;
}

export function distanceBetweenStoryPoints(a, b) {
  if (!Array.isArray(a) || !Array.isArray(b)) return Infinity;
  return Math.hypot(
    (Number(a[0]) || 0) - (Number(b[0]) || 0),
    (Number(a[1]) || 0) - (Number(b[1]) || 0),
    (Number(a[2]) || 0) - (Number(b[2]) || 0),
  );
}

export function merchantRollForDay(dayNumber, random = Math.random) {
  const day = Math.trunc(Number(dayNumber) || 0);
  if (day < 2) return false;
  return Number(random()) < STORY_MERCHANT_DAILY_CHANCE;
}

export function createWorldBallRecord(id, overrides = {}) {
  const value = String(id || 'demo-1').trim() || 'demo-1';
  const now = Number(overrides.createdAt) || Date.now();
  return {
    id: value,
    name: String(overrides.name || (value === 'demo-1' ? '我的第一个小世界' : `小世界 ${value}`)),
    sceneName: String(overrides.sceneName || `StoryDemo_${value.replace(/[^a-z0-9_-]/gi, '_')}`),
    status: ['empty', 'editing', 'playable'].includes(overrides.status) ? overrides.status : 'empty',
    actorCount: Math.max(0, Math.trunc(Number(overrides.actorCount) || 0)),
    coreInstalled: Math.max(0, Math.trunc(Number(overrides.coreInstalled) || 0)),
    playable: Boolean(overrides.playable),
    validation: Array.isArray(overrides.validation) ? overrides.validation.map(String) : [],
    isDefault: Boolean(overrides.isDefault),
    sourceBallId: overrides.sourceBallId ? String(overrides.sourceBallId) : '',
    createdAt: now,
    updatedAt: Number(overrides.updatedAt) || now,
  };
}

export function normalizeWorldBallRegistry(source = {}) {
  const records = [];
  const seen = new Set();
  const rawRecords = Array.isArray(source.worldBalls) ? source.worldBalls : [];
  for (const entry of rawRecords) {
    if (!entry || typeof entry !== 'object') continue;
    const id = String(entry.id || '').trim();
    if (!id || seen.has(id)) continue;
    seen.add(id);
    records.push(createWorldBallRecord(id, entry));
  }
  const legacy = Array.isArray(source.unlockedWorldBalls) ? source.unlockedWorldBalls : [];
  for (const rawId of legacy) {
    const id = String(rawId || '').trim();
    if (!id || seen.has(id)) continue;
    seen.add(id);
    records.push(createWorldBallRecord(id, { status: id === 'demo-1' ? 'editing' : 'empty' }));
  }
  if (records.length && !records.some((record) => record.isDefault)) records[0].isDefault = true;
  const requestedActive = String(source.activeWorldBallId || '').trim();
  const activeWorldBallId = records.some((record) => record.id === requestedActive)
    ? requestedActive
    : (records.find((record) => record.isDefault)?.id || records[0]?.id || '');
  return { records, activeWorldBallId };
}

export function normalizeStoryProgress(source = {}) {
  const value = source && typeof source === 'object' ? source : {};
  const registry = normalizeWorldBallRegistry(value);
  return {
    version: STORY_PROGRESS_VERSION,
    worldBalls: registry.records,
    unlockedWorldBalls: registry.records.map((record) => record.id),
    activeWorldBallId: registry.activeWorldBallId,
    completedQuestIds: Array.isArray(value.completedQuestIds) ? [...new Set(value.completedQuestIds.map(String))] : [],
    activeQuestId: value.activeQuestId ? String(value.activeQuestId) : null,
    merchantByDay: value.merchantByDay && typeof value.merchantByDay === 'object' ? { ...value.merchantByDay } : {},
    creatorUnlocked: Boolean(value.creatorUnlocked),
    questStats: value.questStats && typeof value.questStats === 'object' ? { ...value.questStats } : {},
    merchantPurchases: value.merchantPurchases && typeof value.merchantPurchases === 'object' ? { ...value.merchantPurchases } : {},
    updatedAt: Number(value.updatedAt) || 0,
  };
}

export function storyProgressStorageKey(projectKey) {
  return `corona.story.progress.v1:${encodeURIComponent(String(projectKey || 'active-project').trim().toLowerCase().replace(/\\/g, '/'))}`;
}
