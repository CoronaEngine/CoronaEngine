import assert from 'node:assert/strict';
import test from 'node:test';

import {
  resolveOpenedProjectScene,
  resolveSceneTabs,
} from '../../src/utils/projectSceneLifecycle.js';

test('uses the materialized scene route carried by projectOpened', () => {
  assert.equal(
    resolveOpenedProjectScene({ scene: { path: 'worlds/temple.scene', name: 'Temple' } }),
    'worlds/temple.scene',
  );
});

test('replaces stale tabs with the native project snapshot and bounds its active index', () => {
  assert.deepEqual(
    resolveSceneTabs({
      scenes: [
        { name: 'Main', path: 'scenes/main.scene' },
        { name: 'Temple', path: 'scenes/temple.scene' },
      ],
      active_index: 99,
    }, 'fallback.scene'),
    {
      tabs: [
        { name: 'Main', id: 'scenes/main.scene' },
        { name: 'Temple', id: 'scenes/temple.scene' },
      ],
      activeIndex: 1,
    },
  );
});
