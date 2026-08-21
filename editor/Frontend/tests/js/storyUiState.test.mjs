import assert from 'node:assert/strict';
import test from 'node:test';

import {
  reduceStoryUiState,
  shouldResetStoryCamera,
  storyShortcutFromEvent,
} from '../../src/utils/storyUiState.js';

const readyState = {
  ready: true,
  menuOpen: false,
  inventoryOpen: false,
  mapOpen: false,
};

test('recognizes unmodified story UI shortcuts and ignores repeats or editable targets', () => {
  assert.equal(storyShortcutFromEvent({ code: 'KeyB' }), 'inventory');
  assert.equal(storyShortcutFromEvent({ key: 'm' }), 'map');
  assert.equal(storyShortcutFromEvent({ code: 'Escape' }), 'escape');
  assert.equal(storyShortcutFromEvent({ code: 'KeyR' }), 'reset-camera');
  assert.equal(storyShortcutFromEvent({ code: 'KeyB', repeat: true }), '');
  assert.equal(storyShortcutFromEvent({ code: 'KeyM', ctrlKey: true }), '');
  assert.equal(storyShortcutFromEvent({ code: 'KeyB', target: { tagName: 'INPUT' } }), '');
});

test('keeps inventory and map mutually exclusive', () => {
  const inventory = reduceStoryUiState(readyState, 'inventory');
  assert.equal(inventory.inventoryOpen, true);
  assert.equal(inventory.mapOpen, false);

  const map = reduceStoryUiState(inventory, 'map');
  assert.equal(map.inventoryOpen, false);
  assert.equal(map.mapOpen, true);
});

test('Escape closes game panels before toggling the game menu', () => {
  const withInventory = { ...readyState, inventoryOpen: true };
  assert.deepEqual(reduceStoryUiState(withInventory, 'escape'), readyState);

  const withMap = { ...readyState, mapOpen: true };
  assert.deepEqual(reduceStoryUiState(withMap, 'escape'), readyState);

  const menu = reduceStoryUiState(readyState, 'escape');
  assert.equal(menu.menuOpen, true);
  assert.equal(reduceStoryUiState(menu, 'escape').menuOpen, false);
});

test('menu and loading states reject inventory and map shortcuts', () => {
  assert.deepEqual(reduceStoryUiState({ ...readyState, menuOpen: true }, 'inventory'), {
    ...readyState,
    menuOpen: true,
  });
  assert.deepEqual(reduceStoryUiState({ ...readyState, ready: false }, 'map'), {
    ...readyState,
    ready: false,
  });
});

test('camera reset is only available in a ready, unobstructed managed Story World', () => {
  assert.equal(shouldResetStoryCamera({ ...readyState, managedWorld: true }), true);
  assert.equal(shouldResetStoryCamera({ ...readyState, managedWorld: false }), false);
  assert.equal(
    shouldResetStoryCamera({ ...readyState, managedWorld: true, inventoryOpen: true }),
    false
  );
  assert.equal(
    shouldResetStoryCamera({ ...readyState, managedWorld: true, menuOpen: true }),
    false
  );
  assert.equal(shouldResetStoryCamera({ ...readyState, managedWorld: true, ready: false }), false);
});
