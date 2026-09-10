/** 临时战斗几何体适配器；这些占位网格不是尚未批准的美术模型。 */
import * as THREE from 'three';
export function createMonsterSceneAdapter(scene, monsters) {
  const root = new THREE.Group();
  root.name = 'CombatTestPlaceholders';
  scene.add(root);
  const nodes = new Map();
  for (const m of monsters) {
    const node = new THREE.Mesh(
      new THREE.CapsuleGeometry(0.45, 0.6, 3, 6),
      new THREE.MeshStandardMaterial({ color: 0x758765, roughness: 0.95, flatShading: true })
    );
    node.userData = {
      monsterId: m.id,
      combatTarget: true,
      name: '测试怪物（占位）',
      health: m.health,
    };
    node.castShadow = true;
    root.add(node);
    nodes.set(m.id, node);
  }
  // 只有静态实体几何体阻挡移动和视线；拾取物与地面不参与此检测。
  const obstacles = [];
  scene.updateMatrixWorld(true);
  scene.traverse((node) => {
    if (
      node.isMesh &&
      !node.userData.monsterId &&
      !node.userData.storyGround &&
      !node.userData.interactive &&
      !node.userData.combatTarget &&
      !node.geometry?.type?.includes('Torus') &&
      node.visible
    ) {
      obstacles.push(new THREE.Box3().setFromObject(node));
    }
  });
  const ray = new THREE.Ray(),
    point = new THREE.Vector3();
  const origin = new THREE.Vector3(),
    direction = new THREE.Vector3();
  function clearSegment(a, b, height) {
    origin.set(a.x, a.y + height, a.z);
    direction.set(b.x, b.y, b.z).sub(origin);
    const length = direction.length();
    ray.set(origin, direction.normalize());
    return !obstacles.some(
      (box) => ray.intersectBox(box, point) && point.distanceTo(origin) < length
    );
  }
  return {
    visible(a, b) {
      return clearSegment(a, b, 1);
    },
    canMove(a, b) {
      point.set(b.x, b.y + 0.8, b.z);
      return (
        Math.abs(b.x) < 89 &&
        Math.abs(b.z) < 89 &&
        !obstacles.some((box) => box.distanceToPoint(point) < 0.45)
      );
    },
    sync() {
      for (const m of monsters) {
        const node = nodes.get(m.id);
        node.position.set(m.position.x, m.position.y + 0.8, m.position.z);
        node.rotation.y = m.yaw;
        node.visible = m.health > 0;
        node.userData.health = m.health;
        node.userData.disabled = m.health <= 0;
        node.material.emissive.setHex(m.flash > 0 ? 0x994433 : m.state === 'windup' ? 0x664400 : 0);
      }
    },
    dispose() {
      for (const node of nodes.values()) {
        node.geometry.dispose();
        node.material.dispose();
      }
      root.removeFromParent();
      nodes.clear();
    },
  };
}
