import { FRAGMENT_GUID } from './storyGameplay.mjs';
import { PLACEMENT_PREFIX, placementScene } from './storyProps.mjs';
import { STORY_CHARACTERS, PLAYER_GUID, BOSS_MODEL_REF, PLAYER_MODEL_REF, PROPHET_MODEL_REF, PLAYER_MODEL_YAW_OFFSET, isMonsterRole, unwrap, sceneSnapshot, resolveStoryAssetPath,
  characterTransform, rotatedBounds, hasUsableBounds } from './storyCharacters.mjs';
import { PROPHET_PLACEMENT } from './storyDialogue.mjs';

const canceled = () => Object.assign(new Error('剧情世界初始化已取消'), { name: 'AbortError' });
const success = (response, operation) => {
  const data = unwrap(response);
  if (!data || data.ok === false || !['success', 'loaded'].includes(data.status)) {
    throw new Error(`${operation}：${data?.message || data?.diagnostics?.[0]?.message || '引擎未确认成功'}`);
  }
  return data;
};
const loaded = actor => actor?.load_status === 'loaded' && Number(actor?.handle) > 0;
/**
 * Whether an actor's `route` still names the shipped art for `asset`.
 *
 * The engine exposes `actor.route` as a portable, project-relative path whose file name is the
 * asset it actually imported (the same reading `storyMap.isMapCurrentAsset` relies on). A route
 * that names another file cannot be repaired in place — `create_actor`'s `skip_if_exists` branch
 * rewrites the transform and nothing else — so it has to be deleted and created again. An actor
 * with no route yet is inconclusive and reported as current, which keeps the cheap repair path.
 */
const routeFileName = value => (typeof value === 'string' && value.trim()
  ? value.replace(/\\/g, '/').split('/').pop().toLowerCase() : '');
const routeNamesShippedArt = (actor, asset) => {
  const actual = routeFileName(actor?.route);
  return !actual || actual === routeFileName(asset);
};
/**
 * Delete an actor so the next `createActor` really imports the current route.
 *
 * Mirrors `storyMap.removeMapActor`: a `skip_if_exists` repair never rewrites `actor.route`,
 * so replacing a model is removal + creation and nothing else. Identity is still the actor's
 * own guid — no other actor is ever a candidate — but the snapshot can hold the record under
 * different keys depending on which path created it, so every key it offers is tried in turn.
 */
async function removeActorForRebuild(api, sceneId, actor, operation, submit) {
  const keys = [actor.actor_guid, actor.name, Number(actor.handle) > 0 ? String(actor.handle) : '']
    .filter(key => typeof key === 'string' && key.trim());
  let last = null;
  for (const key of [...new Set(keys)]) {
    try {
      return await submit(operation, () => api.sceneTools.removeActor(sceneId, key));
    } catch (error) {
      last = error;
    }
  }
  throw last || new Error(`${operation}：引擎未确认成功`);
}

const sameVector = (a, b) => Array.isArray(a) && a.length === b.length
  && a.every((value, i) => Math.abs(value - b[i]) < 1e-5);

// Keep the real resource work inside the host's existing world-session barrier,
// including on cancellation. Readiness polling never submits overlapping reads.
async function waitForStoryResources({ api, projectPath, wait = ms => new Promise(resolve => setTimeout(resolve, ms)) }, allowFailed = false) {
  if (!projectPath) return;
  const normalize = value => String(value || '').replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();
  for (;;) {
    const status = unwrap(await api.project.getProjectLoadStatus());
    if (status?.status === 'error' || status?.ok === false) throw new Error(status.message || '无法读取世界资源状态');
    if (normalize(status?.path) !== normalize(projectPath)) throw new Error('资源状态不属于目标世界，已停止加载');
    if (!Number.isFinite(status.pending) || !Number.isFinite(status.failed) || status.pending < 0 || status.failed < 0) {
      throw new Error('引擎未返回有效的世界资源状态');
    }
    if (status.pending === 0 && status.loading !== true && status.archive_service_ready === true) {
      if (status.failed > 0 && !allowFailed) throw new Error(`世界资源加载失败（${status.failed} 个模型）`);
      return;
    }
    await wait(200);
  }
}

/** All native mutations are awaited; the host registers this work before world replacement. */
export async function ensureStoryCharacters(options) {
  await waitForStoryResources(options);
  try { return await initializeStoryCharacters(options); }
  finally { await waitForStoryResources(options, true); }
}

/**
 * Scene-document writes that can fail for a moment and succeed on a retry.
 *
 * The editor replaces `scene.ini` atomically (`MoveFileExW` over `.scene.ini.tmp`,
 * `scene_folder.cpp:1206`), and the temp name is fixed, so two writers racing over the same world
 * make one of them fail with `scene_replace_failed`. That became reachable when the animation
 * work added a second writer on the UI thread (`UiSystem::update` →
 * `tick_native_animation_updates` → `persist_native_scene_actors`) next to the one the Python
 * runtime thread performs for `createActor`/`setActorState`. A single lost race used to abort the
 * whole world load at whichever character happened to be in flight — the error surfaced as
 * 「羽翼兽加载失败」/「Boss加载失败」 with `Unable to replace scene.ini`.
 *
 * Retrying is safe because every call routed through `call` is idempotent: `createActor` matches
 * the guid and `setActorState`/`setActorTransform` assign values.
 */
const TRANSIENT_SCENE_WRITE = /Unable to replace scene\.ini|scene_replace_failed/i;
const TRANSIENT_SCENE_WRITE_ATTEMPTS = 6;

async function initializeStoryCharacters({ api, sceneId, frontendUrl, isCurrent = () => true,
  wait = ms => new Promise(resolve => setTimeout(resolve, ms)), renderAttempts = 150,
  gameplay = null, combatOnly = false, assertSource = async () => {} }) {
  const check = () => { if (!isCurrent()) throw canceled(); };
  const call = async (operation, invoke) => {
    for (let attempt = 1; ; attempt++) {
      check(); await assertSource(); check();
      try {
        const result = success(await invoke(), operation);
        check();
        return result;
      } catch (error) {
        if (attempt >= TRANSIENT_SCENE_WRITE_ATTEMPTS || !TRANSIENT_SCENE_WRITE.test(error?.message || '')) throw error;
        // Back off a little: the other writer is mid-replace, so the retry needs it to finish.
        await wait(250 * attempt);
      }
    }
  };
  check();
  await assertSource();
  let snapshot = sceneSnapshot(await api.scene.getSnapshot(sceneId));
  check();
  const actors = new Map((snapshot?.actors || []).map(actor => [actor.actor_guid, actor]));
  // 一个 guid 只允许一个 actor。换模型走"先删后建"，若删除没生效，`create_actor` 会保留 guid
  // 但给新 actor 起一个派生名字（`玩家_1`），于是同一个角色有两份模型叠在一起——而按 guid 建表
  // 的 Map 只会留下其中一个，页面因此看不出来。这里先清掉多余的，保留名字与角色一致的那个。
  {
    const shipped = new Map(STORY_CHARACTERS.map(character => [character.guid, character.name]));
    const byGuid = new Map();
    for (const actor of snapshot?.actors || []) {
      if (!shipped.has(actor.actor_guid)) continue;
      if (!byGuid.has(actor.actor_guid)) byGuid.set(actor.actor_guid, []);
      byGuid.get(actor.actor_guid).push(actor);
    }
    for (const [guid, group] of byGuid) {
      if (group.length < 2) continue;
      const expectedName = shipped.get(guid);
      const keep = group.find(actor => actor.name === expectedName) || group[group.length - 1];
      for (const actor of group) {
        if (actor === keep) continue;
        check();
        // 必须按**名字**删：`removeActorForRebuild` 的第一优先键是 guid，而这两个 actor 共用 guid，
        // 按 guid 删会命中引擎找到的第一个——也就是要保留的那一个。
        await call(`清理重复的${expectedName || actor.name}`,
          () => api.sceneTools.removeActor(sceneId, actor.name));
        actors.delete(guid);
      }
      actors.set(guid, keep);
    }
  }
  // Creatures belong to the main world and are NOT gated on `boss.hp`, unlike the boss: they
  // are the arena's other monsters, so killing the boss must not empty the arena. The small
  // world keeps its own cast (player + prophet), which is why this branch names 'main'.
  const visibleCharacters = STORY_CHARACTERS.filter(character => !gameplay
    ? !isMonsterRole(character.role)
    : character.role === 'player'
      || (gameplay.role === 'main' && character.role === 'boss' && gameplay.state.boss.hp > 0)
      // A monster stays in the arena until its own health reaches zero; the boss's health
      // has no effect on them. A save that predates the roster has no entry and stays alive.
      || (gameplay.role === 'main' && isMonsterRole(character.role)
          && ((gameplay.state.monsters?.[character.role]?.hp ?? 1) > 0))
      || (gameplay.role === 'child' && character.role === 'prophet'));
  if (gameplay) {
    const visibleGuids = new Set(visibleCharacters.map(character => character.guid));
    // Only manage game-owned actors; scenery and user-authored models stay untouched.
    // FRAGMENT_GUID is no longer part of the cast, but a world saved by the old flow may
    // still contain its red ball — leave it in the managed set so it is hidden rather
    // than left lying in the arena after the reward moved into the bag.
    for (const guid of [...STORY_CHARACTERS.map(character => character.guid), FRAGMENT_GUID]) {
      const actor = actors.get(guid);
      if (actor && actor.visible !== false && !visibleGuids.has(guid)) {
        await call('隐藏当前世界之外的模型', () => api.sceneTools.setActorState(sceneId, actor.actor_guid, { visible: false }));
      }
    }
  }
  // Never use the update subset to decide which actors should be visible.
  // 小世界里内部主角不显示：小世界的相机就是它的眼睛（自由视角 / 俯视视角）。它仍然是场景
  // 里真实存在、并且仍然被存档的 actor——F 的射程、摆放的位置、玩家存档读的都是它。
  const hideProtagonistRole = gameplay?.role === 'child';
  const characters = visibleCharacters.filter(character => !combatOnly || character.role === 'boss');
  for (let character of characters) {
    check();
    const hideProtagonist = hideProtagonistRole && character.role === 'player';
    let actor = actors.get(character.guid);
    const wasPresent = Boolean(actor);
    if (gameplay?.role === 'child' && character.role === 'prophet' && actor?.model_ref !== PROPHET_MODEL_REF) {
      const player = actors.get(PLAYER_GUID);
      if (!player?.geometry) throw new Error('先知初始化前玩家位置未就绪');
      const facing = player.geometry.rotation[1] - PLAYER_MODEL_YAW_OFFSET;
      character = { ...character, x: player.geometry.position[0] + Math.sin(facing) * PROPHET_PLACEMENT.aheadDistance,
        z: player.geometry.position[2] + Math.cos(facing) * PROPHET_PLACEMENT.aheadDistance,
        rotation: [0, facing + PROPHET_PLACEMENT.yawOffset, 0] };
    }
    let source;
    try {
      // An intact portable project must also open after game/art is moved.
      if (!actor || !loaded(actor)) source = resolveStoryAssetPath(frontendUrl, character.asset);
      if (!actor) {
        const created = await call('创建模型', () => api.sceneTools.createActor(sceneId, source, 'model', {
          name: character.name, actor_guid: character.guid, semantic_role: character.role,
          entity_id: `story.${character.role}`, entity_type: 'story_character',
          // Every role that owns a placement version must be created with it. Omitting
          // the prophet's marker made every entry take the legacy repair path, and that
          // path never persisted the marker, so the world failed to load forever.
          ...(character.role === 'player' ? { model_ref: PLAYER_MODEL_REF }
            : character.role === 'prophet' ? { model_ref: PROPHET_MODEL_REF }
            : character.role === 'boss' ? { model_ref: BOSS_MODEL_REF } : {}),
          position: [character.x, 0, character.z], rotation: character.rotation, scale: [1, 1, 1],
          follow_camera: false, physics_enabled: false,
        }));
        actor = created.actor;
      } else if (!loaded(actor)) {
        actor = (await call('重新绑定模型', () => api.sceneTools.rebindActorResource(sceneId, character.guid, source))).actor;
      }
      if (!loaded(actor)) throw new Error(actor?.load_error?.message || '模型解码失败');
      // createActor acknowledges the handle before asynchronous import fills the AABB.
      // Wait for real local bounds instead of guessing a scale or creating a duplicate.
      for (let attempt = 0; !hasUsableBounds(actor.local_aabb); attempt++) {
        if (!isCurrent()) throw canceled();
        if (actor.render_failed || ['Failed', 'Invalid'].includes(actor.gpu_build_state)) {
          throw new Error(actor.load_error?.message || `模型导入失败（${actor.gpu_build_state || 'Failed'}）`);
        }
        if (attempt >= renderAttempts) {
          throw new Error(`等待模型包围盒超时（${actor.gpu_build_state || '未就绪'}），请重试进入世界`);
        }
        await wait(200);
        if (!isCurrent()) throw canceled();
        await assertSource();
        const refreshed = sceneSnapshot(await api.scene.getSnapshot(sceneId));
        if (!isCurrent()) throw canceled();
        actor = refreshed?.actors?.find(item => item.actor_guid === character.guid);
        if (!loaded(actor)) throw new Error(actor?.load_error?.message || '模型实例已失效');
      }
      check();
      // Do not use native ground_align/world_aabb: those bounds omit actor rotation.
      const placement = characterTransform(character, actor.local_aabb);
      // A canceled/failed first import can leave the actor at unit scale and y=0.
      // Only preserve its saved pose once the normalization step was committed, which
      // the normalized scale proves. The marker alone is not enough: an interrupted
      // first attempt owns the marker but still carries the raw placeholder pose.
      const initializedPlayer = wasPresent && character.role === 'player'
        && sameVector(actor.geometry?.scale, placement.scale);
      const legacyPlayer = character.role === 'player' && actor.model_ref !== PLAYER_MODEL_REF;
      // A marker mismatch alone only means "placement generation changed". A *route* that
      // names another file means the imported art itself is stale, and no repair can swap it.
      const stalePlayerModel = character.role === 'player' && wasPresent
        && !routeNamesShippedArt(actor, character.asset);
      const initializedProphet = wasPresent && character.role === 'prophet'
        && actor.model_ref === PROPHET_MODEL_REF && sameVector(actor.geometry?.scale, placement.scale);
      // The prophet is placed on demand, so a first attempt must be committed in one
      // load: waiting for the next entry left it standing at ground level meanwhile.
      const unplacedProphet = character.role === 'prophet'
        && !sameVector(actor.geometry?.scale, placement.scale);
      const legacyProphet = gameplay?.role === 'child' && character.role === 'prophet'
        && (actor.model_ref !== PROPHET_MODEL_REF || unplacedProphet);
      // The boss's pose is authored, not derived, so unlike the player it needs no saved
      // geometry: only its model generation is tracked. An actor without this generation's
      // marker is a dragon (or an older em001 build) that no repair can re-import.
      const initializedBoss = wasPresent && character.role === 'boss'
        && actor.model_ref === BOSS_MODEL_REF;
      const legacyBoss = character.role === 'boss' && actor.model_ref !== BOSS_MODEL_REF;
      let savedGeometry = initializedPlayer || initializedProphet ? actor.geometry : null;
      if (legacyPlayer && savedGeometry) {
        savedGeometry = { ...savedGeometry, rotation: [...savedGeometry.rotation] };
        savedGeometry.rotation[1] += PLAYER_MODEL_YAW_OFFSET;
      }
      const transform = characterTransform(character, actor.local_aabb, savedGeometry);
      if (stalePlayerModel) {
        // Replacement, not repair: the world was built around a different player model, and the
        // engine only imports a route when an actor is created. The guid is preserved, so the
        // controller and the save path keep addressing the same actor. x/z survive the rebuild
        // (the player is standing somewhere); rotation and y are re-derived from the table, so
        // the previous model's facing convention cannot leak into the new one.
        const saved = actor.geometry?.position;
        const position = Array.isArray(saved) ? { position: [saved[0], 0, saved[2]],
          rotation: [...character.rotation] } : null;
        const route = source || resolveStoryAssetPath(frontendUrl, character.asset);
        await removeActorForRebuild(api, sceneId, actor, `替换${character.name}模型`, call);
        check();
        const created = await call(`创建${character.name}`, () => api.sceneTools.createActor(sceneId, route, 'model', {
      ...(character.animation_sources
        ? { animation_sources: character.animation_sources.map(resolveStoryAssetPath) }
        : {}),
          name: character.name, actor_guid: character.guid, semantic_role: character.role,
          entity_id: `story.${character.role}`, entity_type: 'story_character',
          model_ref: PLAYER_MODEL_REF,
          position: [character.x, 0, character.z], rotation: [...character.rotation], scale: [1, 1, 1],
          follow_camera: false, physics_enabled: false,
        }));
        actor = created.actor;
        if (!loaded(actor)) throw new Error(actor?.load_error?.message || '模型解码失败');
        for (let attempt = 0; !hasUsableBounds(actor.local_aabb); attempt++) {
          if (!isCurrent()) throw canceled();
          if (attempt >= renderAttempts) throw new Error('等待模型包围盒超时，请重试进入世界');
          await wait(200);
          if (!isCurrent()) throw canceled();
          await assertSource();
          actor = sceneSnapshot(await api.scene.getSnapshot(sceneId))?.actors
            ?.find(item => item.actor_guid === character.guid);
          if (!loaded(actor)) throw new Error(actor?.load_error?.message || '模型实例已失效');
        }
        check();
        const rebuilt = characterTransform(character, actor.local_aabb, position);
        if (Object.entries(rebuilt).some(([key, value]) => !sameVector(actor.geometry?.[key], value))) {
          actor = (await call('设置模型位置', () => api.scene.setActorTransform(sceneId, character.guid, rebuilt))).actor;
        }
        if (actor.model_ref !== PLAYER_MODEL_REF) {
          throw new Error(`${character.name}模型未被引擎确认，请重试进入世界`);
        }
      } else if (legacyPlayer || legacyProphet) {
        const modelRef = legacyPlayer ? PLAYER_MODEL_REF : PROPHET_MODEL_REF;
        // Commit placement and its marker together, not while the new actor is still unscaled.
        // A lost reply/reentry must neither flip the player again nor preserve an ungrounded prophet.
        // Reuse the portable resource route; an existing world needs no source art.
        const route = actor.route || source || resolveStoryAssetPath(frontendUrl, character.asset);
        actor = (await call(`校正${character.name}摆位`, () => api.sceneTools.createActor(sceneId, route, 'model', {
      ...(character.animation_sources
        ? { animation_sources: character.animation_sources.map(resolveStoryAssetPath) }
        : {}),
          actor_guid: character.guid, skip_if_exists: true, update_if_exists: true,
          model_ref: modelRef, ...transform,
        }))).actor;
        // Name the actor and the field that disagreed: this used to report "玩家朝向" even when
        // the prophet was the one the engine failed to confirm, which sent debugging the wrong way.
        if (actor?.model_ref !== modelRef || !sameVector(actor.geometry?.rotation, transform.rotation)) {
          const unconfirmed = actor?.model_ref !== modelRef ? `摆位标记 ${modelRef}` : '朝向';
          throw new Error(`${character.name}摆位未被引擎确认（${unconfirmed}），请重试进入世界`);
        }
      } else if (legacyBoss) {
        // Replacement, not repair: this is the one path on which the engine imports a route.
        // The guid is stamped on the creation below, so the rebuilt actor keeps its identity
        // and no second boss can appear.
        const route = source || resolveStoryAssetPath(frontendUrl, character.asset);
        await removeActorForRebuild(api, sceneId, actor, `替换${character.name}模型`, call);
        check();
        const created = await call(`创建${character.name}`, () => api.sceneTools.createActor(sceneId, route, 'model', {
      ...(character.animation_sources
        ? { animation_sources: character.animation_sources.map(resolveStoryAssetPath) }
        : {}),
          name: character.name, actor_guid: character.guid, semantic_role: character.role,
          entity_id: `story.${character.role}`, entity_type: 'story_character',
          model_ref: BOSS_MODEL_REF,
          position: [character.x, 0, character.z], rotation: character.rotation, scale: [1, 1, 1],
          follow_camera: false, physics_enabled: false,
        }));
        actor = created.actor;
        if (actor?.model_ref !== BOSS_MODEL_REF || !sameVector(actor.geometry?.rotation, character.rotation)) {
          throw new Error(`${character.name}模型未被引擎确认，请重试进入世界`);
        }
      } else if (Object.entries(transform).some(([key, value]) => !sameVector(actor.geometry?.[key], value))) {
        actor = (await call('设置模型位置', () => api.scene.setActorTransform(sceneId, character.guid, transform))).actor;
      }
      if (actor.mechanics?.physics_enabled !== false) {
        actor = (await call('关闭物理驱动', () => api.sceneTools.setActorPhysics(sceneId, character.guid,
          { physics_enabled: false }))).actor;
      }
      if (actor.follow_camera || Boolean(!actor.visible) !== hideProtagonist) {
        actor = (await call(hideProtagonist ? '隐藏主角' : '设置模型可见性',
          () => api.sceneTools.setActorState(sceneId, character.guid,
            { visible: !hideProtagonist, follow_camera: false }))).actor;
      }
      if (actor.camera_lock?.enabled || actor.camera_lock?.lock_to_camera) {
        actor = (await call('关闭相机锁定', () => api.sceneTools.setActorCameraLock(sceneId, character.guid,
          { enabled: false }))).actor;
      }
      // Finish this model before submitting the next one. Ready GPU build state
      // alone is insufficient: require render slots, bounds, and native
      // triangle collision readiness (older bridges omit the latter field).
      for (let attempt = 0; ; attempt++) {
        if (!isCurrent()) throw canceled();
        await assertSource();
        snapshot = sceneSnapshot(await api.scene.getSnapshot(sceneId));
        if (!isCurrent()) throw canceled();
        actor = snapshot?.actors?.find(item => item.actor_guid === character.guid);
        if (!loaded(actor) || actor.render_failed || ['Failed', 'Invalid'].includes(actor.gpu_build_state)) {
          throw new Error(actor?.load_error?.message || '模型渲染失败');
        }
        if (actor.render_ready === true && actor.collision_ready !== false && hasUsableBounds(actor.local_aabb)) break;
        if (attempt >= renderAttempts - 1) throw new Error('等待模型渲染超时');
        await wait(200);
      }
      check();
      actors.set(character.guid, actor);
    } catch (error) {
      if (error.name === 'AbortError') throw error;
      throw new Error(`${character.name}加载失败（${source || actor?.route || character.asset}）：${error.message}`, { cause: error });
    }
  }
  if (combatOnly) return { snapshot };
  const player = actors.get(PLAYER_GUID);
  const bounds = rotatedBounds(player.local_aabb, player.geometry.rotation);
  const targetOffset = (bounds[1] + (bounds[4] - bounds[1]) * 0.75) * player.geometry.scale[1];
  return { snapshot, player, targetOffset };
}

/**
 * Reconcile the exhibits a small world advertises with the actors it actually has.
 *
 * The layout is authoritative: entries are created or moved into place, and any
 * placement actor the layout no longer lists is removed. Only this game's own
 * placement guids are ever touched, so authored scenery is never disturbed.
 */
export async function syncPlacementActors({ api, sceneId, frontendUrl, placements = [],
  assertSource = async () => {}, isCurrent = () => true,
  wait = ms => new Promise(resolve => setTimeout(resolve, ms)), renderAttempts = 150 }) {
  const check = () => { if (!isCurrent()) throw canceled(); };
  const call = async (operation, invoke) => {
    for (let attempt = 1; ; attempt++) {
      check(); await assertSource(); check();
      try {
        const result = success(await invoke(), operation);
        check();
        return result;
      } catch (error) {
        if (attempt >= TRANSIENT_SCENE_WRITE_ATTEMPTS || !TRANSIENT_SCENE_WRITE.test(error?.message || '')) throw error;
        await wait(250 * attempt);
      }
    }
  };
  const desired = placementScene(placements);
  check();
  await assertSource();
  let snapshot = sceneSnapshot(await api.scene.getSnapshot(sceneId));
  check();
  let actors = new Map((snapshot?.actors || []).map(actor => [actor.actor_guid, actor]));

  // Retire exhibits the layout dropped before creating new ones, so a removal always
  // takes effect even if the rest of the reconciliation fails.
  const wanted = new Set(desired.map(entry => entry.guid));
  for (const guid of [...actors.keys()]) {
    if (!guid.startsWith(PLACEMENT_PREFIX) || wanted.has(guid)) continue;
    await call('移除陈列物', () => api.sceneTools.removeActor(sceneId, guid));
    actors.delete(guid);
  }

  for (const entry of desired) {
    check();
    const { prop, guid } = entry;
    const source = resolveStoryAssetPath(frontendUrl, prop.asset);
    let actor = actors.get(guid);
    // A recorded placement can name a different model than the layout now says. The
    // retired pillar is the concrete case: its old actor is a cube, but the same slot is
    // now a ball. A `skip_if_exists` repair never rewrites `route`, so the stale actor has
    // to be removed before the correct art can be imported.
    if (actor) {
      const entityId = String(actor.entity_id || actor.runtime?.entity_id || '');
      const expectedId = `story.placement.${prop.id}`;
      if ((entityId && entityId !== expectedId) || !routeNamesShippedArt(actor, prop.asset)) {
        await call('替换陈列物模型', () => api.sceneTools.removeActor(sceneId, guid));
        actors.delete(guid);
        actor = null;
      }
    }
    const wasPresent = Boolean(actor);
    try {
      if (!actor) {
        actor = (await call('创建陈列物', () => api.sceneTools.createActor(sceneId, source, 'model', {
          name: `${prop.name}${entry.index + 1}`, actor_guid: guid,
          semantic_role: 'placement', entity_id: `story.placement.${prop.id}`,
          entity_type: 'story_prop',
          position: [...entry.position], rotation: [...entry.rotation],
          scale: [entry.scale, entry.scale, entry.scale],
          follow_camera: false, physics_enabled: false,
        }))).actor;
      } else if (!loaded(actor)) {
        actor = (await call('重新绑定陈列物', () => api.sceneTools.rebindActorResource(sceneId, guid, source))).actor;
      }
      if (!loaded(actor)) throw new Error(actor?.load_error?.message || '模型解码失败');
      // createActor acknowledges the handle before the import fills the AABB, and a
      // ground offset needs real bounds; guessing one would sink the exhibit.
      for (let attempt = 0; !hasUsableBounds(actor.local_aabb); attempt++) {
        if (!isCurrent()) throw canceled();
        if (actor.render_failed || ['Failed', 'Invalid'].includes(actor.gpu_build_state)) {
          throw new Error(actor.load_error?.message || `模型导入失败（${actor.gpu_build_state || 'Failed'}）`);
        }
        if (attempt >= renderAttempts) throw new Error('等待陈列物包围盒超时，请重试进入世界');
        await wait(200);
        if (!isCurrent()) throw canceled();
        await assertSource();
        actor = sceneSnapshot(await api.scene.getSnapshot(sceneId))?.actors
          ?.find(item => item.actor_guid === guid);
        if (!loaded(actor)) throw new Error(actor?.load_error?.message || '陈列物实例已失效');
      }
      check();
      // The stored y is the ground line: the prop stands on it, not centred on it.
      const normalized = characterTransform({ rotation: [...entry.rotation], height: prop.height },
        actor.local_aabb).scale[0];
      const scale = wasPresent && sameVector(actor.geometry?.scale,
        [normalized, normalized, normalized]) ? [...actor.geometry.scale]
        : [entry.scale, entry.scale, entry.scale];
      const bounds = rotatedBounds(actor.local_aabb, entry.rotation);
      const transform = { position: [entry.position[0], entry.position[1] - bounds[1] * scale[1],
        entry.position[2]], rotation: [...entry.rotation], scale };
      if (Object.entries(transform).some(([key, value]) => !sameVector(actor.geometry?.[key], value))) {
        actor = (await call('摆放陈列物', () => api.scene.setActorTransform(sceneId, guid, transform))).actor;
      }
      if (actor.mechanics?.physics_enabled !== false) {
        actor = (await call('关闭陈列物物理', () => api.sceneTools.setActorPhysics(sceneId, guid,
          { physics_enabled: false }))).actor;
      }
      if (actor.follow_camera || !actor.visible) {
        actor = (await call('设置陈列物可见性', () => api.sceneTools.setActorState(sceneId, guid,
          { visible: true, follow_camera: false }))).actor;
      }
      actors.set(guid, actor);
    } catch (error) {
      if (error.name === 'AbortError') throw error;
      throw new Error(`${prop.name}陈列失败（${source}）：${error.message}`, { cause: error });
    }
  }
  return { snapshot, placements: desired };
}
