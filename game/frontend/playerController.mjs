/** Third-person, kinematic player control. Native animation continues independently. */
import { PLAYER_GUID, PLAYER_MODEL_YAW_OFFSET, vector3 } from './storyCharacters.mjs';

export const PLAYER_CONTROLS = Object.freeze({ speed: 3, maxDelta: 0.05,
  jumpDistance: 4, jumpDuration: 0.7, jumpHeight: 1.2,
  distance: 4.5, minDistance: 2.5, maxDistance: 10,
  pitch: 20 * Math.PI / 180, minPitch: 10 * Math.PI / 180, maxPitch: 65 * Math.PI / 180,
  sensitivity: 0.005, edgeWidth: 40, edgeYawSpeed: Math.PI / 2, edgePitchSpeed: Math.PI / 3 });
const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
const movementKey = event => {
  const code = String(event.code || '').toLowerCase();
  const key = code.startsWith('key') ? code.slice(3) : String(event.key || '').toLowerCase();
  return ['w', 'a', 's', 'd'].includes(key) ? key : '';
};
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
  const keys = new Set();
  const ready = () => !disposed && isCurrent() && !isInputLocked() && !landingPending && player && getPose();
  const hasWork = () => jump || keys.size || pointer?.edgeX || pointer?.edgeY;
  function resetInput() {
    epoch++;
    if (frame !== null) cancelFrame(frame);
    frame = null; lastTime = null; pointer = null; keys.clear(); spaceHeld = false;
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
  function poseCamera() {
    const pose = getPose();
    if (!pose || !player || !isCurrent() || disposed) return;
    const forward = [Math.sin(yaw) * Math.cos(pitch), -Math.sin(pitch), Math.cos(yaw) * Math.cos(pitch)];
    const target = [player.position[0], player.position[1] + targetOffset, player.position[2]];
    const next = { ...pose, position: target.map((value, i) => value - forward[i] * distance),
      forward, up: [0, 1, 0] };
    setPose(next);
    if (submitPose(next) === false) throw new Error('第三人称相机实时接口不可用');
  }
  function tick(time, token) {
    if (token !== epoch) return;
    frame = null;
    if (!ready()) { resetInput(); return; }
    const delta = clamp((time - (lastTime ?? time)) / 1000, 0, config.maxDelta);
    lastTime = time;
    try {
      if (pointer) {
        yaw += pointer.edgeX * config.edgeYawSpeed * delta;
        pitch = clamp(pitch + pointer.edgeY * config.edgePitchSpeed * delta, config.minPitch, config.maxPitch);
      }
      if (jump && delta > 0) {
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
          movePlayer([player.position[0] + dx * config.speed * delta, player.position[1],
            player.position[2] + dz * config.speed * delta]);
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
      // O/P saves each world's camera alongside its player. Reuse a valid saved
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
    // Retain the last submitted pose after disposal so the old-world save barrier can flush it.
    snapshotPlayer() {
      return player ? { actorGuid: player.actorGuid, position: [...player.position],
        rotation: [...player.rotation], facingYaw: player.facingYaw, grounded: player.grounded,
        version: player.version } : null;
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
      const key = movementKey(event), space = jumpKey(event);
      if ((!key && !space) || event.defaultPrevented || event.ctrlKey || event.altKey || event.metaKey
        || event.isComposing || event.keyCode === 229 || editing(event) || !ready()) return false;
      event.preventDefault?.();
      if (space) {
        if (event.repeat || spaceHeld) return true;
        spaceHeld = true;
        if (!jump) {
          try { beginJump(); } catch (error) { resetInput(); onError(error); }
        }
      } else { keys.add(key); schedule(); }
      return true;
    },
    keyUp(event) {
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
        yaw += (x - old.x) * config.sensitivity;
        pitch = clamp(pitch + (y - old.y) * config.sensitivity, config.minPitch, config.maxPitch);
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
      if (!ready() || !Number.isFinite(event.deltaY) || !event.deltaY) return false;
      event.preventDefault?.();
      distance = clamp(distance * Math.exp(clamp(event.deltaY * 0.001, -1, 1)), config.minDistance, config.maxDistance);
      schedule(); return true;
    },
    resetInput,
    dispose() { disposed = true; resetInput(); },
  };
}
