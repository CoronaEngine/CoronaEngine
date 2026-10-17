import assert from 'node:assert/strict';
import test from 'node:test';

import {
  emptySceneTreeSettlement,
  resolveSceneTreeSnapshot,
  SCENE_TREE_SETTLE_MS,
  SCENE_TREE_SETTLE_POLL_MS,
} from '../../src/utils/sceneTreeSnapshot.js';

const ROWS = [{ name: 'Temple' }];

test('keeps the visible rows while a freshly opened scene has no actors yet', () => {
  const decision = resolveSceneTreeSnapshot({
    sceneName: 'scene.ini',
    actors: [],
    settlement: emptySceneTreeSettlement(),
    now: 1000,
  });

  assert.equal(decision.commit, false, 'an empty snapshot must not clear the dock');
  assert.equal(decision.retryInMs, SCENE_TREE_SETTLE_POLL_MS, 'the dock must ask again');
  assert.deepEqual(decision.settlement, {
    sceneName: 'scene.ini',
    sawActors: false,
    deadline: 1000 + SCENE_TREE_SETTLE_MS,
  });
});

test('commits a populated snapshot and marks the scene as settled', () => {
  const decision = resolveSceneTreeSnapshot({
    sceneName: 'scene.ini',
    actors: ROWS,
    settlement: emptySceneTreeSettlement(),
    now: 1000,
  });

  assert.equal(decision.commit, true);
  assert.deepEqual(decision.rows, ROWS);
  assert.equal(decision.retryInMs, 0);
  assert.equal(decision.settlement.sawActors, true);
});

test('clears the dock when a settled scene really has no actors left', () => {
  const settled = { sceneName: 'scene.ini', sawActors: true, deadline: 500 };
  const decision = resolveSceneTreeSnapshot({
    sceneName: 'scene.ini',
    actors: [],
    settlement: settled,
    now: 600,
  });

  assert.equal(decision.commit, true, 'deleting every actor must empty the dock');
  assert.deepEqual(decision.rows, []);
  assert.equal(decision.retryInMs, 0);
});

test('commits an empty snapshot once the settle window has passed', () => {
  const decision = resolveSceneTreeSnapshot({
    sceneName: 'scene.ini',
    actors: [],
    settlement: { sceneName: 'scene.ini', sawActors: false, deadline: 2000 },
    now: 2000,
  });

  assert.equal(decision.commit, true);
  assert.deepEqual(decision.rows, []);
  assert.equal(decision.retryInMs, 0, 'an empty scene must stop polling');
});

test('a scene switch drops the previous rows instead of showing another scene', () => {
  const decision = resolveSceneTreeSnapshot({
    sceneName: 'other.scene',
    actors: [],
    settlement: { sceneName: 'scene.ini', sawActors: true, deadline: 1000 },
    now: 1200,
  });

  assert.equal(decision.commit, true);
  assert.deepEqual(decision.rows, []);
  assert.equal(decision.retryInMs, SCENE_TREE_SETTLE_POLL_MS, 'the new scene may still be loading');
  assert.deepEqual(decision.settlement, {
    sceneName: 'other.scene',
    sawActors: false,
    deadline: 1200 + SCENE_TREE_SETTLE_MS,
  });
});

test('a snapshot without an actor array is treated as an empty provisional snapshot', () => {
  const decision = resolveSceneTreeSnapshot({
    sceneName: 'scene.ini',
    actors: undefined,
    settlement: emptySceneTreeSettlement(),
    now: 10,
  });

  assert.equal(decision.commit, false);
  assert.equal(decision.retryInMs, SCENE_TREE_SETTLE_POLL_MS);
});
