import assert from 'node:assert/strict';
import test from 'node:test';

import {
  STORY_INVENTORY_SLOT_COUNT,
  addItemToInventory,
  getStoryItemDefinition,
  normalizeInventoryDocument,
  removeInventorySlotQuantity,
  removeItemFromInventory,
  seedStoryInventory,
  storyInventoryStorageKey,
} from '../../src/utils/storyInventory.js';

test('seeds a 24-slot inventory exactly once with the initial story items', () => {
  const slots = seedStoryInventory();
  assert.equal(slots.length, STORY_INVENTORY_SLOT_COUNT);
  assert.deepEqual(slots.slice(0, 3), [
    { itemId: 'bandage', quantity: 3 },
    { itemId: 'old_key', quantity: 1 },
    { itemId: 'blue_crystal', quantity: 12 },
  ]);
});

test('adds items to existing stacks before consuming empty slots', () => {
  const source = seedStoryInventory();
  const result = addItemToInventory(source, 'bandage', 12);
  assert.equal(result.added, 12);
  assert.equal(result.remaining, 0);
  assert.deepEqual(result.slots[0], { itemId: 'bandage', quantity: 10 });
  assert.deepEqual(result.slots[3], { itemId: 'bandage', quantity: 5 });
});

test('returns the unplaced remainder when every slot is full', () => {
  const full = Array.from({ length: STORY_INVENTORY_SLOT_COUNT }, () => ({
    itemId: 'old_key',
    quantity: 1,
  }));
  const result = addItemToInventory(full, 'bandage', 3);
  assert.equal(result.added, 0);
  assert.equal(result.remaining, 3);
});

test('removes items across stacks and removes one unit from a selected slot', () => {
  let slots = addItemToInventory(seedStoryInventory(), 'bandage', 12).slots;
  const removed = removeItemFromInventory(slots, 'bandage', 11);
  assert.equal(removed.removed, 11);
  assert.equal(removed.remaining, 0);
  assert.equal(removed.slots.reduce((sum, slot) => sum + (slot?.itemId === 'bandage' ? slot.quantity : 0), 0), 4);

  slots = removed.slots;
  const selected = removeInventorySlotQuantity(slots, 0, 1);
  assert.equal(selected.removed, 1);
  assert.equal(selected.slots[0].quantity, 3);
});

test('normalizes damaged documents while preserving unknown item ids', () => {
  const normalized = normalizeInventoryDocument({
    initialized: true,
    slots: [
      { itemId: 'future_item', quantity: 4 },
      { itemId: '', quantity: 8 },
      { itemId: 'bandage', quantity: -2 },
    ],
  });
  assert.deepEqual(normalized.slots[0], { itemId: 'future_item', quantity: 4 });
  assert.equal(normalized.slots[1], null);
  assert.equal(normalized.slots[2], null);
  assert.equal(getStoryItemDefinition('future_item').unknown, true);
});

test('creates case-insensitive project-isolated storage keys', () => {
  assert.equal(
    storyInventoryStorageKey('D:\\Games\\StoryOne\\'),
    storyInventoryStorageKey('d:/games/storyone')
  );
  assert.notEqual(storyInventoryStorageKey('D:/Games/StoryOne'), storyInventoryStorageKey('D:/Games/StoryTwo'));
});

