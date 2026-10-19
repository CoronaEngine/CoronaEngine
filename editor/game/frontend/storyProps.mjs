/**
 * Props the main world yields, and the geometry rules for exhibiting them inside a
 * small world. Pure data and math: no native bridge, no filesystem, no engine calls,
 * so the placement rules stay testable and the world copy stays independent.
 */

/** Stable namespace so a placement keeps the same actor identity across reloads. */
export const PLACEMENT_PREFIX = 'story.placement.';

/**
 * Collection sites inside a small world. The authoring flow offers these instead of
 * absolute coordinates so a small world can be laid out without a scene editor.
 */
export const PLACEMENT_SITES = Object.freeze([
  Object.freeze({ id: 'west', name: '西侧空地', position: [-6, 0, -2] }),
  Object.freeze({ id: 'east', name: '东侧空地', position: [6, 0, -2] }),
  Object.freeze({ id: 'north', name: '北侧高台', position: [0, 0, -8] }),
  Object.freeze({ id: 'south', name: '南侧入口', position: [0, 0, 6] }),
]);

/** Only these reach a small world, keyed by the inventory counter that grants them. */
export const STORY_PROPS = Object.freeze([
  Object.freeze({
    id: 'world-fragment', name: '世界碎片', itemKey: 'worldFragment',
    asset: 'fragment/Ball.obj', description: '主世界击败巨龙后得到的世界残片。',
    height: 0.5, yaw: 0,
    // A keepsake the world hands over: it is counted, never put down.
    placeable: false,
  }),
  Object.freeze({
    // The art a world yields goes straight into the small world: the block stack the bag
    // starts with. It reuses the prophet's cube art, so no new model has to be authored.
    id: 'block', name: '方块', itemKey: 'block',
    asset: 'cube/cube.obj', description: '主世界带回的方块，可直接在小世界中摆放。',
    height: 0.5, yaw: 0,
    placeable: true,
  }),
  Object.freeze({
    // Monster loot: the same ball art the world fragment used before the reward moved
    // into the bag, now a placeable model in its own right.
    id: 'ball', name: '球', itemKey: 'ball',
    asset: 'fragment/Ball.obj', description: '击败怪物得到的球，可在小世界中摆放。',
    height: 0.5, yaw: 0,
    placeable: true,
  }),
]);

/** The props the player may actually put down. A keepsake is never one of them. */
export function placeableProps() {
  return STORY_PROPS.filter(prop => prop.placeable === true);
}

/** Whether a prop may be placed, by id, without throwing on an unknown id. */
export function canPlace(propId) {
  return findProp(propId)?.placeable === true;
}

const LIMIT = 1e6;
const vectors = value => Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);
const inRange = value => vectors(value) && value.every(n => Math.abs(n) <= LIMIT);
const positive = value => Number.isFinite(value) && value > 0 && value <= LIMIT;

/**
 * How many props one small world may hold. Mirrors `MAX_PLACEMENTS` in
 * `game/runtime/placements.py`, which rejects `index >= MAX_PLACEMENTS`: the page needs the
 * same ceiling to refuse a placement *before* the save fails, and the two are asserted to
 * agree by the frontend test.
 */
export const MAX_PLACEMENTS = 64;

/** How far in front of the player a newly placed block lands, in metres. */
export const PLACEMENT_DISTANCE = 2.0;

/**
 * Where the block the player is holding lands: `distance` metres straight ahead on the
 * ground, turned to face the same way the player does.
 *
 * **The y is the ground line, not the object's origin world y.** The engine normalises a
 * static model into a unit cube (the AABB centre becomes the origin), so a placed actor's
 * origin sits `-bounds[1] * scale` above the surface — `storyActors.syncPlacementActors`
 * performs exactly that sink when it consumes this row (`position[1] - bounds[1] * scale`),
 * and `storyCharacters.characterTransform` lands the characters on `heightAt(x, z)` in the
 * same shape. Storing the ground line therefore keeps this row in the one convention every
 * other grounded object on the page already uses, and the existing sink is what makes the
 * block visually touch the surface.
 *
 * The forward vector is `(sin, cos)` of the yaw — the same convention
 * `storyCube.screenAxes`/`cubeSpawnTransform` uses — duplicated here on purpose rather than
 * imported, so this module keeps its promise of importing nothing and a geometry change in
 * the cube can never silently move where a player's own block goes.
 *
 * `y` comes from the same baked grid the characters use: `groundYAt(x, z)` is
 * `storyMapHeight.heightAt`, which answers `null` when no grid is loaded or the sample is
 * junk. Then the old constant is kept — the player's own `position[1]`, the value this
 * landed on before the grid existed — so a world without the data file behaves exactly as it
 * did. Taking the player's y with a grid loaded is what made a block placed on rising
 * terrain hover by the height difference; taking the ground at the target cell is what makes
 * it rest on the surface.
 *
 * `siteId` stays null because the spot is derived from the player, not chosen from a list.
 */
export function blockSpawnTransform(player, yaw, distance = PLACEMENT_DISTANCE, groundYAt = null) {
  const position = player?.position;
  if (!vectors(position) || !inRange(position)) throw new Error('玩家位置未就绪');
  if (!Number.isFinite(yaw)) throw new Error('玩家朝向未就绪');
  if (!positive(distance)) throw new Error('摆放距离无效');
  const x = position[0] + Math.sin(yaw) * distance;
  const z = position[2] + Math.cos(yaw) * distance;
  // `null` (no grid, junk sample, no provider) keeps the pre-grid constant: the player's y.
  const ground = typeof groundYAt === 'function' ? groundYAt(x, z) : null;
  return {
    siteId: null,
    position: [x, Number.isFinite(ground) ? ground : position[1], z],
    rotation: [0, yaw, 0],
    scale: 1,
  };
}

/**
 * How close the player must stand to take a placed block back.
 *
 * Placing puts the block `PLACEMENT_DISTANCE` (2 m) in front of the player, so a reach a
 * little beyond that makes place → retrieve symmetrical: stand where you put it down and F
 * takes it back, without having to walk into it. 2.5 m keeps the round trip one step while
 * staying far shorter than the prophet's 12 m range, so a block can never out-prioritise
 * the prophet the player is actually walking towards.
 */
export const PLACED_BLOCK_REACH = 2.5;

/**
 * Retired prop ids. `pillar` shared the block's cube model, so every old placement and
 * inventory count is treated as a ball instead. Keeping the alias here means old saves
 * open without losing their objects, and the next save writes the canonical id.
 */
export const PROP_ALIASES = Object.freeze({ pillar: 'ball' });

export const canonicalPropId = id => PROP_ALIASES[id] || id;

export function findProp(id) {
  return STORY_PROPS.find(prop => prop.id === canonicalPropId(id)) || null;
}

export function placementGuid(index) {
  if (!Number.isInteger(index) || index < 0) throw new Error('陈列序号必须是非负整数');
  return `${PLACEMENT_PREFIX}${String(index).padStart(4, '0')}`;
}

export function placementIndex(guid) {
  const raw = typeof guid === 'string' && guid.startsWith(PLACEMENT_PREFIX)
    ? guid.slice(PLACEMENT_PREFIX.length) : '';
  if (!/^\d{4}$/.test(raw)) return null;
  return Number(raw);
}

/**
 * A placement is authored by the game, so every field is required and bounded. The
 * caller supplies the absolute position; sites are resolved before this point.
 */
export function validPlacement(value) {
  return Boolean(value) && typeof value === 'object' && !Array.isArray(value)
    && typeof value.propId === 'string' && Boolean(findProp(value.propId))
    && Number.isInteger(value.index) && value.index >= 0
    && inRange(value.position) && inRange(value.rotation) && positive(value.scale)
    && (value.siteId === null || PLACEMENT_SITES.some(site => site.id === value.siteId));
}

/** Rebuild the wire shape from untrusted data so a bad save can never reach the engine. */
export function sanitizePlacements(raw) {
  if (!Array.isArray(raw)) return [];
  const seen = new Set();
  const result = [];
  for (const entry of raw) {
    if (!validPlacement(entry) || seen.has(entry.index)) continue;
    seen.add(entry.index);
    result.push({ propId: canonicalPropId(entry.propId), index: entry.index, siteId: entry.siteId,
      position: [...entry.position], rotation: [...entry.rotation], scale: entry.scale });
  }
  return result.sort((a, b) => a.index - b.index);
}

/** Lowest free index, so removing one exhibit does not renumber the others. */
export function nextPlacementIndex(placements) {
  const used = new Set((Array.isArray(placements) ? placements : [])
    .map(entry => entry?.index).filter(Number.isInteger));
  let index = 0;
  while (used.has(index)) index++;
  return index;
}

/**
 * The placement the player may take back: the nearest one within `reach`.
 *
 * Distance is horizontal (x/z), the same measure the prophet interaction uses, so every
 * proximity rule on the page reads the same way. Only props that may be placed
 * (`canPlace`) are ever candidates — a keepsake that somehow sits in the layout, or an entry
 * written by a future build, is never something F can hand back.
 *
 * The reach is the whole gate: retrieving is symmetric with placing, and
 * placing itself is allowed mid-air or mid-dodge, so a movement state must not silently
 * remove the prompt. Pure: junk player data, junk lists and a junk reach all answer `null`
 * instead of throwing.
 */
export function nearestPlacedBlock(player, placements, reach = PLACED_BLOCK_REACH) {
  const position = player?.position;
  if (!vectors(position) || !inRange(position)) return null;
  if (!Number.isFinite(reach) || reach < 0) return null;
  let best = null;
  for (const entry of sanitizePlacements(placements)) {
    if (!canPlace(entry.propId)) continue;
    const distance = Math.hypot(position[0] - entry.position[0], position[2] - entry.position[2]);
    if (distance > reach) continue;
    if (!best || distance < best.distance) {
      best = { index: entry.index, propId: entry.propId, distance };
    }
  }
  return best;
}

/**
 * The layout without one entry. Every other entry keeps its own index: the sequence is the
 * actor identity (`placementGuid`) persisted in the scene, so renumbering would leave the
 * remaining exhibits addressing the wrong actors. Junk input answers an empty layout, and an
 * index that matches nothing leaves the layout untouched.
 */
export function removePlacement(placements, index) {
  return sanitizePlacements(placements).filter(entry => entry.index !== index);
}

/** The site a placement was authored on, or null when it was moved by hand. */
export function siteFor(placement) {
  return PLACEMENT_SITES.find(site => site.id === placement?.siteId) || null;
}

/** Exhibits are laid out in index order so a reload reproduces the same arrangement. */
export function placementScene(placements, propId) {
  return sanitizePlacements(placements)
    .filter(entry => !propId || entry.propId === canonicalPropId(propId))
    .map(entry => ({ ...entry, guid: placementGuid(entry.index), prop: findProp(entry.propId) }))
    .filter(entry => Boolean(entry.prop));
}

/**
 * What the player may put into a small world, with `owned` from the gameplay inventory
 * counter and `placed` from what is already down, so the two numbers drive the item bar.
 *
 * Only placeable props are listed: the world fragment is a keepsake and must never appear
 * as something to put down, so it cannot be selected or placed by construction.
 */
export function exhibitItems(inventory = {}, placements = []) {
  const entries = sanitizePlacements(placements);
  return placeableProps().map(prop => {
    const raw = inventory?.[prop.itemKey];
    const owned = Number.isInteger(raw) && raw > 0 ? raw : 0;
    const placed = entries.filter(entry => entry.propId === prop.id).length;
    return { id: prop.id, name: prop.name, description: prop.description,
      owned, placed, available: Math.max(0, owned - placed) };
  });
}

/**
 * How many cells the item bar always shows, Minecraft-style: a fixed row rather than one
 * cell per item, so the wheel and the number keys address the same slots no matter what the
 * player is carrying. Empty cells are still selectable — they simply have nothing to place.
 *
 * If the placeable prop list ever grows past this, raise `HOTBAR_SLOTS`: it is the single
 * constant that sizes the bar, and `hotbarSlots` pads or truncates nothing beyond it.
 */
export const HOTBAR_SLOTS = 8;

/**
 * The item bar: exactly `size` cells in slot order. What the player actually holds fills the
 * leading cells (keeping `id`/`name`/`owned`/`placed`/`available`), the rest are empty cells
 * the player can still select but cannot place from. `exhibitItems` lists every placeable prop
 * whether or not it is in the bag, so a prop with `owned: 0` is not an item yet and fills no
 * cell — an empty bag is a row of empty cells, not one unusable one. The row is always the
 * full length, so a bad `size` falls back to `HOTBAR_SLOTS` instead of throwing.
 */
export function hotbarSlots(inventory = {}, placements = [], size = HOTBAR_SLOTS) {
  const total = Number.isInteger(size) && size > 0 ? size : HOTBAR_SLOTS;
  const items = exhibitItems(inventory, placements).filter(item => item.owned > 0);
  return Array.from({ length: total }, (unused, slot) => {
    const item = items[slot];
    return item
      ? { slot, ...item, empty: false }
      : { slot, id: null, name: '', owned: 0, placed: 0, available: 0, empty: true };
  });
}

/**
 * The selected slot after one wheel notch, wrapping in both directions.
 *
 * `delta` is a raw wheel `deltaY` (or any signed number) and only its direction is used: each
 * notch moves the selection exactly one slot, and the wrap is modulo, so scrolling back from
 * the first slot lands on the last one and forward from the last returns to the first — the
 * behaviour a Minecraft-style bar is expected to have. A list with nothing to choose between
 * has no selection to move, so it answers 0 instead of inventing one.
 */
export function cycleSlot(current, length, delta) {
  const size = Number.isInteger(length) && length > 0 ? length : 0;
  if (size <= 1) return 0;
  const raw = Number.isFinite(delta) ? Math.sign(delta) : 0;
  const step = raw > 0 ? 1 : raw < 0 ? -1 : 0;
  const from = Number.isInteger(current) && current >= 0 ? current % size : 0;
  return ((from + step) % size + size) % size;
}
