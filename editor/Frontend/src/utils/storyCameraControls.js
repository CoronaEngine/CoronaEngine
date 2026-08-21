import { DEFAULT_STORY_CAMERA_MOVE_SPEED } from './nativeSceneViewport.js';

export const DEFAULT_STORY_CAMERA_MOUSE_SENSITIVITY = 0.15;
export const STORY_CAMERA_PITCH_DOT_LIMIT = 0.985;
export const STORY_CAMERA_MAX_DELTA_SECONDS = 0.1;

const DEFAULT_FORWARD = [0, 0, 1];
const DEFAULT_UP = [0, 1, 0];
const MOVEMENT_CODES = new Set(['KeyW', 'KeyA', 'KeyS', 'KeyD', 'KeyQ', 'KeyE']);

export function vectorLength(vector) {
  return Math.hypot(Number(vector?.[0]) || 0, Number(vector?.[1]) || 0, Number(vector?.[2]) || 0);
}

export function normalizeVector(vector, fallback = DEFAULT_FORWARD) {
  const source = Array.isArray(vector) && vector.length === 3 ? vector : fallback;
  const numeric = source.map((value) => Number(value) || 0);
  const length = vectorLength(numeric);
  if (length <= 1e-8) {
    const fallbackLength = vectorLength(fallback);
    return fallbackLength > 1e-8
      ? fallback.map((value) => Number(value) / fallbackLength)
      : [...DEFAULT_FORWARD];
  }
  return numeric.map((value) => value / length);
}

export function crossVector(left, right) {
  return [
    left[1] * right[2] - left[2] * right[1],
    left[2] * right[0] - left[0] * right[2],
    left[0] * right[1] - left[1] * right[0],
  ];
}

export function dotVector(left, right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

export function rotateVectorAroundAxis(vector, axis, angleRadians) {
  const normalizedAxis = normalizeVector(axis, DEFAULT_UP);
  const cosine = Math.cos(angleRadians);
  const sine = Math.sin(angleRadians);
  const dot = dotVector(normalizedAxis, vector);
  const cross = crossVector(normalizedAxis, vector);
  return [
    vector[0] * cosine + cross[0] * sine + normalizedAxis[0] * dot * (1 - cosine),
    vector[1] * cosine + cross[1] * sine + normalizedAxis[1] * dot * (1 - cosine),
    vector[2] * cosine + cross[2] * sine + normalizedAxis[2] * dot * (1 - cosine),
  ];
}

export function normalizeStoryCameraKey(eventOrCode) {
  if (typeof eventOrCode === 'string') {
    if (MOVEMENT_CODES.has(eventOrCode)) return eventOrCode;
    const key = eventOrCode.trim().toLowerCase();
    const code = key.length === 1 ? `Key${key.toUpperCase()}` : '';
    return MOVEMENT_CODES.has(code) ? code : '';
  }

  const code = String(eventOrCode?.code ?? '');
  if (MOVEMENT_CODES.has(code)) return code;
  return normalizeStoryCameraKey(String(eventOrCode?.key ?? ''));
}

function hasMovementKey(activeKeys, code) {
  if (activeKeys instanceof Set) return activeKeys.has(code);
  if (Array.isArray(activeKeys)) return activeKeys.includes(code);
  if (activeKeys && typeof activeKeys === 'object') {
    return Boolean(activeKeys[code] ?? activeKeys[code.slice(-1).toLowerCase()]);
  }
  return false;
}

export function isStoryCameraPoseUnsafe(pose = {}, minimumY = -Infinity) {
  const finiteVector = (value, { requireDirection = false } = {}) => {
    if (!Array.isArray(value) || value.length !== 3) return false;
    const numeric = value.map(Number);
    if (!numeric.every(Number.isFinite)) return false;
    return !requireDirection || vectorLength(numeric) > 1e-8;
  };

  if (!finiteVector(pose?.position)) return true;
  if (!finiteVector(pose?.forward, { requireDirection: true })) return true;
  if (!finiteVector(pose?.worldUp ?? pose?.world_up, { requireDirection: true })) return true;
  const safeMinimumY = Number(minimumY);
  return Number.isFinite(safeMinimumY) && Number(pose.position[1]) < safeMinimumY;
}

export function clampStoryCameraPosition(positionValue, bounds = null) {
  const position = Array.isArray(positionValue)
    ? positionValue.map((value) => (Number.isFinite(Number(value)) ? Number(value) : 0))
    : [0, 0, -5];
  if (!bounds || typeof bounds !== 'object') return position;

  const minimumY = Number(bounds.minY);
  const maximumY = Number(bounds.maxY);
  if (Number.isFinite(minimumY)) position[1] = Math.max(position[1], minimumY);
  if (Number.isFinite(maximumY)) position[1] = Math.min(position[1], maximumY);
  return position;
}

function cameraBasis(pose = {}) {
  const forward = normalizeVector(pose.forward, DEFAULT_FORWARD);
  const worldUp = normalizeVector(pose.worldUp ?? pose.world_up ?? pose.up, DEFAULT_UP);
  let right = crossVector(worldUp, forward);
  if (vectorLength(right) <= 1e-8) {
    const alternateUp = Math.abs(forward[1]) < 0.95 ? DEFAULT_UP : [1, 0, 0];
    right = crossVector(alternateUp, forward);
  }
  return {
    forward,
    worldUp,
    right: normalizeVector(right, [1, 0, 0]),
  };
}

export function applyStoryCameraMovement(
  pose = {},
  activeKeys = new Set(),
  deltaSeconds = 0,
  speed = pose.moveSpeed ?? DEFAULT_STORY_CAMERA_MOVE_SPEED,
  positionBounds = null
) {
  const dt = Math.min(
    Math.max(Number.isFinite(Number(deltaSeconds)) ? Number(deltaSeconds) : 0, 0),
    STORY_CAMERA_MAX_DELTA_SECONDS
  );
  const moveSpeed = Math.max(Number(speed) || DEFAULT_STORY_CAMERA_MOVE_SPEED, 0.01);
  const position = Array.isArray(pose.position)
    ? pose.position.map((value) => Number(value) || 0)
    : [0, 0, -5];
  if (dt <= 0) return { position, moved: false };

  const { forward, worldUp, right } = cameraBasis(pose);
  const direction = [0, 0, 0];
  const add = (axis, amount) => {
    for (let index = 0; index < 3; index += 1) direction[index] += axis[index] * amount;
  };

  if (hasMovementKey(activeKeys, 'KeyW')) add(forward, 1);
  if (hasMovementKey(activeKeys, 'KeyS')) add(forward, -1);
  if (hasMovementKey(activeKeys, 'KeyD')) add(right, 1);
  if (hasMovementKey(activeKeys, 'KeyA')) add(right, -1);
  if (hasMovementKey(activeKeys, 'KeyQ')) add(worldUp, 1);
  if (hasMovementKey(activeKeys, 'KeyE')) add(worldUp, -1);

  if (vectorLength(direction) <= 1e-8) return { position, moved: false };
  const unitDirection = normalizeVector(direction);
  const distance = moveSpeed * dt;
  const nextPosition = clampStoryCameraPosition(
    position.map((value, index) => value + unitDirection[index] * distance),
    positionBounds
  );
  return {
    position: nextPosition,
    moved: nextPosition.some((value, index) => Math.abs(value - position[index]) > 1e-8),
  };
}

export function rotateStoryCamera(
  forwardValue,
  worldUpValue,
  deltaX,
  deltaY,
  sensitivity = DEFAULT_STORY_CAMERA_MOUSE_SENSITIVITY
) {
  const forward = normalizeVector(forwardValue, DEFAULT_FORWARD);
  const worldUp = normalizeVector(worldUpValue, DEFAULT_UP);
  const degreesPerPixel = Math.max(Number(sensitivity) || 0, 0);
  const yawRadians = ((Number(deltaX) || 0) * degreesPerPixel * Math.PI) / 180;
  const pitchRadians = (-(Number(deltaY) || 0) * degreesPerPixel * Math.PI) / 180;

  let rotatedForward = normalizeVector(
    rotateVectorAroundAxis(forward, worldUp, yawRadians),
    forward
  );
  let right = crossVector(rotatedForward, worldUp);
  if (vectorLength(right) <= 1e-8) right = [1, 0, 0];
  const currentPitch = Math.asin(Math.max(-1, Math.min(1, dotVector(rotatedForward, worldUp))));
  const maximumPitch = Math.asin(STORY_CAMERA_PITCH_DOT_LIMIT);
  const targetPitch = Math.max(-maximumPitch, Math.min(maximumPitch, currentPitch + pitchRadians));
  rotatedForward = normalizeVector(
    rotateVectorAroundAxis(rotatedForward, right, targetPitch - currentPitch),
    rotatedForward
  );
  return normalizeVector(rotatedForward, forward);
}
