import test from 'node:test';
import assert from 'node:assert/strict';
import {
  HOTBAR_SLOT_COUNT,
  clearHotbar,
  hotbarStore,
  selectHotbarSlot,
  syncHotbarSlots,
} from '../../src/story/hotbarStore.js';
import { STORY_ASSETS } from '../../src/story/config/storyAssetConfig.js';

test('hotbar always exposes seven slots and maps inventory items in order', () => {
  syncHotbarSlots([
    { id: 'wood', quantity: 2 },
    { id: 'fragment', quantity: 1 },
    { id: 'empty', quantity: 0 },
  ]);

  assert.equal(HOTBAR_SLOT_COUNT, 7);
  assert.equal(hotbarStore.slots.length, 7);
  assert.equal(hotbarStore.slots[0].id, 'wood');
  assert.equal(hotbarStore.slots[1].id, 'fragment');
  assert.equal(hotbarStore.slots[2], null);
});

test('hotbar ignores out-of-range selection and accepts digit slot indexes', () => {
  assert.equal(selectHotbarSlot(6), true);
  assert.equal(hotbarStore.selectedIndex, 6);
  assert.equal(selectHotbarSlot(7), false);
  assert.equal(selectHotbarSlot(-1), false);
  assert.equal(hotbarStore.selectedIndex, 6);
});

test('hotbar clears a removed item and resets selection when needed', () => {
  syncHotbarSlots([
    { id: 'wood', quantity: 1 },
    { id: 'stone', quantity: 1 },
  ]);
  selectHotbarSlot(1);
  syncHotbarSlots([
    { id: 'wood', quantity: 0 },
    { id: 'stone', quantity: 1 },
  ]);

  assert.equal(hotbarStore.slots[0], null);
  assert.equal(hotbarStore.slots[1].id, 'stone');

  syncHotbarSlots([]);

  assert.equal(
    hotbarStore.slots.every((slot) => slot === null),
    true
  );
  assert.equal(hotbarStore.selectedIndex, 0);
  clearHotbar();
});

test('story terrain asset points to the public GLB directory', () => {
  assert.equal(STORY_ASSETS.terrain.enabled, false);
  assert.match(STORY_ASSETS.terrain.url, /assets\/story\/terrain\/terrain\.glb$/);
  assert.deepEqual(STORY_ASSETS.terrain.scale, [0.08, 0.08, 0.08]);
});
