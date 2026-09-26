/** Third-person, kinematic player control. Native animation continues independently. */
import { PLAYER_GUID, vector3 } from './storyCharacters.mjs';

export const PLAYER_CONTROLS = Object.freeze({ speed: 3, maxDelta: 0.05,
  distance: 4.5, minDistance: 2.5, maxDistance: 10,
  pitch: 20 * Math.PI / 180, minPitch: 10 * Math.PI / 180, maxPitch: 65 * Math.PI / 180,
  sensitivity: 0.005 });
const clamp = (n, lo, hi) => Math.max(lo, Math.min(hi, n));
const movementKey = event => {
  const code = String(event.code || '').toLowerCase();
  const key = code.startsWith('key') ? code.slice(3) : String(event.key || '').toLowerCase();
  return ['w', 'a', 's', 'd'].includes(key) ? key : '';
};
const editing = event => (event.composedPath?.() || [event.target]).some(target => target?.isContentEditable
  || target?.closest?.('input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"]'));

export function createPlayerController({ getPose, setPose, submitPose, getBridge,
  isCurrent = () => true, isInputLocked = () => false, onError = () => {},
  requestFrame = callback => requestAnimationFrame(callback), cancelFrame = id => cancelAnimationFrame(id),
  now = () => globalThis.performance.now(), config = PLAYER_CONTROLS }) {
  let player = null, targetOffset = 0, yaw = 0, pitch = config.pitch, distance = config.distance;
  let frame = null, epoch = 0, lastTime = null, dragging = null, disposed = false;
  const keys = new Set();
  const ready = () => !disposed && isCurrent() && !isInputLocked() && player && getPose();
  function resetInput() {
    epoch++;
    if (frame !== null) cancelFrame(frame);
    frame = null; lastTime = null; dragging = null; keys.clear();
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
      let x = Number(keys.has('d')) - Number(keys.has('a'));
      let z = Number(keys.has('w')) - Number(keys.has('s'));
      const length = Math.hypot(x, z);
      if (length > 0 && delta > 0) {
        x /= length; z /= length;
        const dx = x * Math.cos(yaw) + z * Math.sin(yaw);
        const dz = z * Math.cos(yaw) - x * Math.sin(yaw);
        const position = [player.position[0] + dx * config.speed * delta, player.position[1],
          player.position[2] + dz * config.speed * delta];
        const rotation = [player.rotation[0], Math.atan2(dx, dz), player.rotation[2]];
        const bridge = getBridge();
        if (!bridge || bridge.actorTransform(player.handle, 0, position) !== true) throw new Error('玩家实时移动接口不可用');
        // Keep every accepted native change even if the next operation fails,
        // so exit saves the actual final pose instead of the previous frame.
        player = { ...player, position, version: player.version + 1 };
        if (bridge.actorTransform(player.handle, 1, rotation) !== true) throw new Error('玩家实时朝向接口不可用');
        player = { ...player, rotation };
      }
      poseCamera();
      if (keys.size) schedule();
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
        position: [...actor.geometry.position], rotation: [...actor.geometry.rotation], version: 0 };
      targetOffset = offset; yaw = player.rotation[1]; pitch = config.pitch; distance = config.distance;
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
        rotation: [...player.rotation], version: player.version } : null;
    },
    keyDown(event) {
      const key = movementKey(event);
      if (!key || event.ctrlKey || event.altKey || event.metaKey || event.isComposing || editing(event) || !ready()) return false;
      event.preventDefault?.(); keys.add(key); schedule(); return true;
    },
    keyUp(event) {
      const key = movementKey(event);
      if (!keys.delete(key)) return false;
      event.preventDefault?.();
      if (!keys.size && !dragging) resetInput();
      return true;
    },
    pointerDown(event) {
      if (event.button !== 2 || !ready()) return false;
      event.preventDefault?.();
      dragging = { id: event.pointerId, x: event.clientX, y: event.clientY };
      return true;
    },
    pointerMove(event) {
      if (!dragging || event.pointerId !== dragging.id) return false;
      if (!ready() || (Number.isFinite(event.buttons) && !(event.buttons & 2))) { resetInput(); return false; }
      const dx = event.clientX - dragging.x, dy = event.clientY - dragging.y;
      if (!Number.isFinite(dx) || !Number.isFinite(dy)) return false;
      yaw += dx * config.sensitivity;
      pitch = clamp(pitch + dy * config.sensitivity, config.minPitch, config.maxPitch);
      dragging = { id: event.pointerId, x: event.clientX, y: event.clientY };
      schedule(); return true;
    },
    pointerUp(event) {
      if (dragging && event.pointerId === dragging.id) { dragging = null; return true; }
      return false;
    },
    wheel(event) {
      if (!ready() || !Number.isFinite(event.deltaY) || !event.deltaY) return false;
      event.preventDefault?.();
      distance = clamp(distance * Math.exp(clamp(event.deltaY * 0.001, -1, 1)), config.minDistance, config.maxDistance);
      schedule(); return true;
    },
    resetInput,
    dispose() { resetInput(); disposed = true; },
  };
}
