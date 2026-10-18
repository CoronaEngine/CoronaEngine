/** Story viewport binding only; free-camera behavior is shared with creative mode. */
import { createViewportCameraController } from './viewportCameraController.js';

const vector = (value, fallback) => Array.isArray(value) && value.length === 3 && value.every(Number.isFinite)
  ? [...value] : [...fallback];

export function createStoryCameraController({
  getBridge, getRect, getPixelRatio = () => 1,
  isCurrent = () => true, isInputLocked = () => false,
  requestFrame, cancelFrame, now, onPlayerChanged, createControls = createViewportCameraController, onError,
}) {
  let camera = null, disposed = false;
  const current = () => !disposed && isCurrent();
  const controls = createControls({
    getBridge, onError,
    getPose: () => camera,
    setPose: (pose) => { camera = pose; },
    submitPose: (pose) => {
      if (!current()) return;
      return getBridge()?.cameraMove?.(pose.handle, [...pose.position], [...pose.forward], [...pose.up], pose.fov) ?? false;
    },
    isCurrent: current, isInputLocked, requestFrame, cancelFrame, now, getRect, onPlayerChanged,
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
      camera = { handle, sceneId, name: active.name, position: vector(active.position, [0, 0, -5]),
        forward: vector(active.forward, [0, 0, 1]), up: vector(active.world_up, [0, 1, 0]),
        fov: Number(active.fov) > 0 ? Number(active.fov) : 60 };
      // Clear transient editor UI without changing scene data or running scripts.
      getBridge()?.setViewportGizmoTarget?.(handle, sceneId, '', 0);
      getBridge()?.setViewportUiMode?.(handle, 'flat2d');
      getBridge()?.setViewportSystemCursorHidden?.(false, false);
      syncViewport();
      controls.onCameraBound?.();
      return true;
    },
    // Capture local changes even when their animation-frame submission was canceled.
    // No handle crosses the acknowledged persistence boundary.
    snapshotPose() {
      if (!current() || !camera) return null;
      return { sceneId: camera.sceneId, cameraName: camera.name, camera: {
        position: [...camera.position], forward: [...camera.forward],
        world_up: [...camera.up], fov: camera.fov,
      } };
    },
    syncViewport,
    dispose() { controls.dispose(); disposed = true; camera = null; },
  };
}
