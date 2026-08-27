import { computed, onUnmounted, ref, unref, watch } from 'vue';
import { editorApi } from '@/api/editorApi.js';
import { STORY_NPC_INTERACTION_RANGE } from '@/config/storyNpc.js';
import { resolveSceneSnapshot } from '@/utils/nativeSceneViewport.js';
import { distanceBetweenStoryPoints } from '@/utils/storyNpc.js';

export function useStoryNpcInteraction({ sceneId, playerState, enabled, onInteract } = {}) {
  const actors = ref([]); const nearbyTarget = ref(null); const refreshError = ref('');
  let timer = null; let disposed = false;
  const refresh = async () => {
    if (disposed || !unref(enabled) || !unref(sceneId)) return;
    try {
      const snapshot = resolveSceneSnapshot(await editorApi.scene.getSnapshot(unref(sceneId)));
      actors.value = Array.isArray(snapshot.actors) ? snapshot.actors : [];
      refreshError.value = '';
    } catch (error) { refreshError.value = error?.message || '无法读取交互对象'; }
  };
  const target = computed(() => {
    const position = unref(playerState)?.position;
    let best = null; let distance = Infinity;
    for (const actor of actors.value) {
      const role = String(actor?.semantic_role || actor?.semanticRole || '').toLowerCase();
      if (!role.startsWith('story_npc_') && role !== 'story_world_ball' && role !== 'story_world_core') continue;
      const actorPosition = actor?.geometry?.position || actor?.position;
      const nextDistance = distanceBetweenStoryPoints(position, actorPosition);
      if (nextDistance <= STORY_NPC_INTERACTION_RANGE && nextDistance < distance) { best = { ...actor, interactionRole: role, distance: nextDistance }; distance = nextDistance; }
    }
    return best;
  });
  watch(target, (value) => { nearbyTarget.value = value; }, { immediate: true });
  watch([() => String(unref(sceneId) || ''), () => Boolean(unref(enabled))], () => { void refresh(); }, { immediate: true });
  timer = window.setInterval(refresh, 2200);
  const interact = () => { if (!nearbyTarget.value) return false; onInteract?.(nearbyTarget.value); return true; };
  onUnmounted(() => { disposed = true; if (timer !== null) window.clearInterval(timer); });
  return { actors, nearbyTarget, refreshError, refresh, interact };
}
