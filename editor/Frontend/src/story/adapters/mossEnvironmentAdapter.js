import { Group } from 'three';

export function addMossEnvironment(scene, library) {
  const root = new Group();
  root.name = 'MossRuinsEnvironment';
  const placements = [
    ['old_oak', -7, -10, 0.3],
    ['ridge_pine', 10, -7, -0.2],
    ['old_oak', -17, -22, 1.2],
    ['ridge_pine', 19, -26, 0.5],
    ['ridge_pine', -11, -37, 0.4],
    ['old_oak', 12, -39, -0.5],
    ['moss_boulder', -4.5, -8, 0.3],
    ['standing_stone', 10, -14, -0.4],
    ['moss_boulder', 17, -19, 1],
    ['standing_stone', -18, -31, 0.8],
    ['broken_pillar', -5, -19, 0.2],
    ['broken_pillar', 5, -26, -0.5],
    ['ruined_wall', -9, -18, 0.3],
    ['ruined_wall', 10, -31, -0.5],
  ];
  for (const [id, x, z, yaw] of placements) {
    const node = library.instantiate(id);
    if (!node) continue;
    node.position.set(x, 0, z);
    node.rotation.y = yaw;
    root.add(node);
  }
  // 地块表面与原程序地面齐平，继续使用原有平地物理，不引入隐形台阶。
  for (const [cx, cz] of [
    [-7, -10],
    [10, -7],
    [-9, -18],
    [10, -31],
  ]) {
    for (let x = -1; x <= 1; x++)
      for (let z = -1; z <= 1; z++) {
        const node = library.instantiate(x === 0 && z === 0 ? 'dirt_tile' : 'grass_tile');
        if (!node) continue;
        node.position.set(cx + x * 2, -0.335, cz + z * 2);
        node.traverse((mesh) => {
          if (mesh.isMesh) {
            mesh.userData.storyGround = true;
            mesh.userData.nonBlocking = true;
          }
        });
        root.add(node);
      }
  }
  scene.add(root);
  return root;
}
