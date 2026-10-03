/**
 * What the prophet says, and how the player reaches them.
 *
 * Every line lives in `PROPHET_DIALOGUE` so the script can be rewritten without
 * touching the page. Interaction rules stay here too, because they must agree with
 * how `storyActors.mjs` places the prophet beside the player.
 */

export const PROPHET_ROLE = 'prophet';
export const PROPHET_NAME = '先知';

/**
 * Placeholder script. Replace the `lines` below with the final text; the panel reads
 * this structure directly, so no code changes are needed for a rewrite.
 */
export const PROPHET_DIALOGUE = Object.freeze({
  id: 'prophet',
  name: PROPHET_NAME,
  title: '小世界的守望者',
  hint: 'F 交谈 · Enter 继续 · Esc 离开',
  lines: Object.freeze([
    '你从小世界之外回来了。',
    '外面的巨龙倒下了，你带回了它的碎片。',
    '把碎片留在这里吧——它会记住你走过的路。',
  ]),
  closing: '去想放什么，就去放吧。',
});

/**
 * The F key means "talk" only when the prophet is genuinely within reach.
 *
 * The prophet is placed four metres ahead of the player before the model is scaled to
 * human height, so the distance from the player to its *body* varies with the imported
 * model's proportions. The range is therefore measured to the body and kept generous
 * enough to cover that placement, while still refusing a prophet across the world.
 */
export const PROPHET_INTERACTION = Object.freeze({ range: 12 });

/** The prophet is a small-world only inhabitant; the main world never has one. */
export function prophetAvailable(role) {
  return role === 'child';
}

export function prophetDistance(from, bounds) {
  const position = from?.position;
  if (!Array.isArray(position) || !Array.isArray(bounds) || bounds.length !== 6) return Infinity;
  return Math.hypot(...[0, 2].map(axis =>
    Math.max(bounds[axis] - position[axis], 0, position[axis] - bounds[axis + 3])));
}

export function canTalkToProphet({ role, player, bounds, range = PROPHET_INTERACTION.range }) {
  if (!prophetAvailable(role) || !Number.isFinite(range) || range <= 0) return false;
  return prophetDistance(player, bounds) <= range;
}

/** Sanitize edited content so a malformed script degrades instead of blanking the panel. */
export function normalizeDialogue(raw) {
  const source = raw && typeof raw === 'object' ? raw : {};
  const lines = (Array.isArray(source.lines) ? source.lines : [])
    .filter(line => typeof line === 'string' && line.trim())
    .map(line => line.trim());
  const text = value => typeof value === 'string' && value.trim() ? value.trim() : '';
  return {
    id: text(source.id) || PROPHET_DIALOGUE.id,
    name: text(source.name) || PROPHET_NAME,
    title: text(source.title),
    hint: text(source.hint),
    closing: text(source.closing),
    lines: lines.length ? lines : [...PROPHET_DIALOGUE.lines],
  };
}
