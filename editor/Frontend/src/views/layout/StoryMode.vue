<template>
  <main class="story-mode" aria-label="剧情模式">
    <div
      ref="viewportRef"
      class="story-mode__viewport"
      :class="{ 'story-mode__viewport--looking': isLooking }"
      aria-label="剧情世界画面"
      tabindex="-1"
      @mousedown.left="handleViewportAttack"
      @dragstart.prevent
    ></div>

    <section v-if="hudVisible" class="story-mode__hud" aria-label="剧情模式状态栏">
      <div class="story-mode__quick-actions">
        <button type="button" class="story-mode__hud-button" @click="openInventory">
          <kbd>B</kbd>
          <span>
            <strong>背包</strong>
            {{ inventory.occupiedSlots }}/{{ inventory.slots.length }} 格
          </span>
        </button>
      </div>

      <StoryMiniMap :markers="mapMarkers" :player-state="playerState" @open="openMap" />
    </section>

    <section v-if="hudVisible && bossHud" class="story-mode__boss-hud" aria-label="Boss 生命">
      <div>
        <span>{{ bossHud.name }}</span>
        <strong>{{ bossHud.health }} / {{ bossHud.maxHealth }}</strong>
      </div>
      <div class="story-mode__boss-track">
        <span :style="{ width: `${(bossHud.health / bossHud.maxHealth) * 100}%` }"></span>
      </div>
    </section>

    <div
      v-if="hudVisible"
      class="story-mode__crosshair"
      :class="{ 'story-mode__crosshair--target': aimedMonster }"
      aria-hidden="true"
    >
      <span></span>
      <span></span>
      <div v-if="aimedMonster" class="story-mode__target-label">
        <strong>{{ aimedMonster.name }}</strong>
        <span>{{ aimedMonster.health }} / {{ aimedMonster.maxHealth }}</span>
      </div>
    </div>

    <section
      v-if="gameReady && !hasBlockingOverlay && !playerDead"
      class="story-mode__health-hud"
      aria-label="玩家生命"
    >
      <div class="story-mode__health-label">
        <span>生命</span>
        <strong>{{ playerHealth }} / {{ playerMaxHealth }}</strong>
      </div>
      <div class="story-mode__health-track">
        <span :class="healthBarClass" :style="{ width: `${healthPercent}%` }"></span>
      </div>
    </section>

    <div v-if="damageNumber" :key="damageNumber.id" class="story-mode__damage-number">
      -{{ damageNumber.amount }}
    </div>
    <div
      v-if="damageNumber"
      :key="`flash-${damageFlash}`"
      class="story-mode__damage-vignette"
    ></div>
    <div v-if="attackPulse" :key="`attack-${attackPulse}`" class="story-mode__attack-swing"></div>
    <div v-if="hitPulse" :key="`hit-${hitPulse}`" class="story-mode__hit-marker">×</div>

    <Transition name="story-location">
      <section v-if="locationVisible" class="story-mode__location" aria-live="polite">
        <p>{{ STORY_WORLD_LOCATION_OBJECTIVE }}</p>
        <h1>{{ STORY_WORLD_LOCATION_TITLE }}</h1>
        <div></div>
      </section>
    </Transition>

    <Transition name="story-toast">
      <div
        v-if="inventory.notice && gameReady && !menuOpen"
        class="story-mode__toast"
        :class="`story-mode__toast--${inventory.notice.kind}`"
        role="status"
      >
        {{ inventory.notice.message }}
      </div>
    </Transition>

    <Transition name="story-toast">
      <div
        v-if="cameraNotice && gameReady && !hasBlockingOverlay"
        class="story-mode__toast story-mode__toast--success"
        role="status"
      >
        {{ cameraNotice }}
      </div>
    </Transition>

    <Transition name="story-toast">
      <div
        v-if="combatNotice && gameReady && !hasBlockingOverlay && !playerDead"
        class="story-mode__toast story-mode__toast--combat"
        :class="`story-mode__toast--${combatNotice.kind}`"
        role="status"
      >
        {{ combatNotice.message }}
      </div>
    </Transition>

    <Transition name="story-toast">
      <div
        v-if="systemWarningNotice && gameReady && !hasBlockingOverlay"
        class="story-mode__toast story-mode__toast--warning story-mode__toast--bootstrap"
        role="status"
      >
        {{ systemWarningNotice }}
      </div>
    </Transition>

    <StoryInventoryPanel v-if="inventoryOpen" @close="closeGamePanel" />
    <StoryMapPanel
      v-if="mapOpen"
      :scene-name="mapSceneName"
      :bounds="sceneBounds"
      :bounds-ready="boundsReady"
      :markers="mapMarkers"
      :player-state="playerState"
      :loading="mapLoading"
      :error-message="mapErrorMessage"
      @close="closeGamePanel"
      @refresh="refreshMap"
    />

    <section v-if="!gameReady" class="story-mode__status" aria-live="polite">
      <div class="story-mode__status-card">
        <div v-if="!activeLoadError" class="story-mode__spinner" aria-hidden="true"></div>
        <p v-if="!activeLoadError" class="story-mode__status-kicker">STORY WORLD BOOTSTRAP</p>
        <h1>{{ activeLoadError ? '世界构建失败' : '正在进入世界' }}</h1>
        <p>{{ activeLoadError || activeLoadMessage }}</p>
        <div v-if="!activeLoadError && viewportStatus === 'ready'" class="story-mode__progress">
          <span :style="{ width: `${bootstrapProgress}%` }"></span>
        </div>
        <small v-if="!activeLoadError && viewportStatus === 'ready'">
          {{ bootstrapProgress }}%
        </small>
        <div v-if="activeLoadError" class="story-mode__status-actions">
          <button
            type="button"
            class="story-button story-button--secondary"
            :disabled="exitPending"
            @click="exitToStart"
          >
            返回主界面
          </button>
          <button
            type="button"
            class="story-button story-button--primary"
            @click="retryCurrentFailure"
          >
            重试
          </button>
        </div>
      </div>
    </section>

    <section v-if="playerDead" class="story-mode__death-overlay" aria-label="玩家死亡">
      <div class="story-mode__death-card" role="dialog" aria-modal="true">
        <p>YOU HAVE FALLEN</p>
        <h1>你倒下了</h1>
        <span>云溪村的风仍在等待你。</span>
        <button type="button" class="story-button story-button--primary" @click="respawnAtVillage">
          返回村口
        </button>
      </div>
    </section>

    <section
      v-if="menuOpen"
      class="story-mode__menu-overlay"
      aria-label="剧情模式菜单"
      @pointerdown.stop
      @mousedown.stop
      @click.stop
      @wheel.stop.prevent
    >
      <div
        class="story-mode__menu"
        role="dialog"
        aria-modal="true"
        aria-labelledby="story-menu-title"
      >
        <div class="story-mode__menu-mark" aria-hidden="true"></div>
        <p class="story-mode__menu-kicker">CORONA ENGINE</p>
        <h1 id="story-menu-title">游戏菜单</h1>
        <p class="story-mode__menu-tip">按 Esc 可继续游戏</p>
        <div class="story-mode__menu-actions">
          <button
            ref="continueButtonRef"
            type="button"
            class="story-button story-button--primary"
            @click="closeMenu"
          >
            继续游戏
          </button>
          <button
            type="button"
            class="story-button story-button--secondary"
            :disabled="exitPending"
            @click="exitToStart"
          >
            {{ exitPending ? '正在退出…' : '退出游戏' }}
          </button>
        </div>
      </div>
    </section>
  </main>
</template>

<script setup>
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue';
import { useRouter } from 'vue-router';

import { editorApi } from '@/api/editorApi.js';
import StoryInventoryPanel from '@/components/story/StoryInventoryPanel.vue';
import StoryMapPanel from '@/components/story/StoryMapPanel.vue';
import StoryMiniMap from '@/components/story/StoryMiniMap.vue';
import {
  STORY_WORLD_CAMERA_BOUNDS,
  STORY_WORLD_CAMERA_MIN_Y,
  STORY_WORLD_CAMERA_SPAWN,
  STORY_WORLD_LOCATION_OBJECTIVE,
  STORY_WORLD_LOCATION_TITLE,
} from '@/config/storyWorld.js';
import { useNativeSceneViewport } from '@/composables/useNativeSceneViewport.js';
import { useStoryCameraControls } from '@/composables/useStoryCameraControls.js';
import { useStoryCombat } from '@/composables/useStoryCombat.js';
import { useStoryGameClock } from '@/composables/useStoryGameClock.js';
import { useStoryMap } from '@/composables/useStoryMap.js';
import { useStoryPlayerState } from '@/composables/useStoryPlayerState.js';
import { useStoryWorldBootstrap } from '@/composables/useStoryWorldBootstrap.js';
import { useStoryInventoryStore } from '@/stores/storyInventory.js';
import { isStoryCameraPoseUnsafe } from '@/utils/storyCameraControls.js';
import {
  reduceStoryUiState,
  shouldResetStoryCamera,
  storyShortcutFromEvent,
} from '@/utils/storyUiState.js';

const router = useRouter();
const inventory = useStoryInventoryStore();
const viewportRef = ref(null);
const continueButtonRef = ref(null);
const menuOpen = ref(false);
const inventoryOpen = ref(false);
const mapOpen = ref(false);
const exitPending = ref(false);
const cameraResetPending = ref(false);
const cameraSafetyPending = ref(true);
const cameraNotice = ref('');
const locationVisible = ref(false);
const activeProjectKey = ref('');
let noticeTimer = null;
let cameraNoticeTimer = null;
let locationTimer = null;
let automaticCameraRecoveryKey = '';

const {
  status: viewportStatus,
  errorMessage: viewportErrorMessage,
  sceneId,
  cameraBinding,
  refreshCameraBinding,
  setCameraPose,
  retry: retryViewport,
} = useNativeSceneViewport(viewportRef);
const viewportReady = computed(() => viewportStatus.value === 'ready');
const playerStateRef = useStoryPlayerState(cameraBinding, viewportReady);
const playerState = computed(() => playerStateRef.value);
const {
  loading: mapLoading,
  errorMessage: mapErrorMessage,
  sceneName: mapSceneName,
  markers: sceneMapMarkers,
  sceneBounds,
  boundsReady,
  refresh: refreshMap,
} = useStoryMap(sceneId, playerStateRef);

const showLocationTitle = () => {
  if (locationTimer !== null) window.clearTimeout(locationTimer);
  locationVisible.value = true;
  locationTimer = window.setTimeout(() => {
    locationTimer = null;
    locationVisible.value = false;
  }, 3600);
};

const showCameraNotice = (message) => {
  if (cameraNoticeTimer !== null) window.clearTimeout(cameraNoticeTimer);
  cameraNotice.value = String(message || '').trim();
  if (!cameraNotice.value) return;
  cameraNoticeTimer = window.setTimeout(() => {
    cameraNoticeTimer = null;
    cameraNotice.value = '';
  }, 3200);
};

const {
  status: bootstrapStatus,
  progress: bootstrapProgress,
  phaseMessage: bootstrapPhaseMessage,
  errorMessage: bootstrapErrorMessage,
  warningMessages: bootstrapWarnings,
  managedWorld,
  worldBounds: bootstrapWorldBounds,
  isReady: bootstrapReady,
  retry: retryBootstrap,
} = useStoryWorldBootstrap({
  sceneId,
  viewportStatus,
  setCameraPose,
  onComplete: async (result) => {
    await refreshMap();
    if (result.generated || Number(result.repairedCount) > 0) showLocationTitle();
  },
});

const gameReady = computed(() => viewportReady.value && bootstrapReady.value);
const hasBlockingOverlay = computed(() => menuOpen.value || inventoryOpen.value || mapOpen.value);
const activeLoadError = computed(() => {
  if (viewportStatus.value === 'error') return viewportErrorMessage.value;
  if (bootstrapStatus.value === 'error') return bootstrapErrorMessage.value;
  return '';
});
const activeLoadMessage = computed(() =>
  viewportStatus.value !== 'ready'
    ? '正在连接当前场景与活动相机…'
    : bootstrapPhaseMessage.value || '正在准备剧情世界…'
);
const bootstrapWarningNotice = computed(() => {
  const warnings = bootstrapWarnings.value;
  if (!Array.isArray(warnings) || warnings.length === 0) return '';
  return warnings.length === 1
    ? warnings[0]
    : `${warnings[0]}（另有 ${warnings.length - 1} 项提示）`;
});

const { totalGameTimeMs, lightingError, shutdown: shutdownStoryClock } = useStoryGameClock({
  sceneId,
  projectKey: activeProjectKey,
  enabled: gameReady,
});

const combatPaused = computed(
  () => !gameReady.value || hasBlockingOverlay.value || cameraSafetyPending.value
);
const combatEnabled = computed(
  () => gameReady.value && managedWorld.value && Boolean(activeProjectKey.value)
);
const {
  playerHealth,
  playerMaxHealth,
  playerDead,
  warningMessage: combatWarningMessage,
  notice: combatNotice,
  aimedMonster,
  bossHud,
  monsterMarkers,
  attackPulse,
  hitPulse,
  damageFlash,
  damageNumber,
  attack: combatAttack,
  respawnPlayer,
  shutdown: shutdownStoryCombat,
} = useStoryCombat({
  sceneId,
  projectKey: activeProjectKey,
  enabled: combatEnabled,
  paused: combatPaused,
  totalGameTimeMs,
  playerState: playerStateRef,
  cameraBinding,
  viewportRef,
  onActorsReady: refreshMap,
});

const controlsEnabled = computed(
  () =>
    gameReady.value && !hasBlockingOverlay.value && !cameraSafetyPending.value && !playerDead.value
);
const hudVisible = computed(
  () => gameReady.value && !hasBlockingOverlay.value && !playerDead.value
);

const mapMarkers = computed(() => {
  const staticMarkers = sceneMapMarkers.value.filter(
    (marker) => !String(marker.name || '').startsWith('StoryMonster_')
  );
  return [...staticMarkers, ...monsterMarkers.value];
});
const healthPercent = computed(() =>
  Math.max(0, Math.min(100, (playerHealth.value / playerMaxHealth) * 100))
);
const healthBarClass = computed(() => ({
  'story-mode__health-fill--warning': healthPercent.value <= 60 && healthPercent.value > 30,
  'story-mode__health-fill--danger': healthPercent.value <= 30,
}));
const systemWarningNotice = computed(
  () => bootstrapWarningNotice.value || combatWarningMessage.value || lightingError.value
);

const cameraPositionBounds = computed(() =>
  managedWorld.value ? STORY_WORLD_CAMERA_BOUNDS : null
);
const {
  isLooking,
  stop: stopCameraControls,
  persistPose,
} = useStoryCameraControls({
  viewportRef,
  cameraBinding,
  enabled: controlsEnabled,
  refreshCameraBinding,
  positionBounds: cameraPositionBounds,
});

watch(
  [bootstrapReady, managedWorld, sceneId, bootstrapWorldBounds],
  async ([ready, isManagedWorld, activeSceneId]) => {
    const normalizedSceneId = String(activeSceneId || '').trim();
    if (!ready || !normalizedSceneId) {
      cameraSafetyPending.value = true;
      if (!normalizedSceneId) automaticCameraRecoveryKey = '';
      return;
    }
    if (!isManagedWorld) {
      cameraSafetyPending.value = false;
      return;
    }
    if (automaticCameraRecoveryKey === normalizedSceneId) return;
    automaticCameraRecoveryKey = normalizedSceneId;
    cameraSafetyPending.value = true;

    const safetyOptions = {
      minimumY: STORY_WORLD_CAMERA_MIN_Y,
      maximumY: STORY_WORLD_CAMERA_BOUNDS.maxY,
      worldBounds: bootstrapWorldBounds.value,
    };

    try {
      await stopCameraControls({ persist: false });
      const refreshed = await refreshCameraBinding({ preservePose: false });
      if (!refreshed) throw new Error('Unable to refresh the Story camera binding.');
      if (!isStoryCameraPoseUnsafe(cameraBinding.value, safetyOptions)) {
        cameraSafetyPending.value = false;
        return;
      }

      await setCameraPose(STORY_WORLD_CAMERA_SPAWN, { persist: true });
      const recovered = await refreshCameraBinding({ preservePose: false });
      if (!recovered || isStoryCameraPoseUnsafe(cameraBinding.value, safetyOptions)) {
        throw new Error('Story camera recovery pose did not pass validation.');
      }
      cameraSafetyPending.value = false;
      showCameraNotice('检测到视角未朝向世界，已返回云溪村村口。');
    } catch (error) {
      automaticCameraRecoveryKey = '';
      cameraSafetyPending.value = true;
      console.warn('[StoryMode] failed to recover the Story World camera pose', error);
      showCameraNotice('自动恢复视角失败，可按 R 返回云溪村村口。');
    }
  },
  { flush: 'post' }
);

const applyUiState = (nextState) => {
  const wasBlocked = hasBlockingOverlay.value;
  menuOpen.value = nextState.menuOpen;
  inventoryOpen.value = nextState.inventoryOpen;
  mapOpen.value = nextState.mapOpen;
  if (!wasBlocked && hasBlockingOverlay.value) void stopCameraControls({ persist: true });
  if (mapOpen.value) void refreshMap();
};

const transitionUi = (shortcut) => {
  applyUiState(
    reduceStoryUiState(
      {
        ready: gameReady.value,
        menuOpen: menuOpen.value,
        inventoryOpen: inventoryOpen.value,
        mapOpen: mapOpen.value,
      },
      shortcut
    )
  );
};

const openInventory = () => {
  if (!gameReady.value || menuOpen.value) return;
  applyUiState({ ready: true, menuOpen: false, inventoryOpen: true, mapOpen: false });
};

const openMap = () => {
  if (!gameReady.value || menuOpen.value) return;
  applyUiState({ ready: true, menuOpen: false, inventoryOpen: false, mapOpen: true });
};

const closeGamePanel = () => {
  inventoryOpen.value = false;
  mapOpen.value = false;
};

const closeMenu = () => {
  menuOpen.value = false;
};

const retryCurrentFailure = () => {
  if (viewportStatus.value === 'error') return retryViewport();
  return retryBootstrap();
};

const resetStoryCamera = async () => {
  if (
    cameraResetPending.value ||
    !shouldResetStoryCamera({
      ready: gameReady.value,
      managedWorld: managedWorld.value,
      menuOpen: menuOpen.value,
      inventoryOpen: inventoryOpen.value,
      mapOpen: mapOpen.value,
    })
  ) {
    return false;
  }

  cameraResetPending.value = true;
  cameraSafetyPending.value = true;
  await stopCameraControls({ persist: false });
  try {
    await setCameraPose(STORY_WORLD_CAMERA_SPAWN, { persist: true });
    const refreshed = await refreshCameraBinding({ preservePose: false });
    if (!refreshed) throw new Error('Unable to refresh the Story camera binding.');
    cameraSafetyPending.value = false;
    automaticCameraRecoveryKey = String(sceneId.value || '').trim();
    showCameraNotice('已返回云溪村村口。');
    return true;
  } catch (error) {
    console.warn('[StoryMode] failed to reset the Story World camera', error);
    showCameraNotice('返回村口失败，请稍后重试。');
    return false;
  } finally {
    cameraResetPending.value = false;
  }
};

const handleViewportAttack = (event) => {
  if (event.button !== 0 || event.ctrlKey || event.altKey || event.metaKey || event.shiftKey)
    return;
  combatAttack();
};

const respawnAtVillage = async () => {
  await resetStoryCamera();
  respawnPlayer();
};

const exitToStart = async () => {
  if (exitPending.value) return;
  exitPending.value = true;
  menuOpen.value = false;
  inventoryOpen.value = false;
  mapOpen.value = false;
  await stopCameraControls({ persist: false });
  await persistPose();
  await shutdownStoryCombat();
  await shutdownStoryClock();
  await router.push('/StartScreen');
};

const handleShortcut = (event) => {
  const shortcut = storyShortcutFromEvent(event);
  if (!shortcut) return;
  if (
    shortcut === 'reset-camera' &&
    !shouldResetStoryCamera({
      ready: gameReady.value,
      managedWorld: managedWorld.value,
      menuOpen: menuOpen.value,
      inventoryOpen: inventoryOpen.value,
      mapOpen: mapOpen.value,
    })
  ) {
    return;
  }

  event.preventDefault();
  event.stopPropagation();
  event.stopImmediatePropagation?.();
  if (shortcut === 'reset-camera') {
    void resetStoryCamera();
    return;
  }
  transitionUi(shortcut);
};

const projectPathFromResponse = (response) => {
  const candidates = [response, response?.data, response?.data?.data];
  for (const candidate of candidates) {
    const projectPath = String(
      candidate?.project_path || candidate?.projectPath || candidate?.path || ''
    ).trim();
    if (projectPath) return projectPath;
  }
  return '';
};

const initializeProjectInventory = async () => {
  let projectPath = '';
  try {
    projectPath = projectPathFromResponse(await editorApi.projectSettings.getActiveProjectInfo());
  } catch (error) {
    console.warn('[StoryMode] failed to resolve the active project for inventory', error);
  }

  if (!projectPath) {
    projectPath = String(window.localStorage?.getItem('corona.activeProjectPath') || '').trim();
  }
  if (!projectPath) {
    try {
      projectPath = projectPathFromResponse(await editorApi.files.getProjectInfo());
    } catch (error) {
      console.warn('[StoryMode] failed to resolve the file project for inventory', error);
    }
  }
  activeProjectKey.value = projectPath || sceneId.value || 'active-project';
  inventory.resetForProject(activeProjectKey.value);
};

watch([menuOpen, inventoryOpen, mapOpen], async ([isMenuOpen, isInventoryOpen, isMapOpen]) => {
  await nextTick();
  if (isMenuOpen) continueButtonRef.value?.focus?.();
  else if (!isInventoryOpen && !isMapOpen) viewportRef.value?.focus?.({ preventScroll: true });
});

watch(gameReady, (ready) => {
  if (ready) return;
  menuOpen.value = false;
  inventoryOpen.value = false;
  mapOpen.value = false;
  locationVisible.value = false;
  void stopCameraControls({ persist: true });
});

watch(
  () => inventory.notice?.id,
  () => {
    if (noticeTimer !== null) window.clearTimeout(noticeTimer);
    if (!inventory.notice) return;
    noticeTimer = window.setTimeout(() => {
      noticeTimer = null;
      inventory.clearNotice();
    }, 2600);
  }
);

onMounted(() => {
  window.addEventListener('keydown', handleShortcut, true);
  void initializeProjectInventory();
});

onUnmounted(() => {
  window.removeEventListener('keydown', handleShortcut, true);
  if (noticeTimer !== null) window.clearTimeout(noticeTimer);
  if (cameraNoticeTimer !== null) window.clearTimeout(cameraNoticeTimer);
  if (locationTimer !== null) window.clearTimeout(locationTimer);
  noticeTimer = null;
  cameraNoticeTimer = null;
  locationTimer = null;
});
</script>
<style scoped>
.story-mode {
  position: relative;
  width: 100vw;
  height: 100vh;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
  background: transparent;
  color: var(--ce-text-primary, #f2ead5);
  pointer-events: none;
  user-select: none;
}

.story-mode__viewport {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  background: transparent;
  pointer-events: auto;
  cursor: default;
  outline: none;
}

.story-mode__viewport--looking {
  cursor: grabbing;
}

.story-mode__hud {
  position: absolute;
  inset: 0;
  z-index: 5;
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  padding: 20px;
  pointer-events: none;
}

.story-mode__quick-actions {
  display: flex;
  flex-direction: column;
  gap: 10px;
  pointer-events: auto;
}

.story-mode__hud-button {
  display: flex;
  align-items: center;
  gap: 10px;
  width: fit-content;
  padding: 9px 12px;
  border: 1px solid rgba(216, 184, 108, 0.32);
  border-radius: 10px;
  background: linear-gradient(145deg, rgba(21, 22, 18, 0.86), rgba(5, 7, 6, 0.88));
  box-shadow: 0 10px 28px rgba(0, 0, 0, 0.36);
  color: #b6ad99;
  text-align: left;
  cursor: pointer;
  backdrop-filter: blur(5px);
}

.story-mode__hud-button:hover,
.story-mode__hud-button:focus-visible {
  border-color: rgba(216, 184, 108, 0.68);
  outline: none;
}

.story-mode__hud-button kbd {
  border: 1px solid rgba(216, 184, 108, 0.35);
  border-radius: 5px;
  background: rgba(0, 0, 0, 0.3);
  color: #d8b86c;
  font: inherit;
}

.story-mode__hud-button kbd {
  min-width: 30px;
  padding: 5px 7px;
  text-align: center;
}

.story-mode__hud-button span {
  display: grid;
  gap: 2px;
  font-size: 10px;
}

.story-mode__hud-button strong {
  color: #e7dbc0;
  font-size: 12px;
}

.story-mode__hud > :last-child {
  pointer-events: auto;
}

.story-mode__boss-hud {
  position: absolute;
  z-index: 17;
  top: 88px;
  left: 50%;
  width: min(520px, calc(100vw - 48px));
  transform: translateX(-50%);
  color: #f4ddd0;
  text-align: center;
  pointer-events: none;
}

.story-mode__boss-hud > div:first-child {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 4px 6px;
  font-size: 11px;
  letter-spacing: 0.08em;
  text-shadow: 0 2px 5px rgba(0, 0, 0, 0.9);
}

.story-mode__boss-hud strong {
  color: #e7b4a1;
  font-size: 10px;
  font-variant-numeric: tabular-nums;
}

.story-mode__boss-track,
.story-mode__health-track {
  overflow: hidden;
  border: 1px solid rgba(255, 255, 255, 0.16);
  border-radius: 999px;
  background: rgba(6, 7, 6, 0.82);
  box-shadow:
    inset 0 1px 4px rgba(0, 0, 0, 0.9),
    0 5px 18px rgba(0, 0, 0, 0.45);
}

.story-mode__boss-track {
  height: 9px;
  border-color: rgba(151, 62, 46, 0.72);
}

.story-mode__boss-track span,
.story-mode__health-track span {
  display: block;
  height: 100%;
  border-radius: inherit;
  transition:
    width 220ms ease,
    background 220ms ease;
}

.story-mode__boss-track span {
  background: linear-gradient(90deg, #5e1715, #a63227 58%, #df7459);
  box-shadow: 0 0 14px rgba(190, 62, 45, 0.58);
}

.story-mode__health-hud {
  position: absolute;
  z-index: 24;
  bottom: 26px;
  left: 50%;
  width: min(360px, calc(100vw - 48px));
  padding: 10px 14px 12px;
  border: 1px solid rgba(216, 184, 108, 0.34);
  border-radius: 12px;
  background: linear-gradient(180deg, rgba(14, 17, 13, 0.9), rgba(5, 7, 6, 0.88));
  box-shadow: 0 12px 34px rgba(0, 0, 0, 0.48);
  transform: translateX(-50%);
  pointer-events: none;
  backdrop-filter: blur(5px);
}

.story-mode__health-label {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 7px;
  color: #d7cfba;
  font-size: 11px;
  letter-spacing: 0.12em;
}

.story-mode__health-label strong {
  color: #eff4dd;
  font-size: 11px;
  font-variant-numeric: tabular-nums;
}

.story-mode__health-track {
  height: 12px;
  border-color: rgba(118, 170, 93, 0.58);
}

.story-mode__health-track span {
  background: linear-gradient(90deg, #2f7f40, #67b958 62%, #9bd26d);
  box-shadow: 0 0 14px rgba(91, 179, 85, 0.48);
}

.story-mode__health-track .story-mode__health-fill--warning {
  background: linear-gradient(90deg, #9b6c1e, #d4a83a, #efd16d);
  box-shadow: 0 0 14px rgba(217, 170, 52, 0.5);
}

.story-mode__health-track .story-mode__health-fill--danger {
  background: linear-gradient(90deg, #7b1818, #c8322c, #e6624f);
  box-shadow: 0 0 16px rgba(212, 53, 43, 0.58);
  animation: story-health-danger 800ms ease-in-out infinite alternate;
}

.story-mode__crosshair--target span {
  background: #ef6a55;
}

.story-mode__target-label {
  position: absolute;
  top: 28px;
  left: 50%;
  display: grid;
  min-width: 126px;
  padding: 6px 10px;
  border: 1px solid rgba(221, 96, 76, 0.42);
  border-radius: 7px;
  background: rgba(12, 8, 7, 0.78);
  color: #e4b4a8;
  font-size: 9px;
  text-align: center;
  transform: translateX(-50%);
  backdrop-filter: blur(3px);
}

.story-mode__target-label strong {
  color: #f4ddd4;
  font-size: 11px;
  font-weight: 600;
}

.story-mode__target-label span {
  position: static;
  display: block;
  margin-top: 2px;
  background: none;
  color: #d88774;
  transform: none;
}

.story-mode__damage-number {
  position: absolute;
  z-index: 36;
  top: 58%;
  left: 50%;
  color: #ff6f5c;
  font-size: 30px;
  font-weight: 800;
  text-shadow: 0 3px 12px rgba(90, 0, 0, 0.9);
  transform: translate(-50%, -50%);
  animation: story-damage-number 720ms ease-out forwards;
  pointer-events: none;
}

.story-mode__damage-vignette {
  position: absolute;
  z-index: 20;
  inset: 0;
  background: radial-gradient(circle, transparent 35%, rgba(155, 12, 5, 0.62) 100%);
  animation: story-damage-vignette 480ms ease-out forwards;
  pointer-events: none;
}

.story-mode__attack-swing {
  position: absolute;
  z-index: 19;
  top: 51%;
  left: 51%;
  width: 230px;
  height: 120px;
  border-top: 5px solid rgba(242, 225, 183, 0.72);
  border-radius: 50%;
  filter: drop-shadow(0 0 8px rgba(230, 192, 104, 0.55));
  transform: translate(-50%, -50%) rotate(-34deg);
  transform-origin: 18% 75%;
  animation: story-attack-swing 260ms ease-out forwards;
  pointer-events: none;
}

.story-mode__hit-marker {
  position: absolute;
  z-index: 25;
  top: 50%;
  left: 50%;
  color: #fff1ce;
  font-size: 30px;
  font-weight: 300;
  line-height: 1;
  text-shadow: 0 0 8px #c34835;
  transform: translate(-50%, -50%);
  animation: story-hit-marker 300ms ease-out forwards;
  pointer-events: none;
}

.story-mode__death-overlay {
  position: absolute;
  z-index: 38;
  inset: 0;
  display: flex;
  align-items: center;
  justify-content: center;
  background:
    radial-gradient(circle at 50% 42%, rgba(87, 17, 12, 0.16), transparent 34%), rgba(3, 2, 2, 0.83);
  pointer-events: auto;
  backdrop-filter: grayscale(0.85) blur(4px);
}

.story-mode__death-card {
  display: grid;
  justify-items: center;
  width: min(430px, calc(100vw - 40px));
  padding: 38px 34px;
  border: 1px solid rgba(178, 74, 59, 0.5);
  border-radius: 14px;
  background: linear-gradient(145deg, rgba(33, 15, 13, 0.97), rgba(8, 6, 5, 0.98));
  box-shadow: 0 28px 90px rgba(0, 0, 0, 0.72);
  text-align: center;
}

.story-mode__death-card p {
  margin: 0;
  color: #93493e;
  font-size: 10px;
  letter-spacing: 0.32em;
}

.story-mode__death-card h1 {
  margin: 10px 0 8px;
  color: #f1d4ca;
  font-family: 'STKaiti', 'KaiTi', serif;
  font-size: 38px;
  font-weight: 500;
  letter-spacing: 0.12em;
}

.story-mode__death-card span {
  margin-bottom: 26px;
  color: #a88e84;
  font-size: 13px;
}

.story-mode__toast--combat {
  bottom: 104px;
}

.story-mode__toast--danger {
  border-color: rgba(220, 82, 63, 0.68);
  color: #f0b5a7;
}
.story-mode__toast {
  position: absolute;
  z-index: 35;
  left: 50%;
  bottom: 34px;
  transform: translateX(-50%);
  max-width: min(520px, calc(100vw - 40px));
  padding: 10px 16px;
  border: 1px solid rgba(216, 184, 108, 0.36);
  border-radius: 999px;
  background: rgba(13, 14, 11, 0.92);
  box-shadow: 0 12px 34px rgba(0, 0, 0, 0.48);
  color: #e0d5bd;
  font-size: 12px;
  pointer-events: none;
}

.story-mode__toast--success {
  border-color: rgba(111, 197, 155, 0.5);
}
.story-mode__toast--warning {
  border-color: rgba(224, 157, 86, 0.55);
}

.story-toast-enter-active,
.story-toast-leave-active {
  transition:
    opacity 160ms ease,
    transform 160ms ease;
}

.story-toast-enter-from,
.story-toast-leave-to {
  opacity: 0;
  transform: translate(-50%, 8px);
}

.story-mode__status,
.story-mode__menu-overlay {
  position: absolute;
  inset: 0;
  z-index: 40;
  display: flex;
  align-items: center;
  justify-content: center;
  pointer-events: auto;
}

.story-mode__status {
  background:
    radial-gradient(circle at 50% 38%, rgba(216, 184, 108, 0.1), transparent 34%), #080806;
}

.story-mode__status-card,
.story-mode__menu {
  width: min(420px, calc(100vw - 40px));
  padding: 34px;
  border: 1px solid rgba(216, 184, 108, 0.42);
  border-radius: 14px;
  background: linear-gradient(145deg, rgba(31, 28, 20, 0.97), rgba(10, 10, 8, 0.98));
  box-shadow: 0 24px 80px rgba(0, 0, 0, 0.62);
  text-align: center;
}

.story-mode__status-card h1,
.story-mode__menu h1 {
  margin: 10px 0 8px;
  color: #f4e8ca;
  font-size: 28px;
  font-weight: 600;
  letter-spacing: 0.08em;
}

.story-mode__status-card p,
.story-mode__menu-tip {
  margin: 0;
  color: #a99e84;
  font-size: 14px;
  line-height: 1.7;
}

.story-mode__spinner {
  width: 36px;
  height: 36px;
  margin: 0 auto 18px;
  border: 3px solid rgba(216, 184, 108, 0.18);
  border-top-color: #d8b86c;
  border-radius: 50%;
  animation: story-spin 0.9s linear infinite;
}

.story-mode__menu-overlay {
  z-index: 30;
  background: rgba(2, 2, 2, 0.72);
  backdrop-filter: blur(8px);
}

.story-mode__menu-mark {
  width: 42px;
  height: 3px;
  margin: 0 auto 18px;
  border-radius: 999px;
  background: #d8b86c;
  box-shadow: 0 0 18px rgba(216, 184, 108, 0.55);
}

.story-mode__menu-kicker {
  margin: 0;
  color: #8f805d;
  font-size: 11px;
  letter-spacing: 0.28em;
}

.story-mode__menu-actions,
.story-mode__status-actions {
  display: grid;
  gap: 12px;
  margin-top: 28px;
}

.story-button {
  width: 100%;
  min-height: 46px;
  padding: 10px 18px;
  border: 1px solid transparent;
  border-radius: 9px;
  font-size: 15px;
  font-weight: 600;
  letter-spacing: 0.04em;
  cursor: pointer;
  transition:
    border-color 160ms ease,
    background 160ms ease,
    color 160ms ease,
    transform 160ms ease;
}

.story-button:hover:not(:disabled) {
  transform: translateY(-1px);
}

.story-button:disabled {
  cursor: wait;
  opacity: 0.65;
}

.story-button:focus-visible {
  outline: 2px solid #ead08e;
  outline-offset: 3px;
}

.story-button--primary {
  background: #d8b86c;
  color: #17130a;
}

.story-button--primary:hover {
  background: #e5c77f;
}

.story-button--secondary {
  border-color: rgba(216, 184, 108, 0.28);
  background: rgba(255, 255, 255, 0.035);
  color: #d8ccb0;
}

.story-button--secondary:hover {
  border-color: rgba(216, 184, 108, 0.62);
  background: rgba(216, 184, 108, 0.09);
  color: #fff5dc;
}

.story-mode__crosshair {
  position: absolute;
  z-index: 18;
  top: 50%;
  left: 50%;
  width: 20px;
  height: 20px;
  transform: translate(-50%, -50%);
  filter: drop-shadow(0 1px 2px rgba(0, 0, 0, 0.9));
  pointer-events: none;
}

.story-mode__crosshair span {
  position: absolute;
  top: 50%;
  left: 50%;
  display: block;
  background: rgba(245, 232, 199, 0.82);
  transform: translate(-50%, -50%);
}

.story-mode__crosshair span:first-child {
  width: 16px;
  height: 1px;
}
.story-mode__crosshair span:last-child {
  width: 1px;
  height: 16px;
}

.story-mode__location {
  position: absolute;
  z-index: 24;
  top: 20%;
  left: 50%;
  width: min(480px, calc(100vw - 40px));
  transform: translateX(-50%);
  color: #f6e9c9;
  text-align: center;
  text-shadow: 0 3px 14px rgba(0, 0, 0, 0.94);
  pointer-events: none;
}

.story-mode__location p {
  margin: 0 0 7px;
  color: #d7c394;
  font-size: 11px;
  letter-spacing: 0.38em;
}

.story-mode__location h1 {
  margin: 0;
  font-family: 'STKaiti', 'KaiTi', serif;
  font-size: clamp(34px, 5vw, 58px);
  font-weight: 500;
  letter-spacing: 0.18em;
}

.story-mode__location div {
  width: 180px;
  height: 1px;
  margin: 15px auto 0;
  background: linear-gradient(90deg, transparent, #d8b86c, transparent);
}

.story-location-enter-active {
  transition:
    opacity 700ms ease,
    transform 700ms ease;
}
.story-location-leave-active {
  transition:
    opacity 900ms ease,
    transform 900ms ease;
}
.story-location-enter-from,
.story-location-leave-to {
  opacity: 0;
  transform: translate(-50%, 12px);
}

.story-mode__toast--bootstrap {
  bottom: 78px;
}

.story-mode__status-kicker {
  color: #8f805d !important;
  font-size: 10px !important;
  letter-spacing: 0.24em;
}

.story-mode__progress {
  overflow: hidden;
  width: 100%;
  height: 4px;
  margin: 24px 0 8px;
  border-radius: 999px;
  background: rgba(216, 184, 108, 0.12);
}

.story-mode__progress span {
  display: block;
  height: 100%;
  border-radius: inherit;
  background: linear-gradient(90deg, #876c31, #e1c477);
  box-shadow: 0 0 16px rgba(216, 184, 108, 0.5);
  transition: width 280ms ease;
}

.story-mode__status-card small {
  color: #766d5a;
  font-size: 10px;
}

@keyframes story-health-danger {
  from {
    opacity: 0.72;
  }
  to {
    opacity: 1;
  }
}

@keyframes story-damage-number {
  0% {
    opacity: 0;
    transform: translate(-50%, 8px) scale(0.8);
  }
  18% {
    opacity: 1;
    transform: translate(-50%, -6px) scale(1.08);
  }
  100% {
    opacity: 0;
    transform: translate(-50%, -48px) scale(0.96);
  }
}

@keyframes story-damage-vignette {
  0% {
    opacity: 0;
  }
  18% {
    opacity: 1;
  }
  100% {
    opacity: 0;
  }
}

@keyframes story-attack-swing {
  0% {
    opacity: 0;
    transform: translate(-50%, -50%) rotate(-55deg) scale(0.65);
  }
  28% {
    opacity: 0.88;
  }
  100% {
    opacity: 0;
    transform: translate(-50%, -50%) rotate(28deg) scale(1.08);
  }
}

@keyframes story-hit-marker {
  0% {
    opacity: 0;
    transform: translate(-50%, -50%) scale(1.65);
  }
  24% {
    opacity: 1;
    transform: translate(-50%, -50%) scale(0.9);
  }
  100% {
    opacity: 0;
    transform: translate(-50%, -50%) scale(1.1);
  }
}
@keyframes story-spin {
  to {
    transform: rotate(360deg);
  }
}
</style>
