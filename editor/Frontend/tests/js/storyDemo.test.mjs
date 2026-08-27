import assert from 'node:assert/strict';
import test from 'node:test';

import {
  STORY_DEMO_COMPONENTS,
  STORY_DEMO_SLOT_TYPES,
  storyDemoActorName,
  storyDemoGeneratedActorId,
  validateStoryCoreSlot,
} from '../../src/config/storyDemo.js';
import { buildPlayableDemoManifest, normalizeDemoActor } from '../../src/utils/storyDemo.js';

test('validates enchanted fragments and keeps ordinary fragments out of core slots', () => {
  for (const slotType of STORY_DEMO_SLOT_TYPES) {
    assert.equal(validateStoryCoreSlot(slotType, {
      itemId: `enchanted_${slotType}_fragment`,
      metadata: { enchantment: { componentType: slotType } },
    }), true);
    assert.equal(validateStoryCoreSlot(slotType, {
      itemId: 'world_fragment',
      metadata: { enchantment: { componentType: slotType } },
    }), false);
    assert.equal(validateStoryCoreSlot(slotType, {
      itemId: `enchanted_${slotType}_fragment`,
      metadata: { enchantment: { componentType: 'other' } },
    }), false);
  }
});

test('uses stable generated actor identities for installed slots', () => {
  for (const slotType of STORY_DEMO_SLOT_TYPES) {
    const id = storyDemoGeneratedActorId(slotType);
    assert.equal(id, `core-${slotType}`);
    assert.equal(storyDemoActorName('ball-1', id), `StoryDemo_ball-1_${id}`);
    assert.ok(STORY_DEMO_COMPONENTS[slotType].asset);
  }
});

test('normalizes demo actors and emits an isolated read-only manifest', () => {
  const actor = normalizeDemoActor({ id: 'a', name: 'Actor', position: ['1', 2, 3] });
  assert.deepEqual(actor.position, [1, 2, 3]);
  const manifest = buildPlayableDemoManifest({
    name: '测试 Demo', worldBallId: 'ball-1', sceneName: 'StoryDemo_ball-1', actors: [actor],
  });
  assert.equal(manifest.format, 'corona-story-demo');
  assert.equal(manifest.readOnly, true);
  assert.equal(manifest.actors.length, 1);
});
