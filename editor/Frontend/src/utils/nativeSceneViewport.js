const MAX_CAMERA_VIEWPORT_RENDER_PIXELS = 1920 * 1080;

export const STORY_CAMERA_NAME = 'StoryCamera';
export const DEFAULT_STORY_CAMERA_MOVE_SPEED = 12;

const FALLBACK_CAMERA_POSITION = [0, 0, -5];
const FALLBACK_CAMERA_FORWARD = [0, 0, 1];
const FALLBACK_CAMERA_UP = [0, 1, 0];

export function computeCameraViewportRenderSize(width, height, scale = 1) {
  const physicalWidth = Math.max(Math.round(Math.max(Number(width) || 0, 0) * scale), 1);
  const physicalHeight = Math.max(Math.round(Math.max(Number(height) || 0, 0) * scale), 1);
  const pixels = physicalWidth * physicalHeight;
  if (pixels <= MAX_CAMERA_VIEWPORT_RENDER_PIXELS) {
    return { width: physicalWidth, height: physicalHeight };
  }

  const ratio = Math.sqrt(MAX_CAMERA_VIEWPORT_RENDER_PIXELS / pixels);
  return {
    width: Math.max(Math.floor(physicalWidth * ratio), 1),
    height: Math.max(Math.floor(physicalHeight * ratio), 1),
  };
}

export function resolveInitialSceneId(initPayload = {}) {
  const initData = initPayload?.data ?? initPayload;
  const scenes = Array.isArray(initData?.scenes) ? initData.scenes : [];
  if (scenes.length > 0) {
    const requestedIndex = Number(initData?.active_index ?? 0);
    const activeIndex = Math.min(
      Math.max(Number.isFinite(requestedIndex) ? requestedIndex : 0, 0),
      scenes.length - 1
    );
    const activeScene = scenes[activeIndex] ?? scenes[0];
    return String(activeScene?.path ?? activeScene?.id ?? activeScene?.name ?? '').trim();
  }

  return String(initData?.path ?? initData?.scene_id ?? initData?.name ?? '').trim();
}

export function resolveSceneSnapshot(payload = {}) {
  const data = payload?.data ?? payload;
  if (data && typeof data === 'object' && !Array.isArray(data)) {
    if (data.scene && typeof data.scene === 'object' && !Array.isArray(data.scene)) {
      return data.scene;
    }
    return data;
  }
  return {};
}

export function cameraListFromPayload(payload = {}) {
  const data = payload?.data ?? payload;
  if (Array.isArray(data)) return data;
  return Array.isArray(data?.cameras) ? data.cameras : [];
}

export function cameraCandidatesFromSnapshot(payload = {}) {
  const snapshot = resolveSceneSnapshot(payload);
  const cameras = Array.isArray(snapshot.cameras) ? [...snapshot.cameras] : [];
  if (
    snapshot.camera &&
    typeof snapshot.camera === 'object' &&
    !cameras.includes(snapshot.camera)
  ) {
    cameras.unshift(snapshot.camera);
  }
  return cameras;
}

export function normalizeCameraVector(value, fallback) {
  if (!Array.isArray(value) || value.length !== 3) return [...fallback];
  const normalized = value.map((component) => Number(component));
  return normalized.every(Number.isFinite) ? normalized : [...fallback];
}

function cameraIdentity(camera = {}) {
  return {
    id: String(camera?.camera_id ?? camera?.id ?? '').trim(),
    name: String(camera?.name ?? '').trim(),
  };
}

function cameraMatches(camera, cameraId, cameraName) {
  const identity = cameraIdentity(camera);
  return Boolean(
    (cameraId && identity.id === String(cameraId)) ||
    (cameraName && identity.name === String(cameraName))
  );
}

function bindingFromCamera(camera, sceneId, fallbackName = '') {
  if (!camera || typeof camera !== 'object') return null;
  const cameraHandle = Number(camera.handle ?? camera.camera_handle ?? 0);
  if (!Number.isFinite(cameraHandle) || cameraHandle <= 0) return null;

  const identity = cameraIdentity(camera);
  const moveSpeed = Number(camera.story_move_speed ?? camera.storyMoveSpeed);
  return {
    sceneId: String(sceneId ?? '').trim(),
    cameraId: identity.id || null,
    cameraName: identity.name || fallbackName || null,
    cameraHandle,
    position: normalizeCameraVector(camera.position, FALLBACK_CAMERA_POSITION),
    forward: normalizeCameraVector(camera.forward, FALLBACK_CAMERA_FORWARD),
    worldUp: normalizeCameraVector(
      camera.world_up ?? camera.worldUp ?? camera.up,
      FALLBACK_CAMERA_UP
    ),
    fov: Number.isFinite(Number(camera.fov)) ? Number(camera.fov) : 45,
    moveSpeed:
      Number.isFinite(moveSpeed) && moveSpeed > 0 ? moveSpeed : DEFAULT_STORY_CAMERA_MOVE_SPEED,
  };
}

export function resolveStoryCameraBinding(
  snapshotPayload = {},
  cameraListPayload = {},
  fallbackSceneId = ''
) {
  const snapshot = resolveSceneSnapshot(snapshotPayload);
  const snapshotCameras = Array.isArray(snapshot.cameras) ? snapshot.cameras : [];
  const listedCameras = cameraListFromPayload(cameraListPayload);
  const snapshotSceneId = String(
    snapshot.scene_id ??
      snapshot.sceneId ??
      snapshot.id ??
      (typeof snapshot.scene === 'string' ? snapshot.scene : '') ??
      ''
  ).trim();
  const sceneId = snapshotSceneId || String(fallbackSceneId ?? '').trim();
  const activeCameraId = snapshot.active_camera_id ?? snapshot.activeCameraId ?? null;
  const activeCameraName =
    snapshot.active_camera_name ?? snapshot.activeCameraName ?? snapshot.camera?.name ?? null;

  const candidates = [];
  const addCandidate = (camera) => {
    if (!camera || candidates.includes(camera)) return;
    candidates.push(camera);
  };

  addCandidate(
    snapshotCameras.find((camera) => cameraMatches(camera, activeCameraId, activeCameraName))
  );
  addCandidate(snapshot.camera);
  addCandidate(
    listedCameras.find((camera) => cameraMatches(camera, activeCameraId, activeCameraName))
  );
  addCandidate(snapshotCameras.find((camera) => camera?.name === STORY_CAMERA_NAME));
  addCandidate(listedCameras.find((camera) => camera?.name === STORY_CAMERA_NAME));
  snapshotCameras.forEach(addCandidate);
  listedCameras.forEach(addCandidate);

  for (const camera of candidates) {
    const binding = bindingFromCamera(camera, sceneId || fallbackSceneId, activeCameraName);
    if (binding) return binding;
  }
  return null;
}

export function resolveActiveCameraBinding(payload = {}, fallbackSceneId = '') {
  return resolveStoryCameraBinding(payload, {}, fallbackSceneId);
}

export function shouldCreateStoryCamera({
  successfulEmptyChecks = 0,
  hasObservedCamera = false,
  createAttempted = false,
} = {}) {
  return successfulEmptyChecks >= 2 && !hasObservedCamera && !createAttempted;
}
