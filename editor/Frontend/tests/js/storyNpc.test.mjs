import assert from 'node:assert/strict';
import test from 'node:test';

import {
  distanceBetweenStoryPoints,
  findStoryNpcByRole,
  merchantRollForDay,
  normalizeStoryProgress,
  storyProgressStorageKey,
} from '../../src/utils/storyNpc.js';


test('merchant does not refresh on day one and uses the configured daily chance afterwards', () => {
  assert.equal(merchantRollForDay(1, () => 0), false);
  assert.equal(merchantRollForDay(2, () => 0.299), true);
  assert.equal(merchantRollForDay(2, () => 0.3), false);
  assert.equal(merchantRollForDay(3, () => 0.1), true);
});

test('normalizes NPC roles and chooses the closest interaction target data', () => {
  const actors = [
    { name: 'Quest', semantic_role: 'story_npc_quest' },
    { name: 'Creator', semanticRole: 'story_npc_creator' },
  ];
  assert.equal(findStoryNpcByRole(actors, 'story_npc_creator')?.name, 'Creator');
  assert.equal(findStoryNpcByRole(actors, 'story_npc_missing'), null);
  assert.equal(distanceBetweenStoryPoints([0, 0, 0], [3, 4, 0]), 5);
  assert.equal(distanceBetweenStoryPoints(null, [0, 0, 0]), Infinity);
});

test('progress storage is project-isolated and corrupted shapes fall back safely', () => {
  const keyA = storyProgressStorageKey('D:\\Games\\StoryWorld');
  const keyB = storyProgressStorageKey('D:\\Games\\OtherWorld');
  assert.notEqual(keyA, keyB);
  const progress = normalizeStoryProgress({
    unlockedWorldBalls: ['demo-1', 'demo-1', 2],
    completedQuestIds: 'not-an-array',
    merchantByDay: { 2: true },
  });
  assert.deepEqual(progress.unlockedWorldBalls, ['demo-1', '2']);
  assert.deepEqual(progress.completedQuestIds, []);
  assert.equal(progress.merchantByDay['2'], true);
  assert.equal(normalizeStoryProgress(null).activeQuestId, null);
});
