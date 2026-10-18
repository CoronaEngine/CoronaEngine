/** Models in game/art/models and stable identities owned by the story game. */
import { PROPHET_PLACEMENT } from './storyDialogue.mjs';
// Maria faces local -Z. Gameplay and the camera use logical +Z instead.
export const PLAYER_MODEL_YAW_OFFSET = Math.PI;
export const PROPHET_MODEL_REF = 'story.prophet.placement.v1';
export const PLAYER_MODEL_REF = 'story.player.facing.v1';
/**
 * The boss's **asset generation** — the migration key for its model.
 *
 * Exactly the reason `storyMap.MAP_MODEL_REF` exists: `create_actor`'s `skip_if_exists`
 * repair branch (`cef_editor_native_api_handlers.cpp:3410-3424`) returns as soon as it matched
 * the guid and only rewrites position/rotation/scale/follow_camera/body_type — it never
 * rewrites `actor.route`. A world whose boss actor still points at the dragon keeps rendering
 * the dragon no matter what this table says, so a swap has to delete the actor and create it
 * again, which is the one path that reaches the importer.
 *
 * **Any future swap of the boss's `asset` MUST bump this string**, or every existing world
 * silently keeps the previous model.
 */
export const BOSS_MODEL_REF = 'story.boss.em001.v1';
export const STORY_CHARACTERS = Object.freeze([
  { role: 'player', name: '玩家', guid: 'f9f5c8b0-7324-4b6c-a011-000000000001',
    asset: 'player/Maria WProp J J Ong.dae', x: 0, z: 0, height: 1.8, rotation: [0, PLAYER_MODEL_YAW_OFFSET, 0] },
  // The forecourt pocket in front of the temple's main hall. Authored at (0, 12) the original
  // dragon stood *inside* the hall's front bay: the shipped map has geometry 3.088 m high
  // within 4 m of that point (`test_no_map_geometry_stands_over_the_boss_spawn_site`
  // re-measures the GLB), which is the temple it was hidden inside. (2, 5.5) is the nearest
  // open ground in front of the hall — the highest map vertex within 4 m is 0.102 m, the
  // ground is the courtyard's y = 0, and the move is 6.80 m toward the player rather than away
  // from them. Past 4 m the temple front rises fast: 1.825 m at 5 m, 2.141 m at 6 m, 3.088 m at
  // 10 m. `size = 8` is therefore a measured ceiling, not a taste knob: 8 m is the largest
  // scale whose world footprint still fits under that ceiling, and everything from 12 m up
  // intersects the hall.
  //
  // em001_04 is a different creature from the dragon, and its own model AABB (26.641 x 17.520
  // x 6.106 model units) has one axis barely a third of the other two. At `size = 8` each
  // candidate rotation was measured against the map's real vertices:
  //   [0, 0, 0]     -> 8.00 w x 5.26 h x 1.83 d m, nothing above 0.102 m   <- shipped
  //   [0, 0, pi/2]  -> 5.26 w x 8.00 h x 1.83 d m, nothing above 0.000 m
  //   [0, pi/2, 0]  -> 1.83 w x 5.26 h x 8.00 d m, nothing above 0.000 m
  //   [pi/2, 0, 0]  -> 8.00 w x 1.83 h x 5.26 d m, geometry at 1.721 m -> intersects
  // Identity is the shipped rotation: it is the one the art was authored in, and rolling the
  // model onto another axis to buy height is what made an earlier build look like a creature
  // lying on its side. The hunt for the tallest box was also unnecessary — the dragon it
  // replaces was itself flat (8.00 w x 5.69 h x 1.68 d m), so 5.26 m of height is in the same
  // league, not a shortfall. What the measurements still decide is that 8 m is the largest
  // `size` whose footprint stays clear, and that only identity and the two yaw/roll variants
  // avoid the hall; see the growth guard in `test_no_map_geometry_stands_over_the_boss_spawn_site`.
  // The GLB carries exactly one action (`Root`, 4.883 s), which is what the engine loops; see
  // BOSS_MODEL_REF for why the actor is replaced rather than repaired when this asset changes.
  { role: 'boss', name: 'Boss', guid: 'f9f5c8b0-7324-4b6c-a011-000000000002',
    asset: 'boss/Em001_04.glb', model_ref: BOSS_MODEL_REF,
    x: 2, z: 5.5, size: 8, rotation: [0, 0, 0] },
  { role: 'merchant', name: '商人', guid: 'f9f5c8b0-7324-4b6c-a011-000000000003',
    asset: 'merchant/Maw J Laygo.dae', x: -30, z: -15, height: 1.8, rotation: [0, Math.PI, 0] },
  { role: 'prophet', name: '先知', guid: 'f9f5c8b0-7324-4b6c-a011-000000000004',
    asset: PROPHET_PLACEMENT.asset, x: 30, z: -15, height: PROPHET_PLACEMENT.height, rotation: [0, Math.PI, 0] },
].map(character => Object.freeze({ ...character, rotation: Object.freeze(character.rotation) })));

export const PLAYER_GUID = STORY_CHARACTERS[0].guid;
export const BOSS_GUID = STORY_CHARACTERS[1].guid;
export const PROPHET_GUID = STORY_CHARACTERS[3].guid;
export const unwrap = value => value?.data ?? value;
export const sceneSnapshot = value => {
  const data = unwrap(value);
  return data?.scene && typeof data.scene === 'object' ? data.scene : data;
};
export const vector3 = value => Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);

/** CEF opens <root>/(editor|CabbageEditor)/Frontend/dist/index.html. */
export function resolveStoryAssetPath(frontendUrl, asset) {
  const base = new URL(frontendUrl);
  if (base.protocol !== 'file:') throw new Error('剧情模型需要在引擎文件页面中加载');
  const url = new URL(`../../game/art/models/${asset.split('/').map(encodeURIComponent).join('/')}`, base);
  const path = decodeURIComponent(url.pathname);
  if (url.hostname) return `//${url.hostname}${path}`;
  return /^\/[a-z]:\//i.test(path) ? path.slice(1) : path;
}

/** Rz * Ry * Rx, matching the engine's ZYX Euler angles (radians). */
export function rotatePoint([x, y, z], [rx, ry, rz]) {
  const y1 = y * Math.cos(rx) - z * Math.sin(rx), z1 = y * Math.sin(rx) + z * Math.cos(rx);
  const x2 = x * Math.cos(ry) + z1 * Math.sin(ry), z2 = -x * Math.sin(ry) + z1 * Math.cos(ry);
  return [x2 * Math.cos(rz) - y1 * Math.sin(rz), x2 * Math.sin(rz) + y1 * Math.cos(rz), z2];
}
export const hasUsableBounds = bounds => Array.isArray(bounds) && bounds.length === 6
  && bounds.every(Number.isFinite) && [0, 1, 2].every(i => bounds[i + 3] > bounds[i]);

export function rotatedBounds(bounds, rotation) {
  if (!hasUsableBounds(bounds)) throw new Error('模型包围盒不可用');
  const points = [];
  for (let i = 0; i < 8; i++) points.push(rotatePoint([
    bounds[(i & 1) ? 3 : 0], bounds[(i & 2) ? 4 : 1], bounds[(i & 4) ? 5 : 2],
  ], rotation));
  return [...[0, 1, 2].map(axis => Math.min(...points.map(p => p[axis]))),
    ...[0, 1, 2].map(axis => Math.max(...points.map(p => p[axis])))];
}
/** Initial authored placement. Preserve the accepted saved Y, including stairs
 * and airborne saves; runtime ground contacts are resolved by native triangles. */
export function characterTransform(character, localBounds, savedGeometry = null) {
  const rotation = ['player', 'prophet'].includes(character.role) && vector3(savedGeometry?.rotation)
    ? [...savedGeometry.rotation] : [...character.rotation];
  const bounds = rotatedBounds(localBounds, rotation);
  const extent = [0, 1, 2].map(i => bounds[i + 3] - bounds[i]);
  const scale = character.height ? character.height / extent[1] : character.size / Math.max(...extent);
  const saved = ['player', 'prophet'].includes(character.role) && vector3(savedGeometry?.position)
    ? savedGeometry.position : null;
  const x = saved ? saved[0] : character.x;
  const z = saved ? saved[2] : character.z;
  const position = [x, saved ? saved[1] : -bounds[1] * scale, z];
  return { position, rotation, scale: [scale, scale, scale] };
}
