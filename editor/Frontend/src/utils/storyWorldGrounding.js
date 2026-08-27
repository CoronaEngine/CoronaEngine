function finiteVector(value, fallback = [0, 0, 0]) {
  if (Array.isArray(value) && value.length >= 3) {
    const result = value.slice(0, 3).map(Number);
    if (result.every(Number.isFinite)) return result;
  }
  if (value && typeof value === 'object') {
    const result = [value.x, value.y, value.z].map(Number);
    if (result.every(Number.isFinite)) return result;
  }
  return [...fallback];
}

export function normalizeStoryGroundingAabb(value) {
  let source = [];
  if (Array.isArray(value)) source = value.slice(0, 6);
  else if (value && typeof value === 'object') {
    if (Array.isArray(value.min) && Array.isArray(value.max)) {
      source = [...value.min.slice(0, 3), ...value.max.slice(0, 3)];
    } else {
      source = [
        value.min_x ?? value.minX,
        value.min_y ?? value.minY,
        value.min_z ?? value.minZ,
        value.max_x ?? value.maxX,
        value.max_y ?? value.maxY,
        value.max_z ?? value.maxZ,
      ];
    }
  }
  const values = source.map(Number);
  if (values.length < 6 || !values.every(Number.isFinite)) return null;
  return [
    Math.min(values[0], values[3]),
    Math.min(values[1], values[4]),
    Math.min(values[2], values[5]),
    Math.max(values[0], values[3]),
    Math.max(values[1], values[4]),
    Math.max(values[2], values[5]),
  ];
}

export function storyActorWorldPosition(actor = {}, fallback = [0, 0, 0]) {
  return finiteVector(actor?.geometry?.position ?? actor?.position, fallback);
}

export function storyActorWorldRotation(actor = {}, fallback = [0, 0, 0]) {
  return finiteVector(actor?.geometry?.rotation ?? actor?.rotation, fallback);
}

export function storyActorWorldScale(actor = {}, fallback = [1, 1, 1]) {
  return finiteVector(actor?.geometry?.scale ?? actor?.scale, fallback);
}

export function calculateStoryActorGroundingCorrection({
  actor = {},
  definition = {},
  terrainHeightAt = null,
  contactOffset = definition?.groundingOffset ?? 0,
  waterY = null,
  threshold = 0.03,
} = {}) {
  const aabb = normalizeStoryGroundingAabb(
    actor?.world_aabb ?? actor?.worldAabb ?? actor?.geometry?.world_aabb ?? actor?.geometry?.worldAabb,
  );
  const position = storyActorWorldPosition(actor, definition?.position ?? [0, 0, 0]);
  const mode = String(definition?.groundingMode || 'terrain').toLowerCase();
  const sampledTerrain =
    typeof terrainHeightAt === 'function'
      ? Number(terrainHeightAt(position[0], position[2]))
      : Number.NaN;
  const explicitTargetHeight = Number(definition?.groundingTargetHeight);
  const baseHeight =
    mode === 'water'
      ? Number(waterY)
      : mode === 'road' && Number.isFinite(explicitTargetHeight)
        ? explicitTargetHeight
        : sampledTerrain;
  if (!aabb || !Number.isFinite(baseHeight)) {
    return {
      correctionY: 0,
      targetMinY: Number.NaN,
      actualMinY: aabb?.[1] ?? Number.NaN,
      grounded: false,
      valid: false,
      reason: !aabb ? 'invalid-aabb' : 'invalid-ground-height',
    };
  }
  const offset = Number.isFinite(Number(contactOffset)) ? Number(contactOffset) : 0;
  const targetMinY = baseHeight + offset;
  const actualMinY = aabb[1];
  const correctionY = targetMinY - actualMinY;
  const tolerance = Math.max(0, Number(threshold) || 0.03);
  return {
    correctionY,
    targetMinY,
    actualMinY,
    grounded: Math.abs(correctionY) <= tolerance,
    valid: true,
  };
}

export function createGroundedActorTransform(actor = {}, correctionY = 0) {
  const position = storyActorWorldPosition(actor);
  position[1] += Number(correctionY) || 0;
  return {
    position,
    rotation: storyActorWorldRotation(actor),
    scale: storyActorWorldScale(actor),
  };
}
