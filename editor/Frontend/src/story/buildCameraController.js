/** 仅建造模式使用的自由摄像机；输入限定于画布操作，不改变玩家身体状态。 */
import * as THREE from 'three';
export function createBuildCameraController(camera, canvas, host = window) {
  const keys = new Set();
  const position = new THREE.Vector3(8, 10, 14);
  let yaw = 0.5,
    pitch = -0.5,
    speed = 12,
    active = false,
    dragging = false;
  const forward = new THREE.Vector3(),
    right = new THREE.Vector3(),
    movement = new THREE.Vector3();
  const codes = new Set([
    'KeyW',
    'KeyA',
    'KeyS',
    'KeyD',
    'KeyQ',
    'KeyE',
    'ArrowLeft',
    'ArrowRight',
    'ArrowUp',
    'ArrowDown',
  ]);
  const clear = () => {
    keys.clear();
    dragging = false;
  };
  const editable = (event) =>
    event.target?.closest?.('input, textarea, select, button, [contenteditable="true"]');
  const down = (event) => {
    if (active && !editable(event) && codes.has(event.code)) {
      keys.add(event.code);
      event.preventDefault();
    }
  };
  const up = (event) => keys.delete(event.code);
  // 编辑输入框、拖动界面和切换焦点时，清除残留的摄像机移动按键。
  const focus = (event) => {
    if (editable(event)) clear();
  };
  const pointerDown = (event) => {
    if (active && event.button === 2) {
      dragging = true;
      canvas.setPointerCapture?.(event.pointerId);
      event.preventDefault();
    }
  };
  const pointerMove = (event) => {
    if (!active || !dragging) return;
    yaw -= (event.movementX * Math.PI) / 1200;
    pitch = Math.max(-1.45, Math.min(1.45, pitch - (event.movementY * Math.PI) / 1200));
  };
  const pointerUp = (event) => {
    dragging = false;
    if (canvas.hasPointerCapture?.(event.pointerId)) canvas.releasePointerCapture(event.pointerId);
  };
  const context = (event) => {
    if (active) event.preventDefault();
  };
  const apply = () => {
    camera.position.copy(position);
    camera.rotation.set(pitch, yaw, 0, 'YXZ');
  };
  const wheel = (event) => {
    if (!active) return;
    event.preventDefault();
    if (event.shiftKey)
      speed = Math.max(0.6, Math.min(120, speed + (event.deltaY > 0 ? -1.2 : 1.2)));
    else {
      apply();
      camera.getWorldDirection(forward);
      position.addScaledVector(forward, (Math.sign(-event.deltaY) * speed) / 6);
      apply();
    }
  };
  const listeners = [
    [host, 'keydown', down],
    [host, 'keyup', up],
    [host, 'blur', clear],
    [host, 'focusin', focus],
    [host, 'dragstart', clear],
    [canvas, 'pointerdown', pointerDown],
    [canvas, 'pointermove', pointerMove],
    [canvas, 'pointerup', pointerUp],
    [canvas, 'pointercancel', pointerUp],
    [canvas, 'contextmenu', context],
    [canvas, 'wheel', wheel],
  ];
  for (const [target, event, handler] of listeners)
    target.addEventListener(event, handler, { passive: false });
  return {
    setActive(value) {
      if (active !== value) clear();
      active = value;
    },
    clear,
    reset() {
      position.set(8, 10, 14);
      yaw = 0.5;
      pitch = -0.5;
      speed = 12;
      clear();
    },
    update(delta) {
      if (!active) return;
      const dt = Math.min(0.05, Math.max(0, delta));
      yaw += ((keys.has('ArrowLeft') ? 1 : 0) - (keys.has('ArrowRight') ? 1 : 0)) * 2 * dt;
      pitch = Math.max(
        -1.45,
        Math.min(
          1.45,
          pitch + ((keys.has('ArrowUp') ? 1 : 0) - (keys.has('ArrowDown') ? 1 : 0)) * 2 * dt
        )
      );
      apply();
      camera.getWorldDirection(forward);
      right.set(Math.cos(yaw), 0, -Math.sin(yaw));
      movement.set(0, 0, 0);
      movement.addScaledVector(forward, Number(keys.has('KeyW')) - Number(keys.has('KeyS')));
      movement.addScaledVector(right, Number(keys.has('KeyD')) - Number(keys.has('KeyA')));
      movement.y += Number(keys.has('KeyQ')) - Number(keys.has('KeyE'));
      if (movement.lengthSq()) position.addScaledVector(movement.normalize(), speed * dt);
      apply();
    },
    getState() {
      return { position: position.toArray(), yaw, pitch, speed, active, held: keys.size };
    },
    dispose() {
      clear();
      for (const [target, event, handler] of listeners) target.removeEventListener(event, handler);
    },
  };
}
