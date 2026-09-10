<!-- 背包三维穿戴预览：共用已载入模型，每次关闭背包释放实例与画布资源。 -->
<template>
  <div ref="host" class="equipment-preview" @pointerdown.stop @wheel.stop>
    <canvas v-if="!unavailable" ref="canvas" aria-label="三维装备人物，拖动可旋转" />
    <span v-if="unavailable" class="preview-note">模型预览暂不可用</span>
    <span v-else class="preview-note">拖动查看穿戴效果</span>
  </div>
</template>
<script setup>
import { onMounted, onUnmounted, ref, watch } from 'vue';
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { createEquipmentDisplay } from '../adapters/equipmentModelAdapter.js';
const props = defineProps({
  equipment: { type: Object, required: true },
  modelLibrary: { type: Object, default: null },
});
const canvas = ref(null),
  host = ref(null),
  unavailable = ref(false);
let renderer, controls, observer, display, scene, camera;
function render() {
  if (renderer && scene && camera) renderer.render(scene, camera);
}
onMounted(() => {
  display = createEquipmentDisplay(props.modelLibrary);
  if (!display.available) {
    unavailable.value = true;
    return;
  }
  try {
    renderer = new THREE.WebGLRenderer({ canvas: canvas.value, antialias: true, alpha: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    scene = new THREE.Scene();
    camera = new THREE.PerspectiveCamera(35, 1, 0.1, 20);
    camera.position.set(1.55, 1.4, 3.05);
    scene.add(display.root, new THREE.HemisphereLight(0xe5f1ff, 0x59432b, 2.5));
    const light = new THREE.DirectionalLight(0xffe3ae, 2.5);
    light.position.set(-2, 4, 3);
    scene.add(light);
    controls = new OrbitControls(camera, canvas.value);
    controls.target.set(0, 0.95, 0);
    controls.enablePan = false;
    controls.enableZoom = false;
    controls.minPolarAngle = Math.PI * 0.25;
    controls.maxPolarAngle = Math.PI * 0.7;
    controls.update();
    controls.addEventListener('change', render);
    display.sync(props.equipment);
    // 静态预览只在尺寸、槽位或转向变化时重绘，不新增长期动画循环。
    observer = new ResizeObserver(() => {
      const width = host.value.clientWidth,
        height = host.value.clientHeight;
      if (!width || !height) return;
      camera.aspect = width / height;
      camera.updateProjectionMatrix();
      renderer.setSize(width, height, false);
      render();
    });
    observer.observe(host.value);
  } catch (error) {
    unavailable.value = true;
    console.warn('装备模型预览初始化失败', error);
  }
});
watch(
  () => props.equipment,
  (value) => {
    display?.sync(value);
    render();
  },
  { deep: true }
);
onUnmounted(() => {
  observer?.disconnect();
  controls?.removeEventListener('change', render);
  controls?.dispose();
  display?.dispose();
  renderer?.dispose();
  renderer?.forceContextLoss();
});
</script>
<style scoped>
.equipment-preview {
  position: relative;
  height: 190px;
  width: 100%;
  margin-bottom: 8px;
  background: radial-gradient(ellipse, #45463555, transparent 72%);
}
canvas {
  display: block;
  width: 100%;
  height: 100%;
  cursor: grab;
  touch-action: none;
}
canvas:active {
  cursor: grabbing;
}
.preview-note {
  position: absolute;
  bottom: 2px;
  left: 0;
  right: 0;
  text-align: center;
  font-size: 10px;
  color: var(--ce-text-secondary);
  pointer-events: none;
}
@media (max-height: 760px) {
  .equipment-preview {
    height: 145px;
  }
}
</style>
