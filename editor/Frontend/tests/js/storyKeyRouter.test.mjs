import assert from 'node:assert/strict';
import test from 'node:test';
import { STORY_KEY_ROUTES, routeStoryKeydown } from '../../src/utils/storyKeyRouter.js';
import { createViewportCameraController } from '../../src/utils/viewportCameraController.js';
import { createPlayerController } from '../../../../game/frontend/playerController.mjs';
import { createStoryWorldControls } from '../../src/utils/viewportStoryWorldCamera.js';

const near = (actual, expected, label = '') => assert.ok(Math.abs(actual - expected) < 1e-9,
  `${label} ${actual} != ${expected}`);

function clock() {
  const frames = new Map(); let id = 0, time = 0;
  return {
    frames, now: () => time,
    requestFrame: callback => { frames.set(++id, callback); return id; },
    cancelFrame: id => frames.delete(id),
    step(next) {
      time = next;
      const callbacks = [...frames.values()]; frames.clear();
      for (const callback of callbacks) callback(time);
    },
  };
}

const keyEvent = (props = {}) => {
  const instance = {
    defaultPrevented: false,
    preventDefault() { instance.defaultPrevented = true; },
    stopPropagation() { instance.stopped = true; },
    ...props,
  };
  return instance;
};

const cameraPose = (position = [0, 0, 0], forward = [0, 0, 1]) => ({
  handle: 12, sceneId: 'scene.ini', name: 'main',
  position: [...position], forward: [...forward], up: [0, 1, 0], fov: 60,
});
const playerActor = (position = [0, 0.9, 0]) => ({ handle: 41,
  actor_guid: 'f9f5c8b0-7324-4b6c-a011-000000000001',
  geometry: { position: [...position], rotation: [0, -Math.PI / 2, 0], scale: [1, 1, 1] } });

const baseState = {
  current: true, adjustMode: false, dialogueOpen: false, inSubworld: true,
  inventoryOpen: false, actionBusy: false, busy: false, needsSave: false,
};

/**
 * The full dispatcher plus the pose it submits, driven by a manual clock — the same stack the
 * page builds, so `controls.keyDown` is the real page fallthrough (`camera.keyDown`).
 */
function world(role = 'child', createThirdPerson = createViewportCameraController) {
  const timer = clock();
  let pose = cameraPose();
  const bridge = { cameraMove: () => true, actorTransform: () => true };
  const controls = createStoryWorldControls({
    createThirdPerson, role: () => role, getBridge: () => bridge,
    getPose: () => pose, setPose: next => { pose = next; }, submitPose: () => true, ...timer,
  });
  controls.bindPlayer(playerActor(), 0.45);
  return { controls, timer, pose: () => pose, step: ms => timer.step(timer.now() + ms) };
}

/**
 * The real page flow: the page root (`onKeyDown`) first asks `routeStoryKeydown` which branch a
 * key belongs to, then runs only that branch. `camera` is the only branch that hands the event
 * to the controller, so this is the exact gate a keydown passes on its way to the camera.
 */
function throughPage(controls, event, state) {
  const route = routeStoryKeydown(event, state);
  if (route === 'camera') return { route, handled: controls.keyDown(event) };
  return { route, handled: false };
}

test('routing is a fixed, closed set of branches so a new key cannot fall through silently', () => {
  assert.deepEqual([...STORY_KEY_ROUTES], [
    'ignore', 'adjust', 'dialogue', 'hotbar-slot', 'place-item', 'escape',
    'tab', 'inventory', 'cycle-view', 'interact', 'space-busy', 'skill', 'camera',
  ]);
});

test('小世界里空格一直路由到相机：即使动作/保存进行中也照常上升', () => {
  const small = world('child');
  const startY = small.pose().position[1];
  // 动作与保存同时进行：改动前这里会走 `space-busy`（主世界的跳跃闸门把小世界的空格也吞了），
  // 相机一格不动。这个状态正是「全套件都绿、真机却按不动」的缺口。
  const state = { ...baseState, inSubworld: true, actionBusy: true, busy: true, needsSave: true };
  const down = keyEvent({ code: 'Space', key: ' ' });
  const result = throughPage(small.controls, down, state);
  assert.equal(result.route, 'camera', '小世界的空格不得进主世界的跳跃闸门');
  assert.equal(result.handled, true, '相机必须接手空格');
  assert.equal(down.defaultPrevented, true, '相机接手后 Space 不滚页面、不点亮聚焦按钮');
  small.step(50);
  near(small.pose().position[1] - startY, 0.6, '空格抬 0.6 m / 50 ms（= Q 那一档）');
  small.controls.dispose();
});

test('主世界保持空格=跳跃：动作/保存进行中仍然被页面吞掉', () => {
  const main = world('main', createPlayerController);
  const ground = main.controls.snapshotPlayer().position[1];
  const state = { ...baseState, inSubworld: false, actionBusy: true, busy: true, needsSave: true };
  const down = keyEvent({ code: 'Space', key: ' ' });
  const result = throughPage(main.controls, down, state);
  assert.equal(result.route, 'space-busy', '主世界忙碌时空格仍属于跳跃闸门');
  assert.equal(result.handled, false, '页面吞掉事件，相机不接手');
  // 非真空：同一台分派器、同一个空格，在没有忙碌时确实起跳。
  const jump = keyEvent({ code: 'Space', key: ' ' });
  const idle = throughPage(main.controls, jump, { ...baseState, inSubworld: false });
  assert.equal(idle.route, 'camera');
  assert.equal(idle.handled, true);
  main.step(50);
  assert.equal(main.controls.snapshotPlayer().grounded, false, '主世界的空格必须起跳');
  assert.ok(main.controls.snapshotPlayer().position[1] > ground, '人离地了');
  main.controls.dispose();
});

test('面板打开时空格不碰相机：行囊与对话都吞掉它', () => {
  const small = world('child');
  const startY = small.pose().position[1];
  const down = keyEvent({ code: 'Space', key: ' ' });
  assert.equal(routeStoryKeydown(down, { ...baseState, inventoryOpen: true }), 'inventory');
  assert.equal(routeStoryKeydown(down, { ...baseState, dialogueOpen: true }), 'dialogue');
  assert.equal(routeStoryKeydown(down, { ...baseState, adjustMode: true }), 'adjust');
  // 面板分支在路由层就挡下，相机从未收到事件。
  const result = throughPage(small.controls, down, { ...baseState, inventoryOpen: true });
  assert.equal(result.handled, false);
  small.step(50);
  near(small.pose().position[1], startY, '面板打开时机位一个数都不动');
  small.controls.dispose();
});

test('E/R 的世界分界与空格同一条规则：小世界留给相机，主世界仍是技能', () => {
  const down = keyEvent({ code: 'KeyE', key: 'e' });
  assert.equal(routeStoryKeydown(down, { ...baseState, inSubworld: true }), 'camera');
  assert.equal(routeStoryKeydown(down, { ...baseState, inSubworld: false }), 'skill');
  // 非真空：小世界的 E 确实把相机往下带（Q/E 升降按得动）。
  const small = world('child');
  const startY = small.pose().position[1];
  const e = throughPage(small.controls, keyEvent({ code: 'KeyE', key: 'e' }), baseState);
  assert.equal(e.route, 'camera');
  assert.equal(e.handled, true);
  small.step(50);
  near(small.pose().position[1] - startY, -0.6, 'E 降 0.6 m');
  small.controls.dispose();
});

test('数字键与 B 只在小世界被物品栏接管，主世界照旧放行', () => {
  const one = keyEvent({ code: 'Digit1', key: '1' });
  assert.equal(routeStoryKeydown(one, { ...baseState, inSubworld: true }), 'hotbar-slot');
  assert.equal(routeStoryKeydown(one, { ...baseState, inSubworld: false }), 'camera');
  const b = keyEvent({ code: 'KeyB', key: 'b' });
  assert.equal(routeStoryKeydown(b, { ...baseState, inSubworld: true }), 'place-item');
  assert.equal(routeStoryKeydown(b, { ...baseState, inSubworld: false }), 'camera');
});

test('V/F/Escape/Tab 与修饰键的分支保持原样', () => {
  assert.equal(routeStoryKeydown(keyEvent({ code: 'KeyV', key: 'v' }), baseState), 'cycle-view');
  assert.equal(routeStoryKeydown(keyEvent({ code: 'KeyF', key: 'f' }), baseState), 'interact');
  assert.equal(routeStoryKeydown(keyEvent({ code: 'Escape', key: 'Escape' }), baseState), 'escape');
  assert.equal(routeStoryKeydown(keyEvent({ code: 'Tab', key: 'Tab' }), baseState), 'tab');
  assert.equal(routeStoryKeydown(keyEvent({ code: 'KeyW', key: 'w', ctrlKey: true }), baseState), 'ignore');
  assert.equal(routeStoryKeydown(keyEvent({ code: 'KeyW', key: 'w' }), { ...baseState, current: false }), 'ignore');
  assert.equal(routeStoryKeydown(keyEvent({ code: 'KeyW', key: 'w' }), baseState), 'camera');
});
