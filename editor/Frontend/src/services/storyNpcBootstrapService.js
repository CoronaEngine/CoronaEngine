import { editorApi } from '@/api/editorApi.js';
import { STORY_NPC_DEFINITIONS, STORY_MERCHANT_CANDIDATES, STORY_NPC_ASSET_FALLBACK, STORY_MERCHANT_STOCK, createStoryNpcActorData, createStoryWorldBallActorData } from '@/config/storyNpc.js';
import { storyWorldTerrainHeight } from '@/config/storyWorld.js';
import { resolveSceneSnapshot } from '@/utils/nativeSceneViewport.js';
import { merchantRollForDay } from '@/utils/storyNpc.js';

function resultData(value) { return value?.data?.data || value?.data || value || {}; }
function assetRoot() {
  const location = String(window.location?.href || '').split('#')[0].split('?')[0].replace(/^file:\/\//i, '');
  const normalized = location.replace(/\\/g, '/').replace(/^\//, '');
  const marker = normalized.toLowerCase().lastIndexOf('/frontend/');
  return marker >= 0 ? `${normalized.slice(0, marker)}/assets/story_mode` : '';
}
function actorMatch(actors, name) { return actors.find((actor) => String(actor?.name || actor?.actor_name || '').toLowerCase() === name.toLowerCase()); }
function withGround(position) { return [position[0], storyWorldTerrainHeight(position[0], position[2]) + 0.05, position[2]]; }

export async function bootstrapStoryNpcs({ sceneId, dayNumber = 1, progressStore, forceMerchantRoll = false } = {}) {
  const scene = String(sceneId || '').trim();
  if (!scene) return { created: [], warnings: ['剧情场景不可用。'], merchant: false };
  const warnings = []; const created = [];
  const root = assetRoot();
  let snapshot = resolveSceneSnapshot(await editorApi.scene.getSnapshot(scene));
  let actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
  const create = async (definition, position = definition.position) => {
    if (actorMatch(actors, definition.name)) return false;
    const path = root ? `${root}/${definition.asset}` : definition.asset;
    try {
      await editorApi.sceneTools.createActor(scene, path, 'model', createStoryNpcActorData({ ...definition, position: withGround(position) }));
      created.push(definition.name); actors.push({ ...createStoryNpcActorData({ ...definition, position: withGround(position) }), handle: 0 }); return true;
    } catch (error) {
      try {
        await editorApi.sceneTools.createActor(scene, root ? `${root}/${STORY_NPC_ASSET_FALLBACK}` : STORY_NPC_ASSET_FALLBACK, 'model', createStoryNpcActorData({ ...definition, position: withGround(position) }));
        warnings.push(`${definition.displayName} 使用了临时模型。`); created.push(definition.name); actors.push({ ...createStoryNpcActorData({ ...definition, position: withGround(position) }), handle: 0 }); return true;
      } catch (fallbackError) { warnings.push(`${definition.displayName} 创建失败：${fallbackError?.message || error?.message || '未知错误'}`); return false; }
    }
  };
  for (const definition of STORY_NPC_DEFINITIONS) await create(definition);
  const day = Math.max(1, Math.trunc(Number(dayNumber) || 1));
  let merchant = progressStore?.merchantForDay?.(day) || false;
  if (day >= 2 && (forceMerchantRoll || !Object.prototype.hasOwnProperty.call(progressStore?.data?.merchantByDay || {}, String(day)))) {
    merchant = merchantRollForDay(day); progressStore?.setMerchantForDay?.(day, merchant);
  }
  const merchantDefinition = { id: 'merchant', name: 'StoryNpc_Merchant', displayName: '行脚商人', semanticRole: 'story_npc_merchant', asset: 'npc_merchant_v1.obj', position: STORY_MERCHANT_CANDIDATES[(day - 2) % STORY_MERCHANT_CANDIDATES.length] };
  const existingMerchant = actorMatch(actors, merchantDefinition.name);
  if (merchant) {
    if (existingMerchant) {
      try {
        await editorApi.scene.setActorTransform(scene, merchantDefinition.name, {
          position: withGround(merchantDefinition.position),
          rotation: [0, 0, 0],
          scale: [1, 1, 1],
        });
      } catch (_) { /* merchant movement is best effort */ }
      try { await editorApi.sceneTools.setActorState(scene, merchantDefinition.name, { visible: true }); } catch (_) { /* optional */ }
    } else {
      await create(merchantDefinition, merchantDefinition.position);
    }
  } else if (existingMerchant) {
    try { await editorApi.sceneTools.setActorState(scene, merchantDefinition.name, { visible: false }); } catch (_) { /* optional */ }
  }
  return { created, warnings, merchant, stock: merchant ? STORY_MERCHANT_STOCK : [], refreshedAt: Date.now() };
}


export async function ensureStoryWorldBall({ sceneId, worldBallId = 'demo-1' } = {}) {
  const scene = String(sceneId || '').trim();
  if (!scene) return { created: false, error: '剧情场景不可用。' };
  const data = createStoryWorldBallActorData(worldBallId);
  let snapshot = resolveSceneSnapshot(await editorApi.scene.getSnapshot(scene));
  const actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
  if (actorMatch(actors, data.name)) return { created: false, name: data.name };
  const root = assetRoot();
  try {
    await editorApi.sceneTools.createActor(scene, root ? `${root}/world_ball_v1.obj` : 'world_ball_v1.obj', 'model', data);
    return { created: true, name: data.name };
  } catch (error) {
    return { created: false, name: data.name, error: error?.message || '世界小球创建失败。' };
  }
}
