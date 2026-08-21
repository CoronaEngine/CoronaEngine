import assert from 'node:assert/strict';
import test from 'node:test';

import {
  CREATIVE_MODE_ROUTE,
  STORY_MODE_ROUTE,
  destinationForWorldMode,
  normalizeWorldMode,
} from '../../src/utils/worldModeRouting.js';

test('story mode routes to the standalone story host', () => {
  assert.equal(destinationForWorldMode('story'), STORY_MODE_ROUTE);
  assert.equal(destinationForWorldMode(' STORY '), STORY_MODE_ROUTE);
});

test('creative, missing, and unknown modes fall back to the editor', () => {
  assert.equal(destinationForWorldMode('creative'), CREATIVE_MODE_ROUTE);
  assert.equal(destinationForWorldMode('3d'), CREATIVE_MODE_ROUTE);
  assert.equal(destinationForWorldMode(''), CREATIVE_MODE_ROUTE);
  assert.equal(destinationForWorldMode(null), CREATIVE_MODE_ROUTE);
});

test('world mode normalization is stable for non-string values', () => {
  assert.equal(normalizeWorldMode(undefined), '');
  assert.equal(normalizeWorldMode(123), '123');
});
