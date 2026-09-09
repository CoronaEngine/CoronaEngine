/**
 * 剧情模式攻击系统：负责攻击冷却、摄像机射线和灰盒目标命中。
 *
 * 运行时可以切换主世界和小世界场景；攻击系统不负责修改目标业务状态。
 */
import * as THREE from 'three';

const DEFAULT_COOLDOWN = 350;
const DEFAULT_DAMAGE = 25;
const DEFAULT_DISTANCE = 8;

/**
 * 创建剧情模式攻击系统。
 * @param {object} options 系统配置。
 * @returns {object} 攻击、冷却和命中结果接口。
 */
export function createStoryCombatSystem({
  camera,
  scene,
  cooldown = DEFAULT_COOLDOWN,
  damage = DEFAULT_DAMAGE,
  maxDistance = DEFAULT_DISTANCE,
  onHit,
} = {}) {
  const raycaster = new THREE.Raycaster();
  const direction = new THREE.Vector3();
  let activeScene = scene || null;
  let lastAttackAt = -Infinity;
  let lastHit = null;

  return {
    /** 切换射线检测使用的场景。 */
    setScene(nextScene) {
      activeScene = nextScene || null;
      lastHit = null;
    },

    /** 清理上一次命中结果。 */
    clearTarget() {
      lastHit = null;
    },

    /** 判断当前时间是否允许攻击。 */
    canAttack(now = performance.now()) {
      return now - lastAttackAt >= cooldown;
    },

    /** 发出攻击射线并返回命中目标。 */
    attack(now = performance.now()) {
      if (!this.canAttack(now)) {
        return { accepted: false, target: null };
      }

      lastAttackAt = now;
      lastHit = null;
      if (!camera || !activeScene) {
        return { accepted: true, target: null };
      }

      camera.getWorldDirection(direction);
      raycaster.set(camera.position, direction);
      const hit = raycaster.intersectObjects(activeScene.children, true).find(({ object, distance }) => (
        distance <= maxDistance
        && object.userData?.combatTarget
        && !object.userData.disabled
      ));
      lastHit = hit?.object || null;
      if (lastHit) {
        onHit?.(lastHit, damage);
      }
      return { accepted: true, target: lastHit };
    },

    /** 获取上一次攻击命中的目标。 */
    getLastHit() {
      return lastHit;
    },
  };
}
