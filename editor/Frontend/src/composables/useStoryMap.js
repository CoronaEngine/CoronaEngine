import { onMounted, onUnmounted, ref, unref, watch } from 'vue';

import { editorApi } from '@/api/editorApi.js';
import { buildStoryMapSnapshot } from '@/utils/storyMap.js';
import { resolveSceneSnapshot } from '@/utils/nativeSceneViewport.js';

const MAP_REFRESH_DEBOUNCE_MS = 180;

export function useStoryMap(sceneId, playerState) {
  const loading = ref(false);
  const errorMessage = ref('');
  const sceneName = ref('');
  const markers = ref([]);
  const sceneBounds = ref(null);
  const boundsReady = ref(false);

  let disposed = false;
  let requestRevision = 0;
  let refreshTimer = null;
  const callbackTokens = new Set();

  const currentPlayerPosition = () => unref(playerState)?.position ?? null;

  const refresh = async () => {
    const activeSceneId = String(unref(sceneId) || '').trim();
    if (!activeSceneId || disposed) return false;
    const revision = ++requestRevision;
    loading.value = markers.value.length === 0;
    errorMessage.value = '';
    try {
      const response = await editorApi.scene.getSnapshot(activeSceneId);
      if (disposed || revision !== requestRevision) return false;
      const snapshot = resolveSceneSnapshot(response);
      const next = buildStoryMapSnapshot(snapshot, currentPlayerPosition());
      sceneName.value = next.sceneName || activeSceneId;
      markers.value = next.markers;
      sceneBounds.value = next.bounds;
      boundsReady.value = next.boundsReady;
      if (!next.bounds) errorMessage.value = '当前场景还没有可用于地图的边界数据。';
      return true;
    } catch (error) {
      if (disposed || revision !== requestRevision) return false;
      console.warn('[StoryMode] failed to refresh story map', error);
      errorMessage.value = '地图数据暂时不可用。';
      return false;
    } finally {
      if (!disposed && revision === requestRevision) loading.value = false;
    }
  };

  const scheduleRefresh = () => {
    if (disposed || refreshTimer !== null) return;
    refreshTimer = window.setTimeout(() => {
      refreshTimer = null;
      void refresh();
    }, MAP_REFRESH_DEBOUNCE_MS);
  };

  const registerCallback = async (factory) => {
    try {
      const token = await factory(scheduleRefresh);
      if (!token) return;
      if (disposed) {
        await editorApi.off(token).catch(() => {});
      } else {
        callbackTokens.add(token);
      }
    } catch (error) {
      console.warn('[StoryMode] failed to subscribe to map scene events', error);
    }
  };

  watch(
    () => String(unref(sceneId) || ''),
    (nextSceneId) => {
      requestRevision += 1;
      if (!nextSceneId) {
        sceneName.value = '';
        markers.value = [];
        sceneBounds.value = null;
        boundsReady.value = false;
        loading.value = false;
        return;
      }
      void refresh();
    },
    { immediate: true }
  );

  onMounted(() => {
    disposed = false;
    void registerCallback((callback) => editorApi.events.onActorChanged(callback));
    void registerCallback((callback) => editorApi.events.onActorTransformUpdated(callback));
    void registerCallback((callback) => editorApi.events.onSceneTreeChanged(callback));
  });

  onUnmounted(() => {
    disposed = true;
    requestRevision += 1;
    if (refreshTimer !== null) {
      window.clearTimeout(refreshTimer);
      refreshTimer = null;
    }
    for (const token of callbackTokens) editorApi.off(token).catch(() => {});
    callbackTokens.clear();
  });

  return {
    loading,
    errorMessage,
    sceneName,
    markers,
    sceneBounds,
    boundsReady,
    refresh,
    scheduleRefresh,
  };
}
