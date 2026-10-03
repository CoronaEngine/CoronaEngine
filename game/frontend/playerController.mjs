/** Kinematic player control with three camera modes. Native animation continues independently. */
import { PLAYER_GUID, PLAYER_MODEL_YAW_OFFSET, vector3 } from './storyCharacters.mjs';

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

export function createPlayerController({ getPose, setPose, submitPose, getBridge,
  isCurrent = () => true, isInputLocked = () => false, onError = () => {},
  getRect = () => null, onPlayerChanged = () => {},
  requestFrame = callback => requestAnimationFrame(callback), cancelFrame = id => cancelAnimationFrame(id),
  now = () => globalThis.performance.now(), config = PLAYER_CONTROLS }) {
  let player = null, targetOffset = 0, yaw = 0, pitch = config.pitch, distance = config.distance;
  let frame = null, epoch = 0, lastTime = null, pointer = null, disposed = false;
  let jump = null, spaceHeld = false, landingPending = false;
  let view = PLAYER_VIEWS.includes(config.view) ? config.view : 'third';
  let hiddenPlayer = false;
  const keys = new Set(), shifts = new Set();
  let shiftSince = null, dodge = null, lastDodge = -Infinity;
  const ready = () => !disposed && isCurrent() && !isInputLocked() && !landingPending && player && getPose();
  // First person ignores edge turning, so an edge pointer must not keep the frame loop
  // alive doing nothing.
  const hasWork = () => jump || dodge || shifts.size || keys.size
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
    if (typeof bridge?.setActorState !== 'function') return;
    try {
      if (bridge.setActorState(player.handle, { visible: !hidden }) !== true) return;
      hiddenPlayer = hidden;
    } catch (error) { onError(error); }
  }
  function resetInput() {
    epoch++;
    if (frame !== null) cancelFrame(frame);
    frame = null; lastTime = null; pointer = null; keys.clear(); shifts.clear(); shiftSince = null; dodge = null; spaceHeld = false;
    if (!jump) return;
    // This is a desired *ground* pose, even if the old world's handles have
    // already expired. Its dirty version must survive until the save barrier.
    player = { ...player, position: [player.position[0], jump.groundY, player.position[2]],
      grounded: false, version: player.version + 1 };
    jump = null; landingPending = true;
    if (!landingPending || disposed || !isCurrent()) return;
    try {
      const bridge = getBridge();
      if (bridge?.actorTransform?.(player.handle, 0, player.position) !== true) {
        throw new Error('玩家落地接口不可用，请重试保存');
      }
      landingPending = false;
      player = { ...player, grounded: true };
      poseCamera();
    } catch (error) { onError(error); }
    onPlayerChanged();
  }
  function movementDirection() {
    const x = Number(keys.has('d')) - Number(keys.has('a'));
    const z = Number(keys.has('w')) - Number(keys.has('s'));
    const length = Math.hypot(x, z);
    return length > 0 ? [(x * Math.cos(yaw) + z * Math.sin(yaw)) / length,
      (z * Math.cos(yaw) - x * Math.sin(yaw)) / length] : null;
  }
  function movePlayer(position) {
    if (getBridge()?.actorTransform?.(player.handle, 0, position) !== true) throw new Error('玩家实时移动接口不可用');
    // Keep accepted native changes even if the next operation fails.
    player = { ...player, position, version: player.version + 1 };
  }
  function facePlayer(facingYaw, markDirty = false) {
    const rotation = [player.rotation[0], facingYaw + PLAYER_MODEL_YAW_OFFSET, player.rotation[2]];
    if (getBridge()?.actorTransform?.(player.handle, 1, rotation) !== true) throw new Error('玩家实时朝向接口不可用');
    player = { ...player, rotation, facingYaw, version: player.version + Number(markDirty) };
  }
  function beginJump() {
    const [dx, dz] = movementDirection() || [Math.sin(player.facingYaw), Math.cos(player.facingYaw)];
    facePlayer(Math.atan2(dx, dz), true);
    jump = { x: player.position[0], z: player.position[2], groundY: player.position[1], dx, dz, elapsed: 0 };
    player = { ...player, grounded: false };
    lastTime = now();
    onPlayerChanged();
    schedule();
  }
  function beginDodge() {
    if (!ready() || jump || dodge || now() - lastDodge < config.dodgeCooldownMs) return;
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
      ? [player.position[0], player.position[1] + config.firstPersonEyeHeight, player.position[2]]
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
  function tick(time, token) {
    if (token !== epoch) return;
    frame = null;
    if (!ready()) { resetInput(); return; }
    const delta = clamp((time - (lastTime ?? time)) / 1000, 0, config.maxDelta);
    lastTime = time;
    try {
      if (pointer && view !== 'first') {
        // Edge turning exists to orbit around the player. In first person it would
        // rotate the view the player is looking through, so look is drag/delta only.
        yaw += pointer.edgeX * config.edgeYawSpeed * delta;
        const [lo, hi] = pitchLimits();
        pitch = clamp(pitch + pointer.edgeY * config.edgePitchSpeed * delta, lo, hi);
      }
      if (dodge && delta > 0) {
        dodge.elapsed = Math.min(config.dodgeDuration, dodge.elapsed + delta);
        if (config.dodgeDuration - dodge.elapsed < 1e-9) dodge.elapsed = config.dodgeDuration;
        const progress = dodge.elapsed / config.dodgeDuration;
        const distance = config.dodgeDistance * (1 - (1 - progress) ** 2);
        movePlayer([dodge.x + dodge.dx * distance, dodge.y, dodge.z + dodge.dz * distance]);
        if (progress === 1) dodge = null;
        onPlayerChanged();
      } else if (jump && delta > 0) {
        let elapsed = Math.min(jump.elapsed + delta, config.jumpDuration);
        if (config.jumpDuration - elapsed < 1e-9) elapsed = config.jumpDuration;
        const t = elapsed / config.jumpDuration;
        movePlayer([jump.x + jump.dx * config.jumpDistance * t,
          jump.groundY + 4 * config.jumpHeight * t * (1 - t),
          jump.z + jump.dz * config.jumpDistance * t]);
        jump.elapsed = elapsed;
        if (t === 1) { jump = null; player = { ...player, grounded: true }; }
        onPlayerChanged();
      } else if (!jump && delta > 0) {
        const direction = movementDirection();
        if (direction) {
          const [dx, dz] = direction;
          const sprintSeconds = shiftSince === null ? 0
            : Math.max(0, time - Math.max(time - delta * 1000, shiftSince + config.sprintThresholdMs)) / 1000;
          const travel = config.speed * (delta + (config.sprintMultiplier - 1) * sprintSeconds);
          movePlayer([player.position[0] + dx * travel, player.position[1],
            player.position[2] + dz * travel]);
          facePlayer(Math.atan2(dx, dz));
          onPlayerChanged();
        }
      }
      poseCamera();
      if (hasWork()) schedule();
      else lastTime = null;
    } catch (error) { resetInput(); onError(error); }
  }
  function schedule() {
    if (!ready()) { resetInput(); return; }
    if (frame !== null) return;
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
      if (typeof getBridge()?.actorTransform !== 'function' || typeof getBridge()?.cameraMove !== 'function') {
        throw new Error('当前引擎缺少玩家实时控制接口');
      }
      player = { handle: Number(actor.handle), actorGuid: actor.actor_guid,
        position: [...actor.geometry.position], rotation: [...actor.geometry.rotation], facingYaw: actor.geometry.rotation[1] - PLAYER_MODEL_YAW_OFFSET,
        grounded: true, version: 0 };
      landingPending = false;
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
    // A deferred landing can be accepted by the persistent API after the real-
    // time bridge failed or the source component was invalidated. Never use an
    // old acknowledgement to overwrite a newer pose or start a new jump.
    acknowledgePlayerSave(pose) {
      if (!player || pose.version !== player.version || !landingPending) return;
      landingPending = false;
      player = { ...player, grounded: true };
      if (!disposed && isCurrent()) {
        try { poseCamera(); } catch (error) { onError(error); }
        onPlayerChanged();
      }
    },
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
        if (!jump && !dodge) {
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
