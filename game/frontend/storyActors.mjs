import { createLoadOperation, LOAD_POLL_MS } from './worldLoading.mjs';
import { FRAGMENT, FRAGMENT_GUID } from './storyGameplay.mjs';
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

/** All native mutations are awaited; the host registers this work before world replacement. */
export async function ensureStoryCharacters({ api, sceneId, frontendUrl, isCurrent = () => true,
  wait = ms => new Promise(resolve => setTimeout(resolve, ms)), renderAttempts = Infinity,
  gameplay = null, combatOnly = false, assertSource = async () => {}, trackWork = promise => promise, operation = createLoadOperation({ isCurrent, trackWork }),
}) {
  const check = () => { if (!isCurrent()) throw canceled(); operation.check(); };
  const call = async (operation, invoke) => { check(); await assertSource(); check();
    const result = success(await invoke(), operation); check(); return result; };
  check();
  let snapshot = await operation.phase('读取剧情角色', async () => {
    await assertSource(); check();
    return sceneSnapshot(await api.scene.getSnapshot(sceneId));
  });
  check();
  const actors = new Map((snapshot?.actors || []).map(actor => [actor.actor_guid, actor]));
  const visibleCharacters = STORY_CHARACTERS.filter(character => !gameplay || character.role === 'player'
    || (gameplay.role === 'main' && character.role === 'boss' && gameplay.state.boss.hp > 0)
    || (gameplay.role === 'child' && character.role === 'prophet'));
  if (gameplay?.role === 'main' && gameplay.state.drop && !gameplay.state.drop.collected) {
    visibleCharacters.push({ ...FRAGMENT, x: gameplay.state.drop.position[0], z: gameplay.state.drop.position[2] });
  }
  if (gameplay) await operation.phase('更新角色可见性', async () => {
    const visibleGuids = new Set(visibleCharacters.map(character => character.guid));
    for (const actor of actors.values()) {
      // Cameras/lights are separate snapshot collections. Do not hide audio/UI actors.
      const model = !actor.actor_type || ['model', 'actor'].includes(actor.actor_type);
      if (model && actor.visible !== false && !visibleGuids.has(actor.actor_guid)) {
        await call('隐藏当前世界之外的模型', () => api.sceneTools.setActorState(sceneId, actor.actor_guid, { visible: false }));
      }
    }
  });
  // Never use the update subset to decide which actors should be visible.
  const characters = visibleCharacters.filter(character => !combatOnly || ['boss', 'fragment'].includes(character.role));
  for (let character of characters) {
    check();
    await operation.phase(`初始化${character.name}`, async () => {
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
          ...(character.role === 'player' ? { model_ref: PLAYER_MODEL_REF }
            : gameplay?.role === 'child' && character.role === 'prophet' ? { model_ref: PROPHET_MODEL_REF } : {}),
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
        await wait(LOAD_POLL_MS);
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
        // Persist the art correction and its version together. Never rotate first
        // and mark later: a lost reply/reentry would otherwise flip the player again.
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
        await wait(LOAD_POLL_MS);
      }
      check();
      actors.set(character.guid, actor);
    } catch (error) {
      if (error.name === 'AbortError') throw error;
      throw new Error(`${character.name}加载失败（${source || actor?.route || character.asset}）：${error.message}`, { cause: error });
    }
    }, { model: character.name, guid: character.guid });
  }
  if (combatOnly) return { snapshot };
  const player = actors.get(PLAYER_GUID);
  const bounds = rotatedBounds(player.local_aabb, player.geometry.rotation);
  const targetOffset = (bounds[1] + (bounds[4] - bounds[1]) * 0.75) * player.geometry.scale[1];
  return { snapshot, player, targetOffset };
}
