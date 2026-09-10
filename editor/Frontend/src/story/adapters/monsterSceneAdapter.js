/** 怪物场景适配器：已批准的苔石怪动画、逐网格命中及加载失败占位回退。 */
import * as THREE from 'three';
export function createMonsterSceneAdapter(scene, monsters, library = null) {
  const root = new THREE.Group();
  root.name = 'StoryMonsters';
  scene.add(root);
  const nodes = new Map();
  for (const m of monsters) {
    const visual = library?.instantiate('moss_golem');
    const node = new THREE.Group();
    let fallback = null;
    if (visual) {
      // 外层节点只负责世界位姿，内层动画保留模型自身的根节点运动。
      visual.scale.setScalar(0.72);
      node.add(visual);
    } else {
      fallback = new THREE.Mesh(
        new THREE.CapsuleGeometry(0.45, 0.6, 3, 6),
        new THREE.MeshStandardMaterial({ color: 0x758765, roughness: 0.95, flatShading: true })
      );
      fallback.position.y = 0.8;
      node.add(fallback);
    }
    const meshes = [];
    node.traverse((mesh) => {
      if (!mesh.isMesh) return;
      mesh.userData = {
        ...mesh.userData,
        monsterId: m.id,
        combatTarget: true,
        name: visual ? '苔石守望者' : '测试怪物（模型加载失败，占位）',
        health: m.health,
      };
      mesh.castShadow = true;
      meshes.push(mesh);
    });
    const mixer = visual ? new THREE.AnimationMixer(visual) : null;
    const actions = new Map(
      (visual?.animations || []).map((clip) => {
        const action = mixer.clipAction(clip);
        if (['attack', 'hit', 'death'].includes(clip.name)) {
          action.setLoop(THREE.LoopOnce, 1);
          action.clampWhenFinished = true;
        }
        // 动画打击点原在 0.5 秒，缩放到现有逻辑的 0.35 秒，不改变伤害规则。
        if (clip.name === 'attack') action.timeScale = 0.5 / 0.35;
        return [clip.name, action];
      })
    );
    root.add(node);
    nodes.set(m.id, {
      node,
      visual,
      fallback,
      meshes,
      mixer,
      actions,
      current: null,
      previousState: null,
      previousFlash: 0,
      hitTime: 0,
    });
  }
  // 只有静态实体几何体阻挡移动和视线；拾取物与地面不参与此检测。
  const obstacles = [];
  scene.updateMatrixWorld(true);
  scene.traverse((node) => {
    if (
      node.isMesh &&
      !node.userData.monsterId &&
      !node.userData.storyGround &&
      !node.userData.nonBlocking &&
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
    /** 暂停时传入零时间：保留画面但不推进动作，也不补算动画时间。 */
    sync(delta = 0) {
      const dt = Math.min(0.05, Math.max(0, delta));
      for (const m of monsters) {
        const record = nodes.get(m.id);
        const { node, meshes, mixer, actions } = record;
        node.position.set(m.position.x, m.position.y, m.position.z);
        node.rotation.y = m.yaw;
        node.visible = Boolean(record.visual) || m.health > 0;
        if (m.flash > record.previousFlash) record.hitTime = 0.4;
        record.hitTime = Math.max(0, record.hitTime - dt);
        let clip = 'idle';
        if (m.health <= 0) clip = 'death';
        else if (
          m.state === 'windup' ||
          (m.state === 'cooldown' &&
            record.current === 'attack' &&
            actions.get('attack')?.isRunning())
        )
          clip = 'attack';
        else if (record.hitTime > 0) clip = 'hit';
        else if (['wander', 'chase', 'return'].includes(m.state)) clip = 'walk';
        const restartAttack = m.state === 'windup' && record.previousState !== 'windup';
        if (record.current !== clip || restartAttack) {
          const previous = actions.get(record.current),
            next = actions.get(clip);
          previous?.fadeOut(0.08);
          next?.reset().fadeIn(0.08).play();
          record.current = clip;
        }
        const walking = actions.get('walk');
        if (walking) walking.timeScale = m.state === 'wander' ? 1 : 1.7;
        mixer?.update(dt);
        for (const mesh of meshes) {
          // 死亡动画仍可见，但尸体既不接受攻击也不遮挡后面的活目标。
          mesh.userData.health = m.health;
          mesh.userData.disabled = m.health <= 0;
          mesh.userData.nonBlocking = m.health <= 0;
          for (const mat of [].concat(mesh.material))
            mat.emissive?.setHex(
              m.flash > 0 && m.health > 0 ? 0x994433 : m.state === 'windup' ? 0x664400 : 0
            );
        }
        record.previousFlash = m.flash;
        record.previousState = m.state;
      }
    },
    dispose() {
      for (const { mixer, visual, fallback } of nodes.values()) {
        mixer?.stopAllAction();
        if (visual) {
          mixer.uncacheRoot(visual);
          library.release(visual);
        }
        if (fallback) {
          fallback.geometry.dispose();
          fallback.material.dispose();
        }
      }
      root.removeFromParent();
      nodes.clear();
    },
  };
}
