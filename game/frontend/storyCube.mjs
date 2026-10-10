/**
 * The cube that sits beside the prophet in a small world.
 *
 * `game/data/prophet.json`'s `cube` block is the authoritative content; this module is
 * the runtime projection of it, kept honest by `game/tests/test_story_assets.py`.
 *
 * Everything geometric here is a pure function so the placement and the adjustment
 * steps can be tested without an engine. The cube is deliberately NOT a
 * `STORY_CHARACTERS` entry: that list is asserted to be exactly the four inhabitants,
 * and the cube is a prop the player manipulates rather than a character.
 */

import { PLAYER_MODEL_YAW_OFFSET, sceneSnapshot } from './storyCharacters.mjs';

export const CUBE_GUID = 'story.prophet.cube';
export const CUBE_MODEL_REF = 'story.prophet.cube.v1';

const COORD_LIMIT = 1e6;
const finiteNumber = value => typeof value === 'number' && Number.isFinite(value);
const positiveNumber = value => finiteNumber(value) && value > 0 && value <= COORD_LIMIT;

/** Mirrors the `cube` block of game/data/prophet.json, field for field. */
export const CUBE_PLACEMENT = Object.freeze({
  asset: 'cube/cube.obj',
  guid: CUBE_GUID,
  modelRef: CUBE_MODEL_REF,
  sideDistance: 1.4,
  forwardDistance: 0.6,
  scale: 0.8,
  // Proportional, not additive: a fixed metre step is barely visible once the object is
  // large, whereas ×1.35 reads as a clear jump at any size.
  scaleFactor: 1.35,
  minScale: 0.15,
  maxScale: 6.0,
  moveStep: 1.2,
  moveRange: 10.0,
});

/** Strict validation, so a bad edit in the JSON table fails loudly instead of drifting. */
export function validateCubePlacement(raw) {
  const source = raw && typeof raw === 'object' ? raw : {};
  if (typeof source.asset !== 'string' || !source.asset.trim()) throw new Error('正方体资源路径无效');
  if (typeof source.guid !== 'string' || !source.guid.trim()) throw new Error('正方体标识无效');
  if (typeof source.modelRef !== 'string' || !source.modelRef.trim()) throw new Error('正方体版本标记无效');
  for (const key of ['sideDistance', 'forwardDistance', 'scale', 'minScale',
    'maxScale', 'moveStep', 'moveRange']) {
    if (!positiveNumber(source[key])) throw new Error(`正方体参数无效：${key}`);
  }
  if (!(source.scaleFactor > 1) || source.scaleFactor > COORD_LIMIT) {
    throw new Error('正方体缩放步进无效');
  }
  if (source.minScale >= source.maxScale) throw new Error('正方体缩放上下限无效');
  if (source.scale < source.minScale || source.scale > source.maxScale) throw new Error('正方体初始缩放超出上下限');
  return Object.freeze({
    asset: source.asset.trim(), guid: source.guid.trim(), modelRef: source.modelRef.trim(),
    sideDistance: source.sideDistance, forwardDistance: source.forwardDistance,
    scale: source.scale, scaleFactor: source.scaleFactor,
    minScale: source.minScale, maxScale: source.maxScale,
    moveStep: source.moveStep, moveRange: source.moveRange,
  });
}

validateCubePlacement(CUBE_PLACEMENT);

const clamp = (value, low, high) => Math.min(high, Math.max(low, value));

/** The logical facing of the player, matching how storyActors places the prophet. */
export function playerFacingYaw(playerActor) {
  const raw = playerActor?.geometry?.rotation?.[1];
  if (!finiteNumber(raw)) throw new Error('玩家朝向未就绪');
  return raw - PLAYER_MODEL_YAW_OFFSET;
}

/**
 * Screen-relative axes for a yaw, using the same `(sin, cos)` forward convention the
 * prophet placement uses, so "前/后/左/右" match what the player sees rather than a
 * fixed world axis.
 */
export function screenAxes(yaw) {
  if (!finiteNumber(yaw)) throw new Error('朝向无效');
  // `0 - sin` rather than `-sin` so a zero yaw yields +0 instead of -0: the transform is
  // compared field by field in tests and handed to the engine as-is.
  return { forward: [Math.sin(yaw), Math.cos(yaw)], right: [Math.cos(yaw), 0 - Math.sin(yaw)] };
}

/** Where the cube first appears: beside the prophet, on the player's right, slightly ahead. */
export function cubeSpawnTransform(prophet, facingYaw, placement = CUBE_PLACEMENT) {
  const position = prophet?.geometry?.position;
  if (!Array.isArray(position) || position.length !== 3 || !position.every(finiteNumber)) {
    throw new Error('先知位置未就绪');
  }
  const { forward, right } = screenAxes(facingYaw);
  return {
    position: [
      position[0] + right[0] * placement.sideDistance + forward[0] * placement.forwardDistance,
      position[1],
      position[2] + right[1] * placement.sideDistance + forward[1] * placement.forwardDistance,
    ],
    rotation: [0, facingYaw + Math.PI, 0],
    scale: [placement.scale, placement.scale, placement.scale],
  };
}

/** One scale step, proportional and clamped to the configured range. `direction` is ±1. */
export function cubeScaleTransform(current, direction, placement = CUBE_PLACEMENT) {
  if (direction !== 1 && direction !== -1) throw new Error(`未知的缩放方向：${direction}`);
  const scale = current?.scale?.[0];
  if (!finiteNumber(scale)) throw new Error('正方体当前缩放未知');
  const step = direction === 1 ? placement.scaleFactor : 1 / placement.scaleFactor;
  const next = clamp(scale * step, placement.minScale, placement.maxScale);
  return {
    position: [...current.position], rotation: [...current.rotation],
    scale: [next, next, next],
  };
}

/**
 * One move step along the player's screen axes, always clamped so the cube cannot be
 * flung away from the prophet it belongs to.
 */
export function cubeMoveTransform(current, direction, prophet, facingYaw, placement = CUBE_PLACEMENT) {
  if (!['forward', 'back', 'left', 'right'].includes(direction)) throw new Error(`未知的移动方向：${direction}`);
  const position = current?.position;
  const prophetPosition = prophet?.geometry?.position;
  if (!Array.isArray(position) || position.length !== 3 || !position.every(finiteNumber)) {
    throw new Error('正方体当前位置未知');
  }
  if (!Array.isArray(prophetPosition) || prophetPosition.length !== 3 || !prophetPosition.every(finiteNumber)) {
    throw new Error('先知位置未就绪');
  }
  const { forward, right } = screenAxes(facingYaw);
  const sign = direction === 'forward' || direction === 'right' ? 1 : -1;
  const axis = direction === 'forward' || direction === 'back' ? forward : right;
  const moved = [position[0] + axis[0] * placement.moveStep * sign, position[1],
    position[2] + axis[1] * placement.moveStep * sign];
  // Keep it beside the prophet: clamp the horizontal offset back into moveRange.
  const dx = moved[0] - prophetPosition[0];
  const dz = moved[2] - prophetPosition[2];
  const distance = Math.hypot(dx, dz);
  if (distance > placement.moveRange) {
    const ratio = placement.moveRange / distance;
    moved[0] = prophetPosition[0] + dx * ratio;
    moved[2] = prophetPosition[2] + dz * ratio;
  }
  return { position: moved, rotation: [...current.rotation], scale: [...current.scale] };
}

/** True for the game-owned cube only, so scenery and user models are never touched. */
export function isCubeActor(actor) {
  return actor?.actor_guid === CUBE_GUID || actor?.model_ref === CUBE_MODEL_REF;
}

export const isCubeReady = actor => Number(actor?.handle) > 0 && actor?.load_status === 'loaded';

/** Read the cube's live transform, or null when it is not ready to be adjusted. */
export function cubeTransformOf(actor) {
  const geometry = actor?.geometry;
  if (!Array.isArray(geometry?.position) || !Array.isArray(geometry?.rotation) || !Array.isArray(geometry?.scale)) {
    return null;
  }
  if (!geometry.position.every(finiteNumber) || !geometry.rotation.every(finiteNumber)
    || !geometry.scale.every(finiteNumber)) return null;
  return { position: [...geometry.position], rotation: [...geometry.rotation], scale: [...geometry.scale] };
}

/**
 * Bring the cube in line with the prophet and the current world, without ever undoing a
 * player's own adjustments: an already-marked cube keeps its saved transform.
 */
export async function ensureStoryCube({ api, sceneId, frontendUrl, role, prophetActor, playerActor,
  resolveAsset = null, placement = CUBE_PLACEMENT }) {
  const actors = sceneSnapshot(await api.scene.getSnapshot(sceneId))?.actors;
  const cube = (Array.isArray(actors) ? actors : []).find(isCubeActor) || null;

  if (role !== 'child') {
    // The cube is a small-world inhabitant; the main world must never show it.
    if (cube && cube.visible !== false) {
      await api.sceneTools.setActorState(sceneId, cube.actor_guid, { visible: false });
    }
    return null;
  }

  // One cube per small world. An existing cube is never re-created and never re-placed:
  // its saved transform is the player's own arrangement, and load state is irrelevant to
  // that decision. Only a cube without this version's marker is refreshed.
  if (cube && cube.model_ref === placement.modelRef) return cube;

  const facingYaw = playerFacingYaw(playerActor);
  const spawn = cube ? null : cubeSpawnTransform(prophetActor, facingYaw, placement);
  const route = cube?.route || (typeof resolveAsset === 'function'
    ? resolveAsset(frontendUrl, placement.asset) : null);
  if (!route) throw new Error('正方体资源路径不可用');

  // `skip_if_exists` + `update_if_exists` are what make the engine update the actor that
  // already carries this GUID instead of appending a second one.
  const created = await api.sceneTools.createActor(sceneId, route, 'model', {
    name: '先知方块', actor_guid: placement.guid, semantic_role: 'prophet-cube',
    entity_id: 'story.prophet.cube', entity_type: 'story_prop',
    model_ref: placement.modelRef, physics_enabled: false, follow_camera: false,
    skip_if_exists: true, update_if_exists: true,
    ...(spawn || { position: cube.geometry.position, rotation: cube.geometry.rotation,
      scale: cube.geometry.scale }),
  });
  const result = created?.actor || cube;
  if (!result) throw new Error('引擎未返回正方体');
  if (result.visible === false || result.follow_camera) {
    return (await api.sceneTools.setActorState(sceneId, result.actor_guid,
      { visible: true, follow_camera: false }))?.actor || result;
  }
  return result;
}
