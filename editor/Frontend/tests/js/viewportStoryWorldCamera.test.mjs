import assert from 'node:assert/strict';
import test from 'node:test';
import { createStoryCameraController } from '../../src/utils/viewportStoryCamera.js';
import { createViewportCameraController } from '../../src/utils/viewportCameraController.js';
import { createPlayerController } from '../../../../game/frontend/playerController.mjs';
import { SMALL_WORLD_CAMERA, SMALL_WORLD_VIEW_LABELS, SMALL_WORLD_VIEWS,
  createSmallWorldCamera, createStoryWorldControls } from '../../src/utils/viewportStoryWorldCamera.js';

const event = (props = {}) => ({ preventDefault() {}, ...props });
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

const cameraPose = (position = [0, 0, 0], forward = [0, 0, 1]) => ({
  handle: 12, sceneId: 'scene.ini', name: 'main',
  position: [...position], forward: [...forward], up: [0, 1, 0], fov: 60,
});
const playerActor = (position = [0, 0.9, 0]) => ({ handle: 41,
  actor_guid: 'f9f5c8b0-7324-4b6c-a011-000000000001',
  geometry: { position: [...position], rotation: [0, -Math.PI / 2, 0], scale: [1, 1, 1] } });

/**
 * One small-world camera plus the pose it submits, driven by a manual frame clock (the same
 * harness `viewportCameraController.test.mjs` uses, so the two controllers are compared under
 * identical frame timing).
 */
function fixture(options = {}) {
  const timer = clock(), moves = [];
  let pose = options.pose || cameraPose();
  const camera = createSmallWorldCamera({
    getPose: () => pose,
    setPose: next => { pose = next; },
    submitPose: next => { moves.push(structuredClone(next)); return true; },
    modelYawOffset: -Math.PI / 2,
    getGroundY: options.getGroundY || null,
    ...timer,
  });
  if (options.bind !== false) camera.bindPlayer(playerActor(options.playerPosition), 0.45);
  return { camera, moves, timer, pose: () => pose,
    step: ms => timer.step(timer.now() + ms), lastMove: () => moves.at(-1) };
}

/** The creative-mode controller on the same clock, as the reference implementation to match. */
function creative(options = {}) {
  const timer = clock();
  let pose = options.pose || cameraPose();
  const controller = createViewportCameraController({
    getPose: () => pose, setPose: next => { pose = next; },
    submitPose: () => true, ...timer,
  });
  if (options.bind !== false) {
    controller.resetInput();
  }
  return { controller, timer, pose: () => pose, step: ms => timer.step(timer.now() + ms) };
}

test('the small world offers exactly 自由视角 and 俯视视角, and starts in the free view', () => {
  assert.deepEqual([...SMALL_WORLD_VIEWS], ['free', 'top']);
  assert.deepEqual(SMALL_WORLD_VIEW_LABELS, { free: '自由视角', top: '俯视视角' });
  const f = fixture();
  assert.equal(f.camera.viewMode(), 'free');
  assert.equal(f.camera.cycleViewMode(), 'top');
  assert.equal(f.camera.cycleViewMode(), 'free');
  assert.equal(f.camera.setViewMode('third'), 'free', '主世界的模式名进不了小世界');
  f.camera.dispose();
});

test('the free camera moves exactly like creative mode: WASD, Q/E and frame time', () => {
  for (const [code, axis, sign] of [['KeyW', 2, 1], ['KeyS', 2, -1], ['KeyD', 0, 1],
    ['KeyA', 0, -1], ['KeyQ', 1, 1], ['KeyE', 1, -1]]) {
    const mine = fixture(), reference = creative();
    mine.camera.keyDown(event({ code }));
    reference.controller.keyDown(event({ code }));
    mine.step(50); reference.step(50);
    // 同一个轴上走同样的距离：0.6 m = 创造模式的 0.2 m/帧 × 60 × 0.05 s。
    near(mine.pose().position[axis], reference.pose().position[axis], `${code} 轴 ${axis}`);
    near(mine.pose().position[axis], 0.6 * sign, `${code} 距离`);
    mine.camera.keyUp(event({ code }));
    reference.controller.keyUp(event({ code }));
    assert.equal(mine.timer.frames.size, 0);
    assert.equal(mine.moves.at(-1).position[axis], mine.pose().position[axis]);
  }
  // 鼠标转向与创造模式同一套敏感度（0.15°/px，右键拖动）。
  const mine = fixture(), reference = creative();
  for (const controller of [mine.camera, reference.controller]) {
    controller.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
    controller.pointerMove(event({ buttons: 2, clientX: 50, clientY: 20 }));
  }
  mine.step(16); reference.step(16);
  mine.pose().forward.forEach((value, i) => near(value, reference.pose().forward[i], `forward[${i}]`));
  // 手柄的 created 前进/后退与前后推拉一致（滚轮本身在小世界归物品栏，见页面）。
  const dolly = fixture();
  dolly.camera.wheel(event({ deltaY: -1 }));
  dolly.step(16);
  near(dolly.pose().position[2], 0.2, '一个滚轮格 = 创造模式的 0.2 m');
  for (const f of [mine, reference, dolly]) {
    f.camera?.dispose(); f.controller?.dispose();
  }
});

test('俯视视角 looks straight down while WASD still pans along the ground and Q/E climbs', () => {
  const f = fixture({ getGroundY: () => 0 });
  f.camera.setViewMode('top');
  const top = f.lastMove();
  assert.deepEqual(top.forward, [0, -1, 0], '俯视就是正下方');
  assert.deepEqual(top.up, [0, 0, 1], '屏幕上方 = 相机当前朝向，没有四元数歧义');
  near(top.position[1], SMALL_WORLD_CAMERA.topHeight, '切到俯视会抬到可用的高度');
  const before = [...top.position];
  f.camera.keyDown(event({ code: 'KeyW' })); f.step(50); f.camera.keyUp(event({ code: 'KeyW' }));
  const panned = f.lastMove();
  assert.deepEqual([panned.forward, panned.up], [[0, -1, 0], [0, 0, 1]], '俯视的朝向不随平移改变');
  near(panned.position[1], before[1], 'W 不往下钻');
  near(panned.position[2], before[2] + 0.6, 'W 沿地面往屏幕上方走');
  f.camera.keyDown(event({ code: 'KeyQ' })); f.step(50); f.camera.keyUp(event({ code: 'KeyQ' }));
  near(f.lastMove().position[1], before[1] + 0.6, 'Q 抬高机位');
  // 来回切换不会一路爬高：已经在高度上就不再加。
  f.camera.setViewMode('free'); f.camera.setViewMode('top');
  near(f.lastMove().position[1], before[1] + 0.6);
  f.camera.dispose();
});

test('Ctrl+滚轮 zooms (fov) in both views and never moves the camera', () => {
  const f = fixture();
  const start = f.lastMove();
  assert.equal(f.camera.zoom(event({ deltaY: -120, ctrlKey: true })), true);
  near(f.lastMove().position[0], start.position[0]);
  near(f.lastMove().position[2], start.position[2]);
  near(f.lastMove().fov, 55, '往上一格 = 视野收窄 5 度');
  f.camera.zoom(event({ deltaY: 120, ctrlKey: true }));
  near(f.lastMove().fov, 60, '往下一格回到原处');
  for (let i = 0; i < 40; i++) f.camera.zoom(event({ deltaY: -120, ctrlKey: true }));
  near(f.lastMove().fov, SMALL_WORLD_CAMERA.minFov, '缩放下限');
  for (let i = 0; i < 40; i++) f.camera.zoom(event({ deltaY: 120, ctrlKey: true }));
  near(f.lastMove().fov, SMALL_WORLD_CAMERA.maxFov, '缩放上限');
  // 俯视同样缩放：缩放不是「拉近机位」，两种视角里是同一件事。
  f.camera.setViewMode('top');
  f.camera.zoom(event({ deltaY: -120, ctrlKey: true }));
  near(f.lastMove().fov, SMALL_WORLD_CAMERA.maxFov - SMALL_WORLD_CAMERA.zoomStep);
  assert.deepEqual(f.lastMove().forward, [0, -1, 0]);
  f.camera.dispose();
});

test('the view mode survives camera re-binding and only a fresh player bind resets it', () => {
  const f = fixture();
  f.camera.setViewMode('top');
  // 「重新渲染」= 页面再读一次模式、再算一次提示；再绑一次场景相机也不该动它。
  const label = () => SMALL_WORLD_VIEW_LABELS[f.camera.viewMode()];
  for (let render = 0; render < 3; render++) assert.equal(label(), '俯视视角');
  f.camera.onCameraBound();
  f.camera.resetInput();
  assert.equal(f.camera.viewMode(), 'top');
  assert.equal(label(), '俯视视角');
  // 重新进入世界（= 重新 bindPlayer）才回到自由视角：俯视是编辑姿态，不进存档。
  f.camera.bindPlayer(playerActor(), 0.45);
  assert.equal(f.camera.viewMode(), 'free');
  f.camera.dispose();
});

test('the invisible player is the ground point under the camera, and its pose is saveable', () => {
  const f = fixture({ getGroundY: () => 2, playerPosition: [0, 2.9, 0] });
  const start = f.camera.snapshotPlayer();
  assert.deepEqual([start.position[0], start.position[2]], [0, 0]);
  near(start.position[1], 2.9, '原点原样落在地面上（原点离地 0.9）');
  near(start.facingYaw, 0);
  f.camera.keyDown(event({ code: 'KeyD' })); f.step(50); f.camera.keyUp(event({ code: 'KeyD' }));
  const moved = f.camera.snapshotPlayer();
  near(moved.position[0], 0.6, '相机飞多远，玩家就在多远的地面上');
  assert.ok(moved.version > start.version, '移动过就必须比存档里的版本新');
  // 旋转仍然是主世界那套「逻辑朝向 + 模型偏移」，否则下次进入世界先知会转 90 度。
  near(moved.rotation[1], moved.facingYaw + (-Math.PI / 2));
  assert.equal(f.camera.snapshotPlayer().actorGuid, playerActor().actor_guid);
  f.camera.dispose();
  assert.equal(f.camera.snapshotPlayer(), null, 'dispose 之后不再有玩家姿态');
});

test('the dispatcher routes each world to its own camera, and zoom only exists in the small world', () => {
  const made = [];
  const createThirdPerson = options => {
    const controller = createPlayerController(options);
    made.push(controller);
    return controller;
  };
  const bridge = { actorTransform: () => true, cameraMove: () => true };
  let role = 'main';
  const clockSource = clock();
  let pose = cameraPose();
  const controls = createStoryWorldControls({
    createThirdPerson, role: () => role, getBridge: () => bridge,
    getPose: () => pose, setPose: next => { pose = next; },
    submitPose: () => true, ...clockSource,
  });
  controls.bindPlayer(playerActor(), 0.45);
  assert.equal(controls.viewMode(), 'third', '主世界还是第三人称');
  controls.cycleViewMode();
  assert.equal(controls.viewMode(), 'first', '主世界的 V 仍然是 第三人称 → 降临 → 俯视');
  controls.cycleViewMode();
  assert.equal(controls.viewMode(), 'top');
  controls.cycleViewMode();
  assert.equal(controls.viewMode(), 'third');
  assert.equal(controls.zoom(event({ deltaY: -120, ctrlKey: true })), false,
    '主世界没有缩放：Ctrl+滚轮因此照原路走（相机拉近）');
  role = 'child';
  controls.bindPlayer(playerActor(), 0.45);
  assert.equal(controls.viewMode(), 'free');
  assert.equal(controls.zoom(event({ deltaY: -120, ctrlKey: true })), true);
  near(pose.fov, 55);
  controls.cycleViewMode();
  assert.equal(controls.viewMode(), 'top');
  assert.deepEqual(pose.forward, [0, -1, 0], '俯视正下方：主世界的第三人称轨道拍不出这个姿态');
  controls.dispose();
  assert.equal(made.length, 1, '只创建了一台第三人称控制器');
});

test('the story controller binds the small world camera through the same four hooks', () => {
  const calls = [];
  const bridge = { cameraMove: (...args) => { calls.push(args); return true; } };
  const role = 'child';
  const controller = createStoryCameraController({
    createControls: options => createStoryWorldControls({ ...options,
      createThirdPerson: createViewportCameraController, role: () => role,
      modelYawOffset: -Math.PI / 2 }),
    getBridge: () => bridge,
    getRect: () => ({ left: 0, top: 0, width: 800, height: 600 }),
    now: () => 0,
    requestFrame: callback => setTimeout(callback, 0),
    cancelFrame: () => {},
    isCurrent: () => true,
  });
  // The page's own order: bind the scene camera first, then the player.
  assert.equal(controller.bind({ cameras: [{ handle: 12, name: 'main', position: [5, 6, 7],
    forward: [0, 0, 1], world_up: [0, 1, 0], fov: 65 }] }, 'scene.ini'), true);
  controller.bindPlayer(playerActor(), 0.45);
  assert.equal(controller.viewMode(), 'free');
  const move = calls.at(-1);
  assert.equal(move[0], 12);
  assert.deepEqual(move[1], [5, 6, 7], '相机从小世界自己保存的机位出发');
  assert.equal(move[4], 65, 'fov 跟着机位走');
  assert.equal(controller.zoom(event({ deltaY: -120, ctrlKey: true })), true);
  assert.equal(controller.snapshotPose().camera.fov, 60, '缩放落在快照里，离开世界时随之保存');
  assert.equal(controller.snapshotPose().cameraName, 'main');
  controller.dispose();
});

/**
 * 「自由视角鼠标移动改变朝向」：不按任何键，鼠标一动就转向，敏感度与创造模式同一个常数。
 *
 * 非真空的证据：把 `pointerMove` 里的裸移动分支去掉（只留创造模式的右拖），下面第一条
 * notDeepEqual 立刻失败；把敏感度换成别的数，与创造模式对齐的那条断言失败。
 */
test('自由视角里一次不按键的 mousemove 就转向，敏感度与创造模式完全相同', () => {
  const f = fixture();
  const start = [...f.pose().forward];
  f.camera.pointerMove(event({ clientX: 400, clientY: 300, movementX: 50, movementY: 0 }));
  const turned = [...f.pose().forward];
  assert.notDeepEqual(turned, start, '没有按键，鼠标移动也必须改变朝向');
  // 0.15°/px × 50px = 7.5°：同一个常数，不是另写的一个数。
  near(turned[0], Math.sin(7.5 * Math.PI / 180), '水平 50 px');
  near(turned[2], Math.cos(7.5 * Math.PI / 180), '水平 50 px');
  // 创造模式的右拖在同样的位移上得到逐位相同的朝向（同一个 lookPose、同一个常数）。
  const reference = creative();
  reference.controller.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
  reference.controller.pointerMove(event({ buttons: 2, clientX: 50, clientY: 0 }));
  reference.step(16);
  reference.pose().forward.forEach((value, i) => near(turned[i], value, `forward[${i}] 与创造模式一致`));
  // 上下移动同样转向（俯仰），且不会翻过头顶。
  f.camera.pointerMove(event({ clientX: 400, clientY: 380, movementX: 0, movementY: 80 }));
  assert.ok(f.pose().forward[1] < turned[1], '往下移鼠标压低视线');
  near(Math.hypot(...f.pose().forward), 1, '朝向始终是单位向量');
  for (let i = 0; i < 40; i++) {
    f.camera.pointerMove(event({ clientX: 400, clientY: 300, movementX: 0, movementY: -200 }));
  }
  assert.ok(f.pose().forward[1] <= 0.99, '一直往上也不会翻到头顶之外');
  f.camera.dispose();
  reference.controller.dispose();
});

test('裸鼠标转向只属于小世界的自由视角：俯视与主世界都不受它影响', () => {
  // 俯视视角：裸移动不改朝向（俯视仍然只有右键拖动改朝向）。
  const top = fixture();
  top.camera.setViewMode('top');
  const heading = [...top.pose().forward];
  top.camera.pointerMove(event({ clientX: 400, clientY: 300, movementX: 60, movementY: 20 }));
  assert.deepEqual(top.pose().forward, heading, '俯视视角不吃裸鼠标');
  assert.equal(top.camera.isLookActive(), false);
  top.camera.dispose();

  // 主世界：同一个事件对它什么也不做（它没有这条能力，姿态一个数都没变）。
  const timer = clock();
  let pose = cameraPose();
  const controls = createStoryWorldControls({
    createThirdPerson: createPlayerController, role: () => 'main',
    getBridge: () => ({ cameraMove: () => true, actorTransform: () => true }),
    getPose: () => pose, setPose: next => { pose = next; }, submitPose: () => true, ...timer,
  });
  controls.bindPlayer(playerActor(), 0.45);
  const before = JSON.stringify(pose);
  assert.equal(controls.isLookActive?.(), undefined, '主世界根本没有裸鼠标转向这条能力');
  controls.pointerMove(event({ clientX: 400, clientY: 300, movementX: 60, movementY: 20 }));
  for (let i = 0; i < 3; i++) timer.step(timer.now() + 16);
  assert.equal(JSON.stringify(pose), before, '主世界的相机姿态一个数都没变');
  controls.dispose();

  // 非真空：同一个事件交给小世界的分派器，朝向确实变了。
  let childPose = cameraPose();
  const childTimer = clock();
  const child = createStoryWorldControls({
    createThirdPerson: createViewportCameraController, role: () => 'child',
    getBridge: () => ({ cameraMove: () => true, actorTransform: () => true }),
    getPose: () => childPose, setPose: next => { childPose = next; },
    submitPose: () => true, ...childTimer,
  });
  child.bindPlayer(playerActor(), 0.45);
  const childStart = [...childPose.forward];
  child.pointerMove(event({ clientX: 400, clientY: 300, movementX: 60, movementY: 20 }));
  assert.notDeepEqual(childPose.forward, childStart, '同一个小世界的分派器会转向');
  child.dispose();
});

test('裸鼠标只在视口矩形里转向，指针锁定之后处处都转向', () => {
  // 视口外的移动不算：鼠标走到面板/HUD 上时相机必须停住。
  const timer = clock();
  let pose = cameraPose();
  let locked = false;
  const camera = createSmallWorldCamera({
    getPose: () => pose, setPose: next => { pose = next; }, submitPose: () => true,
    getRect: () => ({ left: 0, top: 0, width: 800, height: 600 }),
    isPointerLocked: () => locked, ...timer,
  });
  camera.bindPlayer(playerActor(), 0.45);
  const inside = [...pose.forward];
  camera.pointerMove(event({ clientX: 900, clientY: 300, movementX: 50, movementY: 0 }));
  assert.deepEqual(pose.forward, inside, '视口外的移动不改朝向');
  camera.pointerMove(event({ clientX: 400, clientY: 300, movementX: 50, movementY: 0 }));
  assert.notDeepEqual(pose.forward, inside, '视口里的移动改朝向');
  // 锁定指针之后 clientX/Y 不再有意义（浏览器把光标钉住），所以不再按矩形判断。
  const beforeLock = [...pose.forward];
  locked = true;
  camera.pointerMove(event({ clientX: 0, clientY: 0, movementX: 50, movementY: 0 }));
  assert.notDeepEqual(pose.forward, beforeLock, '锁定之后处处都转向');
  camera.dispose();
});

test('主世界的裸鼠标转向仍然是它自己那一套，没有被小世界的实现接管', () => {
  // 主世界（第三人称/降临）本来就靠裸鼠标转向（playerController 的 pointer 播种 + 边缘推镜）。
  // 这条用例钉住的是「本改动没有碰它」：边缘转向仍然由主世界自己完成。
  const timer = clock();
  let pose = cameraPose([0, 0, 0], [0, 0, 1]);
  const controls = createStoryWorldControls({
    createThirdPerson: createPlayerController, role: () => 'main',
    getBridge: () => ({ cameraMove: () => true, actorTransform: () => true }),
    getPose: () => pose, setPose: next => { pose = next; }, submitPose: () => true,
    getRect: () => ({ left: 0, top: 0, width: 800, height: 600 }), ...timer,
  });
  controls.bindPlayer(playerActor(), 0.45);
  const start = [...pose.forward];
  controls.pointerMove(event({ clientX: 799, clientY: 300, pointerId: 1 }));
  controls.pointerMove(event({ clientX: 799, clientY: 300, pointerId: 1 }));
  for (let i = 0; i < 5; i++) timer.step(timer.now() + 16);
  assert.notDeepEqual(pose.forward, start, '主世界的边缘转向照旧工作');
  controls.dispose();
});

/* ============================================================
   「按空格可以上升」：只属于小世界的相机
   ============================================================ */
/** 记录 preventDefault 的键盘事件：要断言「相机接手之后页面滚不动、聚焦按钮点不动」。 */
function keyEvent(props = {}) {
  const instance = { defaultPrevented: false, preventDefault() { instance.defaultPrevented = true; }, ...props };
  return instance;
}
/**
 * 空格在小世界里抬高相机：自由视角与俯视视角都能升，速度与 Q 完全同一档，Q/E 照旧。
 *
 * 非真空：把 `extraMovement: SMALL_WORLD_MOVEMENT_KEYS` 从 `createViewportCameraController`
 * 的实参里删掉，前两条「空格抬高」立刻失败（keyDown 返回 false、机位一格不动）；
 * 把 Q/E 换成空格（改 MOVEMENT 而不是加一个键），「Q/E 照旧」失败；
 * 把两个键各自走一遍而不合并方向，`Q + 空格` 那条 1.2 m 的断言会变成 1.8 m。
 */
test('空格在自由视角与俯视视角都抬高相机，速度与 Q 一致且不影响 Q/E', () => {
  // ---- 自由视角 ----
  const f = fixture();
  const startY = f.pose().position[1];
  const down = keyEvent({ code: 'Space', key: ' ' });
  assert.equal(f.camera.keyDown(down), true, '自由视角必须认空格');
  assert.equal(down.defaultPrevented, true, '相机接手之后 Space 不滚页面、不点亮聚焦按钮');
  assert.equal(f.camera.isInputActive(), true, '按住空格 = 相机正在输入');
  f.step(50);
  near(f.pose().position[1] - startY, 0.6, '空格抬 0.6 m / 50 ms（= Q 那一档）');
  // 逐位对齐创造模式按 Q：不是另写了一套「上升」，而是同一个方向。
  const reference = creative();
  reference.controller.keyDown(keyEvent({ code: 'KeyQ' }));
  reference.step(50);
  near(f.pose().position[1] - startY, reference.pose().position[1], '与创造模式按 Q 完全一致');
  f.camera.keyUp(down);
  f.step(100);
  near(f.pose().position[1] - startY, 0.6, '松开空格就停');
  assert.equal(f.timer.frames.size, 0);

  // ---- Q/E 照旧 ----
  const qe = fixture();
  qe.camera.keyDown(keyEvent({ code: 'KeyQ' })); qe.step(50);
  near(qe.pose().position[1], 0.6, 'Q 还是抬');
  qe.camera.keyUp(keyEvent({ code: 'KeyQ' })); qe.step(100);
  qe.camera.keyDown(keyEvent({ code: 'KeyE' })); qe.step(50);
  near(qe.pose().position[1], 0, 'E 还是降');
  qe.camera.keyUp(keyEvent({ code: 'KeyE' }));
  qe.camera.dispose();

  // ---- 俯视视角 ----
  const top = fixture({ getGroundY: () => 0 });
  top.camera.setViewMode('top');
  const topStart = top.lastMove().position[1];
  const topDown = keyEvent({ code: 'Space', key: ' ' });
  assert.equal(top.camera.keyDown(topDown), true, '俯视视角也必须认空格');
  assert.equal(topDown.defaultPrevented, true);
  top.step(50);
  near(top.lastMove().position[1] - topStart, 0.6, '俯视视角的空格同样抬高机位');
  assert.deepEqual([top.lastMove().forward, top.lastMove().up], [[0, -1, 0], [0, 0, 1]],
    '俯视的朝向不因上升而改变');
  top.camera.keyUp(topDown);
  top.camera.dispose();
  f.camera.dispose();
  reference.controller.dispose();
});

test('面板/对话占着输入时空格既不抬相机也不吞掉事件', () => {
  let locked = true;
  const timer = clock();
  let pose = cameraPose();
  const camera = createSmallWorldCamera({
    getPose: () => pose, setPose: next => { pose = next; }, submitPose: () => true,
    isInputLocked: () => locked, ...timer,
  });
  camera.bindPlayer(playerActor(), 0.45);
  const before = structuredClone(pose);
  const down = keyEvent({ code: 'Space', key: ' ' });
  assert.equal(camera.keyDown(down), false, '输入被面板锁住时空格不属于相机');
  assert.equal(down.defaultPrevented, false, '没有接手就不得 preventDefault：面板要自己处理空格');
  timer.step(timer.now() + 50);
  assert.deepEqual(pose, before, '锁住时机位一个数都不动');
  assert.equal(timer.frames.size, 0);
  // 非真空：放开之后同一个事件确实抬高相机。
  locked = false;
  assert.equal(camera.keyDown(keyEvent({ code: 'Space', key: ' ' })), true);
  timer.step(timer.now() + 50);
  near(pose.position[1] - before.position[1], 0.6, '解锁后空格立刻生效');
  camera.dispose();
});

test('主世界的空格仍然是跳跃，只有小世界的相机把它当上升', () => {
  const timer = clock();
  let pose = cameraPose();
  const controls = createStoryWorldControls({
    createThirdPerson: createPlayerController, role: () => 'main',
    getBridge: () => ({ cameraMove: () => true, actorTransform: () => true }),
    getPose: () => pose, setPose: next => { pose = next; }, submitPose: () => true, ...timer,
  });
  controls.bindPlayer(playerActor(), 0.45);
  assert.equal(controls.viewMode(), 'third');
  const ground = controls.snapshotPlayer().position[1];
  const down = keyEvent({ code: 'Space', key: ' ' });
  assert.equal(controls.keyDown(down), true);
  assert.equal(down.defaultPrevented, true, '跳跃也要挡住页面滚动');
  timer.step(timer.now() + 50);
  assert.equal(controls.snapshotPlayer().grounded, false, '主世界的空格必须起跳');
  assert.ok(controls.snapshotPlayer().position[1] > ground, '人离地了');
  controls.dispose();

  // 非真空对照：同一个分派器、同一个空格，在小世界里抬的是相机。
  let childPose = cameraPose();
  const childTimer = clock();
  const child = createStoryWorldControls({
    createThirdPerson: createViewportCameraController, role: () => 'child',
    getBridge: () => ({ cameraMove: () => true, actorTransform: () => true }),
    getPose: () => childPose, setPose: next => { childPose = next; }, submitPose: () => true, ...childTimer,
  });
  child.bindPlayer(playerActor(), 0.45);
  const before = childPose.position[1];
  assert.equal(child.keyDown(keyEvent({ code: 'Space', key: ' ' })), true);
  childTimer.step(childTimer.now() + 50);
  near(childPose.position[1] - before, 0.6, '小世界的空格抬相机');
  child.dispose();
});
