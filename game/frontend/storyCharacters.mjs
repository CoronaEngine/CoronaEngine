/** Models in game/art/models and stable identities owned by the story game. */
// Maria faces local -Z. Gameplay and the camera use logical +Z instead.
export const PLAYER_MODEL_YAW_OFFSET = Math.PI;
export const PLAYER_MODEL_REF = 'story.player.facing.v1';
export const STORY_CHARACTERS = Object.freeze([
  { role: 'player', name: '玩家', guid: 'f9f5c8b0-7324-4b6c-a011-000000000001',
    asset: 'player/Maria WProp J J Ong.dae', x: 0, z: 0, height: 1.8, rotation: [0, PLAYER_MODEL_YAW_OFFSET, 0] },
  { role: 'boss', name: 'Boss', guid: 'f9f5c8b0-7324-4b6c-a011-000000000002',
    asset: 'boss/Dragon 2.5_dae.dae', x: 0, z: 12, size: 8, rotation: [Math.PI / 2, 0, 0] },
  { role: 'merchant', name: '商人', guid: 'f9f5c8b0-7324-4b6c-a011-000000000003',
    asset: 'merchant/Maw J Laygo.dae', x: -30, z: -15, height: 1.8, rotation: [0, Math.PI, 0] },
  { role: 'prophet', name: '先知', guid: 'f9f5c8b0-7324-4b6c-a011-000000000004',
    asset: 'prophet/dancing_vampire.dae', x: 30, z: -15, height: 1.8, rotation: [0, Math.PI, 0] },
].map(character => Object.freeze({ ...character, rotation: Object.freeze(character.rotation) })));

export const PLAYER_GUID = STORY_CHARACTERS[0].guid;
export const unwrap = value => value?.data ?? value;
export const sceneSnapshot = value => {
  const data = unwrap(value);
  return data?.scene && typeof data.scene === 'object' ? data.scene : data;
};
export const vector3 = value => Array.isArray(value) && value.length === 3 && value.every(Number.isFinite);

/** CEF opens <root>/(editor|CabbageEditor)/Frontend/dist/index.html. */
export function resolveStoryAssetPath(frontendUrl, asset) {
  const base = new URL(frontendUrl);
  if (base.protocol !== 'file:') throw new Error('剧情模型需要在引擎文件页面中加载');
  const url = new URL(`../../../game/art/models/${asset.split('/').map(encodeURIComponent).join('/')}`, base);
  const path = decodeURIComponent(url.pathname);
  if (url.hostname) return `//${url.hostname}${path}`;
  return /^\/[a-z]:\//i.test(path) ? path.slice(1) : path;
}

/** Rz * Ry * Rx, matching the engine's ZYX Euler angles (radians). */
export function rotatePoint([x, y, z], [rx, ry, rz]) {
  const y1 = y * Math.cos(rx) - z * Math.sin(rx), z1 = y * Math.sin(rx) + z * Math.cos(rx);
  const x2 = x * Math.cos(ry) + z1 * Math.sin(ry), z2 = -x * Math.sin(ry) + z1 * Math.cos(ry);
  return [x2 * Math.cos(rz) - y1 * Math.sin(rz), x2 * Math.sin(rz) + y1 * Math.cos(rz), z2];
}
export const hasUsableBounds = bounds => Array.isArray(bounds) && bounds.length === 6
  && bounds.every(Number.isFinite) && [0, 1, 2].every(i => bounds[i + 3] > bounds[i]);

export function rotatedBounds(bounds, rotation) {
  if (!hasUsableBounds(bounds)) throw new Error('模型包围盒不可用');
  const points = [];
  for (let i = 0; i < 8; i++) points.push(rotatePoint([
    bounds[(i & 1) ? 3 : 0], bounds[(i & 2) ? 4 : 1], bounds[(i & 4) ? 5 : 2],
  ], rotation));
  return [...[0, 1, 2].map(axis => Math.min(...points.map(p => p[axis]))),
    ...[0, 1, 2].map(axis => Math.max(...points.map(p => p[axis])))];
}
export function characterTransform(character, localBounds, savedGeometry = null) {
  const rotation = character.role === 'player' && vector3(savedGeometry?.rotation)
    ? [...savedGeometry.rotation] : [...character.rotation];
  const bounds = rotatedBounds(localBounds, rotation);
  const extent = [0, 1, 2].map(i => bounds[i + 3] - bounds[i]);
  const scale = character.height ? character.height / extent[1] : character.size / Math.max(...extent);
  const position = character.role === 'player' && vector3(savedGeometry?.position)
    ? [...savedGeometry.position] : [character.x, -bounds[1] * scale, character.z];
  return { position, rotation, scale: [scale, scale, scale] };
}
