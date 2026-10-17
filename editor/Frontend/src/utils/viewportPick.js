export const indexActorsByHandle = (actors = []) => {
  const index = new Map();
  for (const actor of actors) {
    const handle = Number(actor?.handle || 0);
    if (!Number.isFinite(handle) || handle <= 0) continue;
    index.set(handle, {
      name: actor.name,
      type: actor.type || actor.actor_type || 'actor',
    });
  }
  return index;
};

export const createViewportPickController = ({
  getBridge,
  getCameraBinding,
  getHitRect,
  getRenderRect,
  getViewportRect,
  getViewportSize,
  getActorIndex,
  onPickStart,
  emitActorChange,
  makeRequestId,
} = {}) => {
  let latestRequestId = '';
  const pendingRequests = new Map();
  let sequence = 0;

  const nextRequestId =
    makeRequestId || (() => `pick-${Date.now()}-${++sequence}`);

  const normalizeRect = (rect) => {
    if (!rect) return null;
    const left = Number(rect.left || 0);
    const top = Number(rect.top || 0);
    const width = Number(rect.width || 0);
    const height = Number(rect.height || 0);
    if (
      !Number.isFinite(left) ||
      !Number.isFinite(top) ||
      !Number.isFinite(width) ||
      !Number.isFinite(height) ||
      width <= 0 ||
      height <= 0
    ) {
      return null;
    }
    return { left, top, width, height };
  };

  const containsClientPoint = (rect, clientX, clientY) =>
    rect &&
    clientX >= rect.left &&
    clientY >= rect.top &&
    clientX < rect.left + rect.width &&
    clientY < rect.top + rect.height;

  const callFastPick = (bridge, cameraHandle, sceneId, requestId, x, y, width, height) => {
    bridge.pickActor(cameraHandle, sceneId, requestId, x, y, width, height);
  };

  return {
    dispose() {
      latestRequestId = '';
      pendingRequests.clear();
    },

    pickAt(event, overrides = {}) {
      const bridge = overrides.bridge || getBridge?.();
      if (!bridge || typeof bridge.pickActor !== 'function') {
        return false;
      }

      const binding = overrides.cameraBinding || getCameraBinding?.() || {};
      const cameraHandle = Number(binding.cameraHandle || 0);
      const sceneId = binding.sceneId || '';
      if (!Number.isFinite(cameraHandle) || cameraHandle <= 0 || !sceneId) {
        return false;
      }

      const clientX = Number(event?.clientX);
      const clientY = Number(event?.clientY);
      if (!Number.isFinite(clientX) || !Number.isFinite(clientY)) {
        return false;
      }

      const hitRect = normalizeRect(
        overrides.hitRect || overrides.viewportRect || getHitRect?.() || getViewportRect?.()
      );
      if (!containsClientPoint(hitRect, clientX, clientY)) {
        return false;
      }

      const renderRect = normalizeRect(
        overrides.renderRect || getRenderRect?.() || overrides.viewport || getViewportSize?.() || hitRect
      );
      if (!renderRect) {
        return false;
      }

      const x = clientX - renderRect.left;
      const y = clientY - renderRect.top;
      const width = renderRect.width;
      const height = renderRect.height;
      if (
        !Number.isFinite(x) ||
        !Number.isFinite(y) ||
        x < 0 ||
        y < 0 ||
        x >= width ||
        y >= height
      ) {
        return false;
      }

      const requestId = nextRequestId();
      latestRequestId = requestId;
      pendingRequests.set(requestId, { cameraHandle, sceneId });
       try {
        callFastPick(bridge, cameraHandle, sceneId, requestId, x, y, width, height);
      } catch (_) {
        pendingRequests.delete(requestId);
        return false;
      }
      onPickStart?.(requestId);

       return requestId;
    },

    handlePickResult(payload) {
      const binding = getCameraBinding?.() || {};
      const activeCameraHandle = Number(binding.cameraHandle || 0);
      const activeSceneId = binding.sceneId || '';
      const payloadCameraHandle = Number(payload?.cameraHandle || 0);
      const requestId = payload?.requestId || '';
      const request = pendingRequests.get(requestId);
      if (!payload || !request ||
          (payloadCameraHandle > 0 && payloadCameraHandle !== request.cameraHandle) ||
          (payload?.sceneId && payload.sceneId !== request.sceneId)) {
        return { status: 'stale', payload };
      }

      const isLatest = requestId === latestRequestId;
      pendingRequests.delete(requestId);
      if (!isLatest ||
          (payloadCameraHandle > 0 && payloadCameraHandle !== activeCameraHandle) ||
          (payload?.sceneId && activeSceneId && payload.sceneId !== activeSceneId)) {
        return { status: 'stale', payload };
      }

      if (payload.status === 'miss') {
        return { status: 'miss', payload };
      }

      if (payload.status === 'error') {
        return { status: 'error', payload };
      }

      if (payload.status !== 'success') {
        return { status: 'error', payload };
      }

      const actorHandle = Number(payload.actorHandle || 0);
      const actor = getActorIndex?.().get(actorHandle);
      const payloadActorName = payload.actor_name || payload.actorName || payload.name || payload.actor?.name;
      const payloadActorType = payload.actorType || payload.type || payload.actor?.actor_type || payload.actor?.type;
      const resolvedActor = actor?.name
        ? actor
        : payloadActorName
          ? { name: payloadActorName, type: payloadActorType || 'actor' }
          : null;
      if (!resolvedActor?.name) {
        return { status: 'unknown', payload };
      }

      emitActorChange?.(resolvedActor.type || 'actor', payload.sceneId, resolvedActor.name, { node_index: Number.isInteger(payload.node_index) && payload.node_index >= 0 ? payload.node_index : null });
      return {
        status: 'selected',
        payload,
        actor: {
          handle: actorHandle,
          name: resolvedActor.name,
          type: resolvedActor.type || 'actor',
        },
      };
    },
  };
};
