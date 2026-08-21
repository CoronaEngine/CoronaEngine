import { onMounted, onUnmounted, ref, unref, watch } from 'vue';

import { editorApi } from '@/api/editorApi.js';
import {
  applyStoryCameraMovement,
  normalizeStoryCameraKey,
  rotateStoryCamera,
} from '@/utils/storyCameraControls.js';

function isEditableTarget(target) {
  if (!target || typeof target !== 'object') return false;
  const tagName = String(target.tagName ?? '').toUpperCase();
  return (
    tagName === 'INPUT' ||
    tagName === 'TEXTAREA' ||
    tagName === 'SELECT' ||
    Boolean(target.isContentEditable)
  );
}

export function useStoryCameraControls({
  viewportRef,
  cameraBinding,
  enabled,
  refreshCameraBinding,
}) {
  const isLooking = ref(false);
  const activeKeys = new Set();

  let disposed = false;
  let viewportElement = null;
  let movementFrameId = null;
  let previousFrameTime = 0;
  let lastMouseX = 0;
  let lastMouseY = 0;
  let poseVersion = 0;
  let persistedPoseVersion = 0;
  let refreshToken = 0;

  const isEnabled = () => !disposed && Boolean(unref(enabled)) && Boolean(cameraBinding.value);

  const updateBindingPose = (changes) => {
    const current = cameraBinding.value;
    if (!current) return null;
    const next = { ...current, ...changes };
    cameraBinding.value = next;
    poseVersion += 1;
    return next;
  };

  const publishPose = () => {
    const binding = cameraBinding.value;
    const bridge = window.coronaBridge;
    if (!binding || !bridge || typeof bridge.cameraMove !== 'function') return false;
    try {
      bridge.cameraMove(
        binding.cameraHandle,
        [...binding.position],
        [...binding.forward],
        [...binding.worldUp],
        binding.fov
      );
      return true;
    } catch (error) {
      console.warn('[StoryMode] failed to update the native camera pose', error);
      return false;
    }
  };

  const persistPose = async () => {
    const binding = cameraBinding.value;
    const cameraId = binding?.cameraId || binding?.cameraName;
    if (!binding?.sceneId || !cameraId) return false;

    const versionToPersist = poseVersion;
    try {
      await editorApi.viewport.setCameraPose(binding.sceneId, cameraId, {
        position: [...binding.position],
        forward: [...binding.forward],
        world_up: [...binding.worldUp],
        fov: binding.fov,
        persist: true,
      });
      persistedPoseVersion = Math.max(persistedPoseVersion, versionToPersist);
      return true;
    } catch (error) {
      console.warn('[StoryMode] failed to persist the final camera pose', error);
      return false;
    }
  };

  const persistIfIdle = () => {
    if (activeKeys.size > 0 || isLooking.value || persistedPoseVersion >= poseVersion) {
      return;
    }
    void persistPose();
  };

  const cancelMovementFrame = () => {
    if (movementFrameId !== null) {
      window.cancelAnimationFrame(movementFrameId);
      movementFrameId = null;
    }
    previousFrameTime = 0;
  };

  const movementFrame = (timestamp) => {
    movementFrameId = null;
    if (!isEnabled() || activeKeys.size === 0) {
      previousFrameTime = 0;
      persistIfIdle();
      return;
    }

    const binding = cameraBinding.value;
    const deltaSeconds = previousFrameTime ? (timestamp - previousFrameTime) / 1000 : 0;
    previousFrameTime = timestamp;
    const result = applyStoryCameraMovement(binding, activeKeys, deltaSeconds, binding.moveSpeed);
    if (result.moved) {
      updateBindingPose({ position: result.position });
      publishPose();
    }
    movementFrameId = window.requestAnimationFrame(movementFrame);
  };

  const startMovementFrame = () => {
    if (!isEnabled() || activeKeys.size === 0 || movementFrameId !== null) return;
    previousFrameTime = 0;
    movementFrameId = window.requestAnimationFrame(movementFrame);
  };

  const refreshForNewOperation = async () => {
    const token = ++refreshToken;
    try {
      await refreshCameraBinding?.({ preservePose: true });
    } catch (error) {
      console.warn('[StoryMode] failed to refresh the camera before input', error);
    }
    return !disposed && token === refreshToken && isEnabled();
  };

  const stop = ({ persist = true } = {}) => {
    refreshToken += 1;
    activeKeys.clear();
    isLooking.value = false;
    cancelMovementFrame();
    if (persist && persistedPoseVersion < poseVersion) return persistPose();
    return Promise.resolve(false);
  };

  const handleKeyDown = (event) => {
    const code = normalizeStoryCameraKey(event);
    if (!code || !isEnabled() || isEditableTarget(event.target)) return;
    if (event.ctrlKey || event.altKey || event.metaKey || event.shiftKey) return;

    event.preventDefault();
    if (event.repeat || activeKeys.has(code)) return;
    const beginsOperation = activeKeys.size === 0;
    activeKeys.add(code);
    if (beginsOperation) {
      void refreshForNewOperation().then((ready) => {
        if (ready) startMovementFrame();
      });
    } else {
      startMovementFrame();
    }
  };

  const handleKeyUp = (event) => {
    const code = normalizeStoryCameraKey(event);
    if (!code) return;
    activeKeys.delete(code);
    if (activeKeys.size === 0) {
      cancelMovementFrame();
      persistIfIdle();
    }
  };

  const beginLook = (event) => {
    if (event.button !== 2 || !isEnabled()) return;
    event.preventDefault();
    viewportElement?.focus?.({ preventScroll: true });
    isLooking.value = true;
    lastMouseX = Number(event.clientX) || 0;
    lastMouseY = Number(event.clientY) || 0;
    void refreshForNewOperation();
  };

  const updateLook = (event) => {
    if (!isLooking.value) return;
    if (!isEnabled() || (Number(event.buttons) & 2) === 0) {
      isLooking.value = false;
      persistIfIdle();
      return;
    }

    const currentX = Number(event.clientX) || 0;
    const currentY = Number(event.clientY) || 0;
    const deltaX = currentX - lastMouseX;
    const deltaY = currentY - lastMouseY;
    lastMouseX = currentX;
    lastMouseY = currentY;
    if (deltaX === 0 && deltaY === 0) return;

    const binding = cameraBinding.value;
    const forward = rotateStoryCamera(binding.forward, binding.worldUp, deltaX, deltaY);
    updateBindingPose({ forward });
    publishPose();
  };

  const endLook = (event) => {
    if (event.button !== 2 || !isLooking.value) return;
    isLooking.value = false;
    persistIfIdle();
  };

  const preventContextMenu = (event) => {
    if (!isEnabled()) return;
    event.preventDefault();
  };

  const handleWindowBlur = () => {
    void stop({ persist: true });
  };

  watch(
    () => Boolean(unref(enabled)),
    (controlsEnabled) => {
      if (!controlsEnabled) void stop({ persist: true });
    }
  );

  onMounted(() => {
    disposed = false;
    viewportElement = viewportRef.value;
    viewportElement?.addEventListener('mousedown', beginLook);
    viewportElement?.addEventListener('contextmenu', preventContextMenu);
    window.addEventListener('keydown', handleKeyDown, true);
    window.addEventListener('keyup', handleKeyUp, true);
    window.addEventListener('mousemove', updateLook, true);
    window.addEventListener('mouseup', endLook, true);
    window.addEventListener('blur', handleWindowBlur);
  });

  onUnmounted(() => {
    void stop({ persist: true });
    disposed = true;
    viewportElement?.removeEventListener('mousedown', beginLook);
    viewportElement?.removeEventListener('contextmenu', preventContextMenu);
    viewportElement = null;
    window.removeEventListener('keydown', handleKeyDown, true);
    window.removeEventListener('keyup', handleKeyUp, true);
    window.removeEventListener('mousemove', updateLook, true);
    window.removeEventListener('mouseup', endLook, true);
    window.removeEventListener('blur', handleWindowBlur);
  });

  return {
    isLooking,
    stop,
    persistPose,
  };
}
