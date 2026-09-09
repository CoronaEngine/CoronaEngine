/**
 * 剧情模式输入管理器。
 *
 * 职责：
 * 1. 将键盘和鼠标事件转换为逻辑动作；
 * 2. 维护按键按下、持续按住和鼠标视角增量；
 * 3. 管理 Pointer Lock 与普通鼠标 fallback；
 * 4. 在窗口失焦或页面隐藏时清理残留输入。
 *
 * 运行时系统只应依赖逻辑动作名称，不应直接读取 DOM 按键字符串。
 */

const bindings = Object.freeze({
  KeyW: 'forward',
  KeyS: 'backward',
  KeyA: 'left',
  KeyD: 'right',
  Space: 'jump',
  KeyB: 'inventory',
  KeyM: 'map',
  KeyF: 'interact',
  Digit1: 'hotbar1',
  Digit2: 'hotbar2',
  Digit3: 'hotbar3',
  Digit4: 'hotbar4',
  Digit5: 'hotbar5',
  Digit6: 'hotbar6',
  Digit7: 'hotbar7',
});

const keyFallbackCodes = Object.freeze({
  w: 'KeyW',
  s: 'KeyS',
  a: 'KeyA',
  d: 'KeyD',
  b: 'KeyB',
  m: 'KeyM',
  f: 'KeyF',
  1: 'Digit1',
  2: 'Digit2',
  3: 'Digit3',
  4: 'Digit4',
  5: 'Digit5',
  6: 'Digit6',
  7: 'Digit7',
  ' ': 'Space',
  Spacebar: 'Space',
});

const editableTags = new Set(['INPUT', 'TEXTAREA', 'SELECT', 'BUTTON']);

function resolveCode(event) {
  if (event?.code && bindings[event.code]) return event.code;

  const key = String(event?.key || '');
  return keyFallbackCodes[key] || keyFallbackCodes[key.toLowerCase()] || '';
}

function isEditableTarget(event) {
  const tagName = event?.target?.tagName;
  return Boolean(event?.target?.isContentEditable || editableTags.has(tagName));
}

/**
 * 创建剧情模式输入管理器。
 * @param {Document} target 用于监听鼠标和 Pointer Lock 的文档对象。
 * @returns {object} 剧情模式输入接口。
 */
export function createStoryInputManager(target = document, options = {}) {
  const gameElement = options.gameElement || null;
  const windowTarget = options.windowTarget || globalThis.window || target;
  const held = new Set();
  const pressed = new Set();
  const look = { x: 0, y: 0 };

  let pointerLocked = false;
  let mouseActive = false;
  let lastPointer = null;

  const onKeyDown = (event) => {
    if (isEditableTarget(event)) return;

    const code = resolveCode(event);
    const action = bindings[code];
    if (!action) return;

    event.preventDefault();

    if (!event.repeat) {
      pressed.add(action);
    }
    held.add(action);
  };

  const onKeyUp = (event) => {
    const code = resolveCode(event);
    const action = bindings[code];
    if (action) {
      held.delete(action);
    }
  };

  const isGameSurfaceEvent = (event) => {
    if (!gameElement) return true;
    const eventTarget = event?.target;
    return eventTarget === gameElement || gameElement.contains?.(eventTarget);
  };

  const onMouseMove = (event) => {
    if (!mouseActive && !pointerLocked) return;
    if (!pointerLocked && !isGameSurfaceEvent(event)) return;

    const movementX = Number(event.movementX);
    const movementY = Number(event.movementY);
    if (pointerLocked || Number.isFinite(movementX) || Number.isFinite(movementY)) {
      look.x += Number.isFinite(movementX) ? movementX : 0;
      look.y += Number.isFinite(movementY) ? movementY : 0;
    } else if (Number.isFinite(event.clientX) && Number.isFinite(event.clientY)) {
      if (lastPointer) {
        look.x += event.clientX - lastPointer.x;
        look.y += event.clientY - lastPointer.y;
      }
      lastPointer = { x: event.clientX, y: event.clientY };
    }
  };

  const onMouseDown = (event) => {
    if (event.button !== 0 || (!mouseActive && !pointerLocked)) return;
    if (!pointerLocked && !isGameSurfaceEvent(event)) return;

    event.preventDefault();
    pressed.add('attack');
  };

  const onPointerLockChange = () => {
    const wasPointerLocked = pointerLocked;
    pointerLocked = target.pointerLockElement != null;
    if (pointerLocked) {
      mouseActive = true;
      lastPointer = null;
    } else if (wasPointerLocked) {
      // 用户按 Esc 主动退出 Pointer Lock 后，等待下一次点击重新激活视角。
      mouseActive = false;
      clearTransient();
    }
  };

  const clearTransient = () => {
    pressed.clear();
    look.x = 0;
    look.y = 0;
    lastPointer = null;
  };

  const clearAll = () => {
    held.clear();
    clearTransient();
  };

  const onWindowBlur = () => {
    clearAll();
    mouseActive = false;
    pointerLocked = false;
    target.exitPointerLock?.();
  };

  const onVisibilityChange = () => {
    if (target.visibilityState === 'hidden') {
      onWindowBlur();
    }
  };

  windowTarget.addEventListener('keydown', onKeyDown, true);
  windowTarget.addEventListener('keyup', onKeyUp, true);
  target.addEventListener('mousemove', onMouseMove, true);
  target.addEventListener('mousedown', onMouseDown, true);
  target.addEventListener('pointerlockchange', onPointerLockChange);
  windowTarget.addEventListener('blur', onWindowBlur);
  target.addEventListener('visibilitychange', onVisibilityChange);

  return {
    isHeld: (action) => held.has(action),

    consumePressed: (action) => {
      const value = pressed.has(action);
      pressed.delete(action);
      return value;
    },

    getMoveAxis: () => ({
      x: Number(held.has('right')) - Number(held.has('left')),
      z: Number(held.has('backward')) - Number(held.has('forward')),
    }),

    consumeLookDelta: () => {
      const value = { ...look };
      look.x = 0;
      look.y = 0;
      return value;
    },

    isPointerLocked: () => pointerLocked,
    isMouseActive: () => mouseActive,

    setMouseActive: (active) => {
      mouseActive = Boolean(active);
      if (!mouseActive) {
        clearTransient();
      }
    },

    clearTransient,
    clearAll,

    dispose: () => {
      windowTarget.removeEventListener('keydown', onKeyDown, true);
      windowTarget.removeEventListener('keyup', onKeyUp, true);
      target.removeEventListener('mousemove', onMouseMove, true);
      target.removeEventListener('mousedown', onMouseDown, true);
      target.removeEventListener('pointerlockchange', onPointerLockChange);
      windowTarget.removeEventListener('blur', onWindowBlur);
      target.removeEventListener('visibilitychange', onVisibilityChange);
      clearAll();
    },
  };
}
