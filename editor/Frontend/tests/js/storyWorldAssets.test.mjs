import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { STORY_MONSTER_DEFINITIONS } from '../../src/config/storyCombat.js';
import {
  STORY_WORLD_ACTORS,
  STORY_WORLD_ASSET_METADATA,
  STORY_WORLD_DEPRECATED_ACTORS,
  STORY_WORLD_SCENE_VERSION,
} from '../../src/config/storyWorld.js';

const assetRoot = fileURLToPath(new URL('../../../assets/story_mode/', import.meta.url));

function materialNames(mtlText) {
  return new Set(
    mtlText
      .split(/\r?\n/)
      .filter((line) => line.startsWith('newmtl '))
      .map((line) => line.slice('newmtl '.length).trim())
  );
}

function validateObj(
  filename,
  materials,
  { requireDetailedGeometry = false, materialLibrary = 'story_world_v3.mtl' } = {}
) {
  const text = fs.readFileSync(path.join(assetRoot, filename), 'utf8');
  assert.ok(!/\b(?:NaN|Infinity|undefined)\b/.test(text), `${filename} contains invalid data`);
  const lines = text.split(/\r?\n/);
  const vertices = lines.filter((line) => line.startsWith('v '));
  const textureCoordinates = lines.filter((line) => line.startsWith('vt '));
  const normals = lines.filter((line) => line.startsWith('vn '));
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
    const tokens = line.trim().split(/\s+/).slice(1);
    assert.ok(tokens.length >= 3, `${filename} has an empty face`);
    for (const token of tokens) {
      const [vertexIndex, textureIndex, normalIndex] = token.split('/').map(Number);
      assert.ok(Number.isInteger(vertexIndex) && vertexIndex > 0 && vertexIndex <= vertices.length);
      if (requireDetailedGeometry) {
        assert.ok(
          Number.isInteger(textureIndex) &&
            textureIndex > 0 &&
            textureIndex <= textureCoordinates.length,
          `${filename} has an invalid UV index`
        );
        assert.ok(
          Number.isInteger(normalIndex) && normalIndex > 0 && normalIndex <= normals.length,
          `${filename} has an invalid normal index`
        );
      }
    }
  }

  if (requireDetailedGeometry) {
    assert.match(
      text,
      new RegExp(`^mtllib ${materialLibrary.replaceAll('.', '\\.')}$$`, 'm')
    );
    assert.ok(
      textureCoordinates.length >= vertices.length,
      `${filename} is missing UV coordinates`
    );
    assert.ok(normals.length >= vertices.length, `${filename} is missing vertex normals`);
  }
  return faces.length;
}

test('legacy Story World resources remain available for existing project references', () => {
  const mtlPath = path.join(assetRoot, 'story_world.mtl');
  assert.ok(fs.existsSync(mtlPath));
  const materials = materialNames(fs.readFileSync(mtlPath, 'utf8'));
  assert.ok(materials.size >= 10);
  for (const filename of [
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
  ]) {
    assert.ok(fs.existsSync(path.join(assetRoot, filename)), `${filename} is missing`);
    validateObj(filename, materials);
  }
});

test('v5 Story World models contain grounded detailed geometry and valid local materials', () => {
  assert.equal(STORY_WORLD_SCENE_VERSION, 5);
  const mtlPath = path.join(assetRoot, 'story_world_v5.mtl');
  assert.ok(fs.existsSync(mtlPath));
  const mtlText = fs.readFileSync(mtlPath, 'utf8');
  const materials = materialNames(mtlText);
  assert.ok(materials.size >= 20);

  const referencedTextures = [
    ...mtlText.matchAll(/^map_(?:Kd|Bump)(?:\s+-bm\s+[\d.]+)?\s+(.+)$/gm),
  ].map((match) => match[1].trim());
  assert.ok(referencedTextures.length >= 12);
  for (const texture of referencedTextures) {
    assert.ok(fs.existsSync(path.join(assetRoot, texture)), `missing texture ${texture}`);
  }

  const expectedAssets = new Set(Object.keys(STORY_WORLD_ASSET_METADATA));
  assert.ok(expectedAssets.size >= 17);
  assert.ok(expectedAssets.has('terrain_v5.obj'));
  assert.ok(expectedAssets.has('road_network_v5.obj'));
  assert.equal(
    STORY_WORLD_ACTORS.filter((actor) => actor.entityType === 'road').length,
    1
  );
  assert.ok(
    STORY_WORLD_DEPRECATED_ACTORS.every(
      (name) => !STORY_WORLD_ACTORS.some((actor) => actor.name === name)
    )
  );

  let totalSceneTriangles = 0;
  const trianglesByAsset = new Map();
  for (const filename of expectedAssets) {
    assert.ok(fs.existsSync(path.join(assetRoot, filename)), `${filename} is missing`);
    const triangleCount = validateObj(filename, materials, {
      requireDetailedGeometry: true,
      materialLibrary: 'story_world_v5.mtl',
    });
    trianglesByAsset.set(filename, triangleCount);

    if (!['terrain_v5.obj', 'water_v5.obj', 'road_network_v5.obj'].includes(filename)) {
      const yValues = fs
        .readFileSync(path.join(assetRoot, filename), 'utf8')
        .split(/\r?\n/)
        .filter((line) => line.startsWith('v '))
        .map((line) => Number(line.trim().split(/\s+/)[2]));
      assert.ok(Math.abs(Math.min(...yValues)) <= 1e-6, `${filename} is not grounded`);
    }
  }
  for (const actor of STORY_WORLD_ACTORS) {
    totalSceneTriangles += trianglesByAsset.get(actor.asset) || 0;
  }
  assert.ok(totalSceneTriangles > 20000, 'the upgraded village should contain meaningful detail');
  assert.ok(totalSceneTriangles < 200000, `scene triangle budget exceeded: ${totalSceneTriangles}`);

  const generatorText = fs.readFileSync(
    path.join(assetRoot, 'generate_story_world_assets.mjs'),
    'utf8'
  );
  const v5TerrainSection = generatorText.slice(
    generatorText.indexOf('function terrainMaterialV5'),
    generatorText.indexOf('function resamplePath')
  );
  assert.match(v5TerrainSection, /slope/);
  assert.match(v5TerrainSection, /broadNoise/);
  assert.doesNotMatch(v5TerrainSection, /\(ix \+ iz\) % 4/);
});

test('the deterministic generators and all local texture maps are included', () => {
  assert.ok(fs.existsSync(path.join(assetRoot, 'generate_story_world_assets.mjs')));
  assert.ok(fs.existsSync(path.join(assetRoot, 'generate_story_world_textures.py')));
  for (const textureName of [
    'grass',
    'dirt',
    'stone',
    'plaster',
    'wood',
    'roof_tile',
    'rock',
    'reed',
  ]) {
    for (const suffix of ['diffuse', 'normal']) {
      const file = path.join(assetRoot, 'textures', `${textureName}_${suffix}.png`);
      assert.ok(fs.existsSync(file), `${path.basename(file)} is missing`);
      assert.ok(fs.statSync(file).size > 1000, `${path.basename(file)} is unexpectedly small`);
    }
  }
});

test('story combat monster models contain complete local geometry and boss scale', () => {
  const mtlPath = path.join(assetRoot, 'story_world_v3.mtl');
  const mtlText = fs.readFileSync(mtlPath, 'utf8');
  const materials = materialNames(mtlText);
  for (const material of [
    'monster_skin',
    'monster_hide',
    'monster_bone',
    'monster_eye',
    'monster_boss',
  ]) {
    assert.ok(materials.has(material), `missing monster material ${material}`);
  }

  const vertexBounds = (filename) => {
    const text = fs.readFileSync(path.join(assetRoot, filename), 'utf8');
    const vertices = text
      .split(/\r?\n/)
      .filter((line) => line.startsWith('v '))
      .map((line) => line.trim().split(/\s+/).slice(1).map(Number));
    const minimum = [Infinity, Infinity, Infinity];
    const maximum = [-Infinity, -Infinity, -Infinity];
    for (const vertex of vertices) {
      for (let axis = 0; axis < 3; axis += 1) {
        minimum[axis] = Math.min(minimum[axis], vertex[axis]);
        maximum[axis] = Math.max(maximum[axis], vertex[axis]);
      }
    }
    return maximum.map((value, axis) => value - minimum[axis]);
  };

  const assets = new Set(STORY_MONSTER_DEFINITIONS.map((definition) => definition.asset));
  assert.deepEqual([...assets].sort(), ['monster_boss_v1.obj', 'monster_minion_v1.obj']);
  for (const filename of assets) {
    assert.ok(fs.existsSync(path.join(assetRoot, filename)), `${filename} is missing`);
    validateObj(filename, materials, { requireDetailedGeometry: true });
  }

  const minionBounds = vertexBounds('monster_minion_v1.obj');
  const bossBounds = vertexBounds('monster_boss_v1.obj');
  assert.ok(Math.max(...bossBounds) > Math.max(...minionBounds) * 1.7);
});
