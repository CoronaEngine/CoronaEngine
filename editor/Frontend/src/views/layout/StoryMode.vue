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
      <div v-if="nearbyTarget" class="story-mode__interaction-hint" role="status">
        <kbd>F</kbd> {{ interactionLabel }}
      </div>
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

    <Transition name="story-dialog">
      <section
        v-if="dialogOpen"
        class="story-mode__dialog-overlay"
        aria-label="剧情交互"
        @pointerdown.stop
        @mousedown.stop
        @click.stop
        @wheel.stop.prevent
      >
        <div
          class="story-mode__dialog"
          :class="`story-mode__dialog--${dialogProfile.tone}`"
          role="dialog"
          aria-modal="true"
          aria-labelledby="story-dialog-title"
        >
          <div class="story-mode__dialog-accent" aria-hidden="true"></div>
          <div class="story-mode__dialog-avatar" aria-hidden="true">
            <span>{{ dialogProfile.symbol }}</span>
          </div>

          <div class="story-mode__dialog-main">
            <header class="story-mode__dialog-header">
              <div>
                <p class="story-mode__dialog-kicker">{{ dialogProfile.eyebrow }}</p>
                <h2 id="story-dialog-title">{{ dialogTitle }}</h2>
              </div>
              <button
                type="button"
                class="story-mode__dialog-close"
                aria-label="关闭对话"
                @click="closeDialog"
              >
                <span aria-hidden="true">×</span>
              </button>
            </header>

            <div class="story-mode__dialog-speaker">
              <span class="story-mode__dialog-speaker-dot" aria-hidden="true"></span>
              <span>{{ dialogProfile.subtitle }}</span>
              <span class="story-mode__dialog-separator" aria-hidden="true">/</span>
              <span>{{ dialogTarget?.distance ? `${dialogTarget.distance.toFixed(1)}m` : '近距离交互' }}</span>
            </div>

            <p class="story-mode__dialog-text">{{ dialogText }}</p>

            <div v-if="dialogKind === 'quest'" class="story-mode__quest-list">
              <button
                v-for="quest in questOptions"
                :key="quest.id"
                type="button"
                class="story-mode__quest-button"
                :disabled="storyProgress.completedQuestIds.includes(quest.id) || Boolean(storyProgress.activeQuest && storyProgress.activeQuest.id !== quest.id)"
                @click="acceptQuest(quest)"
              >
                <span class="story-mode__quest-icon" aria-hidden="true">{{ quest.icon || '✦' }}</span>
                <span class="story-mode__quest-copy">
                  <strong>{{ quest.title }}</strong>
                  <span>{{ quest.description }}</span>
                  <small v-if="storyProgress.completedQuestIds.includes(quest.id)">已完成 · 奖励已领取</small>
                  <small v-else-if="storyProgress.activeQuest?.id === quest.id">
                    进度 {{ questProgress(quest).current }} / {{ questProgress(quest).target }}
                    <template v-if="canClaimQuest(quest)"> · 现在可以领取奖励</template>
                  </small>
                  <small v-else-if="storyProgress.activeQuest">请先完成当前委托</small>
                  <small v-else>可选委托 · 完成后回来领取奖励</small>
                </span>
                <span class="story-mode__quest-action">
                  <span>{{ storyProgress.completedQuestIds.includes(quest.id) ? '已领取' : (canClaimQuest(quest) ? '领取奖励' : (storyProgress.activeQuest?.id === quest.id ? '进行中' : '接受')) }}</span>
                  <b aria-hidden="true">›</b>
                </span>
              </button>
            </div>

            <div v-if="dialogKind === 'creator'" class="story-mode__enchant-grid">
              <button
                v-for="type in enchantTypes"
                :key="type.id"
                type="button"
                class="story-mode__choice-card"
                :disabled="!hasWorldFragment"
                @click="enchantFragment(type)"
              >
                <span class="story-mode__choice-icon" aria-hidden="true">{{ type.symbol }}</span>
                <span>
                  <strong>{{ type.label }}</strong>
                  <small>{{ type.description }}</small>
                </span>
                <b aria-hidden="true">›</b>
              </button>
            </div>

            <div v-if="dialogKind === 'merchant'" class="story-mode__quest-list">
              <button
                v-for="stock in merchantStock"
                :key="stock.itemId"
                type="button"
                class="story-mode__quest-button story-mode__merchant-button"
                :disabled="!canBuy(stock)"
                @click="buyStock(stock)"
              >
                <span class="story-mode__quest-icon" aria-hidden="true">{{ getStoryItemDefinition(stock.itemId).symbol }}</span>
                <span class="story-mode__quest-copy">
                  <strong>{{ stock.label }}</strong>
                  <span>库存 ×{{ stock.quantity }} · 每日限购</span>
                  <small>{{ getStoryItemDefinition(stock.itemId).description }}</small>
                </span>
                <span class="story-mode__quest-action story-mode__merchant-price">
                  <span>{{ stock.price }} 蓝晶矿</span>
                  <b aria-hidden="true">›</b>
                </span>
              </button>
            </div>

            <div v-if="dialogKind === 'world_ball'" class="story-mode__dialog-actions">
              <button type="button" class="story-button story-button--primary" @click="enterStoryCreation">
                <span>进入小世界</span><b aria-hidden="true">→</b>
              </button>
            </div>
            <div v-if="dialogKind === 'world_core'" class="story-mode__dialog-actions">
              <button type="button" class="story-button story-button--secondary" @click="enterStoryCreation">
                <span>打开世界核心</span><b aria-hidden="true">→</b>
              </button>
            </div>

            <footer class="story-mode__dialog-footer">
              <span><kbd>Esc</kbd> 返回世界</span>
              <button type="button" class="story-mode__dialog-footer-close" @click="closeDialog">暂时离开</button>
            </footer>
          </div>
        </div>
      </section>
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
import { useStoryProgressStore } from '@/stores/storyProgress.js';
import { useStoryNpcInteraction } from '@/composables/useStoryNpcInteraction.js';
import { bootstrapStoryNpcs, ensureStoryWorldBall } from '@/services/storyNpcBootstrapService.js';
import { STORY_QUEST_DEFINITIONS, STORY_MERCHANT_STOCK } from '@/config/storyNpc.js';
import { getStoryItemDefinition } from '@/utils/storyInventory.js';
import { isStoryCameraPoseUnsafe } from '@/utils/storyCameraControls.js';
import {
  reduceStoryUiState,
  shouldResetStoryCamera,
  storyShortcutFromEvent,
} from '@/utils/storyUiState.js';

const router = useRouter();
const inventory = useStoryInventoryStore();
const storyProgress = useStoryProgressStore();
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
const dialogOpen = ref(false);
const dialogKind = ref('');
const dialogTitle = ref('');
const dialogText = ref('');
const dialogTarget = ref(null);
const dialogProfile = computed(() => {
  const profiles = {
    quest: { eyebrow: 'QUEST BOARD', subtitle: '村口委托人', symbol: '✦', tone: 'quest' },
    creator: { eyebrow: 'WORLD FORGE', subtitle: '创作师', symbol: '◇', tone: 'creator' },
    merchant: { eyebrow: 'TRAVELING MERCHANT', subtitle: '行脚商人', symbol: '◈', tone: 'merchant' },
    world_ball: { eyebrow: 'WORLD BALL', subtitle: '独立 Demo 入口', symbol: '◉', tone: 'world' },
    world_core: { eyebrow: 'WORLD CORE', subtitle: '小世界创作核心', symbol: '◎', tone: 'core' },
  };
  return profiles[dialogKind.value] || { eyebrow: 'STORY INTERACTION', subtitle: '剧情交互', symbol: '✦', tone: 'default' };
});
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
const { nearbyTarget, refresh: refreshNpcActors, interact: interactWithNpc } = useStoryNpcInteraction({
  sceneId,
  playerState: playerStateRef,
  enabled: computed(() => viewportReady.value && !menuOpen.value && !inventoryOpen.value && !mapOpen.value && !dialogOpen.value),
  onInteract: (target) => openInteraction(target),
});
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
    if (activeProjectKey.value) { await bootstrapStoryNpcs({ sceneId: sceneId.value, dayNumber: dayNumber.value, progressStore: storyProgress }); await refreshNpcActors(); }
    if (result.generated || Number(result.repairedCount) > 0) showLocationTitle();
  },
});

const gameReady = computed(() => viewportReady.value && bootstrapReady.value);
const hasBlockingOverlay = computed(() => menuOpen.value || inventoryOpen.value || mapOpen.value || dialogOpen.value);
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

const { totalGameTimeMs, dayNumber, lightingError, shutdown: shutdownStoryClock } = useStoryGameClock({
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
function handleMonsterDefeated(result = {}) {
  const stats = storyProgress.data.questStats || {};
  storyProgress.updateStats({
    minionKills: Math.max(0, Number(stats.minionKills) || 0) + (result.kind === 'minion' ? 1 : 0),
    bossKills: Math.max(0, Number(stats.bossKills) || 0) + (result.kind === 'boss' ? 1 : 0),
  });
}

function handleItemDrop(drop = {}) {
  const quantity = Math.max(0, Math.trunc(Number(drop.quantity) || 0));
  if (!drop.itemId || quantity <= 0) return;
  const result = inventory.addItem(drop.itemId, quantity);
  const stats = storyProgress.data.questStats || {};
  storyProgress.updateStats({
    fragmentCount: Math.max(0, Number(stats.fragmentCount) || 0) + (drop.itemId === 'world_fragment' ? result.added : 0),
  });
}

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
  onMonsterDefeated: handleMonsterDefeated,
  onItemDrop: handleItemDrop,
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
  if (!gameReady.value || menuOpen.value || dialogOpen.value) return;
  applyUiState({ ready: true, menuOpen: false, inventoryOpen: true, mapOpen: false });
};

const openMap = () => {
  if (!gameReady.value || menuOpen.value || dialogOpen.value) return;
  applyUiState({ ready: true, menuOpen: false, inventoryOpen: false, mapOpen: true });
};

const questOptions = STORY_QUEST_DEFINITIONS;
const enchantTypes = [
  { id: 'terrain', label: '附魔为地形组件', symbol: '▰', description: '塑造一片可探索的地形区域。' },
  { id: 'object', label: '附魔为物体组件', symbol: '◆', description: '解锁可以放置到小世界里的物体。' },
  { id: 'enemy', label: '附魔为敌人组件', symbol: '☠', description: '为 Demo 添加一个基础敌人。' },
  { id: 'objective', label: '附魔为目标组件', symbol: '◎', description: '设置一个可完成的 Demo 目标。' },
];
const merchantStock = computed(() => (STORY_MERCHANT_STOCK || []).map((stock) => ({ ...stock, label: getStoryItemDefinition(stock.itemId).name })));
const hasWorldFragment = computed(() => inventory.slots.some((slot) => slot?.itemId === 'world_fragment'));
const interactionLabel = computed(() => {
  const role = nearbyTarget.value?.interactionRole;
  if (role === 'story_npc_quest') return '与任务 NPC 对话';
  if (role === 'story_npc_creator') return '与创造 NPC 对话';
  if (role === 'story_npc_merchant') return '与商人交易';
  if (role === 'story_world_ball') return '进入小世界';
  if (role === 'story_world_core') return '打开世界核心';
  return '交互';
});
const openInteraction = (target = nearbyTarget.value) => {
  if (!target || hasBlockingOverlay.value || !gameReady.value || playerDead.value) return false;
  dialogTarget.value = target;
  const role = String(target.interactionRole || '').toLowerCase();
  dialogKind.value = role.replace(/^story_npc_/, '').replace(/^story_/, '') || 'world';
  if (dialogKind.value === 'quest') { dialogTitle.value = '村口委托'; dialogText.value = '选择一个可选委托，完成后回来领取创作资源。'; }
  else if (dialogKind.value === 'creator') { dialogTitle.value = '创作师'; dialogText.value = '把普通世界碎片附魔成可以安装到世界核心的组件。'; }
  else if (dialogKind.value === 'merchant') { dialogTitle.value = '行脚商人'; dialogText.value = '用蓝晶矿换取世界创作材料。'; }
  else if (dialogKind.value === 'world_ball') { dialogTitle.value = '世界小球'; dialogText.value = '这里将进入你的独立 Demo 小世界。'; }
  else { dialogTitle.value = '世界核心'; dialogText.value = '在创作宿主中管理你的四类世界组件。'; }
  dialogOpen.value = true; void stopCameraControls({ persist: true }); return true;
};
const closeDialog = () => { dialogOpen.value = false; dialogTarget.value = null; };
const questProgress = (quest) => storyProgress.questProgress(quest.id);
const canClaimQuest = (quest) => storyProgress.activeQuest?.id === quest.id && questProgress(quest).complete;
const acceptQuest = (quest) => {
  if (storyProgress.activeQuest?.id === quest.id && canClaimQuest(quest)) {
    const result = storyProgress.claimQuest(quest.id);
    if (result.success) {
      const reward = inventory.addItem(result.reward.itemId, result.reward.quantity);
      if (reward.added > 0 && result.reward.itemId === 'world_ball') {
        storyProgress.unlockWorldBall('demo-1');
        void ensureStoryWorldBall({ sceneId: sceneId.value, worldBallId: 'demo-1' }).then(() => refreshNpcActors());
      }
      dialogText.value = reward.remaining > 0 ? '任务完成，但背包空间不足，部分奖励未能放入。' : `任务完成，获得 ${getStoryItemDefinition(result.reward.itemId).name} ×${reward.added}。`;
    }
    return;
  }
  if (storyProgress.acceptQuest(quest.id)) dialogText.value = `已接受委托：${quest.title}。完成后回来领取奖励。`;
};
const enchantFragment = (type) => {
  const index = inventory.slots.findIndex((slot) => slot?.itemId === 'world_fragment');
  if (index < 0) { dialogText.value = '请先获得普通世界碎片。'; return; }
  inventory.selectSlot(index);
  const result = inventory.enchantSelectedItem(type.id);
  if (result.success) dialogText.value = `附魔完成：${type.label}。`;
};
const canBuy = (stock) => {
  if (!stock || storyProgress.hasMerchantPurchase(dayNumber.value, stock)) return false;
  return inventory.slots.reduce((sum, slot) => sum + (slot?.itemId === stock.currency ? slot.quantity : 0), 0) >= stock.price;
};
const buyStock = (stock) => {
  if (!canBuy(stock)) return;
  const added = inventory.addItem(stock.itemId, stock.quantity);
  if (added.added < stock.quantity) return;
  inventory.removeItem(stock.currency, stock.price);
  storyProgress.markMerchantPurchase(dayNumber.value, stock);
  dialogText.value = `已购买 ${getStoryItemDefinition(stock.itemId).name} ×${stock.quantity}。`;
};

const enterStoryCreation = () => {
  const worldBallId = String(dialogTarget.value?.name || 'StoryWorldBall_demo-1').replace(/^StoryWorldBall_/i, '') || 'demo-1';
  if (!storyProgress.unlockedWorldBalls.includes(worldBallId) && !inventory.slots.some((slot) => slot?.itemId === 'world_ball')) {
    dialogText.value = '你还没有获得这个世界小球，请先完成任务 NPC 的委托。';
    return;
  }
  dialogOpen.value = false;
  router.push({ path: '/StoryCreation', query: { worldBallId, sourceScene: sceneId.value, projectKey: activeProjectKey.value } });
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
  dialogOpen.value = false;
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
  if (dialogOpen.value) {
    event.preventDefault(); event.stopPropagation(); event.stopImmediatePropagation?.();
    if (shortcut === 'escape') closeDialog();
    return;
  }
  if (shortcut === 'interact') {
    event.preventDefault(); event.stopPropagation(); event.stopImmediatePropagation?.();
    interactWithNpc(); return;
  }
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
  storyProgress.load(activeProjectKey.value);
  if (sceneId.value && managedWorld.value) await bootstrapStoryNpcs({ sceneId: sceneId.value, dayNumber: dayNumber.value, progressStore: storyProgress });
  await refreshNpcActors();
};

watch([menuOpen, inventoryOpen, mapOpen], async ([isMenuOpen, isInventoryOpen, isMapOpen]) => {
  await nextTick();
  if (isMenuOpen) continueButtonRef.value?.focus?.();
  else if (!isInventoryOpen && !isMapOpen && !dialogOpen.value) viewportRef.value?.focus?.({ preventScroll: true });
});

watch(dayNumber, (day, previousDay) => {
  if (previousDay == null || day <= previousDay || !sceneId.value || !managedWorld.value) return;
  void bootstrapStoryNpcs({
    sceneId: sceneId.value,
    dayNumber: day,
    progressStore: storyProgress,
  }).then(() => refreshNpcActors()).catch((error) => {
    console.warn('[StoryMode] failed to refresh daily merchant NPC', error);
  });
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


.story-mode__dialog-overlay {
  position: absolute;
  inset: 0;
  z-index: 28;
  display: flex;
  align-items: flex-end;
  justify-content: center;
  padding: 24px;
  background: linear-gradient(180deg, transparent 28%, rgba(3, 5, 4, 0.14) 52%, rgba(3, 5, 4, 0.78) 100%);
  pointer-events: auto;
  backdrop-filter: blur(1.5px);
}

.story-mode__dialog {
  position: relative;
  display: grid;
  grid-template-columns: 92px minmax(0, 1fr);
  width: min(920px, calc(100vw - 48px));
  max-height: min(680px, calc(100vh - 48px));
  overflow: hidden;
  border: 1px solid rgba(216, 184, 108, 0.44);
  border-radius: 18px;
  background:
    linear-gradient(135deg, rgba(42, 37, 25, 0.98), rgba(13, 16, 13, 0.98) 48%, rgba(7, 10, 9, 0.99));
  box-shadow:
    0 28px 90px rgba(0, 0, 0, 0.68),
    0 0 0 1px rgba(0, 0, 0, 0.45),
    inset 0 1px 0 rgba(255, 244, 207, 0.08);
  color: #eee3ca;
  isolation: isolate;
}

.story-mode__dialog::before {
  position: absolute;
  inset: 0;
  z-index: -1;
  background:
    radial-gradient(circle at 8% 12%, rgba(216, 184, 108, 0.12), transparent 24%),
    radial-gradient(circle at 92% 88%, rgba(80, 134, 106, 0.12), transparent 26%);
  content: '';
  pointer-events: none;
}

.story-mode__dialog-accent {
  position: absolute;
  top: 0;
  right: 26px;
  left: 26px;
  height: 2px;
  background: linear-gradient(90deg, transparent, #d8b86c 22%, #f0d891 50%, #d8b86c 78%, transparent);
  box-shadow: 0 0 18px rgba(216, 184, 108, 0.44);
}

.story-mode__dialog--creator .story-mode__dialog-accent {
  background: linear-gradient(90deg, transparent, #79c99e 24%, #c4f0c7 50%, #79c99e 76%, transparent);
}

.story-mode__dialog--merchant .story-mode__dialog-accent {
  background: linear-gradient(90deg, transparent, #76b7d0 24%, #c8eff6 50%, #76b7d0 76%, transparent);
}

.story-mode__dialog-avatar {
  display: grid;
  place-items: center;
  align-self: stretch;
  margin: 24px 0 24px 24px;
  min-height: 82px;
  border: 1px solid rgba(216, 184, 108, 0.32);
  border-radius: 14px;
  background: linear-gradient(145deg, rgba(216, 184, 108, 0.2), rgba(216, 184, 108, 0.04));
  box-shadow: inset 0 0 22px rgba(216, 184, 108, 0.08), 0 10px 24px rgba(0, 0, 0, 0.24);
  color: #f3d98d;
}

.story-mode__dialog-avatar span {
  display: grid;
  width: 48px;
  height: 48px;
  place-items: center;
  border: 1px solid currentColor;
  border-radius: 50%;
  font-size: 25px;
  line-height: 1;
  text-shadow: 0 0 16px currentColor;
}

.story-mode__dialog--creator .story-mode__dialog-avatar {
  border-color: rgba(121, 201, 158, 0.36);
  background: linear-gradient(145deg, rgba(121, 201, 158, 0.2), rgba(121, 201, 158, 0.04));
  color: #a8e7b8;
}

.story-mode__dialog--merchant .story-mode__dialog-avatar {
  border-color: rgba(118, 183, 208, 0.38);
  background: linear-gradient(145deg, rgba(118, 183, 208, 0.2), rgba(118, 183, 208, 0.04));
  color: #a6e4ed;
}

.story-mode__dialog--world .story-mode__dialog-avatar,
.story-mode__dialog--core .story-mode__dialog-avatar {
  border-color: rgba(139, 180, 255, 0.34);
  background: linear-gradient(145deg, rgba(139, 180, 255, 0.2), rgba(139, 180, 255, 0.04));
  color: #aed0ff;
}

.story-mode__dialog-main {
  min-width: 0;
  padding: 24px 28px 20px;
}

.story-mode__dialog-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 18px;
}

.story-mode__dialog-kicker {
  margin: 0 0 6px;
  color: #b39b66;
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.24em;
}

.story-mode__dialog h2 {
  margin: 0;
  color: #fff1cd;
  font-family: 'STKaiti', 'KaiTi', serif;
  font-size: clamp(25px, 3vw, 34px);
  font-weight: 500;
  letter-spacing: 0.08em;
  line-height: 1.15;
  text-shadow: 0 2px 14px rgba(0, 0, 0, 0.42);
}

.story-mode__dialog-close {
  display: grid;
  width: 32px;
  height: 32px;
  flex: 0 0 auto;
  place-items: center;
  border: 1px solid rgba(238, 224, 186, 0.18);
  border-radius: 50%;
  background: rgba(255, 255, 255, 0.045);
  color: #b9ad91;
  cursor: pointer;
  font-size: 24px;
  line-height: 1;
  transition: border-color 160ms ease, background 160ms ease, color 160ms ease, transform 160ms ease;
}

.story-mode__dialog-close:hover,
.story-mode__dialog-close:focus-visible {
  border-color: rgba(216, 184, 108, 0.7);
  background: rgba(216, 184, 108, 0.12);
  color: #f6dda0;
  outline: none;
  transform: rotate(8deg);
}

.story-mode__dialog-speaker {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 10px;
  color: #8f9b88;
  font-size: 11px;
  letter-spacing: 0.06em;
}

.story-mode__dialog-speaker-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #d8b86c;
  box-shadow: 0 0 10px rgba(216, 184, 108, 0.72);
}

.story-mode__dialog-separator {
  color: rgba(216, 184, 108, 0.48);
}

.story-mode__dialog-text {
  max-width: 720px;
  margin: 18px 0 20px;
  color: #d5ccb8;
  font-size: 14px;
  line-height: 1.75;
}

.story-mode__quest-list,
.story-mode__enchant-grid {
  display: grid;
  gap: 9px;
}

.story-mode__quest-button,
.story-mode__choice-card {
  width: 100%;
  border: 1px solid rgba(216, 184, 108, 0.18);
  border-radius: 11px;
  background: rgba(255, 255, 255, 0.045);
  color: #d9cfb9;
  cursor: pointer;
  text-align: left;
  transition: border-color 160ms ease, background 160ms ease, transform 160ms ease, box-shadow 160ms ease;
}

.story-mode__quest-button {
  display: grid;
  grid-template-columns: 34px minmax(0, 1fr) auto;
  align-items: center;
  gap: 12px;
  min-height: 70px;
  padding: 11px 13px;
}

.story-mode__quest-button:hover:not(:disabled),
.story-mode__quest-button:focus-visible,
.story-mode__choice-card:hover:not(:disabled),
.story-mode__choice-card:focus-visible {
  border-color: rgba(216, 184, 108, 0.65);
  background: linear-gradient(100deg, rgba(216, 184, 108, 0.14), rgba(255, 255, 255, 0.06));
  box-shadow: 0 8px 22px rgba(0, 0, 0, 0.2);
  outline: none;
  transform: translateY(-1px);
}

.story-mode__quest-button:disabled,
.story-mode__choice-card:disabled {
  cursor: not-allowed;
  filter: saturate(0.55);
  opacity: 0.48;
}

.story-mode__quest-icon,
.story-mode__choice-icon {
  display: grid;
  place-items: center;
  border: 1px solid rgba(216, 184, 108, 0.28);
  border-radius: 9px;
  background: rgba(216, 184, 108, 0.1);
  color: #e6c879;
  font-size: 17px;
}

.story-mode__quest-icon {
  width: 34px;
  height: 34px;
}

.story-mode__quest-copy {
  display: grid;
  min-width: 0;
  gap: 3px;
}

.story-mode__quest-copy strong {
  overflow: hidden;
  color: #f0e3c1;
  font-size: 13px;
  font-weight: 700;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.story-mode__quest-copy span,
.story-mode__quest-copy small {
  overflow: hidden;
  color: #9e9a89;
  font-size: 11px;
  line-height: 1.35;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.story-mode__quest-copy small {
  color: #cda964;
}

.story-mode__quest-action {
  display: flex;
  align-items: center;
  gap: 8px;
  color: #d8b86c;
  font-size: 11px;
  font-weight: 700;
  white-space: nowrap;
}

.story-mode__quest-action b,
.story-mode__dialog-actions b,
.story-mode__choice-card > b {
  color: #f0d88f;
  font-size: 19px;
  font-weight: 400;
  line-height: 1;
}

.story-mode__merchant-price {
  color: #9fd7e7;
}

.story-mode__merchant-button .story-mode__quest-icon {
  border-color: rgba(118, 183, 208, 0.3);
  background: rgba(118, 183, 208, 0.1);
  color: #a6e4ed;
}

.story-mode__choice-card {
  display: grid;
  grid-template-columns: 38px minmax(0, 1fr) auto;
  align-items: center;
  gap: 11px;
  min-height: 62px;
  padding: 10px 13px;
}

.story-mode__choice-icon {
  width: 38px;
  height: 38px;
  font-size: 19px;
}

.story-mode__choice-card > span:nth-child(2) {
  display: grid;
  gap: 3px;
  min-width: 0;
}

.story-mode__choice-card strong {
  color: #e9e0ca;
  font-size: 13px;
}

.story-mode__choice-card small {
  overflow: hidden;
  color: #9e9a89;
  font-size: 11px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.story-mode__dialog-actions {
  display: grid;
  gap: 10px;
  margin-top: 4px;
}

.story-mode__dialog-actions .story-button {
  display: flex;
  align-items: center;
  justify-content: space-between;
  text-align: left;
}

.story-mode__dialog-footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  margin-top: 20px;
  padding-top: 14px;
  border-top: 1px solid rgba(255, 255, 255, 0.08);
  color: #847f70;
  font-size: 10px;
  letter-spacing: 0.04em;
}

.story-mode__dialog-footer kbd {
  margin-right: 5px;
  padding: 3px 6px;
  border: 1px solid rgba(216, 184, 108, 0.34);
  border-radius: 4px;
  background: rgba(216, 184, 108, 0.08);
  color: #d8b86c;
  font: inherit;
}

.story-mode__dialog-footer-close {
  border: 0;
  background: transparent;
  color: #b7ad98;
  cursor: pointer;
  font: inherit;
  transition: color 160ms ease;
}

.story-mode__dialog-footer-close:hover,
.story-mode__dialog-footer-close:focus-visible {
  color: #f1d68e;
  outline: none;
}

.story-dialog-enter-active,
.story-dialog-leave-active {
  transition: opacity 180ms ease;
}

.story-dialog-enter-active .story-mode__dialog,
.story-dialog-leave-active .story-mode__dialog {
  transition: transform 220ms ease, opacity 180ms ease;
}

.story-dialog-enter-from,
.story-dialog-leave-to {
  opacity: 0;
}

.story-dialog-enter-from .story-mode__dialog,
.story-dialog-leave-to .story-mode__dialog {
  opacity: 0;
  transform: translateY(18px) scale(0.985);
}

@media (max-width: 680px) {
  .story-mode__dialog-overlay {
    padding: 12px;
  }

  .story-mode__dialog {
    grid-template-columns: 1fr;
    width: calc(100vw - 24px);
    max-height: calc(100vh - 24px);
    border-radius: 14px;
  }

  .story-mode__dialog-avatar {
    display: none;
  }

  .story-mode__dialog-main {
    padding: 22px 18px 16px;
  }

  .story-mode__quest-button {
    grid-template-columns: 32px minmax(0, 1fr);
  }

  .story-mode__quest-action {
    grid-column: 2;
    justify-content: flex-start;
    margin-top: -3px;
  }

  .story-mode__choice-card {
    grid-template-columns: 34px minmax(0, 1fr) auto;
  }

  .story-mode__choice-icon {
    width: 34px;
    height: 34px;
  }
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
