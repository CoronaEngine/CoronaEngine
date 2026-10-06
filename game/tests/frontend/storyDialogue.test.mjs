import assert from 'node:assert/strict';
import test from 'node:test';
import { PROPHET_CONTRACT, PROPHET_CONTRACT_VERSION, PROPHET_PLACEMENT, PROPHET_SCRIPT,
  describeProphetContract, validateProphetPlacement } from '../../frontend/storyDialogue.mjs';
import { PROPHET_DIALOGUE, PROPHET_INTERACTION, canTalkToProphet,
  normalizeDialogue } from '../../frontend/prophetDialogue.mjs';
import { STORY_CHARACTERS } from '../../frontend/storyCharacters.mjs';

const prophetCharacter = () => STORY_CHARACTERS.find(character => character.role === 'prophet');
// [minX, minY, minZ, maxX, maxY, maxZ] as the engine reports it.
const boxAt = (x, z, half = 1) => [x - half, 0, z - half, x + half, half * 2, z + half];
const playerAt = (x, z) => ({ position: [x, 0, z] });

test('the frozen contract describes a small-world-only F interaction', () => {
  assert.equal(PROPHET_CONTRACT_VERSION, 1);
  assert.deepEqual([...PROPHET_CONTRACT.worlds], ['child']);
  assert.equal(PROPHET_CONTRACT.interaction.key, 'F');
  assert.equal(PROPHET_CONTRACT.interaction.prompt, 'F 与先知交谈');
  assert.deepEqual([...PROPHET_CONTRACT.interaction.advanceKeys], ['Enter', 'Space']);
  assert.equal(PROPHET_CONTRACT.interaction.closeKey, 'Escape');
  // The live interaction range must be the contract's range, not a second copy of it.
  assert.equal(PROPHET_INTERACTION.range, PROPHET_CONTRACT.interaction.range);
  assert.equal(PROPHET_INTERACTION.range, 12);
});

test('the panel script is the data script, beat for beat', () => {
  const texts = PROPHET_SCRIPT.fragments.map(fragment => fragment.text);
  assert.deepEqual([...PROPHET_SCRIPT.lines], texts);
  assert.deepEqual([...PROPHET_DIALOGUE.lines], texts);
  assert.equal(PROPHET_SCRIPT.lines.length, 5);
  assert.equal(PROPHET_DIALOGUE.title, PROPHET_SCRIPT.title);
  assert.equal(PROPHET_DIALOGUE.closing, PROPHET_SCRIPT.closing);
  const ids = PROPHET_SCRIPT.fragments.map(fragment => fragment.id);
  assert.equal(new Set(ids).size, ids.length);
  assert.ok(ids.every(id => typeof id === 'string' && id.length > 0));
});

test('normalizeDialogue keeps the projection and still rescues edited content', () => {
  // This is the exact call the small-world page makes, so the panel text is asserted here.
  assert.deepEqual(normalizeDialogue(null).lines, [...PROPHET_SCRIPT.lines]);
  assert.deepEqual(normalizeDialogue({ lines: [] }).lines, [...PROPHET_SCRIPT.lines]);
  assert.deepEqual(normalizeDialogue({ lines: ['  ', 7, '只有一句'] }).lines, ['只有一句']);
  assert.equal(normalizeDialogue({ title: '  ' }).title, '');
});

test('placement parameters feed the character table instead of being duplicated in it', () => {
  assert.equal(prophetCharacter().asset, PROPHET_PLACEMENT.asset);
  assert.equal(prophetCharacter().height, PROPHET_PLACEMENT.height);
  assert.equal(PROPHET_PLACEMENT.aheadDistance, 4);
  assert.equal(PROPHET_PLACEMENT.yawOffset, Math.PI);
  assert.equal(PROPHET_PLACEMENT.rotationPolicy, 'face-player');
});

test('placement validation rejects what the placer cannot use, and fills defaults', () => {
  const notPositive = [0, -1, NaN, Infinity, -Infinity, '4', true, false, null, undefined, 1e7];
  const notAFinitAngle = [NaN, Infinity, -Infinity, '4', true, false, null, undefined, 1e7];
  for (const bad of notPositive) {
    for (const key of ['aheadDistance', 'height']) {
      assert.throws(() => validateProphetPlacement({ aheadDistance: 4, height: 1.8, yawOffset: 0, [key]: bad }),
        /摆位参数无效/, `${key}=${String(bad)}`);
    }
  }
  for (const bad of notAFinitAngle) {
    assert.throws(() => validateProphetPlacement({ aheadDistance: 4, height: 1.8, yawOffset: bad }),
      /摆位参数无效/, `yawOffset=${String(bad)}`);
  }
  // A zero or negative facing angle is legitimate; only a non-finite or oversized one is not.
  assert.equal(validateProphetPlacement({ aheadDistance: 4, height: 1.8, yawOffset: 0 }).yawOffset, 0);
  assert.equal(validateProphetPlacement({ aheadDistance: 4, height: 1.8, yawOffset: -Math.PI }).yawOffset, -Math.PI);
  const filled = validateProphetPlacement({ aheadDistance: 5, height: 2, yawOffset: 0 });
  assert.equal(filled.asset, PROPHET_PLACEMENT.asset);
  assert.equal(filled.model, PROPHET_PLACEMENT.model);
  assert.equal(filled.rotationPolicy, PROPHET_PLACEMENT.rotationPolicy);
});

test('only the small world answers F, and only inside the contract range', () => {
  // Main world must never open the prophet panel; this is the regression that matters most.
  assert.equal(canTalkToProphet({ role: 'main', player: playerAt(0, 0), bounds: boxAt(3, 0) }), false);
  assert.equal(canTalkToProphet({ role: null, player: playerAt(0, 0), bounds: boxAt(3, 0) }), false);
  assert.equal(canTalkToProphet({ role: 'child', player: playerAt(0, 0), bounds: boxAt(3, 0) }), true);
  // Distance is measured to the body box, so standing inside it always counts.
  assert.equal(canTalkToProphet({ role: 'child', player: playerAt(0, 0), bounds: boxAt(0, 0) }), true);
  // The range boundary is inclusive and the far side of the world is refused.
  assert.equal(canTalkToProphet({ role: 'child', player: playerAt(0, 0), bounds: boxAt(13, 0) }), true);
  assert.equal(canTalkToProphet({ role: 'child', player: playerAt(0, 0), bounds: boxAt(13.5, 0) }), false);
  // An explicit range still overrides the contract, and a broken one refuses everything.
  assert.equal(canTalkToProphet({ role: 'child', player: playerAt(0, 0), bounds: boxAt(3, 0), range: 1 }), false);
  assert.equal(canTalkToProphet({ role: 'child', player: playerAt(0, 0), bounds: boxAt(0, 0), range: 0 }), false);
  assert.equal(canTalkToProphet({ role: 'child', player: playerAt(0, 0), bounds: null }), false);
});

test('describeProphetContract summarises the exported constants', () => {
  const summary = describeProphetContract();
  assert.equal(summary.version, PROPHET_CONTRACT_VERSION);
  assert.deepEqual(summary.worlds, [...PROPHET_CONTRACT.worlds]);
  assert.equal(summary.key, PROPHET_CONTRACT.interaction.key);
  assert.equal(summary.range, PROPHET_CONTRACT.interaction.range);
  assert.equal(summary.prompt, PROPHET_CONTRACT.interaction.prompt);
  assert.deepEqual(summary.advanceKeys, [...PROPHET_CONTRACT.interaction.advanceKeys]);
  assert.equal(summary.closeKey, PROPHET_CONTRACT.interaction.closeKey);
  assert.deepEqual(summary.fragmentIds, PROPHET_SCRIPT.fragments.map(fragment => fragment.id));
  assert.equal(summary.lineCount, PROPHET_SCRIPT.lines.length);
  assert.deepEqual(summary.placement, { ...PROPHET_PLACEMENT });
});
