import { computed, onUnmounted, ref, unref, watch } from 'vue';

import { runStoryWorldBootstrap } from '@/services/storyWorldBootstrapService.js';

export function useStoryWorldBootstrap({
  sceneId,
  viewportStatus,
  setCameraPose,
  onComplete,
} = {}) {
  const status = ref('idle');
  const progress = ref(0);
  const phaseMessage = ref('等待世界画面');
  const errorMessage = ref('');
  const warningMessages = ref([]);
  const generated = ref(false);
  const skipped = ref(false);
  const managedWorld = ref(false);
  const repairedCount = ref(0);
  const upgradedCount = ref(0);
  const migrationWarnings = ref([]);
  const validation = ref(null);
  const worldBounds = ref(null);

  let disposed = false;
  let revision = 0;
  let activePromise = null;
  let completedSceneId = '';

  const isReady = computed(() => status.value === 'complete');

  const applyProgress = (next) => {
    if (disposed || !next) return;
    status.value = String(next.status || status.value || 'checking');
    progress.value = Math.max(0, Math.min(100, Number(next.progress) || 0));
    phaseMessage.value = String(next.message || phaseMessage.value || '正在构建剧情世界');
  };

  const run = async ({ force = false } = {}) => {
    const activeSceneId = String(unref(sceneId) || '').trim();
    if (!activeSceneId || String(unref(viewportStatus)) !== 'ready' || disposed) return false;
    if (!force && completedSceneId === activeSceneId && status.value === 'complete') return true;
    if (activePromise) return activePromise;

    const runRevision = ++revision;
    errorMessage.value = '';
    warningMessages.value = [];
    generated.value = false;
    skipped.value = false;
    managedWorld.value = false;
    repairedCount.value = 0;
    upgradedCount.value = 0;
    migrationWarnings.value = [];
    validation.value = null;
    worldBounds.value = null;
    applyProgress({ status: 'checking', progress: 3, message: '检查剧情世界' });

    const pending = runStoryWorldBootstrap({
      sceneId: activeSceneId,
      setCameraPose,
      onProgress: (next) => {
        if (!disposed && runRevision === revision) applyProgress(next);
      },
      isCancelled: () => disposed || runRevision !== revision,
    })
      .then(async (result) => {
        if (!result || disposed || runRevision !== revision) return false;
        generated.value = Boolean(result.generated);
        skipped.value = Boolean(result.skipped);
        managedWorld.value = Boolean(result.managedWorld);
        warningMessages.value = Array.isArray(result.warnings) ? result.warnings : [];
        repairedCount.value = Math.max(0, Number(result.repairedCount) || 0);
        upgradedCount.value = Math.max(0, Number(result.upgradedCount) || 0);
        migrationWarnings.value = Array.isArray(result.migrationWarnings)
          ? result.migrationWarnings
          : [];
        validation.value = result.validation || null;
        worldBounds.value = Array.isArray(result.worldBounds) ? [...result.worldBounds] : null;
        if (typeof onComplete === 'function') await onComplete(result);
        completedSceneId = activeSceneId;
        applyProgress({
          status: 'complete',
          progress: 100,
          message: result.skipReason === 'existing-world' ? '保留当前世界' : '世界已就绪',
        });
        return true;
      })
      .catch((error) => {
        if (disposed || runRevision !== revision) return false;
        console.error('[StoryMode] Story World bootstrap failed', error);
        status.value = 'error';
        progress.value = 0;
        phaseMessage.value = '世界构建失败';
        errorMessage.value = String(error?.message || '剧情世界初始化失败，请重试。');
        return false;
      })
      .finally(() => {
        if (activePromise === pending) activePromise = null;
      });
    activePromise = pending;
    return pending;
  };

  const retry = () => {
    completedSceneId = '';
    revision += 1;
    activePromise = null;
    return run({ force: true });
  };

  watch(
    [() => String(unref(sceneId) || ''), () => String(unref(viewportStatus) || '')],
    ([nextSceneId, nextViewportStatus], [previousSceneId] = []) => {
      if (nextSceneId !== previousSceneId) {
        revision += 1;
        activePromise = null;
        completedSceneId = '';
        status.value = 'idle';
        progress.value = 0;
        phaseMessage.value = '等待世界画面';
        errorMessage.value = '';
        warningMessages.value = [];
        generated.value = false;
        skipped.value = false;
        managedWorld.value = false;
        repairedCount.value = 0;
        upgradedCount.value = 0;
        migrationWarnings.value = [];
        validation.value = null;
        worldBounds.value = null;
      }
      if (nextSceneId && nextViewportStatus === 'ready') void run();
    },
    { immediate: true }
  );

  onUnmounted(() => {
    disposed = true;
    revision += 1;
    activePromise = null;
  });

  return {
    status,
    progress,
    phaseMessage,
    errorMessage,
    warningMessages,
    generated,
    skipped,
    managedWorld,
    repairedCount,
    upgradedCount,
    migrationWarnings,
    validation,
    worldBounds,
    isReady,
    retry,
  };
}
