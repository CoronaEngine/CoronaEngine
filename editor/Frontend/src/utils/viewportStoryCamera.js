/** Story viewport binding only; free-camera behavior is shared with creative mode. */
import { createViewportCameraController } from './viewportCameraController.js';

const vector = (value, fallback) => Array.isArray(value) && value.length === 3 && value.every(Number.isFinite)
  ? [...value] : [...fallback];

export function createStoryCameraController({
  getBridge, getRect, getPixelRatio = () => 1,
  isCurrent = () => true, isInputLocked = () => false,
  requestFrame, cancelFrame, now,
}) {
  let camera = null, disposed = false;
  const current = () => !disposed && isCurrent();
  const controls = createViewportCameraController({
    getPose: () => camera,
    setPose: (pose) => { camera = pose; },
    submitPose: (pose) => {
      if (!current()) return;
      getBridge()?.cameraMove?.(pose.handle, [...pose.position], [...pose.forward], [...pose.up], pose.fov);
    },
    isCurrent: current, isInputLocked, requestFrame, cancelFrame, now,
  });
  function syncViewport() {
    const rect = getRect();
    if (!current() || !camera || !rect || rect.width <= 0 || rect.height <= 0) return false;
    const scale = Math.max(Number(getPixelRatio()) || 1, 0.01);
    const width = Math.max(1, Math.round(rect.width * scale));
    const height = Math.max(1, Math.round(rect.height * scale));
    const ratio = Math.min(1, Math.sqrt((1920 * 1080) / (width * height)));
    return getBridge()?.setCameraViewport?.(camera.handle,
      Math.max(0, Math.round(rect.left * scale)), Math.max(0, Math.round(rect.top * scale)), width, height,
      Math.max(1, Math.floor(width * ratio)), Math.max(1, Math.floor(height * ratio))) ?? false;
  }
  return {
    ...controls,
    bind(payload, sceneId) {
      if (!current()) return false;
      controls.resetInput();
      camera = null;
      // Native snapshots use `scene` for the route, not a nested snapshot.
      const data = payload?.data ?? payload;
      const snapshot = data?.scene && typeof data.scene === 'object' && !Array.isArray(data.scene)
        ? data.scene : data;
      const cameras = Array.isArray(snapshot?.cameras) ? snapshot.cameras : [];
      const name = snapshot?.active_camera_name ?? snapshot?.activeCameraName;
      const active = cameras.find((item) => item.name === name) ?? cameras[0] ?? snapshot?.camera;
      const handle = Number(active?.handle ?? active?.camera_handle ?? 0);
      if (!Number.isFinite(handle) || handle <= 0) throw new Error('当前场景没有可用相机');
      camera = { handle, position: vector(active.position, [0, 0, -5]),
        forward: vector(active.forward, [0, 0, 1]), up: vector(active.world_up, [0, 1, 0]),
        fov: Number(active.fov) > 0 ? Number(active.fov) : 60 };
      // Clear transient editor UI without changing scene data or running scripts.
      getBridge()?.setViewportGizmoTarget?.(handle, sceneId, '', 0);
      getBridge()?.setViewportUiMode?.(handle, 'flat2d');
      getBridge()?.setViewportSystemCursorHidden?.(false, false);
      syncViewport();
      return true;
    },
    syncViewport,
    dispose() { controls.dispose(); disposed = true; camera = null; },
  };
}
