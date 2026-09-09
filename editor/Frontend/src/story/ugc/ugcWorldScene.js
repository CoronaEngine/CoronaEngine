/**
 * 小世界 Three.js 场景：创建独立地面、网格、灯光和灰盒对象。
 * 本模块不处理输入、背包或存档；它只负责序列化状态与场景节点之间的同步。
 */
import * as THREE from 'three';

const WORLD_SIZE = 64;
const BLOCK_SIZE = 1;
const DEFAULT_TARGET_HEALTH = 100;
const MATERIAL_COLORS = Object.freeze({
  'material-wood': 0x896447,
  'material-stone': 0x74808a,
  'material-metal': 0x668b92,
});

function colorForMaterial(materialId) {
  if (MATERIAL_COLORS[materialId]) return MATERIAL_COLORS[materialId];
  let hash = 0;
  for (const character of String(materialId || 'block')) {
    hash = ((hash << 5) - hash + character.charCodeAt(0)) | 0;
  }
  return 0x516b76 + (Math.abs(hash) % 0x2c2c2c);
}

function createMaterial(color, options = {}) {
  return new THREE.MeshStandardMaterial({
    color,
    roughness: 0.88,
    metalness: 0.02,
    ...options,
  });
}

function disposeObjectResources(object) {
  const materials = new Set();
  const textures = new Set();

  object.traverse((node) => {
    node.geometry?.dispose?.();
    const list = Array.isArray(node.material) ? node.material : [node.material];
    list.filter(Boolean).forEach((material) => {
      if (materials.has(material)) return;
      Object.values(material).forEach((value) => {
        if (value?.isTexture && !textures.has(value)) {
          textures.add(value);
          value.dispose();
        }
      });
      materials.add(material);
      material.dispose?.();
    });
  });
}

function createObjectGeometry(object) {
  if (object.type === 'block') return new THREE.BoxGeometry(BLOCK_SIZE, BLOCK_SIZE, BLOCK_SIZE);
  if (object.type === 'spawn') return new THREE.CylinderGeometry(0.35, 0.35, 0.12, 16);
  return new THREE.BoxGeometry(0.8, 1.8, 0.8);
}

function createObjectMaterial(object) {
  if (object.type === 'block') return createMaterial(colorForMaterial(object.materialId));
  if (object.type === 'spawn') return createMaterial(0xc6a15b);
  return createMaterial(0x9a5360);
}

function targetIsDisabled(object) {
  return (
    object.type === 'target' &&
    (Number(object.health ?? DEFAULT_TARGET_HEALTH) <= 0 || object.targetState === 'defeated')
  );
}

function updateObjectNode(node, object) {
  const isTarget = object.type === 'target';
  const disabled = targetIsDisabled(object);
  const health = Number.isFinite(object.health) ? object.health : DEFAULT_TARGET_HEALTH;

  node.position.fromArray(object.position);
  node.rotation.fromArray(object.rotation);
  node.scale.fromArray(object.scale);
  node.visible = !disabled;

  if (object.type === 'block' && node.material?.color) {
    node.material.color.setHex(colorForMaterial(object.materialId));
    node.userData.materialId = object.materialId;
  }

  node.userData.objectType = object.type;
  node.userData.interactive = isTarget;
  node.userData.combatTarget = isTarget;
  node.userData.health = health;
  node.userData.maxHealth = node.userData.maxHealth ?? DEFAULT_TARGET_HEALTH;
  node.userData.state = object.targetState ?? 'idle';
  node.userData.targetState = object.targetState ?? 'idle';
  node.userData.disabled = disabled;
  node.userData.interactionPrompt = isTarget ? '按 F：交互目标' : '';
}

function createObjectNode(object, handlers) {
  const node = new THREE.Mesh(createObjectGeometry(object), createObjectMaterial(object));
  const isTarget = object.type === 'target';

  if (object.type === 'block' || isTarget) {
    node.castShadow = true;
    node.receiveShadow = true;
  }

  node.name = `UgcObject_${object.id}`;
  node.userData = {
    storyUgcObject: true,
    objectId: object.id,
    name: isTarget ? '小世界目标' : object.type === 'spawn' ? '小世界出生点' : '小世界方块',
    materialId: object.materialId,
    maxHealth: DEFAULT_TARGET_HEALTH,
    interact: isTarget
      ? (context) =>
          handlers.onInteract?.(node, context) ?? {
            type: 'ugc-interact',
            objectId: object.id,
            target: node,
          }
      : undefined,
  };
  updateObjectNode(node, object);
  return node;
}

function createGround() {
  const ground = new THREE.Mesh(
    new THREE.PlaneGeometry(WORLD_SIZE, WORLD_SIZE),
    createMaterial(0x354b56, { roughness: 0.96 })
  );
  ground.rotation.x = -Math.PI / 2;
  ground.receiveShadow = true;
  ground.userData.storyUgcGround = true;
  return ground;
}

/**
 * 创建独立的小世界场景。
 * @param {object} state 可序列化的小世界状态。
 * @param {object} options 背景、雾和交互回调。
 * @returns {object} 场景、节点查询、同步和销毁接口。
 */
export function createUgcWorldScene(state, options = {}) {
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(options.background ?? 0x16242c);
  scene.fog = new THREE.Fog(options.fog ?? 0x16242c, 25, 90);
  scene.add(new THREE.HemisphereLight(0xd8edff, 0x24323b, 1.7));

  const sun = new THREE.DirectionalLight(0xffe0b0, 2.4);
  sun.position.set(-18, 28, 12);
  sun.castShadow = true;
  scene.add(sun);
  scene.add(new THREE.GridHelper(WORLD_SIZE, WORLD_SIZE, 0x49636d, 0x2b414b));
  scene.add(createGround());

  const objectsRoot = new THREE.Group();
  objectsRoot.name = 'UgcWorldObjects';
  scene.add(objectsRoot);
  const objectNodes = new Map();
  const handlers = { onInteract: options.onInteract };
  let disposed = false;

  function removeObjectNode(id) {
    const node = objectNodes.get(id);
    if (!node) return;
    objectsRoot.remove(node);
    disposeObjectResources(node);
    objectNodes.delete(id);
  }

  /**
   * 按对象 ID 增量同步场景，避免建造或逻辑变化时重建整个场景。
   * @param {object} nextState 最新的小世界状态。
   */
  function syncFromState(nextState) {
    if (disposed) return;

    const nextObjects = nextState?.objects || [];
    const nextIds = new Set(nextObjects.map((object) => object.id));

    for (const id of objectNodes.keys()) {
      if (!nextIds.has(id)) removeObjectNode(id);
    }

    for (const object of nextObjects) {
      let node = objectNodes.get(object.id);
      if (node && node.userData.objectType !== object.type) {
        removeObjectNode(object.id);
        node = null;
      }
      if (!node) {
        node = createObjectNode(object, handlers);
        objectsRoot.add(node);
        objectNodes.set(object.id, node);
      } else {
        updateObjectNode(node, object);
      }
    }
  }

  /** 根据当前状态恢复所有目标的显示状态，不创建新的场景节点。 */
  function resetTargets(nextState = state) {
    const targetState = nextState?.objects || [];
    for (const object of targetState) {
      if (object.type !== 'target') continue;
      const node = objectNodes.get(object.id);
      if (node) updateObjectNode(node, object);
    }
  }

  syncFromState(state);

  return {
    scene,
    objectNodes,
    syncFromState,
    setMode() {
      // 预留模式视觉差异；首版统一使用同一套灰盒场景。
    },
    setHandlers(nextHandlers = {}) {
      handlers.onInteract = nextHandlers.onInteract;
    },
    resetTargets,
    getObjectNode(id) {
      return objectNodes.get(id) || null;
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      disposeObjectResources(scene);
      scene.clear();
      objectNodes.clear();
    },
  };
}
