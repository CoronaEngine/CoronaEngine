import { FRAGMENT, FRAGMENT_GUID } from './storyGameplay.mjs';
import { PLACEMENT_PREFIX, placementScene } from './storyProps.mjs';
import { STORY_CHARACTERS, PLAYER_GUID, PLAYER_MODEL_REF, PROPHET_MODEL_REF, PLAYER_MODEL_YAW_OFFSET, unwrap, sceneSnapshot, resolveStoryAssetPath,
  characterTransform, rotatedBounds, hasUsableBounds } from './storyCharacters.mjs';

const canceled = () => Object.assign(new Error('剧情世界初始化已取消'), { name: 'AbortError' });
const success = (response, operation) => {
  const data = unwrap(response);
  if (!data || data.ok === false || !['success', 'loaded'].includes(data.status)) {
    throw new Error(`${operation}：${data?.message || data?.diagnostics?.[0]?.message || '引擎未确认成功'}`);
  }
  return data;
};
const loaded = actor => actor?.load_status === 'loaded' && Number(actor?.handle) > 0;
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

async function initializeStoryCharacters({ api, sceneId, frontendUrl, isCurrent = () => true,
  wait = ms => new Promise(resolve => setTimeout(resolve, ms)), renderAttempts = 150,
  gameplay = null, combatOnly = false, assertSource = async () => {} }) {
  const check = () => { if (!isCurrent()) throw canceled(); };
  const call = async (operation, invoke) => { check(); await assertSource(); check();
    const result = success(await invoke(), operation); check(); return result; };
  check();
  await assertSource();
  let snapshot = sceneSnapshot(await api.scene.getSnapshot(sceneId));
  check();
  const actors = new Map((snapshot?.actors || []).map(actor => [actor.actor_guid, actor]));
  const visibleCharacters = STORY_CHARACTERS.filter(character => !gameplay || character.role === 'player'
    || (gameplay.role === 'main' && character.role === 'boss' && gameplay.state.boss.hp > 0)
    || (gameplay.role === 'child' && character.role === 'prophet'));
  if (gameplay?.role === 'main' && gameplay.state.drop && !gameplay.state.drop.collected) {
    visibleCharacters.push({ ...FRAGMENT, x: gameplay.state.drop.position[0], z: gameplay.state.drop.position[2] });
  }
  if (gameplay) {
    const visibleGuids = new Set(visibleCharacters.map(character => character.guid));
    // Only manage game-owned actors; scenery and user-authored models stay untouched.
    for (const guid of [...STORY_CHARACTERS.map(character => character.guid), FRAGMENT_GUID]) {
      const actor = actors.get(guid);
      if (actor && actor.visible !== false && !visibleGuids.has(guid)) {
        await call('隐藏当前世界之外的模型', () => api.sceneTools.setActorState(sceneId, actor.actor_guid, { visible: false }));
      }
    }
  }
  // Never use the update subset to decide which actors should be visible.
  const characters = visibleCharacters.filter(character => !combatOnly || ['boss', 'fragment'].includes(character.role));
  for (let character of characters) {
    check();
    let actor = actors.get(character.guid);
    const wasPresent = Boolean(actor);
    if (gameplay?.role === 'child' && character.role === 'prophet' && actor?.model_ref !== PROPHET_MODEL_REF) {
      const player = actors.get(PLAYER_GUID);
      if (!player?.geometry) throw new Error('先知初始化前玩家位置未就绪');
      const facing = player.geometry.rotation[1] - PLAYER_MODEL_YAW_OFFSET;
      character = { ...character, x: player.geometry.position[0] + Math.sin(facing) * 4,
        z: player.geometry.position[2] + Math.cos(facing) * 4, rotation: [0, facing + Math.PI, 0] };
    }
    let source;
    try {
      // An intact portable project must also open after game/art is moved.
      if (!actor || !loaded(actor)) source = resolveStoryAssetPath(frontendUrl, character.asset);
      if (!actor) {
        const created = await call('创建模型', () => api.sceneTools.createActor(sceneId, source, 'model', {
          name: character.name, actor_guid: character.guid, semantic_role: character.role,
          entity_id: `story.${character.role}`, entity_type: 'story_character',
          ...(character.role === 'player' ? { model_ref: PLAYER_MODEL_REF } : {}),
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
      // A canceled/failed first import can leave the player at unit scale and y=0.
      // Only preserve its saved pose once the normalization step was committed.
      const initializedPlayer = wasPresent && character.role === 'player'
        && sameVector(actor.geometry?.scale, placement.scale);
      const legacyPlayer = character.role === 'player' && actor.model_ref !== PLAYER_MODEL_REF;
      const initializedProphet = wasPresent && character.role === 'prophet' && actor.model_ref === PROPHET_MODEL_REF;
      const legacyProphet = gameplay?.role === 'child' && character.role === 'prophet' && actor.model_ref !== PROPHET_MODEL_REF;
      let savedGeometry = initializedPlayer || initializedProphet ? actor.geometry : null;
      if (legacyPlayer && savedGeometry) {
        savedGeometry = { ...savedGeometry, rotation: [...savedGeometry.rotation] };
        savedGeometry.rotation[1] += PLAYER_MODEL_YAW_OFFSET;
      }
      const transform = characterTransform(character, actor.local_aabb, savedGeometry);
      if (legacyPlayer || legacyProphet) {
        const modelRef = legacyPlayer ? PLAYER_MODEL_REF : PROPHET_MODEL_REF;
        // Commit placement and its marker together, not while the new actor is still unscaled.
        // A lost reply/reentry must neither flip the player again nor preserve an ungrounded prophet.
        // Reuse the portable resource route; an existing world needs no source art.
        const route = actor.route || source || resolveStoryAssetPath(frontendUrl, character.asset);
        actor = (await call('校正玩家朝向', () => api.sceneTools.createActor(sceneId, route, 'model', {
          actor_guid: character.guid, skip_if_exists: true, update_if_exists: true,
          model_ref: modelRef, ...transform,
        }))).actor;
        if (actor?.model_ref !== modelRef || !sameVector(actor.geometry?.rotation, transform.rotation)) {
          throw new Error('引擎未确认玩家朝向版本，请重试进入世界');
        }
      } else if (Object.entries(transform).some(([key, value]) => !sameVector(actor.geometry?.[key], value))) {
        actor = (await call('设置模型位置', () => api.scene.setActorTransform(sceneId, character.guid, transform))).actor;
      }
      if (actor.mechanics?.physics_enabled !== false) {
        actor = (await call('关闭物理驱动', () => api.sceneTools.setActorPhysics(sceneId, character.guid,
          { physics_enabled: false }))).actor;
      }
      if (actor.follow_camera || !actor.visible) {
        actor = (await call('设置模型可见性', () => api.sceneTools.setActorState(sceneId, character.guid,
          { visible: true, follow_camera: false }))).actor;
      }
      if (actor.camera_lock?.enabled || actor.camera_lock?.lock_to_camera) {
        actor = (await call('关闭相机锁定', () => api.sceneTools.setActorCameraLock(sceneId, character.guid,
          { enabled: false }))).actor;
      }
      // Finish this model before submitting the next one. Ready GPU build state
      // alone is insufficient: require actual render slots AND valid bounds.
      for (let attempt = 0; ; attempt++) {
        if (!isCurrent()) throw canceled();
        await assertSource();
        snapshot = sceneSnapshot(await api.scene.getSnapshot(sceneId));
        if (!isCurrent()) throw canceled();
        actor = snapshot?.actors?.find(item => item.actor_guid === character.guid);
        if (!loaded(actor) || actor.render_failed || ['Failed', 'Invalid'].includes(actor.gpu_build_state)) {
          throw new Error(actor?.load_error?.message || '模型渲染失败');
        }
        if (actor.render_ready === true && hasUsableBounds(actor.local_aabb)) break;
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
  const call = async (operation, invoke) => { check(); await assertSource(); check();
    const result = success(await invoke(), operation); check(); return result; };
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
