import test from 'node:test';
import assert from 'node:assert/strict';
import {
  actorContextRevision,
  actorRecordFromSceneItem,
  actorRecordsFromSceneTree,
} from '../../src/blockly/utils/actorContext.js';

test('actorRecordFromSceneItem keeps compact AI-relevant actor data', () => {
  const actor = actorRecordFromSceneItem({
    name: ' Black Hole ',
    type: 'model',
    aliases: ['Attractor'],
    tags: ['player', 'player'],
    semantic_role: 'attractor',
    geometry: {
      position: [1, 2, 3],
      rotation: { x: 0, y: 45, z: 0 },
      scale: [2, 2, 2],
    },
    size: [4, 5, 6],
    collision: 'sphere',
    mechanics: { physics_enabled: true, mass: 100 },
    optics: { metallic: 1 },
  });
  assert.deepEqual(actor, {
    name: 'Black Hole',
    type: 'model',
    tags: ['player'],
    aliases: ['Attractor'],
    semanticRole: 'attractor',
    transform: {
      position: [1, 2, 3],
      rotation: { x: 0, y: 45, z: 0 },
      scale: [2, 2, 2],
    },
    size: [4, 5, 6],
    collision: 'sphere',
    physicsEnabled: true,
  });
  assert.equal('optics' in actor, false);
});

test('scene and folder rows are excluded', () => {
  assert.equal(actorRecordFromSceneItem({ name: 'root', type: 'scene' }), null);
  assert.equal(actorRecordFromSceneItem({ name: 'group', type: 'folder' }), null);
});

test('revision changes when transform or physics state changes', () => {
  const base = [
    {
      name: 'ball',
      type: 'model',
      tags: [],
      aliases: [],
      transform: { position: [0, 0, 0] },
      physicsEnabled: false,
    },
  ];
  const moved = [{ ...base[0], transform: { position: [1, 0, 0] }, physicsEnabled: true }];
  assert.notEqual(actorContextRevision('default', base), actorContextRevision('default', moved));
});

test('actorRecordsFromSceneTree recursively keeps actors under folders', () => {
  const actors = actorRecordsFromSceneTree({
    name: 'Scene Root',
    type: 'scene',
    children: [
      {
        name: 'Buildings',
        type: 'folder',
        children: [{ name: 'Building A', type: 'model', tags: ['building'] }],
      },
    ],
    renderCache: { ignored: [{ name: 'Not An Actor', type: 'model' }] },
  });
  assert.deepEqual(actors, [
    {
      name: 'Building A',
      type: 'model',
      tags: ['building'],
      aliases: [],
    },
  ]);
});
