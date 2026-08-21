export const STORY_WORLD_PLAN_ID = 'story-world-v1';
export const STORY_WORLD_SCENE_VERSION = 3;
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

export const STORY_WORLD_ASSET_METADATA = Object.freeze({
  'terrain_v3.obj': Object.freeze({ importScale: 120, sourceSize: [120, 16.40251, 120] }),
  'water_v3.obj': Object.freeze({ importScale: 50, sourceSize: [50, 1.8524, 36.348] }),
  'road_v3.obj': Object.freeze({ importScale: 16, sourceSize: [5.24114, 0.125, 16] }),
  'bridge_v3.obj': Object.freeze({ importScale: 12, sourceSize: [5.04, 2.65, 12] }),
  'gate_v3.obj': Object.freeze({ importScale: 12, sourceSize: [12, 8.596, 2.394] }),
  'house_small_v3.obj': Object.freeze({ importScale: 10.8, sourceSize: [10.8, 7.998, 9.321] }),
  'house_large_v3.obj': Object.freeze({ importScale: 13.8, sourceSize: [13.8, 9.119, 11.287] }),
  'pavilion_v3.obj': Object.freeze({ importScale: 9, sourceSize: [9, 6.925, 8.955] }),
  'tree_v3_a.obj': Object.freeze({ importScale: 9.1, sourceSize: [6.262, 9.1, 4.538] }),
  'tree_v3_b.obj': Object.freeze({ importScale: 9.1, sourceSize: [5.649, 9.1, 4.642] }),
  'rock_v3.obj': Object.freeze({ importScale: 4.48466, sourceSize: [4.48466, 4.154, 3.966] }),
  'fence_v3.obj': Object.freeze({ importScale: 8.2, sourceSize: [8.2, 2.229, 0.3] }),
  'lantern_v3.obj': Object.freeze({ importScale: 4.2, sourceSize: [1.92, 4.2, 0.95] }),
  'courtyard_v3.obj': Object.freeze({ importScale: 10, sourceSize: [10, 2.27, 7.64] }),
  'barrels_v3.obj': Object.freeze({ importScale: 2.3, sourceSize: [2.3, 1.431, 1.109] }),
  'woodpile_v3.obj': Object.freeze({ importScale: 3, sourceSize: [3, 1.51, 1.486] }),
  'reeds_v3.obj': Object.freeze({ importScale: 4.2, sourceSize: [4.2, 2.88, 2.09] }),
});

function finiteScale(scale = [1, 1, 1]) {
  return [0, 1, 2].map((index) => {
    const value = Number(scale?.[index]);
    return Number.isFinite(value) ? value : 1;
  });
}

export function storyWorldFinalScale(definition = {}) {
  const variantScale = finiteScale(definition.scale);
  const importScale = Number(
    definition.importScale ?? STORY_WORLD_ASSET_METADATA[definition.asset]?.importScale
  );
  const compensation = Number.isFinite(importScale) && importScale > 0 ? importScale : 1;
  return variantScale.map((value) => value * compensation);
}

export function storyWorldExpectedSize(definition = {}) {
  const sourceSize = definition.sourceSize ??
    STORY_WORLD_ASSET_METADATA[definition.asset]?.sourceSize ?? [0, 0, 0];
  const variantScale = finiteScale(definition.scale);
  return [0, 1, 2].map(
    (index) => Math.abs(Number(sourceSize[index]) || 0) * Math.abs(variantScale[index])
  );
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

const STATIC_BOX_PHYSICS = Object.freeze({
  ...STATIC_MESH_PHYSICS,
  collision_shape: 'box',
});

const NO_PHYSICS = Object.freeze({
  physics_enabled: false,
  collision_enabled: false,
  collision_shape: 'none',
});

function storyActor(index, definition) {
  const suffix = String(index).padStart(12, '0');
  const actor = {
    guid: `9bce0001-1a11-4a11-8a11-${suffix}`,
    position: [0, 0, 0],
    rotation: [0, 0, 0],
    scale: [1, 1, 1],
    physics: NO_PHYSICS,
    critical: false,
    phase: 'village',
    ...definition,
  };
  const assetMetadata = STORY_WORLD_ASSET_METADATA[actor.asset] || {};
  return Object.freeze({
    ...actor,
    importScale: Number(assetMetadata.importScale) || 1,
    sourceSize: Array.isArray(assetMetadata.sourceSize) ? [...assetMetadata.sourceSize] : [0, 0, 0],
  });
}

const actors = [
  storyActor(1, {
    name: 'StoryWorld_Terrain',
    asset: 'terrain_v3.obj',
    semanticRole: 'terrain_ground',
    entityType: 'terrain',
    physics: STATIC_MESH_PHYSICS,
    critical: true,
    phase: 'terrain',
  }),
  storyActor(2, {
    name: 'StoryWorld_YunxiLake',
    asset: 'water_v3.obj',
    semanticRole: 'water_lake',
    entityType: 'water',
    phase: 'water',
  }),
  ...[
    [-35, 0.22, -27, -25],
    [-28, 0.22, -16, -38],
    [-17, 0.22, -7, -53],
    [-4, 0.22, 1, -68],
    [9, 0.22, 8, -62],
  ].map(([x, y, z, rotationY], index) =>
    storyActor(3 + index, {
      name: `StoryWorld_Road_${index + 1}`,
      asset: 'road_v3.obj',
      position: [x, y, z],
      rotation: [0, rotationY, 0],
      semanticRole: 'terrain_road',
      entityType: 'road',
      physics: STATIC_BOX_PHYSICS,
      phase: 'roads',
    })
  ),
  storyActor(8, {
    name: 'StoryWorld_WoodBridge',
    asset: 'bridge_v3.obj',
    position: [20, -0.15, 3],
    rotation: [0, -28, 0],
    semanticRole: 'landmark_bridge',
    entityType: 'bridge',
    physics: STATIC_BOX_PHYSICS,
    phase: 'roads',
  }),
  storyActor(9, {
    name: 'StoryWorld_VillageGate',
    asset: 'gate_v3.obj',
    position: [-34, 0.15, -23],
    rotation: [0, -26, 0],
    semanticRole: 'landmark_gate',
    entityType: 'landmark',
  }),
  storyActor(10, {
    name: 'StoryWorld_LakesidePavilion',
    asset: 'pavilion_v3.obj',
    position: [15, -0.15, 16],
    rotation: [0, 18, 0],
    semanticRole: 'landmark_pavilion',
    entityType: 'landmark',
    physics: STATIC_BOX_PHYSICS,
  }),
  ...[
    ['House_Liu', 'house_large_v3.obj', -14, 0.2, 8, 15, 1.05],
    ['House_Tea', 'house_small_v3.obj', -1, 0.2, 12, -8, 1],
    ['House_Smith', 'house_small_v3.obj', -17, 0.2, -3, 168, 0.92],
    ['House_Healer', 'house_large_v3.obj', 2, 0.2, -5, 188, 0.94],
    ['House_East', 'house_small_v3.obj', 14, 0.15, -7, 205, 0.9],
    ['House_North', 'house_small_v3.obj', 0, 0.3, 25, 5, 0.9],
    ['House_Fisher', 'house_small_v3.obj', 22, -0.2, 15, -72, 0.82],
  ].map(([label, asset, x, y, z, rotationY, scale], index) =>
    storyActor(11 + index, {
      name: `StoryWorld_${label}`,
      asset,
      position: [x, y, z],
      rotation: [0, rotationY, 0],
      scale: [scale, scale, scale],
      semanticRole: 'building_village_house',
      entityType: 'building',
      physics: STATIC_BOX_PHYSICS,
    })
  ),
];

const treePositions = [
  [-48, 1, -12, 0.9],
  [-43, 1.2, 7, 1.15],
  [-38, 1.5, 28, 1],
  [-27, 0.4, 25, 0.85],
  [-24, 0.3, 14, 0.78],
  [-12, 0.3, 31, 1.05],
  [8, 0.25, 34, 0.88],
  [25, 0.2, 32, 1.1],
  [39, 0.4, 29, 1.2],
  [49, 2, 12, 1.05],
  [48, 2.2, -8, 0.95],
  [37, 0.8, -25, 1.15],
  [20, 0.4, -34, 0.9],
  [2, 0.3, -37, 1.08],
  [-18, 0.5, -35, 0.92],
  [-36, 1.2, -38, 1.18],
  [28, 0.1, 20, 0.72],
  [10, 0.2, 20, 0.76],
];
for (const [x, y, z, scale] of treePositions) {
  const index = actors.length + 1;
  actors.push(
    storyActor(index, {
      name: `StoryWorld_Tree_${String(index - 17).padStart(2, '0')}`,
      asset: index % 2 === 0 ? 'tree_v3_a.obj' : 'tree_v3_b.obj',
      position: [x, y, z],
      rotation: [0, (index * 47) % 360, 0],
      scale: [scale, scale, scale],
      semanticRole: 'vegetation_tree',
      entityType: 'vegetation',
      phase: 'decorations',
    })
  );
}

const rockPositions = [
  [-51, 2.2, 19, 1.4],
  [-31, 0.4, 37, 1],
  [44, 1.4, 34, 1.5],
  [51, 2.8, -25, 1.25],
  [27, 0.2, -43, 1.1],
  [-43, 1.4, -28, 0.9],
];
for (const [x, y, z, scale] of rockPositions) {
  const index = actors.length + 1;
  actors.push(
    storyActor(index, {
      name: `StoryWorld_Rock_${String(index - 35).padStart(2, '0')}`,
      asset: 'rock_v3.obj',
      position: [x, y, z],
      rotation: [0, (index * 31) % 360, 0],
      scale: [scale, scale, scale],
      semanticRole: 'terrain_rock',
      entityType: 'decoration',
      phase: 'decorations',
    })
  );
}

const fencePositions = [
  [-20, 0.15, 13, 10],
  [-10, 0.15, 16, 2],
  [-8, 0.15, -8, -5],
  [7, 0.15, -11, 8],
  [9, 0.1, 27, 0],
  [27, -0.1, 11, 82],
];
for (const [x, y, z, rotationY] of fencePositions) {
  const index = actors.length + 1;
  actors.push(
    storyActor(index, {
      name: `StoryWorld_Fence_${String(index - 41).padStart(2, '0')}`,
      asset: 'fence_v3.obj',
      position: [x, y, z],
      rotation: [0, rotationY, 0],
      semanticRole: 'building_fence',
      entityType: 'decoration',
      phase: 'decorations',
    })
  );
}

for (const [x, y, z, rotationY] of [
  [-27, 0.1, -17, -30],
  [-8, 0.1, -3, -50],
  [6, 0.1, 5, -65],
  [18, -0.1, 9, -55],
]) {
  const index = actors.length + 1;
  actors.push(
    storyActor(index, {
      name: `StoryWorld_Lantern_${String(index - 47).padStart(2, '0')}`,
      asset: 'lantern_v3.obj',
      position: [x, y, z],
      rotation: [0, rotationY, 0],
      semanticRole: 'landmark_lantern',
      entityType: 'decoration',
      phase: 'decorations',
    })
  );
}

const villageDetailActors = [
  ['StoryWorld_Courtyard_Liu', 'courtyard_v3.obj', -14, 0.12, 8, 15, 0.96, 'building_courtyard'],
  ['StoryWorld_Courtyard_Healer', 'courtyard_v3.obj', 2, 0.12, -5, 8, 0.9, 'building_courtyard'],
  [
    'StoryWorld_Barrels_Tea',
    'barrels_v3.obj',
    -4.2,
    0.18,
    9.2,
    -10,
    0.92,
    'decoration_village_prop',
  ],
  [
    'StoryWorld_Barrels_Fisher',
    'barrels_v3.obj',
    19.6,
    -0.05,
    12.7,
    18,
    0.86,
    'decoration_village_prop',
  ],
  [
    'StoryWorld_Woodpile_Smith',
    'woodpile_v3.obj',
    -20.6,
    0.18,
    -0.8,
    82,
    1,
    'decoration_village_prop',
  ],
  [
    'StoryWorld_Woodpile_North',
    'woodpile_v3.obj',
    4.6,
    0.2,
    24,
    -12,
    0.88,
    'decoration_village_prop',
  ],
  ['StoryWorld_Reeds_West', 'reeds_v3.obj', 27.5, -0.58, 16.5, 12, 1, 'vegetation_reeds'],
  ['StoryWorld_Reeds_East', 'reeds_v3.obj', 42, -0.58, 21.5, -20, 0.9, 'vegetation_reeds'],
];
for (const [name, asset, x, y, z, rotationY, scale, semanticRole] of villageDetailActors) {
  const index = actors.length + 1;
  actors.push(
    storyActor(index, {
      name,
      asset,
      position: [x, y, z],
      rotation: [0, rotationY, 0],
      scale: [scale, scale, scale],
      semanticRole,
      entityType: 'decoration',
      phase: 'decorations',
    })
  );
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
