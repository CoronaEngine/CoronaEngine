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



function finiteStoryCameraVector(value, { requireDirection = false } = {}) {
  if (!Array.isArray(value) || value.length !== 3) return null;
  const numeric = value.map(Number);
  if (!numeric.every(Number.isFinite)) return null;
  if (requireDirection && vectorLength(numeric) <= 1e-8) return null;
  return numeric;
}

export function storyCameraGroundY(
  positionValue,
  terrainHeightAt = null,
  groundOffset = 1.6,
  fallbackMinY = Number.NEGATIVE_INFINITY
) {
  const position = finiteStoryCameraVector(positionValue);
  if (!position) return Number.NaN;
  const sampledTerrain =
    typeof terrainHeightAt === 'function'
      ? Number(terrainHeightAt(position[0], position[2]))
      : Number.NaN;
  const offset = Math.max(0, Number(groundOffset) || DEFAULT_STORY_CAMERA_GROUND_OFFSET);
  if (Number.isFinite(sampledTerrain)) return sampledTerrain + offset;
  const fallback = Number(fallbackMinY);
  return Number.isFinite(fallback) ? fallback : Number.NaN;
}

export function groundStoryCameraPose(
  pose = {},
  {
    terrainHeightAt = null,
    groundOffset = 1.6,
    fallbackMinY = Number.NEGATIVE_INFINITY,
    maximumHoverHeight = 3,
    maximumY = Number.POSITIVE_INFINITY,
    fallbackPose = {},
    tolerance = 0.03,
  } = {}
) {
  const numericFallbackMinY = Number(fallbackMinY);
  const fallbackPosition =
    finiteStoryCameraVector(fallbackPose?.position) || [
      0,
      Number.isFinite(numericFallbackMinY) ? numericFallbackMinY : 0,
      0,
    ];
  const fallbackForward =
    finiteStoryCameraVector(fallbackPose?.forward, { requireDirection: true }) || DEFAULT_FORWARD;
  const fallbackWorldUp =
    finiteStoryCameraVector(fallbackPose?.worldUp ?? fallbackPose?.world_up, {
      requireDirection: true,
    }) || DEFAULT_UP;
  const sourcePosition = finiteStoryCameraVector(pose?.position);
  const sourceForward = finiteStoryCameraVector(pose?.forward, { requireDirection: true });
  const sourceWorldUp = finiteStoryCameraVector(pose?.worldUp ?? pose?.world_up, {
    requireDirection: true,
  });
  const reasons = [];
  const position = sourcePosition ? [...sourcePosition] : [...fallbackPosition];
  if (!sourcePosition) reasons.push('invalid-position');

  const groundY = storyCameraGroundY(position, terrainHeightAt, groundOffset, fallbackMinY);
  const hoverLimit = Math.max(0, Number(maximumHoverHeight) || 3);
  const epsilon = Math.max(0, Number(tolerance) || 0.03);
  if (Number.isFinite(groundY)) {
    if (position[1] < groundY - epsilon) {
      position[1] = groundY;
      reasons.push('below-ground');
    } else if (position[1] > groundY + hoverLimit) {
      position[1] = groundY;
      reasons.push('hovering');
    }
  }
  const ceiling = Number(maximumY);
  if (Number.isFinite(ceiling) && position[1] > ceiling) {
    position[1] = ceiling;
    reasons.push('above-ceiling');
  }
  if (!sourceForward) reasons.push('invalid-forward');
  if (!sourceWorldUp) reasons.push('invalid-world-up');

  const fov = Number(pose?.fov);
  const fallbackFov = Number(fallbackPose?.fov);
  return {
    pose: {
      position,
      forward: normalizeVector(sourceForward || fallbackForward, fallbackForward),
      worldUp: normalizeVector(sourceWorldUp || fallbackWorldUp, fallbackWorldUp),
      fov: Number.isFinite(fov) ? fov : Number.isFinite(fallbackFov) ? fallbackFov : 45,
    },
    changed: reasons.length > 0,
    grounded: Number.isFinite(groundY) && Math.abs(position[1] - groundY) <= epsilon,
    groundY,
    reasons,
  };
}

export function isStoryCameraPoseUnsafe(pose = {}, options = -Infinity) {
  if (!finiteStoryCameraVector(pose?.position)) return true;
  if (!finiteStoryCameraVector(pose?.forward, { requireDirection: true })) return true;
  if (!finiteStoryCameraVector(pose?.worldUp ?? pose?.world_up, { requireDirection: true })) {
    return true;
  }

  const settings =
    options && typeof options === 'object'
      ? options
      : { minimumY: Number(options) };
  const position = pose.position.map(Number);
  const forward = normalizeVector(pose.forward, DEFAULT_FORWARD);
  const worldUp = normalizeVector(pose.worldUp ?? pose.world_up, DEFAULT_UP);
  const minimumY = Number(settings.minimumY ?? settings.minY);
  const maximumY = Number(settings.maximumY ?? settings.maxY);
  if (Number.isFinite(minimumY) && position[1] < minimumY) return true;
  if (Number.isFinite(maximumY) && position[1] > maximumY) return true;

  const upwardDotLimit = Number(settings.upwardDotLimit ?? 0.55);
  if (Number.isFinite(upwardDotLimit) && dotVector(forward, worldUp) > upwardDotLimit) {
    return true;
  }

  const bounds = Array.isArray(settings.worldBounds)
    ? settings.worldBounds.slice(0, 6).map(Number)
    : null;
  if (bounds?.length === 6 && bounds.every(Number.isFinite)) {
    const minX = Math.min(bounds[0], bounds[3]);
    const minZ = Math.min(bounds[2], bounds[5]);
    const maxX = Math.max(bounds[0], bounds[3]);
    const maxZ = Math.max(bounds[2], bounds[5]);
    const margin = Math.max(0, Number(settings.outsideMargin) || 8);
    const outside =
      position[0] < minX - margin ||
      position[0] > maxX + margin ||
      position[2] < minZ - margin ||
      position[2] > maxZ + margin;
    if (outside) {
      const centerDirection = normalizeVector(
        [(minX + maxX) * 0.5 - position[0], 0, (minZ + maxZ) * 0.5 - position[2]],
        DEFAULT_FORWARD
      );
      const horizontalForwardValue = [forward[0], 0, forward[2]];
      if (vectorLength(horizontalForwardValue) <= 1e-8) return true;
      const horizontalForward = normalizeVector(horizontalForwardValue, DEFAULT_FORWARD);
      if (dotVector(horizontalForward, centerDirection) <= 0) return true;
    }
  }
  return false;
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

export const DEFAULT_STORY_CAMERA_GRAVITY = 9.8;
export const DEFAULT_STORY_CAMERA_GROUND_OFFSET = 1.6;

export function applyStoryCameraGravity(
  positionValue,
  deltaSeconds = 0,
  verticalVelocity = 0,
  activeKeys = new Set(),
  terrainHeightAt = null,
  groundOffset = DEFAULT_STORY_CAMERA_GROUND_OFFSET,
  positionBounds = null,
  gravity = DEFAULT_STORY_CAMERA_GRAVITY
) {
  const dt = Math.min(
    Math.max(Number.isFinite(Number(deltaSeconds)) ? Number(deltaSeconds) : 0, 0),
    STORY_CAMERA_MAX_DELTA_SECONDS
  );
  const position = Array.isArray(positionValue)
    ? positionValue.map((value) => (Number.isFinite(Number(value)) ? Number(value) : 0))
    : [0, 0, 0];
  const velocity = Number.isFinite(Number(verticalVelocity)) ? Number(verticalVelocity) : 0;
  const manualVerticalInput =
    hasMovementKey(activeKeys, 'KeyQ') || hasMovementKey(activeKeys, 'KeyE');
  const fallbackGround = Number(positionBounds?.minY);
  const sampledTerrain = typeof terrainHeightAt === 'function'
    ? Number(terrainHeightAt(position[0], position[2]))
    : Number.NaN;
  const offset = Math.max(0, Number(groundOffset) || DEFAULT_STORY_CAMERA_GROUND_OFFSET);
  const groundY = Number.isFinite(sampledTerrain)
    ? sampledTerrain + offset
    : Number.isFinite(fallbackGround)
      ? fallbackGround
      : -Infinity;
  if (manualVerticalInput || dt <= 0 || !Number.isFinite(groundY)) {
    const clampedPosition = [...position];
    if (manualVerticalInput && Number.isFinite(groundY) && clampedPosition[1] < groundY) {
      clampedPosition[1] = groundY;
    }
    return {
      position: clampedPosition,
      verticalVelocity: manualVerticalInput ? 0 : velocity,
      grounded: Number.isFinite(groundY) && clampedPosition[1] <= groundY + 1e-5,
      moved: clampedPosition.some((value, index) => Math.abs(value - position[index]) > 1e-8),
    };
  }

  if (position[1] <= groundY && velocity <= 0) {
    const landed = [...position];
    landed[1] = groundY;
    return {
      position: landed,
      verticalVelocity: 0,
      grounded: true,
      moved: Math.abs(landed[1] - position[1]) > 1e-8,
    };
  }

  const acceleration = Math.max(0, Number(gravity) || DEFAULT_STORY_CAMERA_GRAVITY);
  const nextVelocity = velocity - acceleration * dt;
  const nextPosition = [...position];
  nextPosition[1] += (velocity + nextVelocity) * 0.5 * dt;
  if (nextPosition[1] <= groundY) {
    nextPosition[1] = groundY;
    return {
      position: nextPosition,
      verticalVelocity: 0,
      grounded: true,
      moved: Math.abs(nextPosition[1] - position[1]) > 1e-8,
    };
  }
  if (Number.isFinite(positionBounds?.maxY)) {
    nextPosition[1] = Math.min(nextPosition[1], Number(positionBounds.maxY));
  }
  return {
    position: nextPosition,
    verticalVelocity: nextVelocity,
    grounded: false,
    moved: Math.abs(nextPosition[1] - position[1]) > 1e-8,
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
