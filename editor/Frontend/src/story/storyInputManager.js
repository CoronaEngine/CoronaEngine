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
});

const keyFallbackCodes = Object.freeze({
  w: 'KeyW',
  s: 'KeyS',
  a: 'KeyA',
  d: 'KeyD',
  b: 'KeyB',
  m: 'KeyM',
  f: 'KeyF',
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
  return Boolean(
    event?.target?.isContentEditable || editableTags.has(tagName),
  );
}

/**
 * 创建剧情模式输入管理器。
 * @param {Document} target 用于监听鼠标和 Pointer Lock 的文档对象。
 * @returns {object} 剧情模式输入接口。
 */
export function createStoryInputManager(target = document) {
  const held = new Set();
  const pressed = new Set();
  const look = { x: 0, y: 0 };

  let pointerLocked = false;
  let mouseActive = false;

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

  const onMouseMove = (event) => {
    if (!mouseActive && !pointerLocked) return;

    look.x += Number(event.movementX) || 0;
    look.y += Number(event.movementY) || 0;
  };

  const onMouseDown = (event) => {
    if (event.button !== 0 || (!mouseActive && !pointerLocked)) return;

    event.preventDefault();
    pressed.add('attack');
  };

  const onPointerLockChange = () => {
    pointerLocked = target.pointerLockElement != null;
    if (pointerLocked) {
      mouseActive = true;
    }
  };

  const clearTransient = () => {
    pressed.clear();
    look.x = 0;
    look.y = 0;
  };

  const clearAll = () => {
    held.clear();
    clearTransient();
  };

  const onWindowBlur = () => {
    clearAll();
    mouseActive = false;
    pointerLocked = false;
  };

  const onVisibilityChange = () => {
    if (target.visibilityState === 'hidden') {
      onWindowBlur();
    }
  };

  window.addEventListener('keydown', onKeyDown, true);
  window.addEventListener('keyup', onKeyUp, true);
  target.addEventListener('mousemove', onMouseMove, true);
  target.addEventListener('mousedown', onMouseDown, true);
  target.addEventListener('pointerlockchange', onPointerLockChange);
  window.addEventListener('blur', onWindowBlur);
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
      window.removeEventListener('keydown', onKeyDown, true);
      window.removeEventListener('keyup', onKeyUp, true);
      target.removeEventListener('mousemove', onMouseMove, true);
      target.removeEventListener('mousedown', onMouseDown, true);
      target.removeEventListener('pointerlockchange', onPointerLockChange);
      window.removeEventListener('blur', onWindowBlur);
      target.removeEventListener('visibilitychange', onVisibilityChange);
      clearAll();
    },
  };
}
