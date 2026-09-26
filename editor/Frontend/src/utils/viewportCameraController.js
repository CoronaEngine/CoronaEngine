/** Shared free-camera input. Hosts own scene binding, persistence and bridge calls. */
const MOVEMENT = Object.freeze({
  w: 'forward', s: 'backward', a: 'left', d: 'right', q: 'up', e: 'down',
  arrowleft: 'rotateLeft', arrowright: 'rotateRight',
  arrowup: 'rotateUp', arrowdown: 'rotateDown',
});
const MOVEMENT_ENTRIES = Object.entries(MOVEMENT);
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

export function cameraMovementKey(event) {
  const code = String(event.code || '').toLowerCase();
  const key = code.startsWith('key') ? code.slice(3) : code;
  if (Object.hasOwn(MOVEMENT, key)) return key;
  const fallback = String(event.key || '').toLowerCase();
  return Object.hasOwn(MOVEMENT, fallback) ? fallback : '';
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
  getSensitivity = () => 0.15,
  isCurrent = () => true,
  isInputLocked = () => false,
  requestFrame = (callback) => requestAnimationFrame(callback),
  cancelFrame = (id) => cancelAnimationFrame(id),
  now = () => globalThis.performance.now(),
}) {
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
          for (const [key, direction] of MOVEMENT_ENTRIES) {
            if (!keys.has(key)) continue;
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
    const key = cameraMovementKey(event);
    if (!ready() || !key || event.target?.closest?.('input, textarea, select, [contenteditable="true"]')) return false;
    event.preventDefault?.();
    if (!keys.size) lastTime = now();
    keys.add(key);
    queueFrame();
    return true;
  }
  function keyUp(event) {
    const key = cameraMovementKey(event);
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
    const pose = copyPose(getPose()), up = unit(pose.up);
    const radians = getSensitivity() * Math.PI / 180;
    const yaw = rotate(unit(pose.forward), up, dx * radians);
    const pitch = rotate(yaw, unit(cross(yaw, up)), -dy * radians);
    pose.forward = unit(Math.abs(dot(unit(pitch), up)) <= 0.985 ? pitch : yaw);
    setPose(pose);
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
