import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const assetRoot = fileURLToPath(new URL('../../../assets/story_mode/', import.meta.url));

function materialNames(mtlText) {
  return new Set(
    mtlText
      .split(/\r?\n/)
      .filter((line) => line.startsWith('newmtl '))
      .map((line) => line.slice('newmtl '.length).trim())
  );
}

test('Story World OBJ and MTL resources are complete and numerically valid', () => {
  const mtlPath = path.join(assetRoot, 'story_world.mtl');
  assert.ok(fs.existsSync(mtlPath));
  const materials = materialNames(fs.readFileSync(mtlPath, 'utf8'));
  assert.ok(materials.size >= 10);

  const objFiles = fs.readdirSync(assetRoot).filter((name) => name.endsWith('.obj'));
  assert.deepEqual(
    new Set(objFiles),
    new Set([
      'bridge.obj',
      'fence.obj',
      'gate.obj',
      'house_large.obj',
      'house_small.obj',
      'lantern.obj',
      'pavilion.obj',
      'road_segment.obj',
      'rock.obj',
      'terrain.obj',
      'tree.obj',
      'water.obj',
    ])
  );

  for (const filename of objFiles) {
    const text = fs.readFileSync(path.join(assetRoot, filename), 'utf8');
    assert.ok(!/\b(?:NaN|Infinity)\b/.test(text), `${filename} contains an invalid number`);
    assert.match(text, /^mtllib story_world\.mtl$/m);
    const lines = text.split(/\r?\n/);
    const vertices = lines.filter((line) => line.startsWith('v '));
    const faces = lines.filter((line) => line.startsWith('f '));
    assert.ok(vertices.length >= 3, `${filename} has too few vertices`);
    assert.ok(faces.length >= 1, `${filename} has no faces`);

    for (const line of vertices) {
      const values = line.trim().split(/\s+/).slice(1).map(Number);
      assert.equal(values.length, 3);
      assert.ok(values.every(Number.isFinite), `${filename} has an invalid vertex`);
    }
    for (const line of lines.filter((entry) => entry.startsWith('usemtl '))) {
      assert.ok(
        materials.has(line.slice('usemtl '.length).trim()),
        `${filename} references a missing material`
      );
    }
    for (const line of faces) {
      const indices = line
        .trim()
        .split(/\s+/)
        .slice(1)
        .map((token) => Number(token.split('/')[0]));
      assert.ok(indices.length >= 3, `${filename} has an empty face`);
      assert.ok(
        indices.every((index) => Number.isInteger(index) && index > 0 && index <= vertices.length)
      );
    }
  }
});
