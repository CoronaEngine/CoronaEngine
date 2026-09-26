import { STORY_CHARACTERS, PLAYER_GUID, unwrap, sceneSnapshot, resolveStoryAssetPath,
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
  wait = ms => new Promise(resolve => setTimeout(resolve, ms)), renderAttempts = 120 }) {
  const check = () => { if (!isCurrent()) throw canceled(); };
  const call = async (operation, invoke) => { check(); const result = success(await invoke(), operation); check(); return result; };
  check();
  let snapshot = sceneSnapshot(await api.scene.getSnapshot(sceneId));
  check();
  const actors = new Map((snapshot?.actors || []).map(actor => [actor.actor_guid, actor]));
  for (const character of STORY_CHARACTERS) {
    check();
    let actor = actors.get(character.guid);
    const wasPresent = Boolean(actor);
    let source;
    try {
      // An intact portable project must also open after game/art is moved.
      if (!actor || !loaded(actor)) source = resolveStoryAssetPath(frontendUrl, character.asset);
      if (!actor) {
        const created = await call('创建模型', () => api.sceneTools.createActor(sceneId, source, 'model', {
          name: character.name, actor_guid: character.guid, semantic_role: character.role,
          entity_id: `story.${character.role}`, entity_type: 'story_character',
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
        check();
        if (actor.render_failed || ['Failed', 'Invalid'].includes(actor.gpu_build_state)) {
          throw new Error(actor.load_error?.message || `模型导入失败（${actor.gpu_build_state || 'Failed'}）`);
        }
        if (attempt >= renderAttempts) {
          throw new Error(`等待模型包围盒超时（${actor.gpu_build_state || '未就绪'}），请重试进入世界`);
        }
        await wait(100);
        check();
        const refreshed = sceneSnapshot(await api.scene.getSnapshot(sceneId));
        check();
        actor = refreshed?.actors?.find(item => item.actor_guid === character.guid);
        if (!loaded(actor)) throw new Error(actor?.load_error?.message || '模型实例已失效');
      }
      // Do not use native ground_align/world_aabb: those bounds omit actor rotation.
      const placement = characterTransform(character, actor.local_aabb);
      // A canceled/failed first import can leave the player at unit scale and y=0.
      // Only preserve its saved pose once the normalization step was committed.
      const initializedPlayer = wasPresent && character.role === 'player'
        && sameVector(actor.geometry?.scale, placement.scale);
      const transform = characterTransform(character, actor.local_aabb, initializedPlayer ? actor.geometry : null);
      if (Object.entries(transform).some(([key, value]) => !sameVector(actor.geometry?.[key], value))) {
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
      actors.set(character.guid, actor);
    } catch (error) {
      if (error.name === 'AbortError') throw error;
      throw new Error(`${character.name}加载失败（${source || actor?.route || character.asset}）：${error.message}`, { cause: error });
    }
  }
  // CPU import success does not guarantee a renderable GPU resource.
  for (let attempt = 0; attempt < renderAttempts; attempt++) {
    check();
    snapshot = sceneSnapshot(await api.scene.getSnapshot(sceneId));
    check();
    const rendered = new Map((snapshot?.actors || []).map(actor => [actor.actor_guid, actor]));
    const pending = [];
    for (const character of STORY_CHARACTERS) {
      const actor = rendered.get(character.guid);
      if (!loaded(actor) || actor.render_failed || ['Failed', 'Invalid'].includes(actor.gpu_build_state)) {
        throw new Error(`${character.name}渲染失败（${actor?.route || character.asset}）：${actor?.load_error?.message || '模型未就绪'}`);
      }
      if (actor.render_ready !== true || !hasUsableBounds(actor.local_aabb)) {
        pending.push(`${character.name}（${actor.route || character.asset}）`);
      }
    }
    if (!pending.length) {
      const player = rendered.get(PLAYER_GUID);
      const bounds = rotatedBounds(player.local_aabb, player.geometry.rotation);
      const targetOffset = (bounds[1] + (bounds[4] - bounds[1]) * 0.75) * player.geometry.scale[1];
      return { snapshot, player, targetOffset };
    }
    if (attempt === renderAttempts - 1) throw new Error(`等待模型渲染超时：${pending.join('、')}，请重试进入世界`);
    await wait(100);
  }
  throw new Error('无法确认模型渲染状态');
}
