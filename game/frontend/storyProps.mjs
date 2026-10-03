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
    asset: 'fragment/Ball.obj', description: '主世界击败巨龙后得到的世界残片，可陈列于小世界。',
    height: 0.5, yaw: 0,
  }),
]);

const LIMIT = 1e6;
const vectors = value => Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);
const inRange = value => vectors(value) && value.every(n => Math.abs(n) <= LIMIT);
const positive = value => Number.isFinite(value) && value > 0 && value <= LIMIT;

export function findProp(id) {
  return STORY_PROPS.find(prop => prop.id === id) || null;
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
    result.push({ propId: entry.propId, index: entry.index, siteId: entry.siteId,
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

/** The site a placement was authored on, or null when it was moved by hand. */
export function siteFor(placement) {
  return PLACEMENT_SITES.find(site => site.id === placement?.siteId) || null;
}

/** Exhibits are laid out in index order so a reload reproduces the same arrangement. */
export function placementScene(placements, propId) {
  return sanitizePlacements(placements)
    .filter(entry => !propId || entry.propId === propId)
    .map(entry => ({ ...entry, guid: placementGuid(entry.index), prop: findProp(entry.propId) }))
    .filter(entry => Boolean(entry.prop));
}

/**
 * What the main world currently yields that a small world may exhibit. `owned` is the
 * gameplay inventory counter, `placed` is how many are already on display, so the two
 * numbers together drive the authoring affordance.
 */
export function exhibitItems(inventory = {}, placements = []) {
  const entries = sanitizePlacements(placements);
  return STORY_PROPS.map(prop => {
    const raw = inventory?.[prop.itemKey];
    const owned = Number.isInteger(raw) && raw > 0 ? raw : 0;
    const placed = entries.filter(entry => entry.propId === prop.id).length;
    return { id: prop.id, name: prop.name, description: prop.description,
      owned, placed, available: Math.max(0, owned - placed) };
  });
}
