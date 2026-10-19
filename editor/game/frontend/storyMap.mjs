/**
 * The map every story world stands on (山门神殿).
 *
 * `game/data/prophet.json`'s `map` block is the authoritative content — designers edit that
 * file — and this module is the runtime projection of it, kept honest by
 * `game/tests/test_story_assets.py`, which diffs the two field by field and re-derives the
 * two measurements from the shipped GLB.
 *
 * The map arrives exactly the way the prophet's cube does: the page asks the engine to
 * create one actor for it when it enters the world. Nothing writes `scene.ini` for it, so
 * a world that already exists — including the small world, which is a copy of the main
 * scene — gets the map on its next entry just like a freshly created one. That is
 * deliberate: an earlier design seeded the actor from native world-creation code
 * (`seed_story_world_map`), which only ever affected new worlds.
 *
 * Swapping the map's art is the one thing an existing world cannot absorb quietly, because the
 * engine imports a model only at creation time. The swap is therefore a migration: bump
 * `MAP_MODEL_REF`, and the entry path replaces an actor built from the previous generation
 * instead of leaving it to render the old GLB forever. See `MAP_MODEL_REF`.
 *
 * Everything geometric here is a pure function, so the placement can be tested without an
 * engine. The map is deliberately NOT a `STORY_CHARACTERS` entry: that list is asserted to
 * be exactly the four inhabitants, and the map is static scenery rather than a character.
 */

import { sceneSnapshot } from './storyCharacters.mjs';

/** Stable identity, so a second entry adopts the world's map instead of appending one.
 *
 * This guid is the actor's identity, NOT its generation: it must never change, or a world that
 * already carries the map would stop being recognised as carrying it and would get a second one.
 */
export const MAP_GUID = 'corona.story.world-map.v1';
/**
 * The map's **asset generation** — the migration key of this module, and the reason it exists.
 *
 * The engine imports a model only when the actor is *created*: the `skip_if_exists` repair branch
 * of `create_actor` `cef_editor_native_api_handlers.cpp:3410-3424` returns as soon as it matched
 * the guid and only applies position/rotation/scale/follow_camera/body_type — it never rewrites
 * `actor.route` (nothing in `apply_actor_data_to_existing`, `:3377-3405`, touches the route).
 * So a world whose map actor still points at the old GLB keeps rendering the old geometry, no
 * matter what the shipped placement says.
 *
 * **Any future swap of `MAP_PLACEMENT.asset` MUST bump this string.** It is stamped on the actor
 * at creation (`model_ref`), and `ensureStoryMap` treats a mismatch as "this actor was built from
 * another generation of the art": it deletes the actor and creates it again from the current data
 * (removal + creation is the only path that re-imports). Forgetting the bump ships a world that
 * silently keeps the previous map forever — the exact bug the bump to `.v2` fixes.
 */
export const MAP_MODEL_REF = 'corona.story.world-map.v2';
/**
 * The map's record identity inside a scene snapshot. `MAP_ACTOR_DATA` writes both onto the
 * actor, so a snapshot can be recognised even when the guid or the version marker is missing
 * (an older world, or a record the engine re-created). Nothing else in this game uses either
 * value: characters are `story_character`, placed props are `story_prop`.
 */
export const MAP_ENTITY_ID = 'story.world.map';
export const MAP_ENTITY_TYPE = 'story_scenery';

const COORD_LIMIT = 1e6;
const finiteNumber = value => typeof value === 'number' && Number.isFinite(value);
const positiveNumber = value => finiteNumber(value) && value > 0 && value <= COORD_LIMIT;

/** The model's real largest world axis in metres: what the engine divides the model by. */
export const MAP_MAX_AXIS = 1680.0027;

/**
 * Extra world metres on top of the derived placement. Zero by construction, and not a knob.
 *
 * `mapOriginTransform` already lands the courtyard on the engine floor y = 0: `position.y` is
 * the courtyard's offset below the model's AABB mid height, converted from model metres into
 * world metres by `scale / MAP_MAX_AXIS` — the same factor the baked height grid is read
 * through (`storyMapHeight.MODEL_TO_WORLD`), so the map and the ground line move together.
 *
 * This used to be 2.5, hand-tuned to fight "the characters float above the map". That float was
 * never terrain: it was the missing `scale / MAP_MAX_AXIS` factor, and no constant can fix a
 * surface that varies by metres (the grid here spans −0.60 m .. 13.99 m). A non-zero value now
 * only lifts the map off the plane the world floor (`[physics] floor_y = 0`), the authored spawn
 * sites and every placed prop live on. Re-measure `courtyardY` when the art changes; leave this.
 */
export const MAP_SURFACE_LIFT = 0;

/** Mirrors the `map` block of game/data/prophet.json, field for field. */
export const MAP_PLACEMENT = Object.freeze({
  asset: 'scene/01_山门神殿_平地版.glb',
  guid: MAP_GUID,
  modelRef: MAP_MODEL_REF,
  // The map's world depth in metres, NOT life size. At the model's real 1680 m the third-person
  // camera (~4.5 m back, ~2 m up) sits inside the building and all you can see is a wall, and
  // the walkable area is far outside the engine's 50 m preload radius. 90 m makes it a level
  // you can survey: 64.8 x 14.7 x 90 m. The engine puts the model's AABB centre on the world
  // origin (`position.x` = `position.z` = 0 below), so the world footprint is the symmetric
  // +/-32.4 x +/-45 m: wide enough for the characters authored at +/-30 m.
  scale: 90,
  // The walkable courtyard of the flat-ground re-export sits on the model's own y = 0 plane:
  // the floor is one flat surface spanning the whole x/z footprint, which is what makes the four
  // story spawn sites agree to the centimetre instead of sitting on a hillside.
  courtyardY: 0,
  centreY: 124.091286,
});

/** Strict validation, so a bad edit in the JSON table fails loudly instead of drifting. */
export function validateMapPlacement(raw) {
  const source = raw && typeof raw === 'object' ? raw : {};
  if (typeof source.asset !== 'string' || !source.asset.trim()) throw new Error('地图资源路径无效');
  if (typeof source.guid !== 'string' || !source.guid.trim()) throw new Error('地图标识无效');
  if (typeof source.modelRef !== 'string' || !source.modelRef.trim()) throw new Error('地图版本标记无效');
  if (!positiveNumber(source.scale)) throw new Error('地图参数无效：scale');
  // The courtyard may sit exactly on the model's own y = 0, so 0 is legal here.
  if (!finiteNumber(source.courtyardY) || source.courtyardY < 0 || source.courtyardY > COORD_LIMIT) {
    throw new Error('地图参数无效：courtyardY');
  }
  // The AABB mid-height the courtyard is measured against. It must sit inside the model's own
  // height, or `position.y` would be nonsense (a sign or axis error caught here, not on screen).
  if (!finiteNumber(source.centreY) || source.centreY <= 0 || source.centreY > COORD_LIMIT) {
    throw new Error('地图参数无效：centreY');
  }
  if (source.centreY <= source.courtyardY) throw new Error('地图庭院不在模型包围盒中心之下');
  return Object.freeze({
    asset: source.asset.trim(), guid: source.guid.trim(), modelRef: source.modelRef.trim(),
    scale: source.scale, courtyardY: source.courtyardY, centreY: source.centreY,
  });
}

validateMapPlacement(MAP_PLACEMENT);

/**
 * The map's actor transform, derived from the two measurements.
 *
 * The engine normalises every static model into a unit cube: the model's world AABB centre
 * is moved to the origin and the largest axis is scaled to 1
 * (`scale_factor = 1 / max_axis` in
 * `modules/corona_resource/src/resource/types/parse_common.h`, applied at
 * `parse_assimp.h`). An actor's `position` and `scale` are then applied to that normalised
 * geometry (`world_aabb_from_local_bounds` in `src/systems/mechanics/mechanics_system.cpp`;
 * `native_actor_world_aabb` reports `position + local * scale`), and a model point at height `y`
 * lands at:
 *
 *   world_y = position.y + (y - centre_y) * scale_factor * scale
 *
 * with `scale_factor * scale = 1` when `scale` is the model's real largest axis. So:
 *
 *   - `scale = scale` puts the map back at its real size in metres;
 *   - `position.y = (centre_y - courtyardY) * scale / max_axis` puts the courtyard (measured at
 *     `courtyardY`, 0 on the flat-ground re-export) exactly on y = 0, which is the engine floor
 *     and the line `storyCharacters.characterTransform` puts character feet on.
 *
 * `MAP_MAX_AXIS` (1680.0027 m) is the factor that turns model metres into world metres: z is this
 * map's largest axis (1680 m deep) while the map is only 274 m tall, so `centre_y` is 124.091286 m
 * and NOT `max_axis / 2` (840.0 m) or `scale / 2` (45.0 m). The courtyard sits
 * `124.091286 - 0 = 124.091286` m below the AABB centre, which is 6.6477 world metres at the
 * shipped `scale = 90`. Dividing that offset by `scale` instead (1.3788) leaves the map ~5 m below
 * the floor — the "characters float above the map" that a hand-tuned `MAP_SURFACE_LIFT` used to
 * paper over, and that the baked height grid now answers for real. Writing the metre value itself
 * (124.09) would be off by more than a hundred metres.
 *
 * `position.x`/`position.z` stay 0 on purpose: the model's courtyard is within centimetres of
 * its own origin (AABB centre x = -5.16 m), and the story's spawn sites are authored in
 * courtyard coordinates, so the courtyard origin must be the world origin.
 *
 * Returns a fresh object every call: the engine call and its read-back confirmation must
 * never share a mutable value.
 */
export function mapOriginTransform(placement = MAP_PLACEMENT) {
  // Model metres -> world metres. Deliberately the same expression as
  // `storyMapHeight.MODEL_TO_WORLD`: the map and the ground grid read out of it must go through
  // one factor, or the feet land on a surface the map does not have.
  const modelToWorld = placement.scale / MAP_MAX_AXIS;
  return {
    position: [0, (placement.centreY - placement.courtyardY) * modelToWorld
      + MAP_SURFACE_LIFT, 0],
    rotation: [0, 0, 0],
    scale: [placement.scale, placement.scale, placement.scale],
  };
}

/**
 * True for the game-owned map only, so scenery and user-authored models are never touched.
 *
 * 「地图不算物体」has exactly one answer, and this is it: the 调整物体 list and the world-rule
 * target list both ask this question instead of keeping their own copy of the rule. Identity
 * is read from fields no other actor carries — the guid, the version marker the page stamps on
 * it, and its `entity_id`/`entity_type` — never from a display name, so renaming the map (or
 * translating its name) cannot make it adjustable again. `runtime.*` is the same fallback the
 * adjust list already uses for engine records that nest the entity fields.
 */
export function isMapActor(actor) {
  if (!actor || typeof actor !== 'object') return false;
  const runtime = actor.runtime && typeof actor.runtime === 'object' ? actor.runtime : {};
  const identity = field => {
    const value = actor[field] ?? runtime[field];
    return value === null || value === undefined ? '' : String(value);
  };
  return identity('actor_guid') === MAP_GUID
    || identity('model_ref') === MAP_MODEL_REF
    || identity('entity_id') === MAP_ENTITY_ID
    || identity('entity_type') === MAP_ENTITY_TYPE;
}

/** The file name inside an engine route, e.g. `Assets/Models/5f63c8834b9d/山门…优化版.glb`. */
export const assetName = route => (typeof route === 'string' ? route.replace(/\\/g, '/').split('/').pop() : '');

/**
 * True when an existing actor's route still names this build's shipped asset file.
 *
 * `model_ref` is the primary generation key, but the route is the field the engine actually
 * renders from, and it is the one that survives a world whose marker was stamped correctly and
 * then had its asset swapped underneath it. The engine exports `actor.route` in the scene
 * snapshot under `route`/`path`/`model` (`actor_to_json`,
 * `cef_editor_native_api_handlers.cpp:2745-2749`), storing a portable, project-relative route
 * (the same route the page sent, or its imported equivalent under `Assets/Models/<hash>/…`),
 * so the file name — the flat-ground re-export has its own name, "平地版" — is the asset
 * generation in a form no marker can lie about. An actor the engine has not resolved yet, or a
 * snapshot that does not carry a route at all, reports inconclusive and the marker decides; it
 * is never read as stale on its own.
 */
export function isMapCurrentAsset(actor, placement = MAP_PLACEMENT) {
  const route = actor?.route ?? actor?.path ?? actor?.model;
  if (typeof route !== 'string' || !route.trim()) return true;
  return assetName(route) === assetName(placement.asset);
}

/**
 * A map the world already carries and this version owns: nothing may be written for it.
 * An actor without this version's marker (an older entry, or a world made by the removed
 * native seeder) is not "ready" and is repaired in place instead.
 *
 * The transform is part of "ready", not just the scale. A world that already carries the map
 * at an older `position.y` (the map was sunk 40 cm before this was derived from the AABB mid
 * height) must be repaired rather than adopted, or the world would keep the wrong height
 * forever: a scale-only check made exactly that mistake.
 *
 * The asset generation is part of "ready" too, and it is a *different sentence* from the
 * transform: an actor whose route names another GLB cannot be repaired in place at all (see
 * `MAP_MODEL_REF`), it has to be replaced — `isMapGenerationCurrent` is the question
 * `ensureStoryMap` asks first, and a stale transform on a current generation is still repaired
 * in place.
 */
export function isMapGenerationCurrent(actor, placement = MAP_PLACEMENT) {
  return actor?.model_ref === placement.modelRef && isMapCurrentAsset(actor, placement);
}

const sameVector = (a, b) => Array.isArray(a) && a.length === b.length
  && a.every((value, index) => Number.isFinite(value) && Math.abs(value - b[index]) < 1e-4);

export function isMapReady(actor, placement = MAP_PLACEMENT) {
  if (!(Number(actor?.handle) > 0) || actor?.load_status !== 'loaded') return false;
  if (!isMapGenerationCurrent(actor, placement)) return false;
  const transform = mapOriginTransform(placement);
  return isMapStatic(actor) && sameVector(actor?.geometry?.scale, transform.scale)
    && sameVector(actor?.geometry?.position, transform.position)
    && sameVector(actor?.geometry?.rotation, transform.rotation);
}

/** Scenery stays fixed while its real triangles participate in native movement. */
export const isMapStatic = actor => actor?.mechanics?.body_type === 'static';

const unwrap = value => value?.data ?? value;

/** The same acknowledgement rule the other story actors use before the page trusts a reply. */
async function call(operation, invoke) {
  const data = unwrap(await invoke());
  if (!data || data.ok === false || !['success', 'loaded'].includes(data.status)) {
    throw new Error(`${operation}：${data?.message || data?.diagnostics?.[0]?.message || '引擎未确认成功'}`);
  }
  return data;
}

/**
 * Delete a map actor, so the next `createActor` really imports the current route.
 *
 * `sceneTools.removeActor` is the public form of the editor's `remove_actor` handler
 * (`cef_editor_api.cpp:635` → `remove_native_editor_actor`,
 * `cef_editor_native_api_handlers.cpp:3602-3638`), which drops the engine actor, erases the
 * record, persists the scene and emits the tree change — the removal is durable, not just
 * visual. It resolves its target by name, by `actor_guid` or by handle string, and this module
 * cannot know which of those an older world actually stored (the native seeder that used to
 * create the map is gone), so every key the snapshot gives for this one actor is tried in turn.
 * Identity is still the map's own fields — a user-authored actor is never a candidate.
 */
async function removeMapActor(api, sceneId, actor) {
  const keys = [actor.actor_guid, actor.name, Number(actor.handle) > 0 ? String(actor.handle) : '']
    .filter(key => typeof key === 'string' && key.trim());
  let last = null;
  for (const key of [...new Set(keys)]) {
    try {
      return await call('移除旧版本地图', () => api.sceneTools.removeActor(sceneId, key));
    } catch (error) {
      last = error;
    }
  }
  throw last || new Error('移除旧版本地图：引擎未确认成功');
}

/**
 * Bring the world in line with the map table: create it when the world does not have it,
 * repair the transform in place when it carries an older one, and otherwise touch nothing.
 *
 * The map is static triangle geometry. Native movement shares its resident
 * LOD0 topology and octree with all other mesh collision.
 *
 * `follow_camera: false` is the other half of "world object": the engine derives
 * `render_space = follow_camera ? "ui" : "scene"`
 * (`src/systems/ui/cef/cef_editor_native_api_handlers.cpp:2785`), so a map that followed the
 * camera would be drawn in UI space and appear to move with the player instead of standing
 * still under them.
 *
 * Idempotent, and deliberately quiet on the common path: an intact map is returned without a
 * single write. `update_if_exists` is only ever sent when the map is genuinely not ready — the
 * engine answers it by rewriting the whole scene manifest
 * (`persist_native_scene_actors`, `cef_editor_native_api_handlers.cpp:3417-3419`), so sending
 * it on every entry would rewrite the world every time for nothing (`storyCube.mjs` is the
 * precedent for "reuse untouched").
 *
 * An actor on an **older asset generation** is not repairable, only replaceable, so it is
 * removed and created again from scratch — the one path on which the engine imports the current
 * route. A mere transform difference is still written in place through `skip_if_exists` +
 * `update_if_exists`, so a world that only needs its map moved is never rewritten.
 */
export const MAP_ACTOR_DATA = Object.freeze({
  name: '剧情世界地图',
  semantic_role: 'world-map',
  entity_id: MAP_ENTITY_ID,
  entity_type: MAP_ENTITY_TYPE,
  follow_camera: false,
  body_type: 'static',
});

export async function ensureStoryMap({ api, sceneId, frontendUrl, resolveAsset = null,
  placement = MAP_PLACEMENT, wait = ms => new Promise(resolve => setTimeout(resolve, ms)),
  confirmAttempts = 150 }) {
  const actors = sceneSnapshot(await api.scene.getSnapshot(sceneId))?.actors;
  let map = (Array.isArray(actors) ? actors : []).find(isMapActor) || null;

  if (!map) {
    const route = typeof resolveAsset === 'function' ? resolveAsset(frontendUrl, placement.asset) : null;
    if (!route) throw new Error('地图资源路径不可用');
    const created = await call('创建地图', () => api.sceneTools.createActor(sceneId, route, 'model', {
      ...MAP_ACTOR_DATA, actor_guid: placement.guid, model_ref: placement.modelRef,
      ...mapOriginTransform(placement),
    }));
    map = created.actor;
  } else if (!isMapGenerationCurrent(map, placement)) {
    // The world carries a map built from another generation of the art (an older `model_ref`,
    // or a route that still names the previous GLB). No repair can help: `skip_if_exists` only
    // rewrites the transform and never the route, so the engine keeps serving the bundle it
    // imported back then. The only path that reaches the importer is a fresh creation, and the
    // guid stays the same, so this replaces the map instead of adding a second one.
    await removeMapActor(api, sceneId, map);
    const route = typeof resolveAsset === 'function' ? resolveAsset(frontendUrl, placement.asset) : null;
    if (!route) throw new Error('地图资源路径不可用');
    const created = await call('重建地图', () => api.sceneTools.createActor(sceneId, route, 'model', {
      ...MAP_ACTOR_DATA, actor_guid: placement.guid, model_ref: placement.modelRef,
      ...mapOriginTransform(placement),
    }));
    map = created.actor;
  } else if (!isMapReady(map, placement)) {
    // The world already has this actor on the current generation (an existing world, or one
    // whose first creation was interrupted): the same GUID plus `skip_if_exists` makes the
    // engine move it instead of appending a second map over the top of the first. The route
    // this build ships wins over the actor's own `route`, and the engine's `skip_if_exists`
    // resolves the existing actor by GUID before it looks at any path, so a repair imports
    // nothing. Deleting here would be thrash: a mere position change must never re-import.
    const route = (typeof resolveAsset === 'function' ? resolveAsset(frontendUrl, placement.asset) : null)
      || map.route;
    if (!route) throw new Error('地图资源路径不可用');
    const updated = await call('校正地图摆位', () => api.sceneTools.createActor(sceneId, route, 'model', {
      ...MAP_ACTOR_DATA, actor_guid: placement.guid, model_ref: placement.modelRef,
      skip_if_exists: true, update_if_exists: true,
      ...mapOriginTransform(placement),
    }));
    map = updated.actor;
  }
  // A map that is already in place is returned below without computing a route or sending a
  // single write: this is the path every ordinary entry takes.

  if (!map) throw new Error('引擎未返回地图');
  // Confirm on read-back that the engine really accepted the map, instead of trusting the
  // create call's reply: a world whose map never landed must say so rather than drop the
  // player onto nothing. The handle is acknowledged before the asynchronous import fills
  // the record, so a fresh actor needs a bounded wait rather than one read.
  for (let attempt = 0; ; attempt++) {
    const confirmed = sceneSnapshot(await api.scene.getSnapshot(sceneId))?.actors
      ?.find(item => item.actor_guid === placement.guid);
    // Native publishes readiness only after its original triangle data is
    // available. Older bridges omit this field and retain their old behavior.
    if (isMapReady(confirmed, placement) && confirmed.collision_ready !== false) return confirmed;
    if (attempt >= confirmAttempts - 1) throw new Error('等待地图就位超时，请重试进入世界');
    await wait(200);
  }
}
