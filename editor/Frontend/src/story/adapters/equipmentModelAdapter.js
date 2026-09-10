/** 装备外观只读取槽位，不修改物品实例、护甲或存档数据。 */
import * as THREE from 'three';
import { EQUIPMENT_MODELS } from './mossModelLibrary.js';

export function createEquipmentDisplay(library) {
  const root = new THREE.Group();
  const body = library?.instantiate('traveler_mannequin');
  if (body) root.add(body);
  const slots = new Map();
  return {
    root,
    available: Boolean(body),
    sync(equipment) {
      for (const slot of ['head', 'chest', 'legs', 'feet', 'mainHand']) {
        const id = EQUIPMENT_MODELS[equipment?.[slot]?.id];
        const old = slots.get(slot);
        if (old?.id === id) continue;
        if (old) {
          library.release(old.node);
          slots.delete(slot);
        }
        if (!id) continue;
        const node = library?.instantiate(id);
        if (!node) continue;
        // 防具保留底模坐标，刀斧原点为握持点，对齐展示体右手。
        if (slot === 'mainHand') {
          node.position.set(-0.48, 0.81, 0.09);
          node.rotation.z = Math.PI;
        }
        root.add(node);
        slots.set(slot, { id, node });
      }
    },
    dispose() {
      if (body) library.release(body);
      for (const { node } of slots.values()) library.release(node);
      slots.clear();
      root.removeFromParent();
    },
  };
}

/** 第一人称武器采用独立叠加场景，不进入攻击射线，也不进入小世界建造场景。 */
export function createFirstPersonEquipment(library) {
  const scene = new THREE.Scene();
  const camera = new THREE.PerspectiveCamera(55, 1, 0.05, 5);
  scene.add(new THREE.HemisphereLight(0xe9f2ff, 0x4b4032, 2.4));
  const light = new THREE.DirectionalLight(0xffe4b1, 2.5);
  light.position.set(-1, 3, 2);
  scene.add(light);
  const grip = new THREE.Group();
  grip.position.set(0.32, -0.39, -0.9);
  grip.rotation.set(-0.18, -0.4, -0.22);
  grip.scale.setScalar(0.52);
  scene.add(grip);
  let current = null,
    currentId = null,
    swingTime = 0;
  return {
    swing() {
      swingTime = 0.28;
    },
    sync(equipment, delta) {
      const id = EQUIPMENT_MODELS[equipment?.mainHand?.id] || null;
      if (currentId !== id) {
        if (current) library.release(current);
        current = id ? library?.instantiate(id) : null;
        currentId = id;
        if (current) grip.add(current);
      }
      swingTime = Math.max(0, swingTime - delta);
      const swing = Math.sin((swingTime / 0.28) * Math.PI);
      grip.rotation.z = -0.22 + swing * 0.95;
      grip.rotation.x = -0.18 - swing * 0.8;
    },
    render(renderer, aspect) {
      if (!current) return;
      if (camera.aspect !== aspect) {
        camera.aspect = aspect;
        camera.updateProjectionMatrix();
      }
      const autoClear = renderer.autoClear;
      renderer.autoClear = false;
      renderer.clearDepth();
      renderer.render(scene, camera);
      renderer.autoClear = autoClear;
    },
    dispose() {
      if (current) library.release(current);
      current = null;
    },
  };
}
