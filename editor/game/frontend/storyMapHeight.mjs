/**
 * How high the map's surface is at (x, z) — the story world's real ground line.
 *
 * The map is scenery with no collision (`body_type: 'phantom'`): the engine builds no triangle
 * octree for it, so no engine query can answer "what is under the character's feet?". The
 * answer is baked offline instead (`editor/game/tools/bake_story_map_height.py`, output
 * `game/data/story-map-height.json`) and read here as a plain lookup table, which is why
 * standing on the terrain costs nothing per frame.
 *
 * This module is pure and engine-free: JSON in (`setHeightGrid`), metres out (`heightAt`). The
 * placement it maps through is imported from `storyMap.mjs` — `scale`, `centreY` and the
 * surface lift are never re-stated here, so re-placing the map re-places the ground with it.
 *
 * ## The model -> world mapping
 *
 * The engine normalises every static mesh into a unit cube — the model AABB centre becomes the
 * origin and the largest axis becomes 1 (`finalize_normalization_params` /
 * `normalize_vertices_with_global_params`, `modules/corona_resource/src/resource/types/parse_common.h:53,132`)
 * — and then applies the actor's transform to that normalised geometry
 * (`ModelTransform::compute_matrix` is `T * R * S`, `src/shared_data_hub.cpp:8`; the world AABB
 * the editor reports is `position + local * scale`, `native_actor_world_aabb`,
 * `src/systems/ui/cef/cef_editor_native_api_handlers.cpp:3023`). So for the map actor's own
 * rotation (identity) a model point lands at:
 *
 *   world_x = position[0] + (model_x - centre_x) * scale / MAP_MAX_AXIS
 *   world_y = position[1] + (model_y - centreY)   * scale / MAP_MAX_AXIS
 *   world_z = position[2] + (model_z - centre_z) * scale / MAP_MAX_AXIS
 *
 * `MAP_MAX_AXIS` is the model's real largest axis (z, 1680.0027 m), so
 * `scale / MAP_MAX_AXIS` is the one factor that turns model metres into world metres; at the
 * shipped `scale = 90` the map is 64.8 x 14.7 x 90 m, exactly as `storyMap.mjs` documents.
 * The grid file is stored in that same normalised frame (its `bounds` are already relative to
 * the model AABB centre in x/z), so `position[0]`/`position[2]` — both 0 — are the only
 * placement numbers needed horizontally.
 *
 * `heightAt` returns `null` when no grid is loaded, so a caller can fall back to the constant
 * engine floor instead of guessing: nothing regresses when the data file is missing.
 */

import { MAP_MAX_AXIS, MAP_PLACEMENT, mapOriginTransform } from './storyMap.mjs';

/** The grid file format this module understands. */
export const HEIGHT_GRID_VERSION = 1;
/** `heights` are int16 centimetres relative to `minY`. */
export const HEIGHT_GRID_UNIT = 'cm';
/** Guard against a corrupt `res` turning one lookup into an unbounded allocation. */
const MAX_RESOLUTION = 1024;
const INT16_MAX = 32767;

/** Model metres to world metres: the engine's unit-cube normalisation divided back out. */
export const MODEL_TO_WORLD = MAP_PLACEMENT.scale / MAP_MAX_AXIS;

const finite = value => typeof value === 'number' && Number.isFinite(value);
const integer = value => Number.isInteger(value);
const clamp = (value, low, high) => Math.max(low, Math.min(high, value));

/**
 * Validate and freeze one parsed grid document.
 *
 * Strict on purpose: a truncated or hand-edited file must fail loudly here rather than feed a
 * wrong ground height to the player, and a "hole" (a null, missing or out-of-range cell) is
 * rejected instead of being interpolated as if it were terrain. The bake script fills holes by
 * nearest-neighbour for exactly this reason.
 */
export function parseHeightGrid(raw) {
  const source = raw && typeof raw === 'object' ? raw : {};
  if (source.version !== HEIGHT_GRID_VERSION) throw new Error('地图高度网格版本无效');
  if (source.unit !== HEIGHT_GRID_UNIT) throw new Error('地图高度网格单位无效');
  const res = source.res;
  if (!Array.isArray(res) || res.length !== 2 || !res.every(integer)
    || res[0] < 2 || res[1] < 2 || res[0] > MAX_RESOLUTION || res[1] > MAX_RESOLUTION) {
    throw new Error('地图高度网格分辨率无效');
  }
  const bounds = source.bounds;
  if (!Array.isArray(bounds) || bounds.length !== 4 || !bounds.every(finite)
    || bounds[1] <= bounds[0] || bounds[3] <= bounds[2]) {
    throw new Error('地图高度网格范围无效');
  }
  if (!finite(source.minY)) throw new Error('地图高度网格基准高度无效');
  const [nx, nz] = res;
  const heights = source.heights;
  if (!heights || heights.length !== nx * nz) throw new Error('地图高度网格数据长度无效');
  const cells = new Int16Array(nx * nz);
  for (let index = 0; index < cells.length; index++) {
    const value = heights[index];
    // `typeof` first: `Number(null)` is 0, so a hole written as null used to be read as a real
    // sample at `minY` instead of being refused — the one case this strictness exists for.
    if (typeof value !== 'number' || !integer(value) || value < 0 || value > INT16_MAX) {
      throw new Error('地图高度网格数据无效');
    }
    cells[index] = value;
  }
  return Object.freeze({
    res: Object.freeze([nx, nz]),
    bounds: Object.freeze([...bounds]),
    minY: source.minY,
    heights: cells,
  });
}

let grid = null;

/** Install a parsed (or raw) grid. Returns the parsed grid so callers can assert on it. */
export function setHeightGrid(document) {
  grid = document && document.heights instanceof Int16Array && Object.isFrozen(document)
    ? document : parseHeightGrid(document);
  return grid;
}

/** Forget the grid: the ground goes back to the caller's constant (the engine floor). */
export function clearHeightGrid() { grid = null; }

/** True once a grid is loaded, so callers can keep a constant-ground path for its absence. */
export function hasHeightGrid() { return grid !== null; }

/** The loaded grid's own numbers, for tests and diagnostics. `null` without a grid. */
export function heightGridInfo() {
  if (!grid) return null;
  const [nx, nz] = grid.res;
  const cells = grid.heights;
  let low = Infinity, high = -Infinity;
  for (const value of cells) {
    if (value < low) low = value;
    if (value > high) high = value;
  }
  return Object.freeze({ res: [...grid.res], nx, nz, bounds: [...grid.bounds], minY: grid.minY,
    minCell: low, maxCell: high, minYModel: grid.minY + low / 100, maxYModel: grid.minY + high / 100 });
}

/** The world-space x/z rectangle the grid covers, in metres. `null` without a grid. */
export function heightFootprint(placement = MAP_PLACEMENT) {
  if (!grid) return null;
  const transform = mapOriginTransform(placement);
  const [minX, maxX, minZ, maxZ] = grid.bounds;
  return Object.freeze([transform.position[0] + minX * MODEL_TO_WORLD,
    transform.position[0] + maxX * MODEL_TO_WORLD,
    transform.position[2] + minZ * MODEL_TO_WORLD,
    transform.position[2] + maxZ * MODEL_TO_WORLD]);
}

/** World height of one cell's sample, in metres. */
function cellWorldY(index) {
  return mapOriginTransform().position[1] + (grid.minY + grid.heights[index] / 100 - MAP_PLACEMENT.centreY) * MODEL_TO_WORLD;
}

/**
 * World y of the map's surface at world (x, z), or `null` when no grid is loaded.
 *
 * Outside the footprint the nearest edge cell answers (the map has no ground beyond its own
 * outline, and clamping keeps a character who steps off the edge on the ground line instead of
 * falling through it). Inside, the four surrounding cell samples are interpolated bilinearly,
 * so walking up a staircase is a ramp instead of a series of steps.
 */
export function heightAt(x, z) {
  if (!grid || !finite(x) || !finite(z)) return null;
  const origin = mapOriginTransform().position;
  const modelX = (x - origin[0]) / MODEL_TO_WORLD;
  const modelZ = (z - origin[2]) / MODEL_TO_WORLD;
  const [minX, maxX, minZ, maxZ] = grid.bounds;
  const [nx, nz] = grid.res;
  const cellX = (maxX - minX) / nx;
  const cellZ = (maxZ - minZ) / nz;
  // Cell samples sit at cell centres; half a cell outside the bounds clamps to the edge sample.
  const u = (clamp(modelX, minX, maxX) - minX) / cellX - 0.5;
  const v = (clamp(modelZ, minZ, maxZ) - minZ) / cellZ - 0.5;
  const i0 = clamp(Math.floor(u), 0, nx - 1);
  const j0 = clamp(Math.floor(v), 0, nz - 1);
  const i1 = Math.min(i0 + 1, nx - 1);
  const j1 = Math.min(j0 + 1, nz - 1);
  const tx = clamp(u - i0, 0, 1);
  const tz = clamp(v - j0, 0, 1);
  const row = j0 * nx;
  const nextRow = j1 * nx;
  const height = (1 - tz) * ((1 - tx) * cellWorldY(row + i0) + tx * cellWorldY(row + i1))
    + tz * ((1 - tx) * cellWorldY(nextRow + i0) + tx * cellWorldY(nextRow + i1));
  return height;
}
