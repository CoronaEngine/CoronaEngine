export const STORY_WORLD_PLAN_ID = 'story-world-v1';
export const STORY_WORLD_SCENE_VERSION = 4;
export const STORY_WORLD_ACTOR_PREFIX = 'StoryWorld_';
export const STORY_WORLD_LOCATION_TITLE = '云溪村';
export const STORY_WORLD_LOCATION_OBJECTIVE = '探索村落';

export const STORY_WORLD_SUN_DIRECTION = [1, 10, 1];
export const STORY_WORLD_CAMERA_SPAWN = Object.freeze({
  position: [-45, 9, -34],
  forward: [0.737, -0.123, 0.667],
  worldUp: [0, 1, 0],
  fov: 45,
});
export const STORY_WORLD_CAMERA_MIN_Y = 1.5;
export const STORY_WORLD_CAMERA_MAX_Y = 80;
export const STORY_WORLD_CAMERA_BOUNDS = Object.freeze({
  minY: STORY_WORLD_CAMERA_MIN_Y,
  maxY: STORY_WORLD_CAMERA_MAX_Y,
});

export const STORY_WORLD_DEPRECATED_ACTORS = Object.freeze([
  'StoryWorld_Road_1',
  'StoryWorld_Road_2',
  'StoryWorld_Road_3',
  'StoryWorld_Road_4',
  'StoryWorld_Road_5',
]);

export const STORY_WORLD_LAKE = Object.freeze({
  center: Object.freeze([34, 18]),
  radii: Object.freeze([22, 14]),
  waterY: -1.15,
});

export const STORY_WORLD_ROADS = Object.freeze([
  Object.freeze({
    width: 4.3,
    points: Object.freeze([
      [-38, -28],
      [-31, -21],
      [-23, -14],
      [-15, -7],
      [-8, 2],
      [-7, 14],
      [-6, 28],
    ].map((point) => Object.freeze(point))),
  }),
  Object.freeze({
    width: 3.6,
    points: Object.freeze([
      [-8, 2],
      [-1, 1],
      [7, 1],
      [14, 4],
      [20, 8],
    ].map((point) => Object.freeze(point))),
  }),
  Object.freeze({
    width: 3.2,
    points: Object.freeze([
      [-16, 15],
      [-11, 15],
      [-7, 14],
    ].map((point) => Object.freeze(point))),
  }),
  Object.freeze({
    width: 3.2,
    points: Object.freeze([
      [-7, 24],
      [-3, 28],
      [-1, 31],
    ].map((point) => Object.freeze(point))),
  }),
]);

export const STORY_WORLD_ROAD_PATHS = Object.freeze(
  STORY_WORLD_ROADS.map((road) => road.points)
);

export const STORY_WORLD_ASSET_METADATA = Object.freeze({
  'terrain_v4.obj': Object.freeze({ importScale: 120, sourceSize: [120, 16.402511, 120] }),
  'water_v4.obj': Object.freeze({ importScale: 43.027531, sourceSize: [43.027531, 0.008, 27.209498] }),
  'road_network_v4.obj': Object.freeze({ importScale: 61.038173, sourceSize: [59.52028, 3.783623, 61.038173] }),
  'bridge_v4.obj': Object.freeze({ importScale: 12, sourceSize: [5.04, 2.65, 12] }),
  'gate_v4.obj': Object.freeze({ importScale: 11.827528, sourceSize: [11.827528, 8.472384, 2.36] }),
  'house_small_v4.obj': Object.freeze({ importScale: 10.844932, sourceSize: [10.844932, 8.031237, 9.36] }),
  'house_large_v4.obj': Object.freeze({ importScale: 13.644932, sourceSize: [13.644932, 9.01614, 11.16] }),
  'pavilion_v4.obj': Object.freeze({ importScale: 9.044984, sourceSize: [9.044984, 6.96, 9] }),
  'tree_v4_a.obj': Object.freeze({ importScale: 8.860601, sourceSize: [6.097172, 8.860601, 4.418872] }),
  'tree_v4_b.obj': Object.freeze({ importScale: 9.155378, sourceSize: [5.68297, 9.155378, 4.670034] }),
  'rock_v4.obj': Object.freeze({ importScale: 3.460254, sourceSize: [3.460254, 3.205199, 3.060377] }),
  'fence_v4.obj': Object.freeze({ importScale: 8.203424, sourceSize: [8.203424, 2.230118, 0.3] }),
  'lantern_v4.obj': Object.freeze({ importScale: 4.2, sourceSize: [1.92, 4.2, 0.95] }),
  'courtyard_v4.obj': Object.freeze({ importScale: 10, sourceSize: [10, 2.27, 7.64] }),
  'barrels_v4.obj': Object.freeze({ importScale: 2.281324, sourceSize: [2.281324, 1.41853, 1.1] }),
  'woodpile_v4.obj': Object.freeze({ importScale: 2.662435, sourceSize: [2.662435, 1.33995, 1.31904] }),
  'reeds_v4.obj': Object.freeze({ importScale: 4.148509, sourceSize: [4.148509, 2.844571, 2.066766] }),
});

export function storyWorldTerrainHeight(x, z) {
  const px = Number(x) || 0;
  const pz = Number(z) || 0;
  const center = Math.hypot(px * 0.72, pz * 0.72);
  const rim = Math.max(0, (center - 25) / 35);
  const hills = rim * rim * 12.5;
  const peaks =
    7.8 * Math.exp(-((px + 48) ** 2 + (pz - 34) ** 2) / 360) +
    9.5 * Math.exp(-((px - 48) ** 2 + (pz + 38) ** 2) / 310) +
    6.5 * Math.exp(-((px + 45) ** 2 + (pz + 45) ** 2) / 280);
  const basin = 2.7 * Math.exp(-((px - 34) ** 2 + (pz - 18) ** 2) / 270);
  const flat = Math.exp(-(px * px + pz * pz) / 520);
  const ripple =
    (Math.sin(px * 0.14) + Math.cos(pz * 0.12) + Math.sin((px + pz) * 0.09) * 0.6) *
    0.38 *
    (1 - flat);
  return Math.max(-1.3, hills + peaks + ripple - basin);
}

export function storyWorldGroundPosition(x, z, offset = 0) {
  return [x, storyWorldTerrainHeight(x, z) + offset, z];
}

export function isStoryWorldLakePoint(x, z, margin = 0) {
  const rx = Math.max(0.1, STORY_WORLD_LAKE.radii[0] + margin);
  const rz = Math.max(0.1, STORY_WORLD_LAKE.radii[1] + margin);
  const dx = (Number(x) - STORY_WORLD_LAKE.center[0]) / rx;
  const dz = (Number(z) - STORY_WORLD_LAKE.center[1]) / rz;
  return dx * dx + dz * dz <= 1;
}

function distancePointToSegment(point, start, end) {
  const vx = end[0] - start[0];
  const vz = end[1] - start[1];
  const lengthSq = vx * vx + vz * vz;
  if (lengthSq <= 1e-8) return Math.hypot(point[0] - start[0], point[1] - start[1]);
  const t = Math.max(0, Math.min(1, ((point[0] - start[0]) * vx + (point[1] - start[1]) * vz) / lengthSq));
  return Math.hypot(point[0] - (start[0] + vx * t), point[1] - (start[1] + vz * t));
}

export function storyWorldDistanceToRoad(x, z) {
  let distance = Infinity;
  for (const path of STORY_WORLD_ROAD_PATHS) {
    for (let index = 0; index < path.length - 1; index += 1) {
      distance = Math.min(distance, distancePointToSegment([x, z], path[index], path[index + 1]));
    }
  }
  return distance;
}

function pointToSegmentDistance2D(point, start, end) {
  return distancePointToSegment(point, start, end);
}

function segmentsIntersect2D(a, b, c, d) {
  const cross = (p, q, r) =>
    (q[0] - p[0]) * (r[1] - p[1]) - (q[1] - p[1]) * (r[0] - p[0]);
  const abC = cross(a, b, c);
  const abD = cross(a, b, d);
  const cdA = cross(c, d, a);
  const cdB = cross(c, d, b);
  return abC * abD <= 0 && cdA * cdB <= 0;
}

function segmentDistance2D(a, b, c, d) {
  if (segmentsIntersect2D(a, b, c, d)) return 0;
  return Math.min(
    pointToSegmentDistance2D(a, c, d),
    pointToSegmentDistance2D(b, c, d),
    pointToSegmentDistance2D(c, a, b),
    pointToSegmentDistance2D(d, a, b)
  );
}

export function storyWorldFootprintCorners(definition = {}, margin = 0) {
  if (!Array.isArray(definition.footprint) || definition.footprint.length < 2) return [];
  const width = Math.max(0, Number(definition.footprint[0]) || 0) + margin * 2;
  const depth = Math.max(0, Number(definition.footprint[1]) || 0) + margin * 2;
  const centerX = Number(definition.position?.[0]) || 0;
  const centerZ = Number(definition.position?.[2]) || 0;
  const angle = ((Number(definition.rotation?.[1]) || 0) * Math.PI) / 180;
  const cosine = Math.cos(angle);
  const sine = Math.sin(angle);
  return [
    [-width * 0.5, -depth * 0.5],
    [width * 0.5, -depth * 0.5],
    [width * 0.5, depth * 0.5],
    [-width * 0.5, depth * 0.5],
  ].map(([localX, localZ]) => [
    centerX + localX * cosine - localZ * sine,
    centerZ + localX * sine + localZ * cosine,
  ]);
}

function polygonAxes(corners) {
  return corners.map((corner, index) => {
    const next = corners[(index + 1) % corners.length];
    const edge = [next[0] - corner[0], next[1] - corner[1]];
    const length = Math.hypot(edge[0], edge[1]) || 1;
    return [-edge[1] / length, edge[0] / length];
  });
}

function projectionsOverlap(first, second, axis) {
  const project = (point) => point[0] * axis[0] + point[1] * axis[1];
  const firstValues = first.map(project);
  const secondValues = second.map(project);
  return Math.max(...firstValues) >= Math.min(...secondValues) &&
    Math.max(...secondValues) >= Math.min(...firstValues);
}

export function storyWorldFootprintDistance(first, second) {
  const firstCorners = storyWorldFootprintCorners(first);
  const secondCorners = storyWorldFootprintCorners(second);
  if (firstCorners.length < 4 || secondCorners.length < 4) return Infinity;
  const overlaps = [...polygonAxes(firstCorners), ...polygonAxes(secondCorners)].every((axis) =>
    projectionsOverlap(firstCorners, secondCorners, axis)
  );
  if (overlaps) return 0;
  let distance = Infinity;
  for (let firstIndex = 0; firstIndex < firstCorners.length; firstIndex += 1) {
    const firstStart = firstCorners[firstIndex];
    const firstEnd = firstCorners[(firstIndex + 1) % firstCorners.length];
    for (let secondIndex = 0; secondIndex < secondCorners.length; secondIndex += 1) {
      const secondStart = secondCorners[secondIndex];
      const secondEnd = secondCorners[(secondIndex + 1) % secondCorners.length];
      distance = Math.min(
        distance,
        segmentDistance2D(firstStart, firstEnd, secondStart, secondEnd)
      );
    }
  }
  return distance;
}

export function storyWorldFootprintDistanceToRoad(definition = {}) {
  const corners = storyWorldFootprintCorners(definition);
  if (corners.length < 4) return storyWorldDistanceToRoad(
    Number(definition.position?.[0]) || 0,
    Number(definition.position?.[2]) || 0
  );
  let distance = Infinity;
  for (const road of STORY_WORLD_ROADS) {
    for (let index = 0; index < road.points.length - 1; index += 1) {
      const start = road.points[index];
      const end = road.points[index + 1];
      for (let edgeIndex = 0; edgeIndex < corners.length; edgeIndex += 1) {
        distance = Math.min(
          distance,
          segmentDistance2D(
            corners[edgeIndex],
            corners[(edgeIndex + 1) % corners.length],
            start,
            end
          ) - road.width * 0.5
        );
      }
    }
  }
  return Math.max(0, distance);
}

function finiteScale(scale = [1, 1, 1]) {
  return [0, 1, 2].map((index) => {
    const value = Number(scale?.[index]);
    return Number.isFinite(value) ? value : 1;
  });
}

export function storyWorldFinalScale(definition = {}) {
  const variantScale = finiteScale(definition.scale);
  const importScale = Number(definition.importScale ?? STORY_WORLD_ASSET_METADATA[definition.asset]?.importScale);
  const compensation = Number.isFinite(importScale) && importScale > 0 ? importScale : 1;
  return variantScale.map((value) => value * compensation);
}

export function storyWorldExpectedSize(definition = {}) {
  const sourceSize = definition.sourceSize ?? STORY_WORLD_ASSET_METADATA[definition.asset]?.sourceSize ?? [0, 0, 0];
  const variantScale = finiteScale(definition.scale);
  return [0, 1, 2].map((index) => Math.abs(Number(sourceSize[index]) || 0) * Math.abs(variantScale[index]));
}

const STATIC_MESH_PHYSICS = Object.freeze({
  physics_enabled: true,
  collision_enabled: true,
  collision_shape: 'mesh',
  linear_lock: [true, true, true],
  angular_lock: [true, true, true],
  damping: 1,
  restitution: 0.05,
});
const STATIC_BOX_PHYSICS = Object.freeze({ ...STATIC_MESH_PHYSICS, collision_shape: 'box' });
const NO_PHYSICS = Object.freeze({ physics_enabled: false, collision_enabled: false, collision_shape: 'none' });

function storyActor(index, definition) {
  const suffix = String(index).padStart(12, '0');
  const actor = {
    guid: `9bce0001-1a11-4a11-8a11-${suffix}`,
    position: [0, 0, 0],
    rotation: [0, 0, 0],
    scale: [1, 1, 1],
    footprint: null,
    physics: NO_PHYSICS,
    critical: false,
    phase: 'village',
    ...definition,
  };
  const assetMetadata = STORY_WORLD_ASSET_METADATA[actor.asset] || {};
  return Object.freeze({
    ...actor,
    position: Object.freeze([...actor.position]),
    rotation: Object.freeze([...actor.rotation]),
    scale: Object.freeze([...actor.scale]),
    footprint: Array.isArray(actor.footprint) ? Object.freeze([...actor.footprint]) : null,
    importScale: Number(assetMetadata.importScale) || 1,
    sourceSize: Array.isArray(assetMetadata.sourceSize) ? Object.freeze([...assetMetadata.sourceSize]) : Object.freeze([0, 0, 0]),
  });
}

const ground = (x, z, offset = 0) => storyWorldGroundPosition(x, z, offset);
const actors = [
  storyActor(1, { name: 'StoryWorld_Terrain', asset: 'terrain_v4.obj', semanticRole: 'terrain_ground', entityType: 'terrain', physics: STATIC_MESH_PHYSICS, critical: true, phase: 'terrain', footprint: [120, 120] }),
  storyActor(2, { name: 'StoryWorld_YunxiLake', asset: 'water_v4.obj', position: [34, STORY_WORLD_LAKE.waterY, 18], semanticRole: 'water_lake', entityType: 'water', phase: 'water', footprint: [44, 28] }),
  storyActor(100, { name: 'StoryWorld_RoadNetwork', asset: 'road_network_v4.obj', semanticRole: 'terrain_road', entityType: 'road', physics: STATIC_MESH_PHYSICS, phase: 'roads' }),
  storyActor(8, { name: 'StoryWorld_WoodBridge', asset: 'bridge_v4.obj', position: ground(20, 8, -0.05), rotation: [0, 90, 0], semanticRole: 'landmark_bridge', entityType: 'bridge', physics: STATIC_MESH_PHYSICS, phase: 'roads', footprint: [12, 5.04] }),
  storyActor(9, { name: 'StoryWorld_VillageGate', asset: 'gate_v4.obj', position: ground(-38, -28, -0.05), rotation: [0, -45, 0], semanticRole: 'landmark_gate', entityType: 'landmark', physics: STATIC_BOX_PHYSICS, footprint: [11.83, 2.36] }),
  storyActor(10, { name: 'StoryWorld_LakesidePavilion', asset: 'pavilion_v4.obj', position: ground(16, 31, -0.04), rotation: [0, 12, 0], semanticRole: 'landmark_pavilion', entityType: 'landmark', physics: STATIC_BOX_PHYSICS, footprint: [9.05, 9] }),
  storyActor(11, { name: 'StoryWorld_House_Liu', asset: 'house_large_v4.obj', position: ground(-28, 19, -0.05), rotation: [0, -90, 0], scale: [1.02, 1.02, 1.02], semanticRole: 'building_village_house', entityType: 'building', physics: STATIC_BOX_PHYSICS, footprint: [11.4, 13.92] }),
  storyActor(12, { name: 'StoryWorld_House_Tea', asset: 'house_small_v4.obj', position: ground(5, 11, -0.05), rotation: [0, 90, 0], semanticRole: 'building_village_house', entityType: 'building', physics: STATIC_BOX_PHYSICS, footprint: [9.36, 10.85] }),
  storyActor(13, { name: 'StoryWorld_House_Smith', asset: 'house_small_v4.obj', position: ground(-28, -4, -0.05), rotation: [0, -90, 0], scale: [0.96, 0.96, 0.96], semanticRole: 'building_village_house', entityType: 'building', physics: STATIC_BOX_PHYSICS, footprint: [8.99, 10.41] }),
  storyActor(14, { name: 'StoryWorld_House_Healer', asset: 'house_large_v4.obj', position: ground(-28, 37, -0.05), rotation: [0, -90, 0], scale: [0.98, 0.98, 0.98], semanticRole: 'building_village_house', entityType: 'building', physics: STATIC_BOX_PHYSICS, footprint: [10.94, 13.37] }),
  storyActor(15, { name: 'StoryWorld_House_East', asset: 'house_small_v4.obj', position: ground(5, -11, -0.05), rotation: [0, 180, 0], scale: [0.94, 0.94, 0.94], semanticRole: 'building_village_house', entityType: 'building', physics: STATIC_BOX_PHYSICS, footprint: [10.2, 8.8] }),
  storyActor(16, { name: 'StoryWorld_House_North', asset: 'house_small_v4.obj', position: ground(-5, 39, -0.05), rotation: [0, 90, 0], scale: [0.92, 0.92, 0.92], semanticRole: 'building_village_house', entityType: 'building', physics: STATIC_BOX_PHYSICS, footprint: [8.62, 9.98] }),
  storyActor(17, { name: 'StoryWorld_House_Fisher', asset: 'house_small_v4.obj', position: ground(8, 23, -0.05), rotation: [0, 0, 0], scale: [0.86, 0.86, 0.86], semanticRole: 'building_village_house', entityType: 'building', physics: STATIC_BOX_PHYSICS, footprint: [9.33, 8.05] }),
];

const treePositions = [
  [-50, -10, 0.92], [-47, 8, 1.08], [-42, 32, 1], [-40, 48, 0.88], [-16, 50, 1.04], [8, 46, 0.9],
  [28, 43, 1.08], [47, 39, 1.12], [58, 4, 1], [52, -7, 0.94], [40, -26, 1.1], [23, -38, 0.88],
  [2, -44, 1.05], [-18, -43, 0.94], [-38, -41, 1.12], [-52, -29, 1], [20, -24, 0.82], [-8, -31, 0.86],
];
for (let offset = 0; offset < treePositions.length; offset += 1) {
  const [x, z, scale] = treePositions[offset];
  actors.push(storyActor(18 + offset, { name: `StoryWorld_Tree_${String(offset + 1).padStart(2, '0')}`, asset: offset % 2 === 0 ? 'tree_v4_a.obj' : 'tree_v4_b.obj', position: ground(x, z), rotation: [0, (offset * 67 + 19) % 360, 0], scale: [scale, scale, scale], semanticRole: 'vegetation_tree', entityType: 'vegetation', phase: 'decorations', footprint: [5.6 * scale, 4.5 * scale] }));
}

const rockPositions = [[-52, 20, 1.35], [-31, 43, 1], [47, 33, 1.42], [52, -27, 1.2], [28, -45, 1.08], [-48, -34, 0.92]];
for (let offset = 0; offset < rockPositions.length; offset += 1) {
  const [x, z, scale] = rockPositions[offset];
  actors.push(storyActor(36 + offset, { name: `StoryWorld_Rock_${String(offset + 1).padStart(2, '0')}`, asset: 'rock_v4.obj', position: ground(x, z, -0.02), rotation: [0, (offset * 53 + 11) % 360, 0], scale: [scale, scale, scale], semanticRole: 'terrain_rock', entityType: 'decoration', phase: 'decorations', footprint: [3.5 * scale, 3.1 * scale] }));
}

const fencePositions = [[-15, 15, 90], [-19, 22, 0], [-15, -3, 90], [9, -9, 90], [1, 36, 0], [11, 25, 0]];
for (let offset = 0; offset < fencePositions.length; offset += 1) {
  const [x, z, rotationY] = fencePositions[offset];
  actors.push(storyActor(42 + offset, { name: `StoryWorld_Fence_${String(offset + 1).padStart(2, '0')}`, asset: 'fence_v4.obj', position: ground(x, z, -0.02), rotation: [0, rotationY, 0], semanticRole: 'building_fence', entityType: 'decoration', phase: 'decorations', footprint: rotationY === 90 ? [0.3, 8.2] : [8.2, 0.3] }));
}

const lanternPositions = [[-34.5, -24.5, -45], [-18.5, -10.5, -45], [-10, 6, -15], [12, 10.5, 90]];
for (let offset = 0; offset < lanternPositions.length; offset += 1) {
  const [x, z, rotationY] = lanternPositions[offset];
  actors.push(storyActor(48 + offset, { name: `StoryWorld_Lantern_${String(offset + 1).padStart(2, '0')}`, asset: 'lantern_v4.obj', position: ground(x, z), rotation: [0, rotationY, 0], semanticRole: 'landmark_lantern', entityType: 'decoration', phase: 'decorations', footprint: [1.92, 0.95] }));
}

const details = [
  [52, 'StoryWorld_Courtyard_Liu', 'courtyard_v4.obj', -18.5, 19, -90, 0.96, 'building_courtyard', [7.34, 9.6]],
  [53, 'StoryWorld_Courtyard_Healer', 'courtyard_v4.obj', -18.5, 37, -90, 0.92, 'building_courtyard', [7.03, 9.2]],
  [54, 'StoryWorld_Barrels_Tea', 'barrels_v4.obj', -1.8, 8, 15, 0.92, 'decoration_village_prop', [2.1, 1.1]],
  [55, 'StoryWorld_Barrels_Fisher', 'barrels_v4.obj', 11.5, 18, -10, 0.86, 'decoration_village_prop', [2, 1]],
  [56, 'StoryWorld_Woodpile_Smith', 'woodpile_v4.obj', -16.5, -4.5, 90, 1, 'decoration_village_prop', [2.7, 1.4]],
  [57, 'StoryWorld_Woodpile_North', 'woodpile_v4.obj', 1.8, 35.8, -10, 0.88, 'decoration_village_prop', [2.4, 1.25]],
  [58, 'StoryWorld_Reeds_West', 'reeds_v4.obj', 17.5, 14, 12, 1, 'vegetation_reeds', [4.15, 2.1], STORY_WORLD_LAKE.waterY + 0.01],
  [59, 'StoryWorld_Reeds_East', 'reeds_v4.obj', 49, 22, -20, 0.9, 'vegetation_reeds', [3.75, 1.9], STORY_WORLD_LAKE.waterY + 0.01],
];
for (const [index, name, asset, x, z, rotationY, scale, semanticRole, footprint, explicitY] of details) {
  actors.push(storyActor(index, { name, asset, position: [x, Number.isFinite(explicitY) ? explicitY : storyWorldTerrainHeight(x, z) - 0.02, z], rotation: [0, rotationY, 0], scale: [scale, scale, scale], semanticRole, entityType: 'decoration', phase: 'decorations', footprint }));
}

export const STORY_WORLD_ACTORS = Object.freeze(actors);
export const STORY_WORLD_TERRAIN_ACTOR = STORY_WORLD_ACTORS[0];

export function createStoryWorldActorData(definition) {
  return {
    actor_name: definition.name,
    name: definition.name,
    actor_guid: definition.guid,
    position: [...definition.position],
    rotation: [...definition.rotation],
    scale: storyWorldFinalScale(definition),
    semantic_role: definition.semanticRole,
    entity_type: definition.entityType,
    source_plan_id: STORY_WORLD_PLAN_ID,
    source_scene_version: STORY_WORLD_SCENE_VERSION,
    skip_if_exists: true,
    update_if_exists: false,
    physics_enabled: false,
  };
}
