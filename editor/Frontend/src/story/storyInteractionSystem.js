/**
 * 剧情模式交互系统：通过摄像机射线寻找目标并调用统一交互接口。
 */
import * as THREE from 'three';

export function createStoryInteractionSystem({ camera, scene, maxDistance = 6, onTargetChanged } = {}) {
  const raycaster = new THREE.Raycaster();
  const direction = new THREE.Vector3();
  let focusedTarget = null;

  function findTarget() {
    camera.getWorldDirection(direction);
    raycaster.set(camera.position, direction);
    const hits = raycaster.intersectObjects(scene.children, true);
    const hit = hits.find(({ object, distance }) => (
      distance <= maxDistance && object.userData?.interactive && !object.userData.disabled
    ));
    const nextTarget = hit?.object ?? null;
    if (nextTarget !== focusedTarget) {
      focusedTarget = nextTarget;
      onTargetChanged?.(focusedTarget);
    }
    return focusedTarget;
  }

  return {
    getFocusedTarget() {
      return findTarget();
    },

    getPrompt() {
      const target = findTarget();
      return target?.userData?.interactionPrompt ?? (target ? `按 F：${target.userData.name}` : '');
    },

    interact(context = {}) {
      const target = findTarget();
      const handler = target?.userData?.interact;
      if (target && typeof handler === 'function') return handler(context);
      return false;
    },
  };
}
