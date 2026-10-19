/**
 * Runtime projection of the prophet's content: contract, placement and script.
 *
 * `game/data/prophet.json` is the authoritative content — designers edit that file.
 * This module is what the page actually imports, because a JSON import would depend on
 * import-attribute support agreeing between Vite and `node --test`, and a failed
 * frontend build is only a `CMake Warning` in this repository
 * (see tools/build/editor_copy_and_build.py), so a broken import would ship silently.
 *
 * The two are kept honest by `game/tests/test_story_assets.py`, which reads this module
 * with Node and compares every field against the JSON. Editing either side alone turns
 * that test red, so they cannot drift apart unnoticed.
 */

/** Bump only for the changes listed in `contract.bumpVersionWhen`. */
export const PROPHET_CONTRACT_VERSION = 1;

export const PROPHET_CONTRACT = Object.freeze({
  version: PROPHET_CONTRACT_VERSION,
  kind: 'prophet-interaction',
  worlds: Object.freeze(['child']),
  interaction: Object.freeze({
    key: 'F',
    range: 12,
    advanceKeys: Object.freeze(['Enter', 'Space']),
    closeKey: 'Escape',
    prompt: 'F 与先知交谈',
  }),
});

/**
 * Art placement parameters (task L-1).
 *
 * These three numbers were hardcoded in `storyActors.mjs` and `storyCharacters.mjs`.
 * They live here now so art can retune the prophet's distance, size and facing by
 * editing one data file instead of touching logic. The values are unchanged.
 */
export const PROPHET_PLACEMENT = Object.freeze({
  model: 'dancing_vampire.dae',
  asset: 'prophet/dancing_vampire.dae',
  aheadDistance: 4.0,
  height: 1.8,
  yawOffset: Math.PI,
  rotationPolicy: 'face-player',
});

const SCRIPT_FRAGMENTS = Object.freeze([
  Object.freeze({ id: 'return', text: '你从小世界之外回来了。' }),
  Object.freeze({ id: 'dragon', text: '外面的巨龙倒下了，它的碎片留在你手里。' }),
  Object.freeze({ id: 'exhibit', text: '把碎片放在这里——小世界会记住你走过的路。' }),
  Object.freeze({ id: 'build', text: 'V 键可以换一双眼睛：降临，或者俯瞰。' }),
  Object.freeze({ id: 'leave', text: '想回去的时候，按 P。我一直在这里。' }),
]);

/**
 * The script as `normalizeDialogue` expects it: `lines` is the wire format the panel
 * reads, `fragments` is the authoritative per-beat form carrying stable ids, so a
 * future "collect the fragments" feature can attach to them without a rewrite.
 */
export const PROPHET_SCRIPT = Object.freeze({
  id: 'prophet',
  name: '先知',
  title: '小世界的守望者',
  hint: 'F 交谈 · Enter 继续 · Esc 离开',
  closing: '去想放什么，就去放吧。',
  fragments: SCRIPT_FRAGMENTS,
  lines: Object.freeze(SCRIPT_FRAGMENTS.map(fragment => fragment.text)),
});

// Same magnitude limit the placement save uses (game/runtime/placements.py), so a bad
// value can never travel further than the engine can represent. `typeof null` and
// `typeof true` are not 'number', so they fail here rather than reaching the placer.
const COORD_LIMIT = 1e6;
const finiteNumber = value => typeof value === 'number' && Number.isFinite(value);
const positiveNumber = value => finiteNumber(value) && value > 0 && value <= COORD_LIMIT;

/** Strict validation for the placement block; a facing angle may legitimately be zero. */
export function validateProphetPlacement(raw) {
  const source = raw && typeof raw === 'object' ? raw : {};
  for (const key of ['aheadDistance', 'height']) {
    if (!positiveNumber(source[key])) throw new Error(`先知摆位参数无效：${key}`);
  }
  if (!finiteNumber(source.yawOffset) || Math.abs(source.yawOffset) > COORD_LIMIT) {
    throw new Error('先知摆位参数无效：yawOffset');
  }
  const text = key => (typeof source[key] === 'string' && source[key].trim() ? source[key].trim() : '');
  return Object.freeze({
    model: text('model') || PROPHET_PLACEMENT.model,
    asset: text('asset') || PROPHET_PLACEMENT.asset,
    aheadDistance: source.aheadDistance,
    height: source.height,
    yawOffset: source.yawOffset,
    rotationPolicy: text('rotationPolicy') || PROPHET_PLACEMENT.rotationPolicy,
  });
}

validateProphetPlacement(PROPHET_PLACEMENT);

/** Flat summary used by the contract tests so they assert on data, not on literals. */
export function describeProphetContract() {
  return Object.freeze({
    version: PROPHET_CONTRACT_VERSION,
    worlds: [...PROPHET_CONTRACT.worlds],
    key: PROPHET_CONTRACT.interaction.key,
    range: PROPHET_CONTRACT.interaction.range,
    prompt: PROPHET_CONTRACT.interaction.prompt,
    advanceKeys: [...PROPHET_CONTRACT.interaction.advanceKeys],
    closeKey: PROPHET_CONTRACT.interaction.closeKey,
    fragmentIds: PROPHET_SCRIPT.fragments.map(fragment => fragment.id),
    lineCount: PROPHET_SCRIPT.lines.length,
    placement: { ...PROPHET_PLACEMENT },
  });
}
