/** Shared free-camera input. Hosts own scene binding, persistence and bridge calls. */
const MOVEMENT = Object.freeze({
  w: 'forward', s: 'backward', a: 'left', d: 'right', q: 'up', e: 'down',
  arrowleft: 'rotateLeft', arrowright: 'rotateRight',
  arrowup: 'rotateUp', arrowdown: 'rotateDown',
});
const DIRECTIONS = new Set(Object.values(MOVEMENT));
const NEGATIVE_DIRECTIONS = new Set(['backward', 'left', 'down']);
const cross = (a, b) => [
  a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0],
];
const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
const unit = (v) => {
  const length = Math.hypot(...v);
  return length > 1e-8 ? v.map((n) => n / length) : [0, 0, 1];
};
const copyPose = (pose) => ({
  ...pose, position: [...pose.position], forward: [...pose.forward], up: [...pose.up],
});
const rotate = (v, axis, angle) => {
  const c = Math.cos(angle), s = Math.sin(angle), kv = cross(axis, v), d = dot(axis, v);
  return v.map((n, i) => n * c + kv[i] * s + axis[i] * d * (1 - c));
};

/**
 * 空格键在真实事件里的写法不止一种：Chromium 给 `code: 'Space'`、`key: ' '`，老版引擎给
 * `key: 'Spacebar'`，而宿主合成的按键（本页的按键是 SDL 转 CEF 的 `KEYEVENT_RAWKEYDOWN`
 * 送进来的）可能只剩 `keyCode: 32`、`code` 为空。**路由那一层三种都认**
 * （`storyKeyRouter.js` 的 `isSpace`），控制器这边却只认由 `code` 推出的 `'space'`，
 * 于是同一台相机的 Q/E 按得动、空格被静默丢掉。
 */
const isSpacebarEvent = event => event.code === 'Space' || event.key === ' '
  || event.key === 'Spacebar' || event.keyCode === 32;

/**
 * The movement key this event carries, or `''`.
 *
 * `movement` defaults to the creative-mode table, so every existing caller keeps exactly the
 * same keys. A host that registered extra keys (`extraMovement` below) passes its merged table
 * and gets its own additions recognized — 小世界的空格上升就是这么进来的。
 *
 * 空格先按上面几种写法归一化成表里的 `'space'` 再查表；表里没有 `'space'` 的宿主（创造模式
 * 视口、主世界第三人称）连这一步都不会命中，一个键都不会多。
 */
export function cameraMovementKey(event, movement = MOVEMENT) {
  const code = String(event.code || '').toLowerCase();
  const key = code.startsWith('key') ? code.slice(3) : code;
  if (Object.hasOwn(movement, key)) return key;
  if (isSpacebarEvent(event) && Object.hasOwn(movement, 'space')) return 'space';
  const fallback = String(event.key || '').toLowerCase();
  return Object.hasOwn(movement, fallback) ? fallback : '';
}

/**
 * 鼠标转向的唯一实现：把朝向按位移量（像素）转一点。
 *
 * 创造模式视口与剧情小世界的自由视角共用这一份数学，所以「敏感度一样」是构造出来的，
 * 不是两处各写一遍凑出来的。`degreesPerPixel` 默认就是创造模式的 0.15°/px。
 * 纯函数：返回一份新的 pose，`position` / `up` 原样保留。
 */
export const CREATIVE_LOOK_SENSITIVITY = 0.15;
export function lookPose(pose, dx, dy, degreesPerPixel = CREATIVE_LOOK_SENSITIVITY) {
  const next = copyPose(pose);
  const up = unit(pose.up);
  const radians = degreesPerPixel * Math.PI / 180;
  const yaw = rotate(unit(pose.forward), up, dx * radians);
  const pitch = rotate(yaw, unit(cross(yaw, up)), -dy * radians);
  next.forward = unit(Math.abs(dot(unit(pitch), up)) <= 0.985 ? pitch : yaw);
  return next;
}

function rotateView(pose, direction, degrees) {
  const forward = unit(pose.forward), up = unit(pose.up), radians = degrees * Math.PI / 180;
  let next;
  if (direction === 'rotateLeft' || direction === 'rotateRight') {
    next = rotate(forward, up, direction === 'rotateLeft' ? -radians : radians);
  } else {
    next = rotate(forward, unit(cross(forward, up)), direction === 'rotateUp' ? radians : -radians);
    if (Math.abs(dot(unit(next), up)) > 0.985) return;
  }
  pose.forward = unit(next);
}

function translate(pose, direction, distance) {
  const forward = unit(pose.forward), up = unit(pose.up), right = unit(cross(up, forward));
  const axis = direction === 'forward' || direction === 'backward' ? forward
    : direction === 'left' || direction === 'right' ? right : up;
  const sign = NEGATIVE_DIRECTIONS.has(direction) ? -1 : 1;
  for (let i = 0; i < 3; i++) pose.position[i] += axis[i] * distance * sign;
}

export function createViewportCameraController({
  getPose, setPose, submitPose,
  getSpeed, setSpeed: writeSpeed,
  getSensitivity = () => CREATIVE_LOOK_SENSITIVITY,
  isCurrent = () => true,
  isInputLocked = () => false,
  requestFrame = (callback) => requestAnimationFrame(callback),
  cancelFrame = (id) => cancelAnimationFrame(id),
  now = () => globalThis.performance.now(),
  // 调用方额外注册的「键 → 方向」，默认一个都没有：创造模式仍然只有 WASD/QE/方向键。
  // 小世界用它把空格接到 `up`（升降），而主世界的第三人称（空格 = 跳跃）与编辑器视口
  // 都没有传这个参数，所以它们一个键都不会多。
  extraMovement = null,
}) {
  const movement = Object.freeze({ ...MOVEMENT, ...(extraMovement || {}) });
  const movementEntries = Object.entries(movement);
  const keyOf = event => cameraMovementKey(event, movement);
  const keys = new Set();
  let localSpeed = 0.2;
  let frame = null, frameVersion = 0, lastTime = null;
  let dragging = null, dirty = false, disposed = false;
  const speed = () => getSpeed ? getSpeed() : localSpeed;
  const enabled = () => !disposed && isCurrent() && !isInputLocked() && Boolean(getPose());

  function cancelPendingFrame() {
    ++frameVersion;
    if (frame !== null) cancelFrame(frame);
    frame = null;
  }
  function resetInput() {
    cancelPendingFrame();
    keys.clear();
    dragging = null;
    lastTime = null;
    dirty = false;
  }
  function ready() {
    if (enabled()) return true;
    resetInput();
    return false;
  }
  function setSpeed(value) {
    // Settings panels remain usable while preview owns input or a scene is loading.
    if (disposed || !isCurrent()) return speed();
    const number = Number(value);
    const next = Math.min(2, Math.max(0.01, Number.isFinite(number) ? number : speed()));
    if (writeSpeed) writeSpeed(next);
    else localSpeed = next;
    return next;
  }
  function publish() {
    if (!ready()) return;
    if (dirty) {
      dirty = false;
      submitPose(getPose());
    }
  }
  function queueFrame() {
    if (frame !== null) return;
    const version = ++frameVersion;
    frame = requestFrame((time) => {
      // Cancellation also invalidates callbacks already dequeued by the host.
      if (version !== frameVersion) return;
      frame = null;
      if (!ready()) return;
      if (keys.size) {
        const dt = Math.min(Math.max((time - lastTime) / 1000, 0), 0.1);
        lastTime = time;
        if (dt > 0) {
          const pose = copyPose(getPose());
          // Keep creative-mode axis order, including simultaneous movement/rotation.
          // 同一帧里一个方向只走一次：Q 与空格都指向 `up`，两个一起按也还是 Q 的速度。
          const applied = new Set();
          for (const [key, direction] of movementEntries) {
            if (!keys.has(key) || applied.has(direction)) continue;
            applied.add(direction);
            if (direction.startsWith('rotate')) rotateView(pose, direction, 120 * dt);
            else translate(pose, direction, speed() * 60 * dt);
          }
          setPose(pose);
          dirty = true;
        }
      }
      publish();
      if (keys.size && ready()) queueFrame();
    });
  }
  function scheduleUpdate() {
    if (!ready()) return false;
    dirty = true;
    queueFrame();
    return true;
  }
  function move(direction) {
    if (!DIRECTIONS.has(direction) || !ready()) return false;
    const pose = copyPose(getPose());
    if (direction.startsWith('rotate')) rotateView(pose, direction, 2);
    else translate(pose, direction, speed());
    setPose(pose);
    return scheduleUpdate();
  }
  function keyDown(event) {
    const key = keyOf(event);
    if (!ready() || !key || event.target?.closest?.('input, textarea, select, [contenteditable="true"]')) return false;
    event.preventDefault?.();
    if (!keys.size) lastTime = now();
    keys.add(key);
    queueFrame();
    return true;
  }
  function keyUp(event) {
    const key = keyOf(event);
    if (!keys.delete(key)) return false;
    if (!keys.size) {
      lastTime = null;
      if (!dirty) cancelPendingFrame();
    }
    return true;
  }
  function pointerDown(event) {
    if (!ready() || event.button !== 2) return false;
    event.preventDefault?.();
    dragging = { x: event.clientX, y: event.clientY };
    return true;
  }
  function pointerMove(event) {
    if (!ready() || !dragging) return false;
    if (event.buttons !== undefined && !(event.buttons & 2)) {
      resetInput();
      return false;
    }
    const dx = event.clientX - dragging.x, dy = event.clientY - dragging.y;
    dragging = { x: event.clientX, y: event.clientY };
    if (!dx && !dy) return false;
    setPose(lookPose(getPose(), dx, dy, getSensitivity()));
    return scheduleUpdate();
  }
  function pointerUp(event) {
    if (!ready() || !dragging || event.button !== 2) return false;
    dragging = null;
    publish();
    if (!keys.size) cancelPendingFrame();
    return true;
  }
  function wheel(event) {
    if (!ready() || !Number.isFinite(event.deltaY) || !event.deltaY) return false;
    event.preventDefault?.();
    if (event.shiftKey) {
      setSpeed(Math.round((speed() + (event.deltaY > 0 ? -0.02 : 0.02)) * 100) / 100);
      return true;
    }
    return move(event.deltaY > 0 ? 'backward' : 'forward');
  }
  return {
    keyDown, keyUp, pointerDown, pointerMove, pointerUp, wheel, move, setSpeed,
    scheduleUpdate, resetInput,
    isKeyDown: (key) => keys.has(key),
    isLooking: () => dragging !== null,
    isInputActive: () => dragging !== null || keys.size > 0,
    dispose() { resetInput(); disposed = true; },
  };
}
