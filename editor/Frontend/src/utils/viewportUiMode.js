export const VIEWPORT_UI_MODES = Object.freeze(['flat2d', 'stereo3d']);

export const normalizeViewportUiMode = (mode) => (
  mode === 'stereo3d' ? 'stereo3d' : 'flat2d'
);

export const NATIVE_VIEWPORT_CURSOR_STORAGE_KEY = 'corona.nativeViewportCursor.enabled';
export const DEFAULT_NATIVE_VIEWPORT_CURSOR_ENABLED = false;

const safeStorage = () => {
  try {
    return globalThis.localStorage ?? null;
  } catch (_) {
    return null;
  }
};

const TRUE_SWITCH_VALUES = new Set(['1', 'true', 'on', 'yes', 'enabled']);
const FALSE_SWITCH_VALUES = new Set(['0', 'false', 'off', 'no', 'disabled']);

export const normalizeFeatureSwitch = (value, fallback = false) => {
  if (typeof value === 'boolean') return value;
  if (value === null || value === undefined) return fallback;
  const normalized = String(value).trim().toLowerCase();
  if (TRUE_SWITCH_VALUES.has(normalized)) return true;
  if (FALSE_SWITCH_VALUES.has(normalized)) return false;
  return fallback;
};

export const isNativeViewportCursorEnabled = ({
  storage = safeStorage(),
  globalObject = globalThis,
} = {}) => {
  if (Object.prototype.hasOwnProperty.call(globalObject ?? {}, '__CORONA_ENABLE_NATIVE_VIEWPORT_CURSOR__')) {
    return normalizeFeatureSwitch(
      globalObject.__CORONA_ENABLE_NATIVE_VIEWPORT_CURSOR__,
      DEFAULT_NATIVE_VIEWPORT_CURSOR_ENABLED,
    );
  }
  let stored = null;
  try {
    stored = storage?.getItem?.(NATIVE_VIEWPORT_CURSOR_STORAGE_KEY) ?? null;
  } catch (_) {
    stored = null;
  }
  return normalizeFeatureSwitch(stored, DEFAULT_NATIVE_VIEWPORT_CURSOR_ENABLED);
};

const sanitizePart = (value, fallback) => {
  const text = String(value ?? '').trim();
  return text || fallback;
};

export const createViewportUiModeStore = ({ storage = safeStorage() } = {}) => {
  const keyFor = ({
    scope = 'main',
    sceneId = 'scene',
    cameraId = '',
    cameraHandle = '',
  } = {}) => {
    const viewId = cameraHandle || cameraId || 'default';
    return [
      'corona',
      'viewportUiMode',
      sanitizePart(scope, 'main'),
      sanitizePart(sceneId, 'scene'),
      sanitizePart(viewId, 'default'),
    ].join('.');
  };

  const get = (descriptor) => {
    const raw = storage?.getItem?.(keyFor(descriptor));
    return normalizeViewportUiMode(raw);
  };

  const set = (descriptor, mode) => {
    const normalized = normalizeViewportUiMode(mode);
    storage?.setItem?.(keyFor(descriptor), normalized);
    return normalized;
  };

  const applyToBridge = ({ bridge, cameraHandle, mode }) => {
    if (!cameraHandle || typeof bridge?.setViewportUiMode !== 'function') return false;
    bridge.setViewportUiMode(Number(cameraHandle), normalizeViewportUiMode(mode));
    return true;
  };

  return { keyFor, get, set, applyToBridge };
};

// 光场 3D UI 标定：UI 面板与传输层统一使用 Vision 光场语义 (pe/angle/offset)。
// pe/angle/offset 对应 vision::LenticularParams；parallaxScale 是 UI 专有的视差增益，
// 无 Vision 对应。子像素→warp(ViewportUiCalibration) 的换算在 C++ 边界统一完成。
export const DEFAULT_LIGHT_FIELD_CALIBRATION = Object.freeze({
  pe: 19.1813,
  angle: 0.2305,
  offset: 14.1171,
  parallaxScale: 19.1849,
});

export const normalizeLightFieldCalibration = (raw) => {
  const src = raw && typeof raw === 'object' ? raw : {};
  const num = (value, fallback) => {
    const n = Number(value);
    return Number.isFinite(n) ? n : fallback;
  };
  return {
    pe: num(src.pe, DEFAULT_LIGHT_FIELD_CALIBRATION.pe),
    angle: num(src.angle, DEFAULT_LIGHT_FIELD_CALIBRATION.angle),
    offset: num(src.offset, DEFAULT_LIGHT_FIELD_CALIBRATION.offset),
    parallaxScale: num(src.parallaxScale, DEFAULT_LIGHT_FIELD_CALIBRATION.parallaxScale),
  };
};

export const createViewportUiCalibrationStore = ({ storage = safeStorage() } = {}) => {
  const keyFor = ({
    scope = 'main',
    sceneId = 'scene',
    cameraId = '',
    cameraHandle = '',
  } = {}) => {
    const viewId = cameraHandle || cameraId || 'default';
    return [
      'corona',
      'viewportUiCalibration',
      sanitizePart(scope, 'main'),
      sanitizePart(sceneId, 'scene'),
      sanitizePart(viewId, 'default'),
    ].join('.');
  };

  const get = (descriptor) => {
    let raw = null;
    try {
      raw = JSON.parse(storage?.getItem?.(keyFor(descriptor)) ?? 'null');
    } catch (_) {
      raw = null;
    }
    return normalizeLightFieldCalibration(raw);
  };

  const set = (descriptor, calibration) => {
    const normalized = normalizeLightFieldCalibration(calibration);
    try {
      storage?.setItem?.(keyFor(descriptor), JSON.stringify(normalized));
    } catch (_) {
      // ignore storage failures
    }
    return normalized;
  };

  const applyToBridge = ({ bridge, cameraHandle, calibration }) => {
    if (!cameraHandle || typeof bridge?.setViewportUiCalibration !== 'function') return false;
    const c = normalizeLightFieldCalibration(calibration);
    bridge.setViewportUiCalibration(Number(cameraHandle), c.pe, c.angle, c.offset, c.parallaxScale);
    return true;
  };

  return { keyFor, get, set, applyToBridge };
};

export const VIEWPORT_UI_CURSOR_SHAPES = Object.freeze([
  'arrow',
  'hand',
  'crosshair',
  'grab',
  'grabbing',
  'hidden',
]);

export const normalizeViewportUiCursorShape = (shape) => {
  const normalized = String(shape ?? '').trim().toLowerCase();
  if (normalized === 'pointer') return 'hand';
  if (normalized === 'none') return 'hidden';
  return VIEWPORT_UI_CURSOR_SHAPES.includes(normalized) ? normalized : 'arrow';
};

const normalizeRect = (rect) => {
  if (!rect) return null;
  const left = Number(rect.left);
  const top = Number(rect.top);
  const width = Number(rect.width);
  const height = Number(rect.height);
  if (![left, top, width, height].every(Number.isFinite) || width <= 0 || height <= 0) {
    return null;
  }
  const renderWidth = Number.isFinite(Number(rect.renderWidth)) ? Number(rect.renderWidth) : width;
  const renderHeight = Number.isFinite(Number(rect.renderHeight)) ? Number(rect.renderHeight) : height;
  return {
    left,
    top,
    width,
    height,
    renderWidth: Math.max(renderWidth, 0),
    renderHeight: Math.max(renderHeight, 0),
  };
};

const pointerModifiers = (event) => (
  (event?.ctrlKey ? 1 : 0) |
  (event?.shiftKey ? 2 : 0) |
  (event?.altKey ? 4 : 0) |
  (event?.metaKey ? 8 : 0)
);

const pointInRect = (event, rect) => (
  event.clientX >= rect.left &&
  event.clientY >= rect.top &&
  event.clientX <= rect.left + rect.width &&
  event.clientY <= rect.top + rect.height
);

export const createViewportUiPointerController = ({
  getBridge,
  getCameraHandle,
  getHitRect,
  getRenderRect,
  getEnabled = () => true,
  getNativeCursorEnabled = () => isNativeViewportCursorEnabled(),
  defaultCursor = 'arrow',
} = {}) => {
  let lastCameraHandle = 0;
  let lastX = 0;
  let lastY = 0;
  let pendingMove = null;
  let pendingMoveFrame = 0;
  let pointerVisible = false;
  let systemCursorHidden = false;
  let systemCursorCustom = false;

  const cameraHandleForSend = () => {
    const handle = Number(getCameraHandle?.() ?? 0);
    return Number.isFinite(handle) && handle > 0 ? handle : 0;
  };

  const bridgeForSend = () => getBridge?.() ?? globalThis.coronaBridge ?? null;

  const isNativeCursorEnabled = () => getNativeCursorEnabled?.() === true;

  const isEnabled = () => getEnabled?.() !== false && isNativeCursorEnabled();

  const setSystemCursorState = (hidden, custom) => {
    if (systemCursorHidden === hidden && systemCursorCustom === custom) return false;
    const bridge = bridgeForSend();
    if (typeof bridge?.setViewportSystemCursorHidden !== 'function') return false;
    if (bridge.setViewportSystemCursorHidden(hidden, custom) === false) return false;
    systemCursorHidden = hidden;
    systemCursorCustom = custom;
    return true;
  };

  const isHidePayload = ({ type, cursor }) => {
    const eventType = String(type ?? '').toLowerCase();
    return cursor === 'hidden' || eventType === 'pointerleave' ||
      eventType === 'pointercancel' || eventType === 'leave' ||
      eventType === 'cancel' || eventType === 'blur';
  };

  const emitPointer = ({ cameraHandle, type, x, y, buttons, modifiers, cursor }) => {
    const bridge = bridgeForSend();
    if (typeof bridge?.viewportUiPointer !== 'function') return false;
    bridge.viewportUiPointer(cameraHandle, type, x, y, buttons, modifiers, cursor);
    pointerVisible = !isHidePayload({ type, cursor });
    setSystemCursorState(pointerVisible, pointerVisible);
    return true;
  };

  const flushPendingMove = () => {
    pendingMoveFrame = 0;
    const payload = pendingMove;
    pendingMove = null;
    if (!payload) return false;
    return emitPointer(payload);
  };

  const hide = () => {
    pendingMove = null;
    if (pendingMoveFrame && typeof globalThis.cancelAnimationFrame === 'function') {
      globalThis.cancelAnimationFrame(pendingMoveFrame);
    }
    pendingMoveFrame = 0;
    const cameraHandle = cameraHandleForSend() || lastCameraHandle;
    if (!cameraHandle || !pointerVisible) {
      setSystemCursorState(false, false);
      return false;
    }
    return emitPointer({
      cameraHandle,
      type: 'pointerleave',
      x: lastX,
      y: lastY,
      buttons: 0,
      modifiers: 0,
      cursor: 'hidden',
    });
  };

  const send = (event, type = event?.type ?? 'pointermove', cursor = defaultCursor) => {
    const clientX = Number(event?.clientX);
    const clientY = Number(event?.clientY);
    if (!Number.isFinite(clientX) || !Number.isFinite(clientY)) return false;

    const hitRect = normalizeRect(getHitRect?.() ?? getRenderRect?.());
    if (hitRect && !pointInRect({ clientX, clientY }, hitRect)) {
      hide();
      return false;
    }

    const renderRect = normalizeRect(getRenderRect?.() ?? hitRect);
    if (!renderRect || renderRect.renderWidth <= 0 || renderRect.renderHeight <= 0) {
      hide();
      return false;
    }

    const opticsPointerEnabled = isEnabled();
    if (!opticsPointerEnabled) {
      if (pointerVisible) {
        hide();
      } else {
        setSystemCursorState(false, false);
      }
      return false;
    }
    setSystemCursorState(true, true);

    const bridge = bridgeForSend();
    if (typeof bridge?.viewportUiPointer !== 'function') return false;
    const cameraHandle = cameraHandleForSend();
    if (!cameraHandle) {
      hide();
      return false;
    }

    const x = (clientX - renderRect.left) * (renderRect.renderWidth / renderRect.width);
    const y = (clientY - renderRect.top) * (renderRect.renderHeight / renderRect.height);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return false;

    lastCameraHandle = cameraHandle;
    lastX = Math.max(0, Math.min(renderRect.renderWidth - 1, x));
    lastY = Math.max(0, Math.min(renderRect.renderHeight - 1, y));
    const payload = {
      cameraHandle,
      type,
      x: lastX,
      y: lastY,
      buttons: Number(event?.buttons ?? 0) || 0,
      modifiers: pointerModifiers(event),
      cursor: normalizeViewportUiCursorShape(cursor),
    };

    if (type === 'pointermove' && typeof globalThis.requestAnimationFrame === 'function') {
      pendingMove = payload;
      if (!pendingMoveFrame) {
        pendingMoveFrame = globalThis.requestAnimationFrame(flushPendingMove);
      }
      return true;
    }

    return emitPointer(payload);
  };

  return {
    send,
    hide,
    dispose: hide,
  };
};

export const createViewportUiCursorController = ({ getTarget } = {}) => {
  let restoreCursor = null;
  const targetFor = () => getTarget?.() ?? globalThis.document?.body ?? null;

  const restore = () => {
    const target = targetFor();
    if (target?.style && restoreCursor !== null) {
      target.style.cursor = restoreCursor;
    }
    restoreCursor = null;
  };

  const set = (cursor) => {
    const target = targetFor();
    if (!target?.style) return false;
    if (restoreCursor === null) {
      restoreCursor = target.style.cursor || '';
    }
    target.style.cursor = cursor || restoreCursor;
    return true;
  };

  return {
    set,
    restore,
    cancel: restore,
  };
};
