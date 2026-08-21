const DEFAULT_MAP_RADIUS = 40;
const MIN_MAP_SPAN = 1;

function finiteVector3(value) {
  if (!Array.isArray(value) || value.length < 3) return null;
  const vector = value.slice(0, 3).map(Number);
  return vector.every(Number.isFinite) ? vector : null;
}

export function normalizeStoryMapAabb(value) {
  if (!Array.isArray(value) || value.length < 6) return null;
  const values = value.slice(0, 6).map(Number);
  if (!values.every(Number.isFinite)) return null;
  return {
    min: [
      Math.min(values[0], values[3]),
      Math.min(values[1], values[4]),
      Math.min(values[2], values[5]),
    ],
    max: [
      Math.max(values[0], values[3]),
      Math.max(values[1], values[4]),
      Math.max(values[2], values[5]),
    ],
  };
}

export function storyActorWorldPosition(actor = {}) {
  const aabb = normalizeStoryMapAabb(actor.world_aabb ?? actor.aabb);
  if (aabb) {
    return aabb.min.map((value, index) => (value + aabb.max[index]) / 2);
  }
  return finiteVector3(actor.geometry?.position ?? actor.position);
}

function markerKind(actor = {}) {
  const semanticRole = String(actor.semantic_role || '')
    .trim()
    .toLowerCase();
  const actorType = String(actor.actor_type || actor.type || '')
    .trim()
    .toLowerCase();
  if (/enemy|hostile|boss/.test(semanticRole)) return 'danger';
  if (/quest|objective|target/.test(semanticRole)) return 'quest';
  if (/pickup|loot|item|collect/.test(semanticRole)) return 'item';
  if (/light|sun/.test(semanticRole) || actorType === 'light') return 'light';
  if (/water|river|lake/.test(semanticRole) || actorType === 'water') return 'water';
  if (/building|house|village/.test(semanticRole) || actorType === 'building') return 'building';
  if (/landmark|bridge|gate|pavilion/.test(semanticRole)) return 'landmark';
  if (/vegetation|tree|forest/.test(semanticRole) || actorType === 'vegetation')
    return 'vegetation';
  if (/terrain|ground|road/.test(semanticRole) || actorType === 'terrain') return 'terrain';
  return 'actor';
}

export function storyMapMarkerFromActor(actor = {}, index = 0) {
  if (!actor || typeof actor !== 'object') return null;
  if (actor.follow_camera || actor.visible === false) return null;
  const actorType = String(actor.actor_type || actor.type || '')
    .trim()
    .toLowerCase();
  if (actorType === 'audio' || actorType === 'camera') return null;
  if (actor.load_status && String(actor.load_status).toLowerCase() !== 'loaded') return null;

  const position = storyActorWorldPosition(actor);
  if (!position) return null;
  return {
    id: String(
      actor.actor_guid || actor.entity_id || actor.handle || actor.name || `actor-${index}`
    ),
    name: String(actor.name || '未命名对象'),
    type: String(actor.actor_type || actor.type || 'actor'),
    semanticRole: String(actor.semantic_role || ''),
    position,
    kind: markerKind(actor),
  };
}

export function addStoryMapPadding(bounds, ratio = 0.05, minimumPadding = 1) {
  if (!bounds?.min || !bounds?.max) return null;
  const spanX = Math.max(bounds.max[0] - bounds.min[0], MIN_MAP_SPAN);
  const spanZ = Math.max(bounds.max[2] - bounds.min[2], MIN_MAP_SPAN);
  const paddingX = Math.max(spanX * Math.max(Number(ratio) || 0, 0), minimumPadding);
  const paddingZ = Math.max(spanZ * Math.max(Number(ratio) || 0, 0), minimumPadding);
  return {
    min: [bounds.min[0] - paddingX, bounds.min[1], bounds.min[2] - paddingZ],
    max: [bounds.max[0] + paddingX, bounds.max[1], bounds.max[2] + paddingZ],
  };
}

export function storyMapBoundsFromPoints(points = [], paddingRatio = 0.08) {
  const valid = points.map(finiteVector3).filter(Boolean);
  if (valid.length === 0) return null;
  const min = [...valid[0]];
  const max = [...valid[0]];
  for (const point of valid.slice(1)) {
    for (let index = 0; index < 3; index += 1) {
      min[index] = Math.min(min[index], point[index]);
      max[index] = Math.max(max[index], point[index]);
    }
  }
  if (max[0] - min[0] < MIN_MAP_SPAN) {
    min[0] -= MIN_MAP_SPAN / 2;
    max[0] += MIN_MAP_SPAN / 2;
  }
  if (max[2] - min[2] < MIN_MAP_SPAN) {
    min[2] -= MIN_MAP_SPAN / 2;
    max[2] += MIN_MAP_SPAN / 2;
  }
  return addStoryMapPadding({ min, max }, paddingRatio, 1);
}

export function createStoryLocalMapBounds(center, radius = DEFAULT_MAP_RADIUS) {
  const position = finiteVector3(center) ?? [0, 0, 0];
  const extent = Math.max(Number(radius) || DEFAULT_MAP_RADIUS, 1);
  return {
    min: [position[0] - extent, position[1] - extent, position[2] - extent],
    max: [position[0] + extent, position[1] + extent, position[2] + extent],
  };
}

export function projectStoryWorldToMap(position, bounds, { clamp = false } = {}) {
  const point = finiteVector3(position);
  if (!point || !bounds?.min || !bounds?.max) return null;
  const spanX = Math.max(Number(bounds.max[0]) - Number(bounds.min[0]), MIN_MAP_SPAN);
  const spanZ = Math.max(Number(bounds.max[2]) - Number(bounds.min[2]), MIN_MAP_SPAN);
  const rawX = ((point[0] - bounds.min[0]) / spanX) * 100;
  const rawY = ((bounds.max[2] - point[2]) / spanZ) * 100;
  const outOfBounds = rawX < 0 || rawX > 100 || rawY < 0 || rawY > 100;
  return {
    x: clamp ? Math.max(0, Math.min(100, rawX)) : rawX,
    y: clamp ? Math.max(0, Math.min(100, rawY)) : rawY,
    outOfBounds,
  };
}

export function storyPlayerHeadingDegrees(forward) {
  const direction = finiteVector3(forward);
  if (!direction || Math.hypot(direction[0], direction[2]) < 1e-8) return 0;
  return (Math.atan2(direction[0], direction[2]) * 180) / Math.PI;
}

export function buildStoryMapSnapshot(snapshot = {}, playerPosition = null) {
  const nestedScene = snapshot?.data?.scene;
  const source =
    nestedScene && typeof nestedScene === 'object' && !Array.isArray(nestedScene)
      ? nestedScene
      : snapshot?.data && typeof snapshot.data === 'object' && !Array.isArray(snapshot.data)
        ? snapshot.data
        : snapshot && typeof snapshot === 'object' && !Array.isArray(snapshot)
          ? snapshot
          : {};
  const actors = Array.isArray(source.actors) ? source.actors : [];
  const markers = actors.map(storyMapMarkerFromActor).filter(Boolean);
  const nativeBounds = normalizeStoryMapAabb(source.scene_aabb);
  const fallbackPoints = markers.map((marker) => marker.position);
  const player = finiteVector3(playerPosition);
  if (player) fallbackPoints.push(player);
  return {
    sceneName: String(source.scene_name || source.name || source.scene || ''),
    markers,
    bounds: nativeBounds
      ? addStoryMapPadding(nativeBounds)
      : storyMapBoundsFromPoints(fallbackPoints),
    boundsReady: Boolean(nativeBounds),
  };
}
