<template>
  <main class="story-mode" aria-label="剧情模式">
    <div
      ref="viewportRef"
      class="story-mode__viewport"
      :class="{ 'story-mode__viewport--looking': isLooking }"
      aria-label="剧情世界画面"
      tabindex="-1"
      @dragstart.prevent
    ></div>

    <section v-if="status !== 'ready'" class="story-mode__status" aria-live="polite">
      <div class="story-mode__status-card">
        <div v-if="status === 'loading'" class="story-mode__spinner" aria-hidden="true"></div>
        <h1>{{ status === 'loading' ? '正在进入世界' : '世界加载失败' }}</h1>
        <p v-if="status === 'loading'">正在连接当前场景与活动相机…</p>
        <p v-else>{{ errorMessage }}</p>
        <div v-if="status === 'error'" class="story-mode__status-actions">
          <button
            type="button"
            class="story-button story-button--secondary"
            :disabled="exitPending"
            @click="exitToStart"
          >
            返回主界面
          </button>
          <button type="button" class="story-button story-button--primary" @click="retry">
            重试
          </button>
        </div>
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

import { useNativeSceneViewport } from '@/composables/useNativeSceneViewport.js';
import { useStoryCameraControls } from '@/composables/useStoryCameraControls.js';

const router = useRouter();
const viewportRef = ref(null);
const continueButtonRef = ref(null);
const menuOpen = ref(false);
const exitPending = ref(false);
const { status, errorMessage, cameraBinding, refreshCameraBinding, retry } =
  useNativeSceneViewport(viewportRef);
const controlsEnabled = computed(() => status.value === 'ready' && !menuOpen.value);
const {
  isLooking,
  stop: stopCameraControls,
  persistPose,
} = useStoryCameraControls({
  viewportRef,
  cameraBinding,
  enabled: controlsEnabled,
  refreshCameraBinding,
});

const closeMenu = () => {
  menuOpen.value = false;
};

const exitToStart = async () => {
  if (exitPending.value) return;
  exitPending.value = true;
  menuOpen.value = false;
  await stopCameraControls({ persist: false });
  await persistPose();
  await router.push('/StartScreen');
};

const handleEscape = (event) => {
  if (event.repeat || event.ctrlKey || event.altKey || event.metaKey || event.shiftKey) return;
  if (event.key !== 'Escape' && event.code !== 'Escape') return;

  event.preventDefault();
  event.stopPropagation();
  event.stopImmediatePropagation?.();
  if (!menuOpen.value) void stopCameraControls({ persist: true });
  menuOpen.value = !menuOpen.value;
};

watch(menuOpen, async (isOpen) => {
  await nextTick();
  if (isOpen) continueButtonRef.value?.focus?.();
  else viewportRef.value?.focus?.({ preventScroll: true });
});

onMounted(() => {
  window.addEventListener('keydown', handleEscape, true);
});

onUnmounted(() => {
  window.removeEventListener('keydown', handleEscape, true);
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

.story-mode__status,
.story-mode__menu-overlay {
  position: absolute;
  inset: 0;
  z-index: 10;
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

@keyframes story-spin {
  to {
    transform: rotate(360deg);
  }
}
</style>
