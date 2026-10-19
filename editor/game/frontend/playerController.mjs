/** Kinematic player control with three camera modes. Native animation continues independently. */
import { PLAYER_GUID, PLAYER_MODEL_YAW_OFFSET, vector3, rotatedBounds } from './storyCharacters.mjs';

export const PLAYER_CONTROLS = Object.freeze({ speed: 3, maxDelta: 0.05, sprintMultiplier: 1.5, sprintThresholdMs: 200,
  dodgeDistance: 3, dodgeDuration: 0.25, dodgeCooldownMs: 600,
  jumpDistance: 4, jumpDuration: 0.7, jumpHeight: 1.2,
  distance: 4.5, minDistance: 2.5, maxDistance: 10,
  pitch: 20 * Math.PI / 180, minPitch: 10 * Math.PI / 180, maxPitch: 65 * Math.PI / 180,
  sensitivity: 0.005, edgeWidth: 40, edgeYawSpeed: Math.PI / 2, edgePitchSpeed: Math.PI / 3,
  // 'first' is the descent/possession view: eyes at head height looking along yaw.
  // 'top' is the building view: a steep, high orbit instead of a separate projection.
  view: 'third', firstPersonEyeHeight: 1.6,
  topPitch: 82 * Math.PI / 180, topDistance: 16, maxTopDistance: 30 });
export const PLAYER_VIEWS = Object.freeze(['third', 'first', 'top']);
export const VIEW_LABELS = Object.freeze({ third: '第三人称', first: '降临视角', top: '俯视建筑' });
const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
const movementKey = event => {
  const code = String(event.code || '').toLowerCase();
  const key = code.startsWith('key') ? code.slice(3) : String(event.key || '').toLowerCase();
  return ['w', 'a', 's', 'd'].includes(key) ? key : '';
};
const shiftKey = event => ['ShiftLeft', 'ShiftRight'].includes(event.code) ? event.code
  : event.key === 'Shift' ? 'Shift' : '';
const jumpKey = event => event.code === 'Space' || event.key === ' ' || event.key === 'Spacebar';
const editing = event => (event.composedPath?.() || [event.target]).some(target => target?.isContentEditable
  || target?.closest?.('input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"]'));

let controllerSequence = 0;

export function createPlayerController({ getPose, setPose, submitPose, getBridge,
  isCurrent = () => true, isInputLocked = () => false, onError = () => {},
  getRect = () => null, onPlayerChanged = () => {},
  requestFrame = callback => requestAnimationFrame(callback), cancelFrame = id => cancelAnimationFrame(id),
  now = () => globalThis.performance.now(), config = PLAYER_CONTROLS }) {
  let player = null, targetOffset = 0, yaw = 0, pitch = config.pitch, distance = config.distance;
  let frame = null, epoch = 0, lastTime = null, pointer = null, disposed = false;
  let jump = null, spaceHeld = false, feetOffset = 0;
  let pendingMove = null, moveSequence = 0, binding = 0;
  const controllerId = ++controllerSequence;
  let view = PLAYER_VIEWS.includes(config.view) ? config.view : 'third';
  let hiddenPlayer = false;
  const keys = new Set(), shifts = new Set();
  let shiftSince = null, dodge = null, lastDodge = -Infinity;
  const ready = () => !disposed && isCurrent() && !isInputLocked() && player && getPose();
  // First person ignores edge turning, so an edge pointer must not keep the frame loop
  // alive doing nothing.
  const hasWork = () => (player && !player.grounded) || jump || dodge || shifts.size || keys.size
    || (view !== 'first' && Boolean(pointer?.edgeX || pointer?.edgeY));
  // A steep building view needs more headroom than the third-person orbit, so the
  // pitch and distance ceilings follow the active mode instead of one global cap.
  const pitchLimits = () => view === 'top'
    ? [config.topPitch, Math.PI / 2 - 1e-3] : [config.minPitch, config.maxPitch];
  const distanceLimits = () => view === 'top'
    ? [config.minDistance, config.maxTopDistance] : [config.minDistance, config.maxDistance];
  // Visibility is persisted by the native side, so it is only written on an actual
  // transition and always reclaimed by bindPlayer and dispose.
  function setPlayerHidden(hidden) {
    if (!player || hidden === hiddenPlayer) return;
    const bridge = getBridge();
    if (typeof bridge?.setProperty !== 'function') return;
    try {
      // PropertyFast's Visible property is the native realtime channel. The
      // sceneTools.setActorState API is asynchronous and is not a bridge method.
      if (bridge.setProperty(player.handle, 3, !hidden) !== true) return;
      hiddenPlayer = hidden;
    } catch (error) { onError(error); }
  }
  function resetInput() {
    epoch++;
    if (frame !== null) cancelFrame(frame);
    frame = null; lastTime = null; pointer = null; keys.clear(); shifts.clear();
    shiftSince = null; dodge = null; spaceHeld = false; jump = null;
    // Preserve the last accepted pose. A save must wait for an already submitted
    // native move's callback, not manufacture a landing or poll the transform.
    return pendingMove?.settled ?? Promise.resolve();
  }
  function movementDirection() {
    const x = Number(keys.has('d')) - Number(keys.has('a'));
    const z = Number(keys.has('w')) - Number(keys.has('s'));
    const length = Math.hypot(x, z);
    return length > 0 ? [(x * Math.cos(yaw) + z * Math.sin(yaw)) / length,
      (z * Math.cos(yaw) - x * Math.sin(yaw)) / length] : null;
  }
  function beginJump() {
    if (!player.grounded || pendingMove) return;
    const [dx, dz] = movementDirection() || [Math.sin(player.facingYaw), Math.cos(player.facingYaw)];
    jump = { dx, dz, elapsed: 0, requested: true };
    lastTime = now();
    schedule();
  }
  function beginDodge() {
    if (!ready() || !player.grounded || pendingMove || jump || dodge || now() - lastDodge < config.dodgeCooldownMs) return;
    const [dx, dz] = movementDirection() || [-Math.sin(player.facingYaw), -Math.cos(player.facingYaw)];
    dodge = { x: player.position[0], z: player.position[2], y: player.position[1], dx, dz, elapsed: 0 };
    lastDodge = now(); lastTime = now();
    onPlayerChanged(); schedule();
  }
  function poseCamera() {
    const pose = getPose();
    if (!pose || !player || !isCurrent() || disposed) return;
    const forward = [Math.sin(yaw) * Math.cos(pitch), -Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch)];
    const target = [player.position[0], player.position[1] + targetOffset, player.position[2]];
    // 'first' places the eye at head height and looks straight along yaw/pitch, so the
    // body must be hidden or the camera renders from inside the head. 'top' only pulls
    // the existing orbit further back and steeper.
    const eye = view === 'first'
      ? [player.position[0], player.position[1] + feetOffset + config.firstPersonEyeHeight, player.position[2]]
      : target;
    const travel = view === 'first' ? 0 : distance;
    const next = { ...pose, position: eye.map((value, i) => value - forward[i] * travel),
      forward, up: [0, 1, 0] };
    setPose(next);
    if (submitPose(next) === false) throw new Error('相机实时接口不可用');
  }
  function setView(next) {
    if (!PLAYER_VIEWS.includes(next) || next === view) return view;
    view = next;
    // Entering a mode that has its own defaults should not inherit the previous cap;
    // returning to 'third' must restore a persistable orbit.
    if (view === 'top') distance = config.topDistance;
    else if (view === 'third') {
      distance = clamp(distance, config.minDistance, config.maxDistance);
      pitch = clamp(pitch, config.minPitch, config.maxPitch);
    }
    const [lo, hi] = pitchLimits();
    pitch = clamp(pitch, lo, hi);
    setPlayerHidden(view === 'first');
    // Outside a live session there is no camera to move; the next bind re-poses with
    // this mode, and never restores a first-person eye as a saved orbit.
    if (ready()) {
      resetInput();
      poseCamera();
    }
    return view;
  }
  function acceptMeshMove(event) {
    const pending = pendingMove;
    if (!pending || event?.requestId !== pending.id || Number(event.actorHandle) !== pending.handle) return false;
    pendingMove = null;
    clearTimeout(pending.timer);
    try {
      if (event.status === 'success') {
        if (!vector3(event.position) || !vector3(event.rotation)) throw new Error('三角形碰撞回调姿态无效');
        if (pending.binding === binding && player?.handle === pending.handle) {
          const changed = event.position.some((value, i) => Math.abs(value - player.position[i]) > 1e-7)
            || event.rotation.some((value, i) => Math.abs(value - player.rotation[i]) > 1e-7);
          player = { ...player, position: [...event.position], grounded: event.grounded === true,
            rotation: [...event.rotation], facingYaw: event.rotation[1] - PLAYER_MODEL_YAW_OFFSET,
            version: player.version + Number(changed) };
          if (jump && jump === pending.jump) {
            jump.elapsed += pending.delta; jump.requested = false;
            if (player.grounded) jump = null;
          }
          if (dodge === pending.dodge && dodge) {
            dodge.elapsed = pending.dodgeElapsed;
            if (dodge.elapsed >= config.dodgeDuration || event.collided && Math.hypot(event.normal?.[0] ?? 0, event.normal?.[2] ?? 0) > .5) dodge = null;
          }
          if (ready()) {
            poseCamera();
          }
          onPlayerChanged();
        }
      } else if (!['unavailable', 'expired', 'superseded'].includes(event.status)) {
        throw new Error(`原生三角形碰撞失败：${event.status}`);
      }
      pending.resolve();
      if (ready() && hasWork()) schedule();
    } catch (error) { pending.reject(error); resetInput(); onError(error); }
    return true;
  }
  function tick(time, token) {
    if (token !== epoch) return;
    frame = null;
    if (!ready()) { resetInput(); return; }
    if (pendingMove) return;
    const delta = clamp((time - (lastTime ?? time)) / 1000, 0, config.maxDelta);
    lastTime = time;
    try {
      if (pointer && view !== 'first') {
        yaw += pointer.edgeX * config.edgeYawSpeed * delta;
        const [lo, hi] = pitchLimits();
        pitch = clamp(pitch + pointer.edgeY * config.edgePitchSpeed * delta, lo, hi);
      }
      poseCamera();
      if (delta <= 0) { schedule(); return; }
      let vx = 0, vz = 0, facing = null, dodgeElapsed = dodge?.elapsed ?? 0;
      if (dodge) {
        dodgeElapsed = Math.min(config.dodgeDuration, dodge.elapsed + delta);
        const curve = t => config.dodgeDistance * (1 - (1 - t / config.dodgeDuration) ** 2);
        const speed = (curve(dodgeElapsed) - curve(dodge.elapsed)) / delta;
        vx = dodge.dx * speed; vz = dodge.dz * speed;
      } else if (jump && jump.elapsed < config.jumpDuration) {
        vx = jump.dx * config.jumpDistance / config.jumpDuration;
        vz = jump.dz * config.jumpDistance / config.jumpDuration;
        facing = Math.atan2(jump.dx, jump.dz);
      } else {
        const direction = movementDirection();
        if (direction) {
          const sprint = shiftSince !== null && time - shiftSince >= config.sprintThresholdMs;
          const speed = config.speed * (sprint ? config.sprintMultiplier : 1);
          vx = direction[0] * speed; vz = direction[1] * speed;
          facing = Math.atan2(direction[0], direction[1]);
        }
      }
      const id = `${controllerId}:${binding}:${++moveSequence}`;
      const pending = { id, handle: player.handle, binding, jump, dodge, delta, dodgeElapsed, facing };
      pending.settled = new Promise((resolve, reject) => { pending.resolve = resolve; pending.reject = reject; });
      // Keep late callbacks authoritative after timeout; never retry an unknown
      // native move or allow saving a guessed pose.
      pending.settled.catch(() => {});
      pending.timer = setTimeout(() => {
        const error = new Error('等待原生碰撞回调超时，移动已暂停');
        pending.reject(error); resetInput(); onError(error);
      }, 15000);
      pendingMove = pending;
      if (getBridge()?.moveActorMesh?.(player.handle, getPose().handle, id, [vx, 0, vz], delta, jump?.requested === true,
        facing === null ? null : facing + PLAYER_MODEL_YAW_OFFSET) !== true) {
        pendingMove = null; clearTimeout(pending.timer); pending.resolve();
        throw new Error('原生三角形移动接口不可用');
      }
    } catch (error) { resetInput(); onError(error); }
  }
  function schedule() {
    if (!ready()) { resetInput(); return; }
    if (frame !== null || pendingMove) return;
    if (lastTime === null) lastTime = now();
    const token = epoch;
    frame = requestFrame(time => tick(time, token));
  }
  return {
    bindPlayer(actor, offset) {
      resetInput();
      if (!Number.isFinite(Number(actor?.handle)) || Number(actor.handle) <= 0
        || actor.actor_guid !== PLAYER_GUID || !vector3(actor.geometry?.position)
        || !vector3(actor.geometry?.rotation) || !Number.isFinite(offset)) throw new Error('玩家模型尚未就绪');
      if (typeof getBridge()?.moveActorMesh !== 'function' || typeof getBridge()?.cameraMove !== 'function') {
        throw new Error('当前引擎缺少玩家实时控制接口');
      }
      player = { handle: Number(actor.handle), actorGuid: actor.actor_guid,
        position: [...actor.geometry.position], rotation: [...actor.geometry.rotation], facingYaw: actor.geometry.rotation[1] - PLAYER_MODEL_YAW_OFFSET,
        grounded: false, version: 0 };
      binding++;
      if (!Array.isArray(actor.local_aabb) || actor.local_aabb.length !== 6 || !vector3(actor.geometry.scale)) throw new Error('玩家三角形几何边界未就绪');
      feetOffset = rotatedBounds(actor.local_aabb, actor.geometry.rotation)[1] * actor.geometry.scale[1];
      targetOffset = offset; yaw = player.facingYaw; pitch = config.pitch; distance = config.distance;
      // Every bind starts in third person: a first-person eye or a top-down orbit must
      // never be written into the scene camera that the next world restores.
      view = 'third';
      setPlayerHidden(false);
      // Navigation saves each world's camera alongside its player. Reuse a valid saved
      // orbit rather than resetting its yaw/pitch/distance on every scene bind.
      const saved = getPose();
      if (vector3(saved?.position) && vector3(saved?.forward)) {
        const target = [player.position[0], player.position[1] + targetOffset, player.position[2]];
        const offset = target.map((value, i) => value - saved.position[i]);
        const length = Math.hypot(...saved.forward);
        const direction = saved.forward.map(value => value / length);
        const savedDistance = offset.reduce((sum, value, i) => sum + value * direction[i], 0);
        const savedPitch = Math.asin(clamp(-direction[1], -1, 1));
        const error = Math.hypot(...offset.map((value, i) => value - direction[i] * savedDistance));
        if (length > 0 && error < 0.001
          && savedDistance >= config.minDistance - 0.001 && savedDistance <= config.maxDistance + 0.001
          && savedPitch >= config.minPitch - 0.001 && savedPitch <= config.maxPitch + 0.001) {
          yaw = Math.atan2(direction[0], direction[2]); pitch = savedPitch;
          distance = clamp(savedDistance, config.minDistance, config.maxDistance);
          return; // Keep the saved pose exactly; subsequent input uses the restored orbit.
        }
      }
      poseCamera();
    },
    onCameraBound: poseCamera,
    viewMode() { return view; },
    setViewMode(next) { return setView(next); },
    cycleViewMode() { return setView(PLAYER_VIEWS[(PLAYER_VIEWS.indexOf(view) + 1) % PLAYER_VIEWS.length]); },
    // Retain the last submitted pose after disposal so the old-world save barrier can flush it.
    snapshotPlayer() {
      return player ? { actorGuid: player.actorGuid, position: [...player.position],
        rotation: [...player.rotation], facingYaw: player.facingYaw, grounded: player.grounded,
        version: player.version, movementState: jump ? 'jumping' : dodge ? 'dodging'
          : movementDirection() ? (shiftSince !== null && now() - shiftSince >= config.sprintThresholdMs ? 'sprinting' : 'walking') : 'idle' } : null;
    },
    acceptMeshMove,
    resume() { if (ready()) schedule(); },
    acknowledgePlayerSave() {},
    keyDown(event) {
      const key = movementKey(event), space = jumpKey(event), shift = shiftKey(event);
      if ((!key && !space && !shift) || event.defaultPrevented || event.ctrlKey || event.altKey || event.metaKey
        || event.isComposing || event.keyCode === 229 || editing(event) || !ready()) return false;
      event.preventDefault?.();
      if (shift) {
        if (!event.repeat && !shifts.has(shift) && !jump && !dodge) {
          if (!shifts.size) shiftSince = now();
          shifts.add(shift); schedule();
        }
      } else if (space) {
        if (event.repeat || spaceHeld) return true;
        spaceHeld = true;
        if (!jump && !dodge && player.grounded) {
          try { beginJump(); } catch (error) { resetInput(); onError(error); }
        }
      } else { keys.add(key); schedule(); }
      return true;
    },
    keyUp(event) {
      const shift = shiftKey(event);
      if (shift) {
        if (!shifts.delete(shift)) return false;
        event.preventDefault?.();
        if (!ready()) { resetInput(); return true; }
        if (!shifts.size) {
          const tapped = shiftSince !== null && now() - shiftSince < config.sprintThresholdMs;
          shiftSince = null;
          if (tapped) { try { beginDodge(); } catch (error) { resetInput(); onError(error); } }
          if (!hasWork()) { if (frame !== null) cancelFrame(frame); frame = null; lastTime = null; epoch++; }
        }
        return true;
      }
      if (jumpKey(event)) {
        if (!spaceHeld) return false;
        event.preventDefault?.(); spaceHeld = false; return true;
      }
      const key = movementKey(event);
      if (!keys.delete(key)) return false;
      event.preventDefault?.();
      // Releasing WASD must not stop an airborne trajectory.
      if (!hasWork()) {
        epoch++;
        if (frame !== null) cancelFrame(frame);
        frame = null; lastTime = null;
      }
      // The player is now standing still, and the frame loop that used to report movement has
      // just stopped — without this, subscribers keep the last state they saw and the character
      // never leaves its walking animation.
      onPlayerChanged();
      return true;
    },
    pointerDown(event) {
      if (!ready() || !Number.isFinite(event.clientX) || !Number.isFinite(event.clientY)) return false;
      // Seed on clicks as well as first entry; no held mouse button is required.
      pointer = { id: event.pointerId, x: event.clientX, y: event.clientY, edgeX: 0, edgeY: 0 };
      return true;
    },
    pointerMove(event) {
      if (!ready()) { resetInput(); return false; }
      const x = event.clientX, y = event.clientY, rect = getRect();
      if (!Number.isFinite(x) || !Number.isFinite(y)) return false;
      if (rect && (x < rect.left || y < rect.top || x >= rect.left + rect.width || y >= rect.top + rect.height)) {
        resetInput(); return false;
      }
      const old = pointer?.id === event.pointerId ? pointer : null;
      if (old) {
        const [lo, hi] = pitchLimits();
        yaw += (x - old.x) * config.sensitivity;
        pitch = clamp(pitch + (y - old.y) * config.sensitivity, lo, hi);
      }
      const edge = (coordinate, start, extent) => {
        const band = Math.min(config.edgeWidth, extent / 2);
        if (band <= 0) return 0;
        return clamp((coordinate - start - extent + band) / band, 0, 1)
          - clamp((start + band - coordinate) / band, 0, 1);
      };
      pointer = { id: event.pointerId, x, y,
        edgeX: rect ? edge(x, rect.left, rect.width) : 0,
        edgeY: rect ? edge(y, rect.top, rect.height) : 0 };
      schedule(); return true;
    },
    pointerUp() { return false; },
    pointerLeave: resetInput,
    wheel(event) {
      // The eye is fixed to the head in first person; zooming it out would silently
      // turn the possession view back into an orbit.
      if (!ready() || view === 'first' || !Number.isFinite(event.deltaY) || !event.deltaY) return false;
      event.preventDefault?.();
      const [lo, hi] = distanceLimits();
      distance = clamp(distance * Math.exp(clamp(event.deltaY * 0.001, -1, 1)), lo, hi);
      schedule(); return true;
    },
    resetInput,
    // Reclaim a hidden player even when the source world is already going away;
    // visibility is persisted, so leaving it false would erase the model for good.
    dispose() { setPlayerHidden(false); disposed = true; resetInput(); },
  };
}
