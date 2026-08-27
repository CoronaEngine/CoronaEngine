import assert from 'node:assert/strict';
import test from 'node:test';

import { buildPlayableDemoManifest } from '../../src/utils/storyDemo.js';


test('export manifest keeps each world ball and scene isolated and read-only', () => {
  const manifest = buildPlayableDemoManifest({
    name: '云溪村试玩',
    worldBallId: 'ball-2',
    sceneName: 'StoryDemo_ball-2',
    slots: { terrain: { itemId: 'enchanted_terrain_fragment' } },
    actors: [{ name: 'StoryDemo_ball-2_actor-1' }],
  });
  assert.equal(manifest.readOnly, true);
  assert.equal(manifest.worldBallId, 'ball-2');
  assert.equal(manifest.sceneName, 'StoryDemo_ball-2');
  assert.equal(manifest.actors[0].name, 'StoryDemo_ball-2_actor-1');
  assert.equal(manifest.coreSlots.terrain.itemId, 'enchanted_terrain_fragment');
});

test('empty demo data produces a valid package manifest shape', () => {
  const manifest = buildPlayableDemoManifest({});
  assert.equal(manifest.format, 'corona-story-demo');
  assert.equal(manifest.readOnly, true);
  assert.equal(manifest.worldBallId, '');
  assert.equal(manifest.sceneName, '');
  assert.deepEqual(manifest.actors, []);
});
