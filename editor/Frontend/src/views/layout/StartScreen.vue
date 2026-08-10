<template>
  <div class="start-screen-root">
    <div ref="canvasContainer" class="canvas-container"></div>

    <!-- 标题固定居中不动 -->
    <div class="main-title">
      <span class="title-word">Corona</span>
      <span class="title-word">Engine</span>
    </div>

    <!-- 导航按钮（GSAP 控制居中/左移） -->
    <div ref="navContainer" class="nav-container">
      <button
        v-for="item in navItems"
        :key="item.id"
        class="nav-btn"
        :class="{ active: activeNav === item.id }"
        @click="handleNavClick(item.id)"
      >
        {{ t(item.label) }}
      </button>
    </div>

    <!-- 右侧面板 -->
    <div v-if="showPanel" ref="panelRef" class="page-panel">
      <div class="page-panel-body">
        <div v-if="activePage === 'panel-exit'" key="panel-exit" class="exit-panel-content">
          <div class="exit-panel-icon">
            <svg width="48" height="48" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4"/><polyline points="16 17 21 12 16 7"/><line x1="21" y1="12" x2="9" y2="12"/></svg>
          </div>
          <h2 class="exit-panel-title">断开连接</h2>
          <p class="exit-panel-desc">切断与 Corona 系统的连接后，所有未保存的宇宙演化进程将在后台处于休眠状态。</p>
          <div class="exit-panel-actions">
            <button class="exit-action cancel" @click="handleBack">取消</button>
            <button class="exit-action confirm" @click="confirmExit">确认离开</button>
          </div>
        </div>
        <Transition v-else name="page-fade" mode="out-in"><component :is="pageComponent" :key="activePage" />
        </Transition>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, shallowRef, nextTick, onMounted, onUnmounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { appService } from '@/services/appService.js';
import * as THREE from 'three';
import gsap from 'gsap';

import NewGame from './NewGame.vue';
import RecentGames from './RecentGames.vue';
import JoinGame from './JoinGame.vue';

const { t } = useI18n();
const canvasContainer = ref(null);
const navContainer = ref(null);
const panelRef = ref(null);
const activeNav = ref(null);

const showPanel = ref(false);
const activePage = ref(null);
const pageComponent = shallowRef(null);
const hasMoved = ref(false);

const pageMap = {
  'panel-new':      { component: NewGame,     name: 'start.newGame' },
  'panel-continue': { component: RecentGames, name: 'start.continueGame' },
  'panel-multi':    { component: JoinGame,    name: 'start.joinGame' },
};

const navItems = [
  { id: 'panel-new',      label: 'start.newGame',      page: true },
  { id: 'panel-continue', label: 'start.continueGame', page: true },
  { id: 'panel-multi',    label: 'start.joinGame',     page: true },
  { id: 'panel-exit',     label: 'start.leaveGame',    page: false },
];

let scene, camera, renderer, particleSystem, clock;
let mouseX = 0, mouseY = 0;
let animationFrameId = null;
let resizeTimer = null;

const animateParticles = (id) => {
  const d = 0.6;
  switch (id) {
    case 'panel-new':
      gsap.to(particleSystem.material.color, { r: 0.4, g: 0.66, b: 1, duration: d, force3D: true });
      gsap.to(particleSystem.scale, { x: 1, y: 1, z: 1, duration: d, force3D: true });
      gsap.to(camera.position, { z: 80, duration: d, ease: 'power2.out', force3D: true });
      break;
    case 'panel-continue':
      gsap.to(particleSystem.material.color, { r: 0.1, g: 0.8, b: 0.4, duration: d, force3D: true });
      gsap.to(particleSystem.scale, { x: 1, y: 1, z: 1, duration: d, force3D: true });
      gsap.to(camera.position, { z: 42, duration: d, ease: 'power2.inOut', force3D: true });
      break;
    case 'panel-multi':
      gsap.to(particleSystem.material.color, { r: 1.0, g: 0.4, b: 0.1, duration: d, force3D: true });
      gsap.to(particleSystem.scale, { x: 1, y: 1, z: 1, duration: d, force3D: true });
      gsap.to(camera.position, { z: 65, duration: d, ease: 'power2.out', force3D: true });
      break;
    case 'panel-exit':
      gsap.to(particleSystem.material.color, { r: 0.8, g: 0.1, b: 0.2, duration: d, force3D: true });
      gsap.to(particleSystem.scale, { x: 0.4, y: 0.4, z: 0.4, duration: d, ease: 'power3.inOut', force3D: true });
      gsap.to(camera.position, { z: 100, duration: d, ease: 'power3.inOut', force3D: true });
      break;
  }
};

const showAndSlidePanel = async (id, component) => {
  activePage.value = id;
  pageComponent.value = component;
  showPanel.value = true;
  await nextTick();
  gsap.fromTo(panelRef.value,
    { x: '105%' },
    { x: '0%', duration: 0.45, ease: 'power3.out', force3D: true }
  );
};

const handleNavClick = async (id) => {
  if (id === 'panel-exit') {
    animateParticles(id);
    activeNav.value = id;
    if (!hasMoved.value) {
      hasMoved.value = true;
      gsap.to(navContainer.value, {
        x: '6vw', xPercent: 0,
        duration: 0.7, ease: 'power3.inOut', force3D: true,
      });
    }
    await showAndSlidePanel('panel-exit', null);
    return;
  }

  if (activePage.value === id && showPanel.value) {
    handleBack();
    return;
  }

  activeNav.value = id;
  animateParticles(id);

  if (!hasMoved.value) {
    hasMoved.value = true;
    gsap.to(navContainer.value, {
      x: '6vw', xPercent: 0,
      duration: 0.7, ease: 'power3.inOut', force3D: true,
    });
  }

  await showAndSlidePanel(id, pageMap[id].component);
};

const handleBack = () => {
  if (!panelRef.value) return;
  gsap.to(panelRef.value, {
    x: '105%', duration: 0.25, ease: 'power2.in', force3D: true,
    onComplete: () => {
      showPanel.value = false;
      activePage.value = null;
      activeNav.value = null;
      pageComponent.value = null;

      gsap.to(navContainer.value, {
        x: '50vw', xPercent: -50,
        duration: 0.5, ease: 'power3.inOut', force3D: true,
      });
      hasMoved.value = false;
    },
  });
};

const confirmExit = () => {
  if (!panelRef.value) return;
  gsap.to(panelRef.value, {
    x: '105%', duration: 0.25, ease: 'power2.in', force3D: true,
    onComplete: () => {
      showPanel.value = false;
      appService.closeProcess();
    },
  });
};

const initThree = () => {
  const container = canvasContainer.value;
  if (!container) return;

  scene = new THREE.Scene();
  scene.fog = new THREE.FogExp2(0x030305, 0.012);

  camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.1, 1000);
  camera.position.set(0, 30, 70);

  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  renderer.setClearColor(0x000000, 0);
  renderer.setSize(window.innerWidth, window.innerHeight);
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
  container.appendChild(renderer.domElement);

  const particleCount = 30000;
  const geometry = new THREE.BufferGeometry();
  const positions = new Float32Array(particleCount * 3);
  const colors = new Float32Array(particleCount * 3);
  const colorInside = new THREE.Color(0x00f3ff);
  const colorOutside = new THREE.Color(0xff0066);

  for (let i = 0; i < particleCount; i++) {
    const i3 = i * 3;
    const r0 = Math.random() * Math.random() * 55;
    const branchAngle = (i % 3) * ((Math.PI * 2) / 3);
    const spinAngle = r0 * 0.18;
    const randX = Math.pow(Math.random(), 3) * (Math.random() < 0.5 ? 1 : -1) * 7 * (r0 * 0.09);
    const randY = Math.pow(Math.random(), 3) * (Math.random() < 0.5 ? 1 : -1) * 7 * (r0 * 0.09);
    const randZ = Math.pow(Math.random(), 3) * (Math.random() < 0.5 ? 1 : -1) * 7 * (r0 * 0.09);
    positions[i3] = Math.cos(branchAngle + spinAngle) * r0 + randX;
    positions[i3 + 1] = randY;
    positions[i3 + 2] = Math.sin(branchAngle + spinAngle) * r0 + randZ;

    let colorBlend = Math.random();
    colorBlend = colorBlend < 0.5 ? Math.pow(colorBlend * 2, 2) / 2 : 1 - Math.pow((1 - colorBlend) * 2, 2) / 2;
    const mixedColor = colorInside.clone().lerp(colorOutside, colorBlend);
    colors[i3] = mixedColor.r; colors[i3 + 1] = mixedColor.g; colors[i3 + 2] = mixedColor.b;
  }
  geometry.setAttribute("position", new THREE.BufferAttribute(positions, 3));
  geometry.setAttribute("color", new THREE.BufferAttribute(colors, 3));

  const material = new THREE.PointsMaterial({
    size: 0.25,
    vertexColors: true,
    blending: THREE.AdditiveBlending,
    depthWrite: false,
    transparent: true,
    opacity: 0.95,
    color: 0xffffff,
  });

  particleSystem = new THREE.Points(geometry, material);
  scene.add(particleSystem);

  const starCount = 3200;
  const starRadiusMin = 160;
  const starRadiusMax = 280;
  const starGoldRatio = 0.08;
  const starColorGold = [1.0, 0.8235, 0.5412];
  const starColorBlue = [0.5882, 0.7843, 1.0];

  const starPositions = new Float32Array(starCount * 3);
  const starColorsArr = new Float32Array(starCount * 3);
  const starSizesArr = new Float32Array(starCount);
  const starPhasesArr = new Float32Array(starCount);
  const starSpeedsArr = new Float32Array(starCount);
  const starBasesArr = new Float32Array(starCount);
  const starAmpsArr = new Float32Array(starCount);

  for (let i = 0; i < starCount; i++) {
    const i3 = i * 3;
    const su = Math.random(), sv = Math.random();
    const sTheta = 2 * Math.PI * su;
    const sPhi = Math.acos(2 * sv - 1);
    const sr = starRadiusMin + Math.random() * (starRadiusMax - starRadiusMin);
    starPositions[i3] = sr * Math.sin(sPhi) * Math.cos(sTheta);
    starPositions[i3 + 1] = sr * Math.sin(sPhi) * Math.sin(sTheta);
    starPositions[i3 + 2] = sr * Math.cos(sPhi);

    const isGold = Math.random() < starGoldRatio;
    const sc = isGold ? starColorGold : starColorBlue;
    starColorsArr[i3] = sc[0]; starColorsArr[i3 + 1] = sc[1]; starColorsArr[i3 + 2] = sc[2];
    starSizesArr[i] = 1.1 + Math.pow(Math.random(), 3) * 2.8;
    starPhasesArr[i] = Math.random() * Math.PI * 2;
    starSpeedsArr[i] = 0.5 + Math.random() * 0.8;
    starBasesArr[i] = 0.35 + Math.random() * 0.3;
    starAmpsArr[i] = 0.3 + Math.random() * 0.3;
  }

  const starGeometry = new THREE.BufferGeometry();
  starGeometry.setAttribute("position", new THREE.BufferAttribute(starPositions, 3));
  starGeometry.setAttribute("aColor", new THREE.BufferAttribute(starColorsArr, 3));
  starGeometry.setAttribute("aSize", new THREE.BufferAttribute(starSizesArr, 1));
  starGeometry.setAttribute("aPhase", new THREE.BufferAttribute(starPhasesArr, 1));
  starGeometry.setAttribute("aSpeed", new THREE.BufferAttribute(starSpeedsArr, 1));
  starGeometry.setAttribute("aBase", new THREE.BufferAttribute(starBasesArr, 1));
  starGeometry.setAttribute("aAmp", new THREE.BufferAttribute(starAmpsArr, 1));

  const starVertexShader = [
    "attribute vec3 aColor;",
    "attribute float aSize;",
    "attribute float aPhase;",
    "attribute float aSpeed;",
    "attribute float aBase;",
    "attribute float aAmp;",
    "uniform float uTime;",
    "uniform float uPixelRatio;",
    "varying vec3 vColor;",
    "varying float vAlpha;",
    "void main() {",
    "  vColor = aColor;",
    "  float tw = sin(uTime * aSpeed + aPhase) * 0.5 + 0.5;",
    "  vAlpha = aBase + tw * aAmp;",
    "  vec4 mvPosition = modelViewMatrix * vec4(position, 1.0);",
    "  gl_Position = projectionMatrix * mvPosition;",
    "  gl_PointSize = aSize * uPixelRatio * (150.0 / -mvPosition.z);",
    "}",
  ].join("\n");

  const starFragmentShader = [
    "precision mediump float;",
    "varying vec3 vColor;",
    "varying float vAlpha;",
    "void main() {",
    "  vec2 uv = gl_PointCoord - vec2(0.5);",
    "  float d = length(uv) * 2.0;",
    "  float core = exp(-d * d * 3.0);",
    "  if (core < 0.02) discard;",
    "  gl_FragColor = vec4(vColor, vAlpha * core);",
    "}",
  ].join("\n");

  const starMaterial = new THREE.ShaderMaterial({
    uniforms: {
      uTime: { value: 0 },
      uPixelRatio: { value: Math.min(window.devicePixelRatio, 2) },
    },
    vertexShader: starVertexShader,
    fragmentShader: starFragmentShader,
    transparent: true,
    depthWrite: false,
    blending: THREE.AdditiveBlending,
  });

  const starSystem = new THREE.Points(starGeometry, starMaterial);
  starSystem.renderOrder = -1;
  scene.add(starSystem);

  clock = new THREE.Clock();

  let targetCamX = 0, targetCamY = 30, targetCamZ = 70;
  const onMouseMove = (e) => {
    mouseX = e.clientX - window.innerWidth / 2;
    mouseY = e.clientY - window.innerHeight / 2;
    targetCamX = mouseX * 0.04;
    targetCamY = 30 - mouseY * 0.04;
  };
  document.addEventListener("mousemove", onMouseMove);

  const onResize = () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => {
      camera.aspect = window.innerWidth / window.innerHeight;
      camera.updateProjectionMatrix();
      renderer.setSize(window.innerWidth, window.innerHeight);
      starMaterial.uniforms.uPixelRatio.value = Math.min(window.devicePixelRatio, 2);
    }, 80);
  };
  window.addEventListener("resize", onResize);

  const animate = () => {
    animationFrameId = requestAnimationFrame(animate);
    const elapsed = clock.getElapsedTime();
    starMaterial.uniforms.uTime.value = elapsed;
    particleSystem.rotation.y += 0.0005;
    particleSystem.rotation.x = Math.sin(elapsed * 0.06) * 0.012;
    camera.position.x += (targetCamX - camera.position.x) * 0.03;
    camera.position.y += (targetCamY - camera.position.y) * 0.03;
    camera.position.z += (targetCamZ - camera.position.z) * 0.03;
    camera.lookAt(scene.position);
    renderer.render(scene, camera);
  };
  animate();

  return () => {
    document.removeEventListener("mousemove", onMouseMove);
    window.removeEventListener("resize", onResize);
    clearTimeout(resizeTimer);
    if (animationFrameId) cancelAnimationFrame(animationFrameId);
    starMaterial.dispose();
    starGeometry.dispose();
    renderer.dispose();
    if (container.contains(renderer.domElement)) {
      container.removeChild(renderer.domElement);
    }
  };
};
let cleanupThree = null;

onMounted(() => {
  cleanupThree = initThree();
  gsap.set(navContainer.value, {
    x: '50vw', xPercent: -50, yPercent: -50,
  });
});

onUnmounted(() => {
  if (cleanupThree) cleanupThree();
});
</script>

<style scoped>
.start-screen-root {
  position: fixed;
  inset: 0;
  width: 100%;
  height: 100%;
  background-color: #050505;
  overflow: hidden;
}

.canvas-container {
  position: fixed;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  z-index: 0;
  contain: strict;
}

/* ——— 标题固定居中，不动 ——— */
.main-title {
  position: fixed;
  top: 15vh;
  left: 50%;
  transform: translateX(-50%);
  display: flex;
  gap: 1.2rem;
  font-size: 7rem;
  letter-spacing: 12px;
  font-weight: 800;
  text-transform: uppercase;
  text-align: center;
  background: linear-gradient(90deg, #ffcc33 0%, #ffffff 50%, #ffcc33 100%);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  filter: drop-shadow(0 2px 12px rgba(0,0,0,0.55)) drop-shadow(0 0 30px rgba(255,204,51,0.35));
  z-index: 40;
  pointer-events: none;
}

/* ——— 导航按钮，GSAP 驱动位置（初始居中，点击左移） ——— */
.nav-container {
  position: fixed;
  top: 50%;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 22px;
  min-width: 320px;
  pointer-events: auto;
  will-change: transform;
  z-index: 10;
}

.nav-btn {
  background: rgba(0, 0, 0, 0.35);
  border: none;
  border-left: 5px solid rgba(255, 255, 255, 0.08);
  color: #999;
  font-size: 2rem;
  text-align: left;
  padding: 20px 34px;
  cursor: pointer;
  transition: color 0.2s ease, border-color 0.2s ease, background 0.2s ease;
  letter-spacing: 3px;
  backdrop-filter: blur(5px);
  border-radius: 0 8px 8px 0;
}

.nav-btn:hover {
  color: #fff;
  border-left-color: rgba(255, 255, 255, 0.5);
  background: linear-gradient(90deg, rgba(255, 255, 255, 0.08) 0%, transparent 100%);
}

.nav-btn.active {
  color: #fff;
  border-left-color: #66aaff;
  background: linear-gradient(90deg, rgba(102, 170, 255, 0.12) 0%, transparent 100%);
  text-shadow: 0 0 14px rgba(102, 170, 255, 0.4);
}
/* ——— 右侧面板 ——— */
.page-panel {
  position: fixed;
  top: 28vh;
  right: 3vw;
  width: 44vw;
  height: 70vh;
  z-index: 30;
  background: rgba(5, 5, 5, 0.92);
  backdrop-filter: blur(4px);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  border-radius: 14px;
  border: 2px solid #d8b86c;
  box-shadow: -8px 0 40px rgba(0, 0, 0, 0.4);
  contain: layout style paint;
}

.page-panel-body {
  flex: 1;
  overflow-y: auto;
  overflow-x: hidden;
}

.page-panel-body :deep(.min-h-screen),
.page-panel-body :deep(.h-screen) {
  min-height: 100% !important;
  height: 100% !important;
}

.page-fade-enter-active,
.page-fade-leave-active {
  transition: opacity 0.18s ease;
}
.page-fade-enter-from,
.page-fade-leave-to {
  opacity: 0;
}

/* ——— 退出确认面板（字体放大） ——— */
.exit-panel-content {
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 16px;
  padding: 36px;
  text-align: center;
}

.exit-panel-icon {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 80px;
  height: 80px;
  border-radius: 50%;
  background: rgba(255, 68, 68, 0.08);
  color: #ff4444;
  margin-bottom: 8px;
}

.exit-panel-title {
font-size: 2.8rem;
  font-weight: 700;
  color: #fff;
  letter-spacing: 4px;
  margin: 0;
}

.exit-panel-desc {
font-size: 1.3rem;
  line-height: 1.6;
  color: #999;
  max-width: 420px;
  margin: 0;
}









.exit-panel-actions {
  display: flex;
  gap: 16px;
  margin-top: 12px;
}

.exit-action {
padding: 14px 40px;
border-radius: 8px;
font-size: 1.15rem;
  font-weight: 600;
  cursor: pointer;
  transition: all 0.2s ease;
  letter-spacing: 1px;
  border: none;
}

.exit-action.cancel {
  background: rgba(255, 255, 255, 0.06);
  color: #aaa;
  border: 1px solid rgba(255, 255, 255, 0.1);
}

.exit-action.cancel:hover {
  background: rgba(255, 255, 255, 0.12);
  color: #fff;
}

.exit-action.confirm {
  background: rgba(255, 68, 68, 0.12);
  color: #ff4444;
  border: 1px solid rgba(255, 68, 68, 0.25);
}


  .exit-action.confirm:hover {
  background: rgba(255, 68, 68, 0.22);
  box-shadow: 0 0 18px rgba(255, 68, 68, 0.1);
}
</style>
