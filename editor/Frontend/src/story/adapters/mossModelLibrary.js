/** 苔石模型资源库：只加载已批准的项目资源，实例共享几何体但独享材质。 */
import { Mesh } from 'three';
import { mergeGeometries } from 'three/addons/utils/BufferGeometryUtils.js';
import { GLTFLoader } from 'three/addons/loaders/GLTFLoader.js';

export const MOSS_MODEL_IDS = Object.freeze([
  'moss_golem',
  'traveler_mannequin',
  'traveler_helmet',
  'traveler_cuirass',
  'traveler_trousers',
  'traveler_boots',
  'traveler_blade',
  'traveler_axe',
  'grass_tile',
  'dirt_tile',
  'moss_boulder',
  'standing_stone',
  'old_oak',
  'ridge_pine',
  'broken_pillar',
  'ruined_wall',
]);
export const EQUIPMENT_MODELS = Object.freeze({
  'starter-helmet': 'traveler_helmet',
  'starter-cuirass': 'traveler_cuirass',
  'starter-trousers': 'traveler_trousers',
  'starter-boots': 'traveler_boots',
  'starter-blade': 'traveler_blade',
  'starter-axe': 'traveler_axe',
});

/** 按刚性部件和材质合并网格，保留全部动画节点，降低程序化造型的绘制调用。 */
function compactRigidMeshes(gltf) {
  const animatedNames = new Set(
    gltf.animations.flatMap((clip) => clip.tracks.map((track) => track.name.split('.')[0]))
  );
  const groups = [];
  const originals = new Set();
  gltf.scene.traverse((node) => {
    if (node.children.length) groups.push(node);
  });
  for (const parent of groups) {
    const batches = new Map();
    for (const child of parent.children) {
      if (
        !child.isMesh ||
        child.children.length ||
        Array.isArray(child.material) ||
        animatedNames.has(child.name)
      )
        continue;
      const batch = batches.get(child.material) || [];
      batch.push(child);
      batches.set(child.material, batch);
    }
    for (const [material, children] of batches) {
      if (children.length < 2) continue;
      const parts = children.map((child) => {
        originals.add(child.geometry);
        child.updateMatrix();
        const geometry = child.geometry.index
          ? child.geometry.toNonIndexed()
          : child.geometry.clone();
        // 这一套资源不含贴图或顶点色，仅保留形状和法线以统一网格属性。
        for (const name of Object.keys(geometry.attributes))
          if (!['position', 'normal'].includes(name)) geometry.deleteAttribute(name);
        return geometry.applyMatrix4(child.matrix);
      });
      const geometry = mergeGeometries(parts);
      for (const part of parts) part.dispose();
      if (!geometry) continue;
      const mesh = new Mesh(geometry, material);
      mesh.name = `${parent.name}_合并部件`;
      parent.remove(...children);
      parent.add(mesh);
    }
  }
  const retained = new Set();
  gltf.scene.traverse((node) => {
    if (node.geometry) retained.add(node.geometry);
  });
  for (const geometry of originals) if (!retained.has(geometry)) geometry.dispose();
  return gltf;
}

/** 部分资源失败时保留已成功模型，由调用方使用占位物，不中断游戏初始化。 */
export async function loadMossModelLibrary(config, loader = new GLTFLoader()) {
  const templates = new Map();
  const instances = new Set();
  const errors = [];
  let disposed = false;
  if (config?.enabled)
    await Promise.all(
      MOSS_MODEL_IDS.map(async (id) => {
        try {
          templates.set(
            id,
            compactRigidMeshes(await loader.loadAsync(`${config.baseUrl}${id}.glb`))
          );
        } catch (error) {
          errors.push(id);
          console.warn(`苔石模型加载失败，使用备用显示：${id}`, error);
        }
      })
    );
  function release(instance) {
    if (!instances.delete(instance)) return;
    instance.removeFromParent();
    instance.traverse((node) => {
      if (node.isMesh) for (const mat of [].concat(node.material)) mat.dispose();
    });
  }
  return {
    errors,
    has: (id) => templates.has(id) && !disposed,
    instantiate(id) {
      if (disposed || !templates.has(id)) return null;
      const template = templates.get(id);
      const instance = template.scene.clone(true);
      instance.name = `ModelInstance:${id}`;
      instance.animations = template.animations;
      instance.traverse((node) => {
        if (!node.isMesh) return;
        node.material = Array.isArray(node.material)
          ? node.material.map((mat) => mat.clone())
          : node.material.clone();
        node.castShadow = true;
        node.receiveShadow = true;
        node.userData.modelId = id;
      });
      instances.add(instance);
      return instance;
    },
    release,
    dispose() {
      if (disposed) return;
      disposed = true;
      for (const instance of instances) release(instance);
      // 原始资源在所有实例释放后统一销毁，避免重复释放共享几何体。
      const geometries = new Set(),
        materials = new Set(),
        textures = new Set();
      for (const { scene } of templates.values())
        scene.traverse((node) => {
          if (node.geometry) geometries.add(node.geometry);
          if (node.material) for (const mat of [].concat(node.material)) materials.add(mat);
        });
      for (const mat of materials) {
        for (const value of Object.values(mat)) if (value?.isTexture) textures.add(value);
        mat.dispose();
      }
      for (const geometry of geometries) geometry.dispose();
      for (const texture of textures) texture.dispose();
      templates.clear();
    },
  };
}
