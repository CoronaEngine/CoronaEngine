/**
 * The map's ground line, as arithmetic: JSON in, world metres out.
 *
 * These tests exist because the first wiring of this feature shipped with zero coverage and was
 * therefore never exercised end to end — the loader was never called and the grid never reached
 * the player. Everything asserted here is a claim the controller's ground and the characters'
 * feet depend on: a known cell answers exactly, cells interpolate, the footprint clamps instead
 * of returning nothing, a hole is refused rather than guessed at, and a missing grid is `null`
 * so the caller can keep the constant engine floor.
 */
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import { clearHeightGrid, hasHeightGrid, heightAt, heightFootprint, heightGridInfo,
  MODEL_TO_WORLD, parseHeightGrid, setHeightGrid } from '../../frontend/storyMapHeight.mjs';
import { loadStoryMapHeight, mapHeightUrl } from '../../frontend/storyMapHeightLoader.mjs';
import { MAP_PLACEMENT, mapOriginTransform } from '../../frontend/storyMap.mjs';

/**
 * A grid whose cells carry the given *world* heights in metres.
 *
 * `minY` is chosen so cell 0 is exactly `world[0]`: the cells are stored as centimetres relative
 * to `minY`, so this is what lets the assertions below be closed-form numbers instead of a second
 * copy of the module's arithmetic.
 */
function gridOf(world, { res = 2, bounds = [-2, 2, -2, 2] } = {}) {
  const minY = MAP_PLACEMENT.centreY
    + (world[0] - mapOriginTransform().position[1]) / MODEL_TO_WORLD;
  const heights = world.map(value => Math.round((MAP_PLACEMENT.centreY
    + (value - mapOriginTransform().position[1]) / MODEL_TO_WORLD - minY) * 100));
  return parseHeightGrid({ version: 1, res: [res, res], bounds, minY, unit: 'cm', heights });
}

/**
 * Cells are stored as whole centimetres, so an exact world height is only known to ±0.5 cm.
 * Anything larger than that is a wrong frame or a wrong factor, which is what these assert.
 */
const CM = 0.005;

/** World x/z of the centre of cell (i, j), which is where a cell's sample sits. */
const cellCentre = (i, j, { res = 2, bounds = [-2, 2, -2, 2] } = {}) => [
  (bounds[0] + (i + 0.5) * (bounds[1] - bounds[0]) / res) * MODEL_TO_WORLD,
  (bounds[2] + (j + 0.5) * (bounds[3] - bounds[2]) / res) * MODEL_TO_WORLD];

test.afterEach(() => clearHeightGrid());

test('a known cell answers with exactly its own world height', () => {
  setHeightGrid(gridOf([0, 1, 2, 3]));
  for (const [i, j, expected] of [[0, 0, 0], [1, 0, 1], [0, 1, 2], [1, 1, 3]]) {
    const [x, z] = cellCentre(i, j);
    assert.ok(Math.abs(heightAt(x, z) - expected) < CM,
      `cell (${i},${j}) 必须恰好是 ${expected}，实际 ${heightAt(x, z)}`);
  }
});

test('between two cells the surface is a bilinear ramp, not a step', () => {
  setHeightGrid(gridOf([0, 2, 0, 2]));
  const [leftX, z] = cellCentre(0, 0);
  const [rightX] = cellCentre(1, 0);
  const middle = heightAt((leftX + rightX) / 2, z);
  assert.ok(Math.abs(middle - 1) < CM, `两格中间必须是 1 m 的斜坡，实际 ${middle}`);
  // Halfway across a step of 2 m is 1 m: a nearest-cell lookup would answer 0 or 2 here.
  assert.ok(heightAt(leftX + (rightX - leftX) * 0.25, z) < middle);
  assert.ok(heightAt(leftX + (rightX - leftX) * 0.75, z) > middle);
});

test('outside the footprint the nearest edge sample answers instead of nothing', () => {
  setHeightGrid(gridOf([0, 1, 2, 3]));
  const [edgeX, edgeZ] = cellCentre(1, 1);
  const far = heightAt(1e6, 1e6);
  assert.ok(Number.isFinite(far), '走出地图外必须是有限高度，否则角色会掉下去');
  assert.ok(Math.abs(far - heightAt(edgeX, edgeZ)) < 1e-9, '地图外沿用最边上的采样值');
  const [nearX, nearZ] = cellCentre(0, 0);
  assert.ok(Math.abs(heightAt(-1e6, -1e6) - heightAt(nearX, nearZ)) < 1e-9);
  // A non-finite query is not terrain: answer null instead of a NaN ground line.
  assert.equal(heightAt(NaN, 0), null);
  assert.equal(heightAt(0, Infinity), null);
});

test('a grid hole is refused instead of being interpolated as terrain', () => {
  const base = { version: 1, res: [2, 2], bounds: [-2, 2, -2, 2], minY: 0, unit: 'cm' };
  assert.throws(() => parseHeightGrid({ ...base, heights: [0, 100, null, 300] }), /数据无效/);
  assert.throws(() => parseHeightGrid({ ...base, heights: [0, 100, 200] }), /长度无效/);
  assert.throws(() => parseHeightGrid({ ...base, heights: [0, 100, 200, -1] }), /数据无效/);
  assert.throws(() => parseHeightGrid({ ...base, heights: [0, 100, 200, 32768] }), /数据无效/);
  assert.throws(() => parseHeightGrid({ ...base }), /长度无效|数据无效/);
  // Version/unit/resolution/bounds are part of the contract too.
  assert.throws(() => parseHeightGrid({ ...base, version: 2, heights: [0, 1, 2, 3] }), /版本/);
  assert.throws(() => parseHeightGrid({ ...base, unit: 'm', heights: [0, 1, 2, 3] }), /单位/);
  assert.throws(() => parseHeightGrid({ ...base, res: [1, 2], heights: [0, 1] }), /分辨率/);
  assert.throws(() => parseHeightGrid({ ...base, bounds: [2, -2, -2, 2], heights: [0, 1, 2, 3] }), /范围/);
  assert.throws(() => parseHeightGrid({ ...base, minY: NaN, heights: [0, 1, 2, 3] }), /基准高度/);
});

test('no grid means null: the caller keeps the constant engine floor', () => {
  clearHeightGrid();
  assert.equal(hasHeightGrid(), false);
  assert.equal(heightAt(0, 0), null);
  assert.equal(heightFootprint(), null);
  assert.equal(heightGridInfo(), null);
  const installed = setHeightGrid(gridOf([4, 4, 4, 4]));
  assert.equal(hasHeightGrid(), true);
  assert.equal(installed.res[0], 2);
  assert.ok(Math.abs(heightAt(0, 0) - 4) < CM, '平坦的网格给出平坦的地面');
  // The footprint is the model bounds mapped through the same factor the map is placed with.
  const [minX, maxX, minZ, maxZ] = heightFootprint();
  assert.deepEqual([minX, maxX, minZ, maxZ].map(v => Math.round(v * 1e6) / 1e6),
    [-2, 2, -2, 2].map(v => Math.round(v * MODEL_TO_WORLD * 1e6) / 1e6));
  clearHeightGrid();
  assert.equal(heightAt(0, 0), null);
});

test('a cell whose footprint is full of building still answers with the ground line', () => {
  // This is the "walking near a building launches the character" bug, reproduced as a grid.
  // The map's walls, eaves and roofs put vertices hundreds of metres above the courtyard, and
  // the first bake kept the *maximum* y per cell — so the ground a cell reported was the roof
  // over it and the player rode it upwards as soon as they came near a building. The shipped
  // grid is baked from `editor/game/tools/bake_story_map_height.py`, which now keeps only vertices inside a
  // couple of metres of the courtyard: a cell that contains a building reports the ground the
  // building stands on. A grid built the old way is the control below.
  // One cell of the profile is a building: the ground the characters walk on at 1 m, the roof
  // over the same footprint at 6 m. The other three cells are the flat ground the story places
  // its characters on, so a flat grid of `world = 1` stores code 0 in every cell and the 5 m
  // between floor and roof is the only code the test has to compute (in model-frame centimetres).
  const roof = Math.round((6 - 1) / MODEL_TO_WORLD * 100);
  const ground = gridOf([1, 1, 1, 1]);
  ground.heights[1] = 0;                               // cell (1,0): the ground under the roof
  setHeightGrid(ground);
  const [x, z] = cellCentre(1, 0);
  assert.ok(Math.abs(heightAt(x, z) - 1) < CM,
    `墙脚所在格必须回答地面 1 米，实际 ${heightAt(x, z)}`);
  // Non-vacuity: `heightAt` itself is unchanged and still faithful. The *same* call on a grid
  // that stores the profile's maximum — what the old bake wrote — returns the roof, so the
  // assertion above is a statement about which surface the bake keeps, not about the lookup
  // being unable to climb.
  const buggy = gridOf([1, 1, 1, 1]);
  buggy.heights[1] = roof;                             // cell (1,0): the roof over that ground
  setHeightGrid(buggy);
  assert.ok(Math.abs(heightAt(x, z) - 6) < CM,
    `旧的最大值规则必须会把角色抬上 6 米高的屋顶，实际 ${heightAt(x, z)}`);

  // The shipped grid is walkable ground too: it must carry that profile's ground, not its roof.
  setHeightGrid(JSON.parse(readFileSync(new URL('../../data/story-map-height.json', import.meta.url),
    'utf8')));
  const info = heightGridInfo();
  assert.ok(info.minYModel + (info.maxCell - info.minCell) / 100 < 3.5,
    `真实网格的起伏必须还是可行走地面，实际最高 ${info.maxYModel} 米`);
});

/** A minimal XMLHttpRequest stand-in: records the request and answers with `document`. */
function fakeXhr(document, log) {
  return () => ({
    open(method, url) { log.push([method, url]); },
    set responseType(value) { log.push(['responseType', value]); },
    responseText: JSON.stringify(document),
    send() { this.response = document; this.onload(); },
  });
}

const DOCUMENT = { version: 1, res: [2, 2], bounds: [-1, 1, -1, 1], minY: 0, unit: 'cm',
  heights: [0, 100, 200, 300] };

test('a file: page reads the grid over XHR, because fetch cannot open file: URLs', async () => {
  // The story page is a file: document (`cef_app.cpp` sets allow-file-access-from-files, which is
  // what makes XHR work there). A fetch-only loader looks wired and reads nothing on the real
  // page — the silent half of "the characters still float".
  const url = mapHeightUrl('file:///D:/engine/editor/Frontend/dist/index.html');
  assert.equal(url, 'file:///D:/engine/editor/game/data/story-map-height.json');
  assert.equal(mapHeightUrl('file:///D:/engine/CabbageEditor/Frontend/dist/index.html'),
    'file:///D:/engine/CabbageEditor/game/data/story-map-height.json');
  assert.equal(mapHeightUrl('file://server/share/CabbageEditor/Frontend/dist/index.html'),
    'file://server/share/CabbageEditor/game/data/story-map-height.json');
  const log = [];
  const grid = await loadStoryMapHeight(url, {
    createXhr: fakeXhr(DOCUMENT, log),
    fetchImpl: () => { throw new Error('file: 页面不许走 fetch'); },
  });
  assert.deepEqual(log[0], ['GET', url]);
  assert.equal(log[1][1], 'json');
  assert.equal(grid.res[0], 2);
  assert.equal(hasHeightGrid(), true);
  assert.ok(Number.isFinite(heightAt(0, 0)), '读到的网格必须就是地面');
});

test('an http page reads the grid over fetch, and an unreadable grid installs nothing', async () => {
  const seen = [];
  await loadStoryMapHeight('http://localhost/game/data/story-map-height.json', {
    fetchImpl: async url => { seen.push(url); return { ok: true, json: async () => DOCUMENT }; },
  });
  assert.deepEqual(seen, ['http://localhost/game/data/story-map-height.json']);
  assert.equal(hasHeightGrid(), true);

  clearHeightGrid();
  await assert.rejects(loadStoryMapHeight('file:///D:/engine/game/data/story-map-height.json',
    { createXhr: () => { throw new Error('文件不存在'); } }), /文件不存在/);
  assert.equal(hasHeightGrid(), false, '读不到就保持「没有网格」，不能装半张');
  await assert.rejects(loadStoryMapHeight('http://localhost/gone.json',
    { fetchImpl: async () => ({ ok: false, status: 404 }) }), /404/);
  assert.equal(hasHeightGrid(), false);
});
