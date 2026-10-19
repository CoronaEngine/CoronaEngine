/** Models in game/art/models and stable identities owned by the story game. */
import { PROPHET_PLACEMENT } from './storyDialogue.mjs';
/**
 * How far the player model's own forward axis sits from the logical +Z the game steers by.
 *
 * `playerController.facePlayer` writes `rotation.yaw = facingYaw + offset`, and the logical
 * facing is read back as `rotation.yaw - offset`, so this constant is the single place that
 * reconciles "which way the art faces" with "which way the player walks". In other words it is
 * the Y rotation that maps the model's own forward axis onto logical +Z.
 *
 * Its value is a property of the model, so it changed with the art:
 *   - `Maria WProp J J Ong.dae` faces local -Z → `Math.PI`
 *   - `E3D-Assassin.glb` (Max Biped) faces local +X → `-Math.PI / 2`
 *
 * How each value was established, in order, because the two 90-degree candidates are easy to
 * confuse: with `Math.PI` the character walked *sideways* toward its own left; with `Math.PI/2`
 * it walked **backwards**, i.e. W played what S should — exactly 180 degrees from correct, which
 * puts the model's forward on +X and the answer at `-Math.PI / 2` (= `3pi/2`). The sideways
 * observation alone was ambiguous about the sign; the W/S inversion settled it.
 *
 * If a future model walks backwards, rotate by pi again (`Math.PI / 2`); if sideways, by pi/2
 * either way. There is no way to read this off the glTF bytes — the facing axis is authored
 * geometry, not metadata — so it has to be observed once per model.
 */
export const PLAYER_MODEL_YAW_OFFSET = -Math.PI / 2;
export const PROPHET_MODEL_REF = 'story.prophet.placement.v1';
/**
 * The player's **placement generation**: which model file the shipped table describes, plus
 * the facing convention that goes with it.
 *
 * A world whose player actor carries an older marker is a player built from the previous art.
 *   `story.player.facing.v1` -> `Maria WProp J J Ong.dae` (COLLADA, faces local -Z)
 *   `story.player.waik.v1`   -> `waik.glb` (same Max Biped rig as below, faces local +X)
 *   `story.player.walk.v1`   -> `player_walk.glb`, that same rig with a 1.100 s walk cycle
 *                               instead of the 3.367 s one. Same AABB and same joint list, so
 *                               the facing convention is unchanged: `-Math.PI / 2` holds for
 *                               both, which is why only the asset changed here.
 *   `story.player.walk.v2`   -> the same mesh re-exported with seven motion clips **embedded**
 *                               (`IdleA`, `Walk`, `Run`, `AttackA`, `AttackB`, `JumpA`, `Dead`).
 *                               The clips come from the E3D-Archer package, whose rig shares all
 *                               65 `Bip001` deformation bones with this mesh by name — which is
 *                               what `AnimationPlayer` binds on — so the character is unchanged
 *                               and only its motion is new. The marker still has to be bumped:
 *                               the route keeps the same file name, and `create_actor` never
 *                               rewrites `actor.route`, so an old world would otherwise keep the
 *                               clip-less import cached under that name.
 *   `story.player.archer.v1` -> `E3D-Archer.glb`: the archer package's own mesh (107 bones,
 *                               12 991 triangles, bow included) with the same seven clips. The
 *                               clips were authored for this body — driving the previous mesh with
 *                               them raised its left arm into the package's bow-holding idle.
 *
 * **Bump this whenever the player's `asset` changes**, the same rule as `BOSS_MODEL_REF`.
 */
export const PLAYER_MODEL_REF = 'story.player.assassin.v6';
/**
 * The boss's **asset generation** — the migration key for its model.
 *
 * Exactly the reason `storyMap.MAP_MODEL_REF` exists: `create_actor`'s `skip_if_exists`
 * repair branch (`cef_editor_native_api_handlers.cpp:3410-3424`) returns as soon as it matched
 * the guid and only rewrites position/rotation/scale/follow_camera/body_type — it never
 * rewrites `actor.route`. A world whose boss actor still points at the dragon keeps rendering
 * the dragon no matter what this table says, so a swap has to delete the actor and create it
 * again, which is the one path that reaches the importer.
 *
 * **Any future swap of the boss's `asset` MUST bump this string**, or every existing world
 * silently keeps the previous model.
 */
export const BOSS_MODEL_REF = 'story.boss.em001_00.v1';
// ---------------------------------------------------------------------------
// 主世界怪物 (main-world creatures)
//
// These inhabit the boss arena. The visibility filter in `storyActors.mjs` shows them when
// `gameplay.role === 'main'` and hides them everywhere else, so the small world's cast stays
// player + prophet and nothing here can reach it.
//
// **Not gated on `boss.hp > 0`.** They are additional monsters, not the boss: tying them to
// the boss's health would empty the whole arena the instant it dies, which is the opposite of
// "the main world has monsters". The boss keeps its own gate.
//
// **`rotation` is [0, 0, 0] for every one of them, on purpose and permanently.** An earlier
// boss build "corrected" a model's orientation to buy height and shipped a creature lying on
// its side. These keep the orientation the art was authored in; the numbers that place them
// honestly are `size` (the model's largest axis, in metres) and a site proven clear of the
// map, never a rotation.
//
// Every value was measured, not chosen. Model AABB -> world box at the shipped `size`:
//   file        model AABB w x h x d            size  world box (w x h x d m)     site
//   em135_03    29.819 x 40.140 x 17.536         5.0  3.71 x 5.00 x 2.18       (-30,  42)
//   em001_01    26.641 x 17.520 x  6.106         6.0  6.00 x 3.95 x 1.38       ( 30,  42)
//   em001_03    same AABB                        6.0  6.00 x 3.95 x 1.38       (  0, -42)
//   em001_05    same AABB                        6.0  6.00 x 3.95 x 1.38       (  0,  36)
//   em3         65.541 x 34.525 x 21.079         4.0  4.00 x 2.11 x 1.29       (-30, -42)
//   em1          0.568 x  0.309 x  0.157         3.0  3.00 x 1.63 x 0.83       (-30,  12)
//   em2          2.180 x  0.952 x  0.764         3.0  3.00 x 1.31 x 1.05       ( 27, -42)
//
// The sites are the output of a vertex-level search, not eyeballed: the main world's open
// ground is tight — the boss stands at (2, 5.5) with only 4 m of clearance, and past that the
// temple front rises (1.825 m at 5 m, 2.141 m at 6 m, 3.088 m at 10 m) — so a creature placed
// by eye ends up inside a wall, which is exactly how the original dragon got buried at (0, 12).
// Every pair here is 27-48 m apart and clear of the map at its own height.
//
// `em001_04` is absent by construction: it is the main world's boss, not an inhabitant.
const MONSTERS = Object.freeze([
  // 从模型包（AN_NPC_MOB_*）转换而来：每个文件内嵌 Idle / Walk / Run / Attack1 / Die 五条动作，
  // 且 **Idle 排在第一条**——引擎默认循环第一条 clip，所以放上去就会自己待机。
  //
  // **不旋转**：这六只的高度本来就在 Y 轴上。此前按"Z 包围盒比 Y 大就是 Z-up"去判断是错的——
  // 蜥蜴和幼虫在 Z 方向更"长"（体长），不是更"高"；实测带 -90° 的那两只正是躺着的那两只。
  //
  // **已剔除动作里的缩放通道**：那些 squash/stretch 曲线是按源骨架比例制作的，
  // 套到本网格的绑定姿态上会在动起来时把模型拉伸（刀螳那个问题）。
  //
  // size = 目标身高 ÷ 该模型的 Y 归一化占比，因此渲染高度可控。
  { role: 'monster-wyvern', name: '地龙', guid: 'f9f5c8b0-7324-4b6c-a011-000000000021',
    asset: 'monster/landwyvern.fbx', x: -9, z: -5, size: 3.0, rotation: [0, 0, 0] },   // 高 3.0 m
  { role: 'monster-lizardf', name: '蜥蜴女猎手', guid: 'f9f5c8b0-7324-4b6c-a011-000000000023',
    asset: 'monster/lizardf.fbx', x: -7, z: 8, size: 3.5, rotation: [0, 0, 0] },   // 高 2.2 m
  { role: 'monster-lizardm', name: '蜥蜴战士', guid: 'f9f5c8b0-7324-4b6c-a011-000000000024',
    asset: 'monster/lizardm.fbx', x: 10, z: 6, size: 3.1, rotation: [0, 0, 0] },   // 高 2.0 m
  { role: 'monster-lion', name: '荒原狮兽', guid: 'f9f5c8b0-7324-4b6c-a011-000000000025',
    asset: 'monster/lion.fbx', x: 0, z: -12, size: 3.7, rotation: [0, 0, 0] },   // 高 1.6 m
  { role: 'monster-mantis', name: '刀螳', guid: 'f9f5c8b0-7324-4b6c-a011-000000000026',
    asset: 'monster/mantis.fbx', x: -12, z: 2, size: 2.3, rotation: [0, 0, 0] },   // 高 2.2 m
  { role: 'monster-larva', name: '沙原幼虫', guid: 'f9f5c8b0-7324-4b6c-a011-000000000027',
    asset: 'monster/larva.fbx', x: 6, z: -11, size: 2.4, rotation: [0, 0, 0] },   // 高 0.8 m
]);

export const STORY_CHARACTERS = Object.freeze([
  { role: 'player', name: '玩家', guid: 'f9f5c8b0-7324-4b6c-a011-000000000001',
    asset: 'player/E3D-Assassin.glb', x: 0, z: 0, height: 1.8, rotation: [0, PLAYER_MODEL_YAW_OFFSET, 0] },
  // The forecourt pocket in front of the temple's main hall. Authored at (0, 12) the original
  // dragon stood *inside* the hall's front bay: the shipped map has geometry 3.088 m high
  // within 4 m of that point (`test_no_map_geometry_stands_over_the_boss_spawn_site`
  // re-measures the GLB), which is the temple it was hidden inside. (2, 5.5) is the nearest
  // open ground in front of the hall — the highest map vertex within 4 m is 0.102 m, the
  // ground is the courtyard's y = 0, and the move is 6.80 m toward the player rather than away
  // from them. Past 4 m the temple front rises fast: 1.825 m at 5 m, 2.141 m at 6 m, 3.088 m at
  // 10 m. `size = 8` is therefore a measured ceiling, not a taste knob: 8 m is the largest
  // scale whose world footprint still fits under that ceiling, and everything from 12 m up
  // intersects the hall.
  //
  // em001_04 is a different creature from the dragon, and its own model AABB (26.641 x 17.520
  // x 6.106 model units) has one axis barely a third of the other two. At `size = 8` each
  // candidate rotation was measured against the map's real vertices:
  //   [0, 0, 0]     -> 8.00 w x 5.26 h x 1.83 d m, nothing above 0.102 m   <- shipped
  //   [0, 0, pi/2]  -> 5.26 w x 8.00 h x 1.83 d m, nothing above 0.000 m
  //   [0, pi/2, 0]  -> 1.83 w x 5.26 h x 8.00 d m, nothing above 0.000 m
  //   [pi/2, 0, 0]  -> 8.00 w x 1.83 h x 5.26 d m, geometry at 1.721 m -> intersects
  // Identity is the shipped rotation: it is the one the art was authored in, and rolling the
  // model onto another axis to buy height is what made an earlier build look like a creature
  // lying on its side. The hunt for the tallest box was also unnecessary — the dragon it
  // replaces was itself flat (8.00 w x 5.69 h x 1.68 d m), so 5.26 m of height is in the same
  // league, not a shortfall. What the measurements still decide is that 8 m is the largest
  // `size` whose footprint stays clear, and that only identity and the two yaw/roll variants
  // avoid the hall; see the growth guard in `test_no_map_geometry_stands_over_the_boss_spawn_site`.
  // The GLB carries exactly one action (`Root`, 4.883 s), which is what the engine loops; see
  // BOSS_MODEL_REF for why the actor is replaced rather than repaired when this asset changes.
  { role: 'boss', name: 'Boss', guid: 'f9f5c8b0-7324-4b6c-a011-000000000002',
    asset: 'boss/Em001_00.glb', model_ref: BOSS_MODEL_REF,
    // 模型自带的轴向是"高度落在 Z 上"（3ds Max 的 Z-up 未被导入器转换），
    // 所以用角色旋转把它立起来：绕 X 轴 -90° 把 +Z 映射到 +Y。
    // `characterTransform` 用 rotatedBounds(local_aabb, rotation) 算缩放和落地，
    // 因此旋转后 size 不变即可维持 8.00 x 5.26 x 1.83 m，寺庙前庭的净空约束仍然成立。
    // 落点由资产测试的净空约束保证：4 m 内最高点 0.10 m（上限 0.35 m），
    // 5 m 处寺檐 1.96 m、6 m 处 2.59 m、10 m 处大殿 3.09 m —— 8 m 翅膀不会埋进建筑。
    // 寺庙那一侧（+z）的空地：全图 9–20 m 范围内只有 4 处满足净空，这是唯一贴近寺庙的一处。
    // 距出生点 19.4 m，4 m 内最高点 0.33 m（上限 0.35），10 m 处大殿 6.4 m —— 既在庙旁又不埋进建筑。
    // 离最近的怪物 10.0 m，不会和它们挤在一起。
    x: 11, z: 16, size: 8, rotation: [-Math.PI / 2, 0, 0] },
  { role: 'merchant', name: '商人', guid: 'f9f5c8b0-7324-4b6c-a011-000000000003',
    asset: 'merchant/Maw J Laygo.dae', x: -30, z: -15, height: 1.8, rotation: [0, Math.PI, 0] },
  { role: 'prophet', name: '先知', guid: 'f9f5c8b0-7324-4b6c-a011-000000000004',
    asset: PROPHET_PLACEMENT.asset, x: 30, z: -15, height: PROPHET_PLACEMENT.height, rotation: [0, Math.PI, 0] },
  // Main-world creatures. Appended after the four inhabitants: `PLAYER_GUID`, `BOSS_GUID` and
  // `PROPHET_GUID` address them by index, so appending keeps those stable.
  ...MONSTERS,
].map(character => Object.freeze({ ...character, rotation: Object.freeze(character.rotation) })));

/** Main-world creatures, in table order. One predicate, so the visibility filter and any
 * future per-creature rule cannot disagree about what counts as a monster. */
export const MONSTER_ROLES = Object.freeze(MONSTERS.map(monster => monster.role));
export const isMonsterRole = role => MONSTER_ROLES.includes(role);

export const PLAYER_GUID = STORY_CHARACTERS[0].guid;
export const BOSS_GUID = STORY_CHARACTERS[1].guid;
export const PROPHET_GUID = STORY_CHARACTERS[3].guid;
/** The prophet's `entity_id` as `storyActors` creates it (`story.${role}`). */
export const PROPHET_ENTITY_ID = 'story.prophet';
/**
 * True for the game's prophet only, so no world rule can ever animate or resize it.
 *
 * 「碎片不影响先知」has exactly one answer, and this is it: the world-rule target list asks
 * this function instead of keeping its own copy of the rule, the same way `isMapActor`
 * answers 地图不算物体 for the map. A fragment never names its targets (see
 * `storyWorldRules.collectSwayTargets`), so the only place a prophet-targeted rule can be
 * refused is where the targets are resolved — which is why the identity has to be readable
 * from the actor record alone, never from a display name: a renamed (or translated) 先知
 * must stay excluded, and a rule the player installed before this guard existed must be
 * harmless on the next entry.
 *
 * Identity is read from the fields no other actor carries — the guid, the placement
 * generation the page stamps on it, its `entity_id` and its semantic role — never from
 * `name`. `runtime.*` is the same fallback the other identity readers use for engine
 * records that nest these fields.
 */
export function isProphetActor(actor) {
  if (!actor || typeof actor !== 'object') return false;
  const runtime = actor.runtime && typeof actor.runtime === 'object' ? actor.runtime : {};
  const identity = field => {
    const value = actor[field] ?? runtime[field];
    return value === null || value === undefined ? '' : String(value);
  };
  return identity('actor_guid') === PROPHET_GUID
    || identity('model_ref') === PROPHET_MODEL_REF
    || identity('entity_id') === PROPHET_ENTITY_ID
    || identity('semantic_role') === 'prophet';
}
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
  const url = new URL(`../../game/art/models/${asset.split('/').map(encodeURIComponent).join('/')}`, base);
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
/** Initial authored placement. Preserve the accepted saved Y, including stairs
 * and airborne saves; runtime ground contacts are resolved by native triangles. */
export function characterTransform(character, localBounds, savedGeometry = null) {
  const rotation = ['player', 'prophet'].includes(character.role) && vector3(savedGeometry?.rotation)
    ? [...savedGeometry.rotation] : [...character.rotation];
  const bounds = rotatedBounds(localBounds, rotation);
  const extent = [0, 1, 2].map(i => bounds[i + 3] - bounds[i]);
  const scale = character.height ? character.height / extent[1] : character.size / Math.max(...extent);
  const saved = ['player', 'prophet'].includes(character.role) && vector3(savedGeometry?.position)
    ? savedGeometry.position : null;
  const x = saved ? saved[0] : character.x;
  const z = saved ? saved[2] : character.z;
  const position = [x, saved ? saved[1] : -bounds[1] * scale, z];
  return { position, rotation, scale: [scale, scale, scale] };
}
