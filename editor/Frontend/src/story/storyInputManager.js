/**
 * 剧情模式输入：将 DOM 键鼠事件转换成逻辑动作，并统一管理鼠标锁定。
 * 所有剧情模式键鼠输入都经过此模块，业务层不直接依赖 DOM 按键字符串。
 */
const bindings = {
  KeyW: 'forward',
  KeyS: 'backward',
  KeyA: 'left',
  KeyD: 'right',
  Space: 'jump',
  KeyB: 'inventory',
  KeyM: 'map',
  KeyF: 'interact',
};

const editableTags = new Set(['INPUT', 'TEXTAREA', 'SELECT', 'BUTTON']);

export function createStoryInputManager(target = document) {
  const held = new Set();
  const pressed = new Set();
  const look = { x: 0, y: 0 };
  let pointerLocked = false;
  let mouseActive = false;

  const isEditableTarget = (event) => editableTags.has(event.target?.tagName);

  const onKeyDown = (event) => {
    if (isEditableTarget(event)) return;
    const action = bindings[event.code];
    if (!action) return;

    event.preventDefault();
    if (!event.repeat) pressed.add(action);
    held.add(action);
  };

  const onKeyUp = (event) => {
    const action = bindings[event.code];
    if (action) held.delete(action);
  };

  const onMouseMove = (event) => {
    if (!mouseActive && !pointerLocked) return;
    look.x += event.movementX || 0;
    look.y += event.movementY || 0;
  };

  const onMouseDown = (event) => {
    if (event.button !== 0 || (!mouseActive && !pointerLocked)) return;
    event.preventDefault();
    pressed.add('attack');
  };

  const onPointerLock = () => {
    pointerLocked = target.pointerLockElement != null;
    if (pointerLocked) mouseActive = true;
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
  };

  target.addEventListener('keydown', onKeyDown, true);
  target.addEventListener('keyup', onKeyUp, true);
  target.addEventListener('mousemove', onMouseMove, true);
  target.addEventListener('mousedown', onMouseDown, true);
  target.addEventListener('pointerlockchange', onPointerLock);
  window.addEventListener('blur', onWindowBlur);

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
    setMouseActive: (active) => {
      mouseActive = Boolean(active);
      if (!mouseActive) clearTransient();
    },
    clearTransient,
    clearAll,
    dispose: () => {
      target.removeEventListener('keydown', onKeyDown, true);
      target.removeEventListener('keyup', onKeyUp, true);
      target.removeEventListener('mousemove', onMouseMove, true);
      target.removeEventListener('mousedown', onMouseDown, true);
      target.removeEventListener('pointerlockchange', onPointerLock);
      window.removeEventListener('blur', onWindowBlur);
      clearAll();
    },
  };
}
