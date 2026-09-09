import test from 'node:test';
import assert from 'node:assert/strict';
import { createUgcWorldBuilder } from '../../src/story/ugc/ugcWorldBuilder.js';
import { createUgcWorldController } from '../../src/story/ugc/ugcWorldController.js';
import {
  createUgcWorldLogicRunner,
  validateUgcLogic,
} from '../../src/story/ugc/ugcWorldLogicRunner.js';
import { createUgcWorldPersistence } from '../../src/story/ugc/ugcWorldPersistence.js';
import { createUgcWorldResourceService } from '../../src/story/ugc/ugcWorldResourceService.js';
import { createUgcWorldSession } from '../../src/story/ugc/ugcWorldSession.js';
import {
  UGC_MAX_OBJECTS,
  createUgcWorldState,
  deserializeUgcWorld,
  serializeUgcWorld,
} from '../../src/story/ugc/ugcWorldState.js';

function makeWorld(id = 'world-test') {
  return createUgcWorldState({
    id,
    resources: {
      materials: [{ id: 'material-wood', name: '木材', category: 'material', quantity: 8 }],
      fragments: [],
    },
  });
}

function makeSceneFactory() {
  return () => ({
    scene: {},
    syncFromState() {},
    setMode() {},
    dispose() {},
  });
}

function makeItems(materialQuantity = 8) {
  return [
    {
      id: 'material-wood',
      name: '木材',
      category: 'material',
      quantity: materialQuantity,
    },
    {
      id: 'world-orb-demo',
      name: '世界小球',
      category: 'ugc',
      quantity: 1,
    },
  ];
}

test('UGC world defaults to build mode and round-trips through the save format', () => {
  const world = makeWorld();
  const payload = serializeUgcWorld(world);
  const restored = deserializeUgcWorld(payload);

  assert.equal(world.mode, 'build');
  assert.equal(world.dirty, false);
  assert.equal(restored.id, world.id);
  assert.equal(restored.mode, 'build');
  assert.deepEqual(restored.objects, []);
});

test('UGC builder snaps objects, enforces the object limit, and blocks edits in play mode', () => {
  const world = makeWorld();
  const resources = createUgcWorldResourceService({
    state: world,
    materials: [{ id: 'material-wood', category: 'material', quantity: UGC_MAX_OBJECTS + 1 }],
  });
  const builder = createUgcWorldBuilder({ state: world, resources });

  for (let index = 0; index < UGC_MAX_OBJECTS; index += 1) {
    const x = index % 31;
    const z = Math.floor(index / 31);
    const result = builder.placeObject({
      type: 'block',
      materialId: 'material-wood',
      position: [x, 0, z],
    });
    assert.equal(result.ok, true);
  }

  assert.equal(world.objects.length, UGC_MAX_OBJECTS);
  assert.equal(
    builder.placeObject({
      type: 'block',
      materialId: 'material-wood',
      position: [0, 1, 0],
    }).ok,
    false
  );

  world.mode = 'play';
  assert.equal(
    builder.placeObject({
      type: 'block',
      materialId: 'material-wood',
      position: [31, 0, 4],
    }).error,
    '试玩模式不能修改场景。'
  );
});

test('resource commits distinguish material and fragment IDs with the same value', () => {
  const state = makeWorld();
  const resources = createUgcWorldResourceService({
    state,
    materials: [{ id: 'same-id', category: 'material', quantity: 2 }],
    fragments: [{ id: 'same-id', category: 'ugc', quantity: 1, logic: {} }],
  });
  const cost = resources.consumeCost({
    materials: [{ id: 'same-id', quantity: 1 }],
    fragments: [{ id: 'same-id', quantity: 1 }],
  });
  const items = [
    { id: 'same-id', category: 'material', quantity: 5 },
    { id: 'same-id', category: 'ugc', quantity: 3 },
  ];

  assert.ok(cost);
  assert.deepEqual(resources.getPendingCommit(), [
    { type: 'materials', id: 'same-id', quantity: 1 },
    { type: 'fragments', id: 'same-id', quantity: 1 },
  ]);
  assert.deepEqual(resources.prepareCommit(items), [
    { id: 'same-id', category: 'material', quantity: 4 },
    { id: 'same-id', category: 'ugc', quantity: 2 },
  ]);
});

test('controller commits resources only after a successful save and does not charge twice', async () => {
  const items = makeItems(2);
  const receipts = [];
  const persistence = {
    async saveWorld(worldId, worldData) {
      receipts.push({ worldId, worldData });
      return {
        worldId,
        version: 1,
        relativePath: `ugc/worlds/${worldId}.json`,
      };
    },
    async listWorlds() {
      return [];
    },
  };
  const controller = createUgcWorldController({
    persistence,
    mainItems: items,
    sceneFactory: makeSceneFactory(),
  });

  await controller.enter({
    worldId: 'world-save-test',
    mainItems: items,
    mainWorldSnapshot: { player: { x: 2, y: 1.7, z: 3 }, items },
  });
  assert.equal(
    controller.placeObject({
      type: 'block',
      materialId: 'material-wood',
      position: [0, 0, 0],
    }).ok,
    true
  );
  assert.equal(items.find((item) => item.id === 'material-wood').quantity, 2);

  assert.equal((await controller.save()).ok, true);
  assert.equal(items.find((item) => item.id === 'material-wood').quantity, 1);
  assert.equal((await controller.save()).ok, true);
  assert.equal(items.find((item) => item.id === 'material-wood').quantity, 1);
  assert.equal(receipts.length, 2);

  assert.equal(
    controller.placeObject({
      type: 'block',
      materialId: 'material-wood',
      position: [1, 0, 0],
    }).ok,
    true
  );
  assert.equal((await controller.save()).ok, true);
  assert.equal(
    items.some((item) => item.id === 'material-wood'),
    false
  );

  await controller.exit({ discard: true });
});

test('controller keeps the main inventory unchanged when saving fails', async () => {
  const items = makeItems(2);
  const controller = createUgcWorldController({
    persistence: {
      async saveWorld() {
        throw new Error('模拟存档失败');
      },
    },
    mainItems: items,
    sceneFactory: makeSceneFactory(),
  });

  await controller.enter({ worldId: 'world-failure-test', mainItems: items });
  assert.equal(
    controller.placeObject({
      type: 'block',
      materialId: 'material-wood',
      position: [0, 0, 0],
    }).ok,
    true
  );
  const result = await controller.save();

  assert.equal(result.ok, false);
  assert.equal(items.find((item) => item.id === 'material-wood').quantity, 2);
  assert.equal(controller.isDirty(), true);
  await controller.exit({ discard: true });
});

test('persistence requires an exact save receipt and never accepts a missing path', async () => {
  const world = makeWorld('receipt-test');
  const api = {
    ugc: {
      async saveWorld() {
        return { data: {} };
      },
    },
  };
  const persistence = createUgcWorldPersistence({ api });

  await assert.rejects(persistence.saveWorld(world.id, world), /worldId 与请求不一致/);
});

test('logic runner executes only the allowlisted data actions', () => {
  const validLogic = {
    triggers: [{ type: 'onInteract', targetId: 'target-1' }],
    conditions: [],
    actions: [{ type: 'setVariable', key: 'activated', value: true }],
  };
  const state = createUgcWorldState({
    id: 'logic-test',
    objects: [{ id: 'target-1', type: 'target', position: [0, 1, 0] }],
  });
  const runner = createUgcWorldLogicRunner({
    state,
    fragments: [{ id: 'fragment-1', name: '碎片', logic: validLogic }],
    fragmentBindings: new Map([['fragment-1', 'target-1']]),
  });

  assert.equal(validateUgcLogic(validLogic).valid, true);
  assert.equal(runner.runTrigger('onInteract', { targetId: 'target-1' }).executed, 1);
  assert.equal(state.logicState.variables.activated, true);
  assert.equal(
    validateUgcLogic({ actions: [{ type: 'runJavaScript', code: 'globalThis.evil = true' }] })
      .valid,
    false
  );
});

test('session returns an isolated snapshot and does not leak world mutations', () => {
  const session = createUgcWorldSession();
  const snapshot = {
    player: { x: 1, y: 1.7, z: 2 },
    items: makeItems(),
  };
  session.create();
  session.loadResources([{ id: 'material-wood', quantity: 2 }], []);
  session.enter(snapshot, { worldId: 'session-test' });
  const world = session.getMutableWorld();
  world.name = '小世界已修改';
  world.objects.push({
    id: 'object-1',
    type: 'target',
    position: [0, 1, 0],
    rotation: [0, 0, 0],
    scale: [1, 1, 1],
    materialId: null,
    fragmentIds: [],
    cost: { materials: [], fragments: [], committed: true },
    health: 100,
    targetState: 'idle',
  });

  assert.equal(session.getSnapshot().player.x, 1);
  assert.equal(snapshot.player.x, 1);
  assert.equal(session.getWorld().name, '小世界已修改');
  session.dispose();
});
