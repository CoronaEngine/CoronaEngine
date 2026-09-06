/**
 * 剧情模式灰盒场景适配器：读取编辑器场景快照，并补充可交互的测试场景。
 */
import * as THREE from 'three';
import { editorApi } from '@/api/editorApi.js';

const unwrap = (value) => value?.data ?? value ?? {};

function material(color, options = {}) {
  return new THREE.MeshStandardMaterial({
    color,
    roughness: 0.78,
    metalness: 0.05,
    ...options,
  });
}

function addBox(scene, position, size, color, name, options = {}) {
  const mesh = new THREE.Mesh(new THREE.BoxGeometry(...size), material(color, options));
  mesh.position.set(...position);
  mesh.castShadow = true;
  mesh.receiveShadow = true;
  mesh.userData.name = name;
  scene.add(mesh);
  return mesh;
}

function addTree(scene, x, z, scale = 1) {
  const trunk = new THREE.Mesh(
    new THREE.CylinderGeometry(0.24 * scale, 0.34 * scale, 2.2 * scale, 8),
    material(0x604633),
  );
  trunk.position.set(x, 1.1 * scale, z);
  trunk.castShadow = true;
  scene.add(trunk);

  const crown = new THREE.Mesh(
    new THREE.ConeGeometry(1.25 * scale, 2.7 * scale, 8),
    material(0x315c4b),
  );
  crown.position.set(x, 3.1 * scale, z);
  crown.castShadow = true;
  scene.add(crown);
}

function addTestEnvironment(scene) {
  const objects = {};
  const grid = new THREE.GridHelper(160, 80, 0x7d8f9f, 0x566676);
  grid.position.y = 0.012;
  grid.material.transparent = true;
  grid.material.opacity = 0.28;
  scene.add(grid);

  addBox(scene, [0, 1.5, -16], [10, 3, 1], 0x425a6f, '灰盒遗迹入口');
  addBox(scene, [-4.5, 3.2, -16], [1.2, 6.4, 1.2], 0x667f91, '左侧遗迹柱');
  addBox(scene, [4.5, 3.2, -16], [1.2, 6.4, 1.2], 0x667f91, '右侧遗迹柱');
  addBox(scene, [0, 6.2, -16], [10, 1.2, 1.2], 0x7892a3, '遗迹横梁');

  addBox(scene, [-11, 0.7, -10], [2, 1.4, 2], 0x806b55, '采集石');
  addBox(scene, [11, 0.6, -15], [1.6, 1.2, 1.6], 0x806b55, '采集石');
  addBox(scene, [-8, 0.45, -28], [1.2, 0.9, 1.2], 0x687b88, '采集石');
  addTree(scene, -14, -7, 1.1);
  addTree(scene, 14, -10, 1.25);
  addTree(scene, -12, -28, 1.5);
  addTree(scene, 13, -32, 1.35);

  const boss = new THREE.Mesh(
    new THREE.IcosahedronGeometry(2.2, 1),
    material(0x8d3f4b, { emissive: 0x260b10, emissiveIntensity: 0.4 }),
  );
  boss.position.set(0, 2.2, -34);
  boss.castShadow = true;
  boss.userData = {
    name: '灰盒 Boss',
    interactive: true,
    combatTarget: true,
    health: 100,
    maxHealth: 100,
    interactionPrompt: '按 F：查看 Boss 状态',
  };
  scene.add(boss);
  objects.boss = boss;

  const bossRing = new THREE.Mesh(
    new THREE.TorusGeometry(3.2, 0.08, 8, 48),
    material(0xd8b86c, { emissive: 0x6b4c15, emissiveIntensity: 0.7 }),
  );
  bossRing.rotation.x = Math.PI / 2;
  bossRing.position.set(0, 0.08, -34);
  scene.add(bossRing);

  const orb = new THREE.Mesh(
    new THREE.SphereGeometry(0.8, 24, 16),
    material(0x56c7db, { emissive: 0x155a72, emissiveIntensity: 1 }),
  );
  orb.position.set(6, 1.2, -12);
  orb.userData = {
    name: '世界小球',
    interactive: true,
    itemId: 'world-orb-demo',
    interactionPrompt: '按 F：拾取世界小球',
  };
  scene.add(orb);
  objects.orb = orb;

  const orbLight = new THREE.PointLight(0x36d7ff, 5, 8);
  orbLight.position.copy(orb.position);
  scene.add(orbLight);

  const fragment = new THREE.Mesh(
    new THREE.OctahedronGeometry(0.75),
    material(0xf0b85e, { emissive: 0x7a3f08, emissiveIntensity: 0.8 }),
  );
  fragment.position.set(-6, 1.1, -12);
  fragment.userData = {
    name: '世界碎片',
    interactive: true,
    itemId: 'world-fragment-demo',
    interactionPrompt: '按 F：拾取世界碎片',
  };
  scene.add(fragment);
  objects.fragment = fragment;

  scene.userData.storyObjects = objects;
}

export async function createFallbackScene(sceneName = '场景1') {
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0x1b2b3a);
  scene.fog = new THREE.Fog(0x1b2b3a, 30, 110);
  scene.add(new THREE.HemisphereLight(0xd9edff, 0x263342, 1.8));

  const sun = new THREE.DirectionalLight(0xffe3b0, 3.2);
  sun.position.set(-20, 30, 12);
  sun.castShadow = true;
  sun.shadow.mapSize.set(1024, 1024);
  scene.add(sun);

  const ground = new THREE.Mesh(
    new THREE.PlaneGeometry(200, 200),
    material(0x566879, { roughness: 0.96 }),
  );
  ground.rotation.x = -Math.PI / 2;
  ground.receiveShadow = true;
  scene.add(ground);
  addTestEnvironment(scene);

  let actors = [];
  try {
    const snapshot = unwrap(await editorApi.scene.getSnapshot(sceneName));
    actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
  } catch (error) {
    console.warn('[StoryMode] scene snapshot unavailable; using graybox environment:', error);
  }

  actors.forEach((actor) => {
    const transform = actor.geometry || actor.transform || {};
    const position = Array.isArray(transform.position) ? transform.position : [0, 0.5, 0];
    const rotation = Array.isArray(transform.rotation) ? transform.rotation : [0, 0, 0];
    const scale = Array.isArray(transform.scale) ? transform.scale : [1, 1, 1];
    const mesh = addBox(
      scene,
      position,
      [1, 1, 1],
      actor.type === 'light' ? 0xffd166 : 0x71869b,
      actor.name || '场景对象',
    );
    mesh.rotation.fromArray(rotation);
    mesh.scale.fromArray(scale);
    mesh.userData.actor = actor;
  });

  return {
    scene,
    actors,
    storyObjects: scene.userData.storyObjects,
    dispose() {
      scene.traverse((object) => {
        object.geometry?.dispose?.();
        if (Array.isArray(object.material)) {
          object.material.forEach((value) => value.dispose?.());
        } else {
          object.material?.dispose?.();
        }
      });
    },
  };
}
