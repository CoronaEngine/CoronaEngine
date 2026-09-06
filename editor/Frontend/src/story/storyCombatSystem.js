/**
 * 剧情模式攻击系统：负责攻击冷却、摄像机射线和灰盒目标命中。
 */
import * as THREE from 'three';

export function createStoryCombatSystem({ camera, scene, cooldown = 350, damage = 25, onHit } = {}) {
  const raycaster = new THREE.Raycaster();
  const direction = new THREE.Vector3();
  let lastAttackAt = -Infinity;
  let lastHit = null;

  return {
    canAttack(now = performance.now()) {
      return now - lastAttackAt >= cooldown;
    },

    attack(now = performance.now()) {
      if (!this.canAttack(now)) return { accepted: false, target: null };
      lastAttackAt = now;
      camera.getWorldDirection(direction);
      raycaster.set(camera.position, direction);
      const hit = raycaster.intersectObjects(scene.children, true).find(({ object, distance }) => (
        distance <= 8 && object.userData?.combatTarget && !object.userData.disabled
      ));
      lastHit = hit?.object ?? null;
      if (lastHit) onHit?.(lastHit, damage);
      return { accepted: true, target: lastHit };
    },

    getLastHit() {
      return lastHit;
    },
  };
}
