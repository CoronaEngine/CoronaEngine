import { nextTick, onMounted, onUnmounted, ref } from 'vue';

import { editorApi } from '@/api/editorApi.js';
import {
  cameraCandidatesFromSnapshot,
  cameraListFromPayload,
  computeCameraViewportRenderSize,
  resolveInitialSceneId,
  resolveStoryCameraBinding,
  shouldCreateStoryCamera,
  STORY_CAMERA_NAME,
} from '@/utils/nativeSceneViewport.js';

const CAMERA_DISCOVERY_TIMEOUT_MS = 3000;
const CAMERA_DISCOVERY_INTERVAL_MS = 200;

export function useNativeSceneViewport(viewportRef) {
  const status = ref('loading');
  const errorMessage = ref('');
  const sceneId = ref('');
  const cameraHandle = ref(0);
  const cameraBinding = ref(null);

  let disposed = false;
  let requestRevision = 0;
  let resizeObserver = null;
  let viewportSyncRafId = null;
  let lastViewportSignature = '';
  let refreshPromise = null;
  const retryTimers = new Map();

  const fail = (message, error = null) => {
    if (disposed) return;
    status.value = 'error';
    errorMessage.value = message;
    if (error) console.error('[StoryMode] native scene viewport initialization failed', error);
  };

  const waitForRetry = (delayMs, revision) =>
    new Promise((resolve) => {
      if (disposed || revision !== requestRevision) {
        resolve(false);
        return;
      }
      const timerId = window.setTimeout(() => {
        retryTimers.delete(timerId);
        resolve(!disposed && revision === requestRevision);
      }, delayMs);
      retryTimers.set(timerId, resolve);
    });

  const clearRetryTimers = () => {
    for (const [timerId, resolve] of retryTimers) {
      window.clearTimeout(timerId);
      resolve(false);
    }
    retryTimers.clear();
  };

  const applyBinding = (nextBinding, { preservePose = false } = {}) => {
    if (!nextBinding) return null;
    const previous = cameraBinding.value;
    const binding =
      preservePose && previous
        ? {
            ...nextBinding,
            position: [...previous.position],
            forward: [...previous.forward],
            worldUp: [...previous.worldUp],
            fov: previous.fov,
            moveSpeed: previous.moveSpeed,
          }
        : {
            ...nextBinding,
            position: [...nextBinding.position],
            forward: [...nextBinding.forward],
            worldUp: [...nextBinding.worldUp],
          };

    cameraBinding.value = binding;
    sceneId.value = binding.sceneId || sceneId.value;
    cameraHandle.value = binding.cameraHandle;
    lastViewportSignature = '';
    return binding;
  };

  const setCameraPose = async (pose = {}, { persist = true } = {}) => {
    const current = cameraBinding.value;
    const cameraId = current?.cameraId || current?.cameraName;
    if (!current?.sceneId || !cameraId) {
      throw new Error('Story camera binding is unavailable.');
    }

    const nextBinding = applyBinding({
      ...current,
      position: Array.isArray(pose.position) ? [...pose.position] : [...current.position],
      forward: Array.isArray(pose.forward) ? [...pose.forward] : [...current.forward],
      worldUp: Array.isArray(pose.worldUp ?? pose.world_up)
        ? [...(pose.worldUp ?? pose.world_up)]
        : [...current.worldUp],
      fov: Number.isFinite(Number(pose.fov)) ? Number(pose.fov) : current.fov,
    });

    const bridge = window.coronaBridge;
    if (bridge && typeof bridge.cameraMove === 'function') {
      bridge.cameraMove(
        nextBinding.cameraHandle,
        [...nextBinding.position],
        [...nextBinding.forward],
        [...nextBinding.worldUp],
        nextBinding.fov
      );
    }

    if (persist) {
      await editorApi.viewport.setCameraPose(nextBinding.sceneId, cameraId, {
        position: [...nextBinding.position],
        forward: [...nextBinding.forward],
        world_up: [...nextBinding.worldUp],
        fov: nextBinding.fov,
        persist: true,
      });
    }
    scheduleViewportSync();
    return true;
  };

  const syncViewportRect = () => {
    if (disposed) return false;
    const bridge = window.coronaBridge;
    const element = viewportRef.value;
    const handle = Number(cameraBinding.value?.cameraHandle ?? cameraHandle.value ?? 0);
    if (!bridge || typeof bridge.setCameraViewport !== 'function' || !element || !handle) {
      return false;
    }

    const rect = element.getBoundingClientRect?.();
    if (!rect || Number(rect.width) <= 0 || Number(rect.height) <= 0) return false;

    const scale = Math.max(Number(window.devicePixelRatio || 1), 0.01);
    const x = Math.max(Math.round(Number(rect.left || 0) * scale), 0);
    const y = Math.max(Math.round(Number(rect.top || 0) * scale), 0);
    const width = Math.max(Math.round(Number(rect.width || 0) * scale), 1);
    const height = Math.max(Math.round(Number(rect.height || 0) * scale), 1);
    const renderSize = computeCameraViewportRenderSize(rect.width, rect.height, scale);
    const signature = `${handle}:${x}:${y}:${width}:${height}:${renderSize.width}:${renderSize.height}`;
    if (signature === lastViewportSignature) return true;

    try {
      const applied = bridge.setCameraViewport(
        handle,
        x,
        y,
        width,
        height,
        renderSize.width,
        renderSize.height
      );
      if (applied) {
        lastViewportSignature = signature;
        status.value = 'ready';
        errorMessage.value = '';
        return true;
      }
    } catch (error) {
      fail('无法绑定世界画面，请返回主界面后重试。', error);
    }
    return false;
  };

  const scheduleViewportSync = () => {
    if (disposed || viewportSyncRafId !== null) return;
    viewportSyncRafId = window.requestAnimationFrame(() => {
      viewportSyncRafId = null;
      syncViewportRect();
    });
  };

  const createStoryCamera = async (activeSceneId, revision) => {
    const createdResult = await editorApi.sceneTools.createCameraView(
      activeSceneId,
      STORY_CAMERA_NAME
    );
    if (disposed || revision !== requestRevision) return null;

    let binding = resolveStoryCameraBinding(createdResult, createdResult, activeSceneId);
    const createdCamera = createdResult?.data?.camera ?? createdResult?.camera ?? null;
    const cameraId = String(
      createdCamera?.camera_id ?? createdCamera?.id ?? createdCamera?.name ?? STORY_CAMERA_NAME
    ).trim();

    if (cameraId) {
      try {
        const updatedResult = await editorApi.sceneTools.updateCameraView(activeSceneId, cameraId, {
          view_open: false,
        });
        if (disposed || revision !== requestRevision) return null;
        binding = resolveStoryCameraBinding(updatedResult, updatedResult, activeSceneId) ?? binding;
      } catch (error) {
        console.warn('[StoryMode] failed to close the automatically created camera view', error);
      }
    }
    return binding;
  };

  const discoverCamera = async (
    activeSceneId,
    revision,
    { allowCreate = true, timeoutMs = CAMERA_DISCOVERY_TIMEOUT_MS } = {}
  ) => {
    const deadline = Date.now() + timeoutMs;
    let successfulEmptyChecks = 0;
    let hasObservedCamera = false;
    let createAttempted = false;
    let hasSuccessfulCameraList = false;
    let lastError = null;

    while (!disposed && revision === requestRevision) {
      const [snapshotResult, cameraListResult] = await Promise.allSettled([
        editorApi.scene.getSnapshot(activeSceneId),
        editorApi.sceneTools.listCameraViews(activeSceneId),
      ]);
      if (disposed || revision !== requestRevision) return null;

      const snapshotPayload = snapshotResult.status === 'fulfilled' ? snapshotResult.value : {};
      const cameraListPayload =
        cameraListResult.status === 'fulfilled' ? cameraListResult.value : {};
      if (snapshotResult.status === 'rejected') lastError = snapshotResult.reason;
      if (cameraListResult.status === 'rejected') lastError = cameraListResult.reason;

      const binding = resolveStoryCameraBinding(snapshotPayload, cameraListPayload, activeSceneId);
      if (binding) return binding;

      const snapshotCameras = cameraCandidatesFromSnapshot(snapshotPayload);
      const listedCameras = cameraListFromPayload(cameraListPayload);
      if (snapshotCameras.length > 0 || listedCameras.length > 0) {
        hasObservedCamera = true;
      }

      if (cameraListResult.status === 'fulfilled') {
        hasSuccessfulCameraList = true;
        if (listedCameras.length === 0) successfulEmptyChecks += 1;
        else successfulEmptyChecks = 0;
      }

      if (
        allowCreate &&
        shouldCreateStoryCamera({
          successfulEmptyChecks,
          hasObservedCamera,
          createAttempted,
        })
      ) {
        createAttempted = true;
        const createdBinding = await createStoryCamera(activeSceneId, revision);
        if (createdBinding) return createdBinding;
      }

      if (Date.now() >= deadline) {
        const error = new Error(
          hasObservedCamera
            ? 'Camera exists but its native handle is unavailable.'
            : createAttempted
              ? 'StoryCamera was created but its native handle is unavailable.'
              : hasSuccessfulCameraList
                ? 'No camera is available in the current scene.'
                : 'Unable to query cameras for the current scene.',
          { cause: lastError ?? undefined }
        );
        error.code = hasObservedCamera ? 'CAMERA_HANDLE_UNAVAILABLE' : 'CAMERA_UNAVAILABLE';
        throw error;
      }

      const shouldContinue = await waitForRetry(CAMERA_DISCOVERY_INTERVAL_MS, revision);
      if (!shouldContinue) return null;
    }
    return null;
  };

  const refreshCameraBinding = ({ preservePose = true } = {}) => {
    if (disposed || !sceneId.value) return Promise.resolve(false);
    if (refreshPromise) return refreshPromise;

    const revision = requestRevision;
    const activeSceneId = sceneId.value;
    const pending = discoverCamera(activeSceneId, revision, {
      allowCreate: false,
      timeoutMs: CAMERA_DISCOVERY_INTERVAL_MS,
    })
      .then(async (binding) => {
        if (!binding || disposed || revision !== requestRevision) return false;
        applyBinding(binding, { preservePose });
        await nextTick();
        if (disposed || revision !== requestRevision) return false;
        scheduleViewportSync();
        return true;
      })
      .catch((error) => {
        console.warn('[StoryMode] failed to refresh camera binding', error);
        return false;
      })
      .finally(() => {
        if (refreshPromise === pending) refreshPromise = null;
      });
    refreshPromise = pending;
    return pending;
  };

  const initialize = async () => {
    const revision = ++requestRevision;
    clearRetryTimers();
    status.value = 'loading';
    errorMessage.value = '';
    sceneId.value = '';
    cameraHandle.value = 0;
    cameraBinding.value = null;
    lastViewportSignature = '';

    const bridge = window.coronaBridge;
    if (!bridge || typeof bridge.setCameraViewport !== 'function') {
      fail('当前环境不支持原生世界画面。请在 CoronaEngine 中打开剧情模式。');
      return;
    }

    try {
      const initResult = await editorApi.main.onInit();
      if (disposed || revision !== requestRevision) return;

      const activeSceneId = resolveInitialSceneId(initResult);
      if (!activeSceneId) {
        fail('当前项目没有可用的场景。');
        return;
      }
      sceneId.value = activeSceneId;

      const binding = await discoverCamera(activeSceneId, revision);
      if (disposed || revision !== requestRevision || !binding) return;

      applyBinding(binding);
      await nextTick();
      if (disposed || revision !== requestRevision) return;

      if (!syncViewportRect()) {
        fail('无法绑定世界画面，请返回主界面后重试。');
      }
    } catch (error) {
      if (disposed || revision !== requestRevision) return;
      if (error?.code === 'CAMERA_HANDLE_UNAVAILABLE') {
        fail('检测到场景摄像机，但原生句柄尚未就绪。请稍后重试。', error);
      } else if (error?.code === 'CAMERA_UNAVAILABLE') {
        fail('当前场景没有可用的摄像机，自动创建 StoryCamera 失败。', error);
      } else {
        fail('世界加载失败，请返回主界面后重试。', error);
      }
    }
  };

  onMounted(async () => {
    disposed = false;
    window.addEventListener('resize', scheduleViewportSync);
    if (typeof ResizeObserver !== 'undefined' && viewportRef.value) {
      resizeObserver = new ResizeObserver(scheduleViewportSync);
      resizeObserver.observe(viewportRef.value);
    }
    await initialize();
  });

  onUnmounted(() => {
    disposed = true;
    requestRevision += 1;
    clearRetryTimers();
    refreshPromise = null;
    window.removeEventListener('resize', scheduleViewportSync);
    resizeObserver?.disconnect?.();
    resizeObserver = null;
    if (viewportSyncRafId !== null) {
      window.cancelAnimationFrame(viewportSyncRafId);
      viewportSyncRafId = null;
    }
  });

  return {
    status,
    errorMessage,
    sceneId,
    cameraHandle,
    cameraBinding,
    refreshCameraBinding,
    setCameraPose,
    retry: initialize,
    scheduleViewportSync,
  };
}
