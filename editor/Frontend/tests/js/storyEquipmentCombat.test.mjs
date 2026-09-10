/** 战斗与装备回归测试使用纯领域服务及可注入的时钟和随机源，不依赖显卡渲染帧。 */
import test from 'node:test';
import assert from 'node:assert/strict';
import * as THREE from 'three';
import {
  emptyEquipment,
  createEquipmentSystem,
  armorValue,
  attackProfile,
} from '../../src/story/equipmentSystem.js';
import { mitigatedDamage, createPlayerVitals } from '../../src/story/playerVitals.js';
import { createMonsterSystem } from '../../src/story/monsterSystem.js';
import { createBuildCameraController } from '../../src/story/buildCameraController.js';
import { createStoryCombatSystem } from '../../src/story/storyCombatSystem.js';
import { createUgcWorldSession } from '../../src/story/ugc/ugcWorldSession.js';
const setup = () => {
  const state = { items: [], equipment: emptyEquipment() };
  const api = createEquipmentSystem(state);
  api.seed();
  return { state, api };
};
test('starter equipment seeds exactly once, even after equip/unequip', () => {
  const { state, api } = setup();
  api.equip('starter-helmet', 'head');
  api.seed();
  assert.equal(state.items.length, 5);
  assert.equal(api.unequip('head').ok, true);
  api.seed();
  assert.equal(state.items.length, 6);
});
test('wrong slot, nonexistent/external instance and quantity forgery are rejected without mutation', () => {
  const { state, api } = setup();
  const before = JSON.stringify(state);
  assert.equal(api.equip('starter-helmet', 'chest').ok, false);
  assert.equal(api.equip('external', 'head').ok, false);
  assert.equal(JSON.stringify(state), before);
  state.items[0].quantity = 2;
  assert.equal(api.equip('starter-helmet', 'head').ok, false);
});
test('four armor pieces total 15; weapon slot swaps are atomic and do not add armor', () => {
  const { state, api } = setup();
  for (const item of [...state.items].filter((item) => item.armor)) api.equip(item.id, item.slot);
  assert.equal(armorValue(state.equipment), 15);
  api.equip('starter-blade', 'mainHand');
  assert.equal(attackProfile(state.equipment).damage, 25);
  api.equip('starter-axe', 'mainHand');
  assert.equal(attackProfile(state.equipment).damage, 40);
  assert.equal(state.items.filter((item) => item.id === 'starter-blade').length, 1);
  assert.equal(armorValue(state.equipment), 15);
});
test('full inventory rejects unequip but allows replacement with no loss', () => {
  const { state, api } = setup();
  api.equip('starter-blade', 'mainHand');
  while (state.items.length < 21)
    state.items.push({ id: `filler-${state.items.length}`, quantity: 1 });
  assert.equal(api.unequip('mainHand').ok, false);
  assert.equal(api.equip('starter-axe', 'mainHand').ok, true);
  assert.equal(state.items.length, 21);
  assert.equal(state.equipment.mainHand.id, 'starter-axe');
});
test('armor mitigation is clamped and death/respawn preserves equipment', () => {
  assert.equal(mitigatedDamage(10, 0), 10);
  assert.equal(mitigatedDamage(10, 15), 4);
  assert.ok(Math.abs(mitigatedDamage(10, 100) - 2) < 1e-8);
  const state = { health: 100, maxHealth: 100, dead: false };
  const vitals = createPlayerVitals(state, () => 15);
  vitals.damage(10);
  assert.equal(state.health, 96);
  vitals.damage(1000);
  assert.equal(state.dead, true);
  assert.equal(vitals.damage(10), 0);
  vitals.respawn();
  assert.deepEqual(state, { health: 100, maxHealth: 100, dead: false });
});
const target = () => ({ position: { x: 0, y: 1.7, z: 1 }, height: 1.7, dead: false });
const advance = (system, player, steps = 40, paused = false) => {
  for (let i = 0; i < steps; i++) system.update(0.05, player, paused);
};
test('monster windup rechecks distance, visibility and height, and pause freezes timers', () => {
  let damage = 0,
    sight = true;
  const player = target();
  const system = createMonsterSystem({
    spawns: [[0, 0, 0]],
    visible: () => sight,
    onDamage: (value) => (damage += value),
  });
  system.update(0.05, player);
  assert.equal(system.monsters[0].state, 'windup');
  const before = JSON.stringify(system.monsters);
  advance(system, player, 100, true);
  assert.equal(JSON.stringify(system.monsters), before);
  sight = false;
  advance(system, player, 8);
  assert.equal(damage, 0);
  sight = true;
  advance(system, player, 30);
  assert.ok(damage > 0);
  system.monsters[0].state = 'windup';
  system.monsters[0].timer = 0.01;
  player.position.y = 8;
  const old = damage;
  system.update(0.05, player);
  assert.equal(damage, old);
  system.monsters[0].state = 'windup';
  system.monsters[0].timer = 0.01;
  player.position.y = 1.7;
  player.position.z = 5;
  system.update(0.05, player);
  assert.equal(damage, old);
});
test('monsters wander, chase, disengage, avoid blocked movement, and remain dead', () => {
  const player = target();
  player.position.z = 70;
  const system = createMonsterSystem({ spawns: [[0, 0, 0]], random: () => 0.5 });
  advance(system, player, 25);
  assert.equal(system.monsters[0].state, 'wander');
  advance(system, player, 5);
  assert.ok(Math.abs(system.monsters[0].position.x) > 0);
  player.position.z = 4;
  system.update(0.05, player);
  assert.equal(system.monsters[0].state, 'chase');
  player.position.z = 70;
  system.update(0.05, player);
  assert.equal(system.monsters[0].state, 'return');
  system.hit('monster-0', 60);
  advance(system, player);
  assert.equal(system.monsters[0].state, 'dead');
  const blocked = createMonsterSystem({ spawns: [[0, 0, 0]], canMove: () => false });
  player.position.z = 5;
  advance(blocked, player);
  assert.deepEqual(blocked.monsters[0].position, { x: 0, y: 0, z: 0 });
});
test('player attacks use equipped parameters and stop at walls before targets', () => {
  const scene = new THREE.Scene(),
    camera = new THREE.PerspectiveCamera();
  let damage = 0;
  const victim = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1), new THREE.MeshBasicMaterial());
  victim.position.z = -2;
  victim.userData.combatTarget = true;
  scene.add(victim);
  const wall = new THREE.Mesh(new THREE.BoxGeometry(2, 2, 0.1), new THREE.MeshBasicMaterial());
  wall.position.z = -1;
  scene.add(wall);
  scene.updateMatrixWorld(true);
  const system = createStoryCombatSystem({
    camera,
    scene,
    getAttackProfile: () => ({ damage: 40, cooldown: 800, maxDistance: 3 }),
    onHit: (_, value) => (damage += value),
  });
  system.attack(0);
  assert.equal(damage, 0);
  wall.visible = false;
  assert.equal(system.attack(400).accepted, false);
  system.attack(800);
  assert.equal(damage, 40);
});
test('build camera uses QE elevation and shift-wheel speed without moving a player', () => {
  class Surface extends EventTarget {
    closest() {
      return null;
    }
  }
  const canvas = new Surface(),
    host = new Surface(),
    camera = new THREE.PerspectiveCamera();
  const api = createBuildCameraController(camera, canvas, host);
  const event = (type, props) => {
    const value = new Event(type, { cancelable: true });
    Object.assign(value, props);
    return value;
  };
  api.setActive(true);
  const start = api.getState();
  host.dispatchEvent(event('keydown', { code: 'KeyQ' }));
  api.update(0.05);
  assert.ok(camera.position.y > start.position[1]);
  host.dispatchEvent(new Event('blur'));
  assert.equal(api.getState().held, 0);
  canvas.dispatchEvent(event('wheel', { shiftKey: true, deltaY: -100 }));
  assert.ok(api.getState().speed > start.speed);
  api.setActive(false);
  const saved = api.getState();
  api.update(1);
  assert.deepEqual(api.getState(), saved);
  api.setActive(true);
  api.update(0);
  assert.deepEqual(camera.position.toArray(), saved.position);
  api.dispose();
  host.dispatchEvent(event('keydown', { code: 'KeyW' }));
  assert.equal(api.getState().held, 0);
});
test('UGC snapshots deep-copy vitals and equipment without including them in resources', () => {
  const { state, api } = setup();
  api.equip('starter-helmet', 'head');
  const session = createUgcWorldSession();
  session.enter({ equipment: state.equipment, vitals: { health: 77 } });
  const snapshot = session.getSnapshot();
  snapshot.equipment.head.armor = 999;
  assert.equal(session.getSnapshot().equipment.head.armor, 2);
  assert.equal(session.getSnapshot().vitals.health, 77);
  session.dispose();
});

// 物品守恒检查覆盖重复转移、背包已满以及异常的重复实例标识。
test('all transfers conserve equipment identities and full initialization is atomic', () => {
  const { state, api } = setup();
  const original = state.items.map((item) => item.instanceId).sort();
  for (let i = 0; i < 40; i++) {
    for (const item of [...state.items]) api.equip(item.id, item.slot);
    for (const slot of Object.keys(state.equipment)) api.unequip(slot);
  }
  assert.deepEqual(
    [...state.items, ...Object.values(state.equipment).filter(Boolean)]
      .map((i) => i.instanceId)
      .sort(),
    original
  );
  const full = {
    items: Array.from({ length: 21 }, (_, i) => ({ id: `material-${i}` })),
    equipment: emptyEquipment(),
  };
  const before = JSON.stringify(full);
  assert.equal(createEquipmentSystem(full).seed().ok, false);
  assert.equal(JSON.stringify(full), before);
  api.equip('starter-helmet', 'head');
  state.items.push({ ...state.equipment.head, id: 'forged-id' });
  assert.equal(api.equip('forged-id', 'head').ok, false);
});

test('hotbar follows equipment removal without changing the equipped weapon', async () => {
  const { hotbarSystem } = await import('../../src/story/hotbarSystem.js');
  const { state, api } = setup();
  api.equip('starter-blade', 'mainHand');
  hotbarSystem.syncFromInventory(state.items);
  hotbarSystem.select(0);
  assert.ok(!hotbarSystem.getSlots().some((i) => i?.id === 'starter-blade'));
  assert.equal(attackProfile(state.equipment).damage, 25);
  api.unequip('mainHand');
  hotbarSystem.syncFromInventory(state.items);
  assert.equal(hotbarSystem.getSlots().filter((i) => i?.id === 'starter-blade').length, 1);
  hotbarSystem.clear();
});

test('build camera movement is frame-rate independent and editing/drag cancels held input', () => {
  const run = (dt) => {
    const host = new EventTarget(),
      canvas = new EventTarget();
    const controller = createBuildCameraController(new THREE.PerspectiveCamera(), canvas, host);
    const key = new Event('keydown');
    Object.assign(key, { code: 'KeyW' });
    controller.setActive(true);
    host.dispatchEvent(key);
    for (let t = 0; t < Math.round(1 / dt); t++) controller.update(dt);
    const position = controller.getState().position;
    host.dispatchEvent(new Event('dragstart'));
    assert.equal(controller.getState().held, 0);
    host.dispatchEvent(key);
    const focus = new Event('focusin');
    Object.defineProperty(focus, 'target', { value: { closest: () => true } });
    host.dispatchEvent(focus);
    assert.equal(controller.getState().held, 0);
    controller.dispose();
    return position;
  };
  const a = run(1 / 30),
    b = run(1 / 60);
  for (let i = 0; i < 3; i++) assert.ok(Math.abs(a[i] - b[i]) < 1e-8);
});

test('revival clears all living monster aggro while dead monsters remain dead', () => {
  const system = createMonsterSystem({
    spawns: [
      [0, 0, 0],
      [3, 0, 0],
      [6, 0, 0],
    ],
  });
  for (const m of system.monsters) m.state = 'windup';
  system.hit('monster-1', 60);
  system.resetAggro();
  assert.deepEqual(
    system.monsters.map((m) => m.state),
    ['return', 'dead', 'return']
  );
});

test('monster scene blocks melee visibility with walls and releases placeholder meshes', async () => {
  const { createMonsterSceneAdapter } =
    await import('../../src/story/adapters/monsterSceneAdapter.js');
  const scene = new THREE.Scene();
  const wall = new THREE.Mesh(new THREE.BoxGeometry(4, 4, 0.2), new THREE.MeshBasicMaterial());
  wall.position.set(0, 2, 1);
  scene.add(wall);
  const sim = createMonsterSystem({ spawns: [[0, 0, 0]] });
  const adapter = createMonsterSceneAdapter(scene, sim.monsters);
  assert.equal(adapter.visible({ x: 0, y: 0, z: 0 }, { x: 0, y: 1.7, z: 2 }), false);
  assert.equal(adapter.canMove({ x: 0, y: 0, z: 0 }, { x: 0, y: 0, z: 1 }), false);
  adapter.sync();
  assert.equal(scene.children.length, 2);
  adapter.dispose();
  assert.equal(scene.children.length, 1);
  wall.geometry.dispose();
  wall.material.dispose();
});

test('UGC resource commits preserve main equipment and discard temporary transfers', async () => {
  const { createUgcWorldController } = await import('../../src/story/ugc/ugcWorldController.js');
  const { state, api } = setup();
  api.equip('starter-helmet', 'head');
  state.items.unshift(
    { id: 'world-orb-demo', category: 'ugc', quantity: 1 },
    { id: 'material-wood', category: 'material', quantity: 12 }
  );
  const committed = structuredClone(state.items);
  const receipts = [];
  let fail = true;
  const controller = createUgcWorldController({
    mainItems: committed,
    persistence: {
      async saveWorld(id, data) {
        if (fail) throw Error('disk failure');
        receipts.push(data);
        return { id };
      },
    },
    sceneFactory: () => ({ scene: {}, syncFromState() {}, setMode() {}, dispose() {} }),
  });
  await controller.enter({
    mainWorldSnapshot: { items: state.items, equipment: state.equipment, vitals: { health: 77 } },
  });
  assert.equal(
    controller.placeObject({ type: 'block', materialId: 'material-wood', position: [0, 0, 0] }).ok,
    true
  );
  api.unequip('head');
  api.equip('starter-axe', 'mainHand');
  assert.equal((await controller.save()).ok, false);
  assert.equal(committed.find((i) => i.id === 'material-wood').quantity, 12);
  fail = false;
  assert.equal((await controller.save()).ok, true);
  assert.equal((await controller.save()).ok, true);
  const { snapshot } = await controller.exit({ discard: true });
  assert.equal(snapshot.items.find((i) => i.id === 'material-wood').quantity, 11);
  assert.equal(snapshot.equipment.head.armor, 2);
  assert.equal(snapshot.equipment.mainHand, null);
  assert.equal(snapshot.vitals.health, 77);
  assert.equal(snapshot.items.filter((i) => i.id === 'starter-axe').length, 1);
  assert.ok(!JSON.stringify(receipts).includes('starter-'));
  controller.dispose();
});
