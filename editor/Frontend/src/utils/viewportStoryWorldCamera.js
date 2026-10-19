/**
 * The story page's two camera worlds, behind one controls object.
 *
 * 主世界 keeps the existing third-person controller (`game/frontend/playerController.mjs`):
 * the player walks, the camera follows. 小世界 does not: the protagonist is not rendered
 * (the actor is hidden by `storyActors`, still saved), and the camera is the player's eyes —
 * 自由视角 flies and 俯视视角 looks straight down at the ground you are arranging.
 *
 * **The free camera is the creative-mode camera, not a copy of it.** Movement, speed, mouse
 * look and the vertical keys come from `viewportCameraController.js` — the same module the
 * editor viewport uses — so "自由视角和创造模式的移动一样" holds by construction and cannot
 * drift when creative mode is retuned. What this file adds on top is only:
 *   - the 俯视 view, which reuses that controller but submits a straight-down orientation
 *     (the internal forward stays horizontal, so WASD pans and Q/E still climb);
 *   - **空格 = 上升**: an extra key mapping registered on the small world's controller only, so
 *     自由视角 and 俯视视角 both climb with Space while the main world's Space stays 跳跃 and the
 *     editor viewport gains no key at all;
 *   - `Ctrl+滚轮` zoom (a field-of-view change), because the plain wheel belongs to the
 *     hotbar in the small world and must keep cycling slots;
 *   - **自由视角的鼠标转向（不按键）**: the creative controller turns on right-drag only; in
 *     the small world's free view any mouse movement turns, because that camera is the
 *     player's eyes. The turn is the same pure `lookPose` the creative viewport uses, at the
 *     same 0.15°/px, and it exists only here — the editor viewport and the main world keep
 *     their own handlers untouched. Pointer lock is optional (the page asks for it on a
 *     click so the cursor cannot leave the viewport); when the browser refuses it, bare
 *     movement still turns, because the deltas come from the events themselves;
 *   - the invisible protagonist: `snapshotPlayer()` answers with the ground point under the
 *     camera, so F/摆放/存档 keep working exactly where the player is looking.
 *
 * The dispatcher is deliberately dumb: one factory per world, and every call is routed to
 * the controller of the world the page is actually in.
 */
import { CREATIVE_LOOK_SENSITIVITY, createViewportCameraController, lookPose } from './viewportCameraController.js';

/** 小世界的两个视角。`V` 在两者之间切换；主世界仍是 第三人称 → 降临 → 俯视。 */
export const SMALL_WORLD_VIEWS = Object.freeze(['free', 'top']);
export const SMALL_WORLD_VIEW_LABELS = Object.freeze({ free: '自由视角', top: '俯视视角' });

/**
 * 空格 = 上升。它只是「又多一个键指到 `up`」，所以速度、斜坡和 Q 完全同一档（同一个方向，
 * 同一帧只走一次）；两种视角共用这台控制器，因此自由视角与俯视视角一起生效。
 *
 * 只在下面 `free` 那一台控制器上注册，所以：
 *   - 主世界的第三人称收不到它 —— 那里的 Space 仍然是跳跃（`playerController.jumpKey`）；
 *   - 编辑器视口（创造模式）也没有这个参数 —— 它一个键都不多；
 *   - 页面把事件交到相机手上时由控制器自己 `preventDefault`，于是 Space 既不会滚动页面，
 *     也不会「点击」当前聚焦的按钮；输入框里打字时控制器照样先放手（`input/textarea` 守卫）。
 */
export const SMALL_WORLD_MOVEMENT_KEYS = Object.freeze({ space: 'up' });

export const SMALL_WORLD_CAMERA = Object.freeze({
  // 眼睛高度与 `playerController.PLAYER_CONTROLS.firstPersonEyeHeight` 同一个数：内部主角
  // 不显示，相机就站在它的眼睛上，于是「进入小世界」看到的位置和主世界离开时一致。
  eyeHeight: 1.6,
  // 切到俯视时抬到这个高度（地面之上）。已经更高就不动，所以来回按 V 不会一路爬高。
  topHeight: 18,
  fov: 60, minFov: 20, maxFov: 100, zoomStep: 5,
  // 自由视角的鼠标敏感度：直接引用创造模式的常数，两处不可能调漂。
  lookSensitivity: CREATIVE_LOOK_SENSITIVITY,
});

const finiteNumber = value => typeof value === 'number' && Number.isFinite(value);
const vector3 = value => Array.isArray(value) && value.length === 3 && value.every(finiteNumber);
const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
/** The horizontal part of a forward vector, or null when it points straight up/down. */
const flatForward = value => {
  const length = Math.hypot(value[0], value[2]);
  return length > 1e-6 ? [value[0] / length, 0, value[2] / length] : null;
};
/** The logical facing yaw of a forward vector, in the `(sin, cos)` convention `storyActors` uses. */
const yawOfForward = value => {
  const forward = flatForward(value) || [0, 0, 1];
  return Math.atan2(forward[0], forward[2]);
};
/** The yaw of the player actor's model, converted back to the logical facing. */
const facingYawOf = (rotation, modelYawOffset) => (vector3(rotation)
  ? rotation[1] - modelYawOffset : 0);

/**
 * The small world's camera: 自由视角 (creative-mode free fly) and 俯视视角, plus the invisible
 * protagonist the rest of the page still asks about.
 *
 * Everything engine-facing goes through the same four hooks the third-person controller gets
 * (`getPose`/`setPose`/`submitPose`/`getBridge`), so `viewportStoryCamera.bind` and the save
 * barrier cannot tell the two controllers apart.
 */
export function createSmallWorldCamera({
  getBridge, getPose, setPose, submitPose,
  getGroundY = null,
  isCurrent = () => true, isInputLocked = () => false,
  onPlayerChanged = () => {}, onError = () => {},
  // 帧时钟原样交给创造模式相机：页面的 requestFrame 与测试的假时钟都必须透传，
  // 否则自由视角会自己去要一个没人驱动的 requestAnimationFrame。
  requestFrame, cancelFrame, now,
  // 玩家模型自己的朝向偏移（`storyCharacters.PLAYER_MODEL_YAW_OFFSET`）：写回存档时要把
  // 逻辑朝向换算成模型 rotation.y，否则下次进入世界时先知/方块的「前方」会转 90 度。
  modelYawOffset = 0,
  // 自由视角的鼠标转向（只在 free 模式生效）：
  //   - `getRect` 是视口在页面里的矩形。鼠标在视口里才转向，走到 HUD/面板上就交回给页面；
  //     页面没有提供矩形（或量不到尺寸）时按「在视口里」处理，测试与极简宿主都可用。
  //   - `isPointerLocked` 只影响那条矩形判断：锁定鼠标之后 clientX/Y 不再有意义（浏览器
  //     把光标钉住甚至不更新坐标），此时一律算作在视口里。指针锁定拿不到也不影响转向。
  getRect = null,
  isPointerLocked = () => false,
  lookSensitivity = SMALL_WORLD_CAMERA.lookSensitivity,
  config = SMALL_WORLD_CAMERA,
}) {
  let view = 'free';
  let pose = null;      // 内部姿态：up 永远是 [0,1,0]，俯视时 forward 已是水平方向
  let player = null;    // 内部主角：只记住 handle/身份和脚下偏移，不写它
  let groundOffset = 0;
  // 上一次鼠标位置：只在事件没有 movementX/movementY 时用来算像素差（老浏览器/合成事件）。
  let pointer = null;
  let version = 0, disposed = false;

  const ready = () => !disposed && Boolean(pose) && Boolean(player)
    && isCurrent() && !isInputLocked();
  // 一个地面采样，或者 null（页面没有高度网格时，脚 y 保持进入世界时的那个值）。
  const groundYAt = (x, z) => {
    if (typeof getGroundY !== 'function') return null;
    const value = getGroundY(x, z);
    return finiteNumber(value) ? value : null;
  };
  // 记住主角进入世界时「原点离地面多高」，之后相机的每个地面点都用同一个偏移。
  function bindGround(x, z, originY) {
    const ground = groundYAt(x, z);
    groundOffset = ground === null ? 0 : originY - ground;
  }

  /**
   * The pose handed to the engine. 自由视角 submits the internal pose untouched — the creative
   * camera already produces unit forward vectors, and copying them verbatim is what keeps the
   * "leave and come back" round trip bit-identical. 俯视视角 looks straight down while the
   * screen's up direction stays the heading, so panning with W is "up the screen".
   */
  function viewPose() {
    const base = getPose() || {};
    const forward = view === 'top' ? [0, -1, 0] : [...pose.forward];
    const up = view === 'top' ? (flatForward(pose.forward) || [0, 0, 1]) : [0, 1, 0];
    return { ...base, position: [...pose.position], forward, up,
      fov: finiteNumber(pose.fov) && pose.fov > 0 ? pose.fov : config.fov };
  }

  /** Write the current view pose to the engine and let the page recompute its proximity. */
  function commit() {
    if (!pose || disposed) return false;
    const next = viewPose();
    setPose?.(next);
    const accepted = submitPose?.(next);
    if (accepted === false) return false;
    version += 1;
    onPlayerChanged();
    return true;
  }

  /** Every write goes through here: 俯视 keeps the internal forward horizontal. */
  function writePose(next) {
    if (!next || !vector3(next.position) || !vector3(next.forward)) return;
    pose = { position: [...next.position],
      forward: view === 'top' ? (flatForward(next.forward) || [0, 0, 1]) : [...next.forward],
      up: [0, 1, 0],
      fov: finiteNumber(next.fov) && next.fov > 0 ? next.fov : pose.fov };
  }

  // The creative camera. Its own frame loop, speed, sensitivity, Q/E and right-drag look are
  // reused as-is; only the pose it writes is ours — plus 空格 → `up`, registered here and
  // nowhere else, so no other host of this controller gains a key.
  const free = createViewportCameraController({
    getPose: () => (ready() ? pose : null),
    setPose: next => { if (!ready()) return; writePose(next); },
    submitPose: () => commit(),
    isCurrent,
    isInputLocked,
    requestFrame, cancelFrame, now,
    extraMovement: SMALL_WORLD_MOVEMENT_KEYS,
  });

  function viewMode() { return view; }

  // ---- 自由视角的鼠标转向（不按键） ------------------------------------------------
  // 只有 free 模式、只有鼠标没按着键、只有指针在视口里时才由这里改变朝向。右键拖动仍然走
  // `free.pointerMove`（创造模式那条路），所以两条路不会同时改同一个姿态；俯视视角一个
  // 字节都不经过这里，它仍然只有右键拖动改朝向。
  const bareMove = event => !(typeof event?.buttons === 'number' && event.buttons !== 0);
  /** 一次移动的像素差：优先用事件自带的 movementX/Y，没有就退回与上一次位置的差。 */
  function deltaOf(event, previous) {
    const mx = Number(event?.movementX), my = Number(event?.movementY);
    if (finiteNumber(mx) && finiteNumber(my)) return [mx, my];
    if (!previous) return null;
    return [Number(event.clientX) - previous.x, Number(event.clientY) - previous.y];
  }
  /** 指针在视口里吗？锁定的指针永远算在视口里——它的坐标被浏览器钉住了。 */
  function insideViewport(event) {
    if (isPointerLocked()) return true;
    if (typeof getRect !== 'function') return true;
    const rect = getRect();
    if (!rect || !(Number(rect.width) > 0) || !(Number(rect.height) > 0)) return false;
    const x = Number(event?.clientX), y = Number(event?.clientY);
    // 只有 movement 的合成事件没有坐标：它只可能来自已经锁定的指针，按在视口里处理。
    if (!finiteNumber(x) || !finiteNumber(y)) return true;
    return x >= rect.left && x <= rect.left + rect.width
      && y >= rect.top && y <= rect.top + rect.height;
  }
  /** 按像素差转向。转向的唯一落点，敏感度就是创造模式那一个常数。 */
  function lookBy(dx, dy) {
    if (!ready() || (!dx && !dy)) return false;
    pose = lookPose(pose, dx, dy, lookSensitivity);
    return commit();
  }
  function pointerMove(event) {
    const previous = pointer;
    if (finiteNumber(Number(event?.clientX)) && finiteNumber(Number(event?.clientY))) {
      pointer = { x: Number(event.clientX), y: Number(event.clientY) };
    }
    if (view !== 'free' || !bareMove(event) || !insideViewport(event)) return free.pointerMove(event);
    const delta = deltaOf(event, previous);
    return delta ? lookBy(delta[0], delta[1]) : false;
  }

  /** V 的行为：自由 ↔ 俯视。切到俯视只抬高一次，回到自由保留当前高度。 */
  function setViewMode(next) {
    if (!SMALL_WORLD_VIEWS.includes(next) || next === view) return view;
    view = next;
    if (pose) {
      if (view === 'top') {
        const ground = groundYAt(pose.position[0], pose.position[2]);
        const floor = ground === null ? pose.position[1] : ground;
        pose = { ...pose, position: [pose.position[0],
          Math.max(pose.position[1], floor + config.topHeight), pose.position[2]] };
      } else {
        pose = { ...pose, forward: flatForward(pose.forward) || [0, 0, 1] };
      }
    }
    free.resetInput();
    pointer = null;
    if (pose && player && !disposed) commit();
    return view;
  }

  function cycleViewMode() {
    return setViewMode(SMALL_WORLD_VIEWS[(SMALL_WORLD_VIEWS.indexOf(view) + 1) % SMALL_WORLD_VIEWS.length]);
  }

  return {
    /**
     * Bind the (invisible) protagonist.
     *
     * 小世界也像主世界一样记住自己的机位：相机从这台场景相机上一次保存的姿势出发，于是
     * 「离开再回来」仍然站在原地（`flushCamera` 存的就是它）。没有可用机位时才退回主角的
     * 眼睛。上一版遗留的竖直机位（旧俯视）按水平朝向读回来，免得自由视角一进入就是俯拍。
     */
    bindPlayer(actor, offset) {
      free.resetInput();
      pointer = null;
      const handle = Number(actor?.handle);
      const position = vector3(actor?.geometry?.position) ? actor.geometry.position : null;
      if (!Number.isInteger(handle) || handle <= 0 || !position) throw new Error('玩家模型尚未就绪');
      player = { handle, actorGuid: actor.actor_guid,
        position: [...position], rotation: vector3(actor?.geometry?.rotation)
          ? [...actor.geometry.rotation] : [0, 0, 0] };
      // 模型的脚在原点之下，所以地面是按「原点到地面」的差值跟着走的。
      bindGround(position[0], position[2], position[1]);
      const facingYaw = facingYawOf(player.rotation, modelYawOffset);
      const saved = getPose();
      const savedPosition = vector3(saved?.position) ? saved.position : null;
      const savedForward = vector3(saved?.forward) ? saved.forward : null;
      // A vertical saved forward carries no heading: fall back to where the protagonist looks.
      const forward = savedForward && flatForward(savedForward)
        ? [...savedForward] : [Math.sin(facingYaw), 0, Math.cos(facingYaw)];
      pose = {
        position: savedPosition ? [...savedPosition]
          : [position[0], position[1] + config.eyeHeight, position[2]],
        forward, up: [0, 1, 0],
        fov: finiteNumber(saved?.fov) && saved.fov > 0 ? Number(saved.fov) : config.fov };
      // 重新进入世界一律从自由视角开始：俯视是编辑姿态，不该被当成下一份存档的机位。
      view = 'free';
      commit();
      // 绑定写回的就是场景里本来就有的那个机位，所以算不上「玩家动了」：版本从 0 起算，
      // 一次没飞就离开的小世界不会为玩家多写一次盘。
      version = 0;
      onPlayerChanged();
      return true;
    },
    onCameraBound() { return false; },
    isBound: () => Boolean(pose && player) && !disposed,
    viewMode, setViewMode, cycleViewMode,
    /**
     * The invisible protagonist, as the rest of the page asks for it: the ground point under
     * the camera, facing the way the camera faces. F 的射程、摆放的位置和存档写的都是它，
     * 所以相机飞到哪儿，玩家就在哪儿——只是看不见。
     */
    snapshotPlayer() {
      if (!player || !pose) return null;
      const [x, , z] = pose.position;
      const ground = groundYAt(x, z);
      // 没有高度网格时脚 y 保持进入世界时的那个值（与主世界的固定地板一致）。
      const y = ground === null ? player.position[1] : ground + groundOffset;
      const facingYaw = yawOfForward(pose.forward);
      return { actorGuid: player.actorGuid, position: [x, y, z],
        rotation: [player.rotation[0], facingYaw + modelYawOffset, player.rotation[2]],
        facingYaw, grounded: true, version, movementState: 'idle' };
    },
    // 小世界没有腾空/落地这一说，但存档条会照原样回调它。
    acknowledgePlayerSave() {},
    /** Ctrl+滚轮：真正的缩放（改 fov），与滚轮换物品栏、与创造模式的前后推拉都不冲突。 */
    zoom(event) {
      if (!ready()) return false;
      const delta = Number(event?.deltaY);
      if (!finiteNumber(delta) || !delta) return false;
      const current = finiteNumber(pose.fov) && pose.fov > 0 ? pose.fov : config.fov;
      const next = clamp(current + (delta > 0 ? config.zoomStep : -config.zoomStep),
        config.minFov, config.maxFov);
      pose = { ...pose, fov: next };
      commit();
      return true;
    },
    keyDown: event => free.keyDown(event),
    keyUp: event => free.keyUp(event),
    pointerDown: event => free.pointerDown(event),
    pointerMove,
    pointerUp: event => free.pointerUp(event),
    wheel: event => free.wheel(event),
    move: direction => free.move(direction),
    setSpeed: value => free.setSpeed(value),
    resetInput() { pointer = null; free.resetInput(); },
    isInputActive: () => free.isInputActive(),
    /** 自由视角下裸鼠标会不会转向：主世界与俯视都是 false，页面据此决定提示与光标。 */
    isLookActive: () => view === 'free' && ready(),
    isLooking: () => free.isLooking(),
    isKeyDown: key => free.isKeyDown(key),
    dispose() { free.dispose(); disposed = true; pose = null; player = null; pointer = null; },
  };
}

/**
 * One controls object for the page, routing every call to the world it is in.
 *
 * `role()` is read at bind time and on each call, so a page that learns its role after setup
 * (the story page loads its world state first) still ends up with the right camera.
 */
export function createStoryWorldControls({
  createThirdPerson, role = () => 'main', ...options
}) {
  const third = createThirdPerson(options);
  const small = createSmallWorldCamera(options);
  // 小世界的相机只有在「主角已经绑上」之后才有意义：相机的一切（机位、F 的射程、摆放的
  // 位置）都是从它脚下的地面算出来的。真实页面在 bind 之后立刻 bindPlayer，所以在小世界里
  // 它总是就绪的；如果某个调用方先把世界切成 child、却还没绑玩家，就继续用第三人称那台，
  // 而不是让整个视口停摆。
  const childWorld = () => role() === 'child';
  const inSmallWorld = () => childWorld() && small.isBound();
  const active = () => (inSmallWorld() ? small : third);
  const call = (name, args) => {
    const controller = active();
    return typeof controller[name] === 'function' ? controller[name](...args) : undefined;
  };
  return {
    // 两个控制器都建好、只让当前世界的那个收输入：构造是纯的，另一个永远不动相机。
    isInputActive: () => call('isInputActive', []),
    isLooking: () => call('isLooking', []),
    // 自由视角的裸鼠标转向只有小世界才有；主世界返回 undefined（= 没有这条能力）。
    isLookActive: () => call('isLookActive', []),
    isKeyDown: key => call('isKeyDown', [key]),
    keyDown: event => call('keyDown', [event]),
    keyUp: event => call('keyUp', [event]),
    pointerDown: event => call('pointerDown', [event]),
    pointerMove: event => call('pointerMove', [event]),
    pointerUp: event => call('pointerUp', [event]),
    pointerLeave: event => call('pointerLeave', [event]),
    wheel: event => call('wheel', [event]),
    // 只有小世界有缩放；主世界返回 false，滚轮就照原路走（物品栏换格 / 相机拉近）。
    zoom: event => (inSmallWorld() ? call('zoom', [event]) : false),
    move: direction => call('move', [direction]),
    setSpeed: value => call('setSpeed', [value]),
    // 绑定按「世界角色」路由，不看就绪标记：正是这次绑定让另一台控制器变得可用。
    bindPlayer: (actor, offset) => (childWorld() ? small : third).bindPlayer(actor, offset),
    onCameraBound: () => call('onCameraBound', []),
    snapshotPlayer: () => call('snapshotPlayer', []),
    acknowledgePlayerSave: pose => call('acknowledgePlayerSave', [pose]),
    viewMode: () => call('viewMode', []),
    setViewMode: next => call('setViewMode', [next]),
    cycleViewMode: () => call('cycleViewMode', []),
    // 输入复位与释放对两个控制器都做：切世界之后另一个手里的按键不能留在按下状态。
    resetInput() { third.resetInput(); small.resetInput(); },
    dispose() { third.dispose(); small.dispose(); },
  };
}
