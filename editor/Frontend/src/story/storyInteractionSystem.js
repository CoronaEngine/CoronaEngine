/**
 * 剧情模式交互系统：通过摄像机射线寻找目标并调用统一交互接口。
 *
 * 运行时可以切换主世界和小世界场景；系统本身不负责创建或销毁场景。
 */
import * as THREE from 'three';

const DEFAULT_MAX_DISTANCE = 6;

/**
 * 创建剧情模式交互系统。
 * @param {object} options 系统配置。
 * @returns {object} 交互目标查询和触发接口。
 */
export function createStoryInteractionSystem({
  camera,
  scene,
  maxDistance = DEFAULT_MAX_DISTANCE,
  onTargetChanged,
} = {}) {
  const raycaster = new THREE.Raycaster();
  const direction = new THREE.Vector3();
  let activeScene = scene || null;
  let focusedTarget = null;

  function notifyTargetChanged(nextTarget) {
    if (nextTarget === focusedTarget) return;
    focusedTarget = nextTarget;
    onTargetChanged?.(focusedTarget);
  }

  function findTarget() {
    if (!camera || !activeScene) {
      notifyTargetChanged(null);
      return null;
    }

    camera.getWorldDirection(direction);
    raycaster.set(camera.position, direction);
    const hits = raycaster.intersectObjects(activeScene.children, true);
    const hit = hits.find(({ object, distance }) => (
      distance <= maxDistance
      && object.userData?.interactive
      && !object.userData.disabled
    ));
    notifyTargetChanged(hit?.object || null);
    return focusedTarget;
  }

  return {
    /** 切换射线检测使用的场景。 */
    setScene(nextScene) {
      activeScene = nextScene || null;
      notifyTargetChanged(null);
    },

    /** 清理当前聚焦目标。 */
    clearTarget() {
      notifyTargetChanged(null);
    },

    /** 获取摄像机前方当前可交互目标。 */
    getFocusedTarget() {
      return findTarget();
    },

    /** 获取当前目标的交互提示。 */
    getPrompt() {
      const target = findTarget();
      return target?.userData?.interactionPrompt
        || (target ? `按 F：${target.userData.name || '交互'}` : '');
    },

    /** 调用当前目标的交互处理器。 */
    interact(context = {}) {
      const target = findTarget();
      const handler = target?.userData?.interact;
      if (target && typeof handler === 'function') {
        return handler(context);
      }
      return false;
    },
  };
}
