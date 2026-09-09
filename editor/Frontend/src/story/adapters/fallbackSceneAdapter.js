/**
 * 剧情模式场景适配器：加载外部地形模型、基础地面、剧情占位物和编辑器场景快照。
 *
 * 当前适配器只负责视觉场景和灰盒玩法对象，不负责复杂地形碰撞。
 * 地形 GLB 文件放在 public/assets/story/terrain/terrain.glb。
 */
import * as THREE from 'three';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';
import { editorApi } from '@/api/editorApi.js';
import { STORY_ASSETS } from '@/story/config/storyAssetConfig.js';

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

function addStoryObjects(scene) {
  const objects = {};

  const boss = new THREE.Mesh(
    new THREE.IcosahedronGeometry(2.2, 1),
    material(0x8d3f4b, { emissive: 0x260b10, emissiveIntensity: 0.4 })
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
    material(0xd8b86c, { emissive: 0x6b4c15, emissiveIntensity: 0.7 })
  );
  bossRing.rotation.x = Math.PI / 2;
  bossRing.position.set(0, 0.08, -34);
  scene.add(bossRing);
  objects.bossRing = bossRing;

  const orb = new THREE.Mesh(
    new THREE.SphereGeometry(0.8, 24, 16),
    material(0x56c7db, { emissive: 0x155a72, emissiveIntensity: 1 })
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
  objects.orbLight = orbLight;

  const fragment = new THREE.Mesh(
    new THREE.OctahedronGeometry(0.75),
    material(0xf0b85e, { emissive: 0x7a3f08, emissiveIntensity: 0.8 })
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

/**
 * 应用资源中的变换配置。
 * @param {THREE.Object3D} object 场景对象。
 * @param {object} config 位置、旋转和缩放配置。
 */
function applyTransform(object, config) {
  object.position.fromArray(config.position);
  object.rotation.fromArray(config.rotation);
  object.scale.fromArray(config.scale);
}

/**
 * 从 GLB 场景中提取真实网格。
 *
 * 3ds Max 导出的 GLB 可能包含相机、灯光、辅助节点和额外的根节点变换。
 * 剧情模式只接管网格本身，避免导出的辅助节点改变地形朝向或位置。
 *
 * @param {THREE.Object3D} sourceScene GLTFLoader 返回的场景根节点。
 * @returns {THREE.Group} 剧情模式使用的地形根节点。
 */
function extractTerrainMeshes(sourceScene) {
  const terrain = new THREE.Group();
  const meshes = [];

  sourceScene.traverse((object) => {
    if (object.isMesh) meshes.push(object);
  });

  if (meshes.length === 0) {
    throw new Error('terrain GLB does not contain a mesh');
  }

  meshes.forEach((sourceMesh, index) => {
    const mesh = sourceMesh.clone();
    mesh.name = sourceMesh.name || `StoryTerrainMesh${index + 1}`;
    mesh.geometry = sourceMesh.geometry.clone();
    mesh.material = Array.isArray(sourceMesh.material)
      ? sourceMesh.material.map((value) => value.clone())
      : (sourceMesh.material?.clone?.() ?? sourceMesh.material);

    // 忽略 GLB 中网格的父级变换，使用剧情模式资源配置统一控制变换。
    mesh.position.set(0, 0, 0);
    mesh.rotation.set(0, 0, 0);
    mesh.scale.set(1, 1, 1);
    mesh.matrix.identity();
    mesh.matrixAutoUpdate = true;
    mesh.castShadow = true;
    mesh.receiveShadow = true;
    mesh.frustumCulled = false;
    mesh.userData = {
      ...sourceMesh.userData,
      storyTerrain: true,
    };

    mesh.geometry.computeBoundingBox();
    mesh.geometry.computeBoundingSphere();
    terrain.add(mesh);
  });

  terrain.name = 'StoryTerrain';
  terrain.userData.storyTerrain = true;
  return terrain;
}

/**
 * 异步加载剧情模式地形模型。
 * @param {THREE.Scene} scene 剧情模式场景。
 * @returns {Promise<THREE.Object3D|null>} 加载成功的地形根节点。
 */
async function addTerrainModel(scene) {
  if (!STORY_ASSETS.terrain.enabled) {
    return null;
  }

  const loader = new GLTFLoader();

  try {
    const result = await loader.loadAsync(STORY_ASSETS.terrain.url);
    const terrain = extractTerrainMeshes(result.scene);

    applyTransform(terrain, STORY_ASSETS.terrain);
    terrain.updateMatrixWorld(true);
    scene.add(terrain);
    return terrain;
  } catch (error) {
    console.warn('[StoryMode] terrain GLB unavailable; using the basic ground.', error);
    return null;
  }
}

/**
 * 将编辑器场景快照中的基础对象转换为剧情模式占位物。
 * @param {THREE.Scene} scene 剧情模式场景。
 * @param {Array<object>} actors 编辑器场景对象。
 */
function addSnapshotActors(scene, actors) {
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
      actor.name || '场景对象'
    );

    mesh.rotation.fromArray(rotation);
    mesh.scale.fromArray(scale);
    mesh.userData.actor = actor;
  });
}

/**
 * 释放 Three.js 场景中的几何体、材质和纹理。
 * @param {THREE.Object3D} root 场景根节点。
 */
function disposeSceneResources(root) {
  const disposedMaterials = new Set();
  const disposedTextures = new Set();

  root.traverse((object) => {
    object.geometry?.dispose?.();

    const materials = Array.isArray(object.material) ? object.material : [object.material];
    materials.filter(Boolean).forEach((currentMaterial) => {
      if (disposedMaterials.has(currentMaterial)) return;

      Object.values(currentMaterial).forEach((value) => {
        if (value?.isTexture && !disposedTextures.has(value)) {
          value.dispose();
          disposedTextures.add(value);
        }
      });
      currentMaterial.dispose?.();
      disposedMaterials.add(currentMaterial);
    });
  });
}

/**
 * 创建剧情模式场景。
 * @param {string} sceneName 编辑器场景快照名称。
 * @returns {Promise<object>} 场景及其销毁接口。
 */
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
    material(0x566879, { roughness: 0.96 })
  );
  ground.rotation.x = -Math.PI / 2;
  ground.receiveShadow = true;
  ground.userData.storyGround = true;
  scene.add(ground);

  const terrain = await addTerrainModel(scene);

  // 地形模型加载成功后隐藏视觉平面，避免平面遮挡地形。
  // 物理系统仍独立使用 groundY = 0，不依赖复杂地形碰撞。
  ground.visible = !terrain;

  addStoryObjects(scene);

  let actors = [];
  try {
    const snapshot = unwrap(await editorApi.scene.getSnapshot(sceneName));
    actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
  } catch (error) {
    console.warn('[StoryMode] scene snapshot unavailable; using the empty story scene.', error);
  }

  addSnapshotActors(scene, actors);

  return {
    scene,
    actors,
    terrain,
    storyObjects: scene.userData.storyObjects,
    dispose() {
      disposeSceneResources(scene);
    },
  };
}
