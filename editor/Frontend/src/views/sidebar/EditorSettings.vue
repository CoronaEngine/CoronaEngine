<template>
  <main class="esc-panel">
    <DockTitleBar
      v-if="!isDocked"
      :title="t('editorSettings.title')"
      extraClass="bg-[#242724]"
      routePath="/SetUp"
      @close="handleContinue"
    />

    <section class="esc-body" aria-labelledby="esc-menu-title">

      <section class="settings-section">
        <div class="section-heading">
          <h3>{{ t('editorSettings.common') }}</h3>
        </div>
        <label class="locale-field">
          <span>{{ t('locale.language') }}</span>
          <div class="locale-switch" role="group" :aria-label="t('locale.switchTo')">
            <button
              type="button"
              class="locale-option"
              :class="{ active: locale === 'zh-CN' }"
              :aria-pressed="locale === 'zh-CN'"
              @click="handleLocaleChange('zh-CN')"
            >
              {{ t('locale.zhCN') }}
            </button>
            <button
              type="button"
              class="locale-option"
              :class="{ active: locale === 'en-US' }"
              :aria-pressed="locale === 'en-US'"
              @click="handleLocaleChange('en-US')"
            >
              English
            </button>
          </div>
        </label>
      </section>

      <section class="settings-section viewport-settings" data-guidance="settings-viewport">
        <div class="section-heading">
          <h3>{{ t('editorSettings.viewport') }}</h3>
          <span v-if="viewportControlState.sceneId" class="current-scene" :title="viewportControlState.sceneId">
            {{ viewportControlState.sceneId }}
          </span>
        </div>

        <div class="viewport-setting-row" data-guidance="settings-viewport-ui">
          <span class="setting-label">{{ t('editorSettings.viewportUiMode') }}</span>
          <div class="viewport-mode-switch" role="group" :aria-label="t('editorSettings.viewportUiMode')">
            <button
              v-for="item in viewportControlState.viewportUiModes"
              :key="item.mode"
              type="button"
              class="viewport-mode-option"
              :class="{ active: viewportControlState.viewportUiMode === item.mode }"
              :disabled="!viewportControlState.available"
              :title="viewportModeTitle(item)"
              @click="setViewportUiMode(item.mode)"
            >
              {{ item.label }}
            </button>
          </div>
        </div>

        <label class="viewport-setting-row speed-setting" data-guidance="settings-camera-speed">
          <span class="setting-label">{{ t('editorSettings.cameraSpeed') }}</span>
          <input
            v-model.number="viewportControlState.cameraSpeed"
            type="range"
            min="0.01"
            max="2"
            step="0.01"
            :disabled="!viewportControlState.available"
            :title="t('editorSettings.cameraSpeed')"
            @input="handleCameraSpeedInput"
          />
          <strong>{{ cameraSpeedLabel }}</strong>
        </label>

        <label class="viewport-setting-row grid-setting" data-guidance="settings-grid">
          <span class="setting-label">{{ t('editorSettings.editGrid') }}</span>
          <input
            v-model="viewportControlState.gridEnabled"
            type="checkbox"
            :disabled="!viewportControlState.available || gridApplying"
            :title="t('editorSettings.editGridDescription')"
            @change="setGridEnabled"
          />
          <span class="setting-description">{{ t('editorSettings.editGridDescription') }}</span>
        </label>
      </section>

      <section class="settings-section leave-section">
        <div class="section-heading">
          <h3>{{ t('editorSettings.leave') }}</h3>
        </div>
        <div class="home-panel" :class="{ confirming: confirmHome }">
          <template v-if="confirmHome">
            <p>{{ t('editorSettings.confirmHome') }}</p>
            <div class="confirm-actions">
              <button class="small-button" type="button" @click="cancelHome">{{ t('common.cancel') }}</button>
              <button class="small-button danger" type="button" @click="goHome">
                {{ t('editorSettings.confirmHomeButton') }}
              </button>
            </div>
          </template>
          <button v-else class="home-button" type="button" @click="confirmHome = true">
            {{ t('editorSettings.home') }}
          </button>
        </div>
      </section>
    </section>
  </main>
</template>

<script setup>
import { computed, onBeforeUnmount, onMounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import { useI18n } from 'vue-i18n';
import { useDockPanel } from '@/composables/useDockPanel.js';
import { setLocale } from '@/i18n/index.js';
import { appService } from '@/services/appService.js';
import { coronaEventBus } from '@/utils/eventBus.js';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';

const router = useRouter();
const { t, locale } = useI18n();
const { closePanel, isDocked } = useDockPanel();
const confirmHome = ref(false);
const EDITOR_CONTROLS_KEY = '__coronaEditorControls';
const defaultViewportControls = {
  available: false,
  sceneId: '',
  viewportUiMode: 'flat2d',
  viewportUiModes: [
    { mode: 'flat2d', label: '2D UI' },
    { mode: 'stereo3d', label: '3D UI' },
  ],
  cameraSpeed: 0.2,
  gridEnabled: true,
};
const viewportControlState = ref({ ...defaultViewportControls });
const gridApplying = ref(false);
const cameraSpeedLabel = computed(() => Number(viewportControlState.value.cameraSpeed || 0).toFixed(2));
let viewportControlPollTimer = null;
let speedApplyTimer = null;

function normalizeViewportControls(state = {}) {
  const modes = Array.isArray(state.viewportUiModes) && state.viewportUiModes.length
    ? state.viewportUiModes
    : defaultViewportControls.viewportUiModes;
  const speed = Number(state.cameraSpeed);
  return {
    available: Boolean(state.available),
    sceneId: String(state.sceneId || ''),
    viewportUiMode: String(state.viewportUiMode || defaultViewportControls.viewportUiMode),
    viewportUiModes: modes.map((item) => ({
      mode: String(item.mode || ''),
      label: String(item.label || item.mode || ''),
      title: String(item.title || item.label || item.mode || ''),
    })).filter((item) => item.mode),
    cameraSpeed: Number.isFinite(speed)
      ? Math.min(2, Math.max(0.01, speed))
      : defaultViewportControls.cameraSpeed,
    gridEnabled: typeof state.gridEnabled === 'boolean'
      ? state.gridEnabled
      : defaultViewportControls.gridEnabled,
  };
}

function getEditorControls() {
  return typeof window === 'undefined' ? null : window[EDITOR_CONTROLS_KEY] || null;
}

function syncViewportControls(state = {}) {
  viewportControlState.value = normalizeViewportControls(state);
}

function requestViewportControlsState() {
  const controls = getEditorControls();
  if (controls && typeof controls.getState === 'function') {
    try {
      syncViewportControls(controls.getState());
      return;
    } catch (error) {
      console.warn('[EditorSettings] failed to read viewport controls', error);
    }
  }
  appService.crossTabBroadcast('viewport-controls-request', { action: 'getState' }).catch(() => {});
}

async function setViewportUiMode(mode) {
  const controls = getEditorControls();
  if (controls && typeof controls.setViewportUiMode === 'function') {
    const nextState = await controls.setViewportUiMode(mode);
    if (nextState !== false) syncViewportControls(nextState);
    return;
  }
  viewportControlState.value.viewportUiMode = mode;
  appService.crossTabBroadcast('viewport-controls-request', {
    action: 'setViewportUiMode',
    mode,
  }).catch(() => requestViewportControlsState());
}

async function applyCameraSpeed(value) {
  const speed = Math.min(2, Math.max(0.01, Number(value) || defaultViewportControls.cameraSpeed));
  const controls = getEditorControls();
  if (controls && typeof controls.setCameraSpeed === 'function') {
    const nextState = await controls.setCameraSpeed(speed);
    if (nextState !== false) syncViewportControls(nextState);
    return;
  }
  appService.crossTabBroadcast('viewport-controls-request', {
    action: 'setCameraSpeed',
    value: speed,
  }).catch(() => requestViewportControlsState());
}

function handleCameraSpeedInput() {
  if (speedApplyTimer) window.clearTimeout(speedApplyTimer);
  speedApplyTimer = window.setTimeout(() => {
    speedApplyTimer = null;
    void applyCameraSpeed(viewportControlState.value.cameraSpeed);
  }, 60);
}

async function setGridEnabled() {
  if (gridApplying.value || !viewportControlState.value.sceneId) return;
  const enabled = Boolean(viewportControlState.value.gridEnabled);
  const sceneId = viewportControlState.value.sceneId;
  gridApplying.value = true;
  try {
    const controls = getEditorControls();
    if (controls && typeof controls.setGridEnabled === 'function') {
      const nextState = await controls.setGridEnabled(enabled, sceneId);
      if (nextState === false) requestViewportControlsState();
      else syncViewportControls(nextState);
      return;
    }
    await appService.crossTabBroadcast('viewport-controls-request', {
      action: 'setGridEnabled',
      enabled,
      sceneId,
    });
  } catch (error) {
    console.warn('[EditorSettings] failed to update scene grid', error);
    requestViewportControlsState();
  } finally {
    gridApplying.value = false;
  }
}

function viewportModeTitle(item = {}) {
  if (item.mode === 'flat2d') return t('editorSettings.viewport2dDescription');
  if (item.mode === 'stereo3d') return t('editorSettings.viewport3dDescription');
  return item.title || item.label || '';
}

function onViewportControlsState(state) {
  syncViewportControls(state);
}



function handleLocaleChange(nextLocale) {
  setLocale(nextLocale);
}

onMounted(() => {
  coronaEventBus.on('viewport-controls-state', onViewportControlsState);
  requestViewportControlsState();
  viewportControlPollTimer = window.setInterval(requestViewportControlsState, 1000);
});

onBeforeUnmount(() => {
  coronaEventBus.off('viewport-controls-state', onViewportControlsState);
  if (viewportControlPollTimer) window.clearInterval(viewportControlPollTimer);
  viewportControlPollTimer = null;
  if (speedApplyTimer) window.clearTimeout(speedApplyTimer);
  speedApplyTimer = null;
});

function handleContinue() {
  confirmHome.value = false;
  closePanel();

  if (!isDocked && !hasNativeDockCommand()) {
    router.push('/');
  }
}

function hasNativeDockCommand() {
  return Boolean(
    typeof window !== 'undefined' &&
      window.coronaBridge &&
      typeof window.coronaBridge.dockCommand === 'function'
  );
}

function cancelHome() {
  confirmHome.value = false;
}

function goHome() {
  confirmHome.value = false;
  closePanel();
  router.push('/StartScreen');
}
</script>

<style scoped>
.esc-panel {
  --panel-bg: #151715;
  --panel-surface: #1c1f1c;
  --panel-surface-hover: #242924;
  --panel-border: #313731;
  --panel-border-strong: #4e5a4b;
  --text-main: #eef1eb;
  --text-muted: #9da79a;
  --accent: #8ca96f;
  --accent-strong: #a8c783;
  --danger: #c7826f;
  --danger-surface: #30201d;
  --warn: #d3aa68;

  flex: 1;
  min-height: 0;
  width: 100%;
  height: 100%;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  background: linear-gradient(180deg, #1b1d1b 0%, var(--panel-bg) 100%);
  color: var(--text-main);
  font-family:
    "Segoe UI",
    "Microsoft YaHei UI",
    "Microsoft YaHei",
    "Noto Sans SC",
    sans-serif;
  -webkit-font-smoothing: antialiased;
}

.esc-body {
  flex: 1;
  min-height: 0;
  display: flex;
  flex-direction: column;
  gap: 14px;
  padding: 18px;
  overflow-y: auto;
  scrollbar-width: thin;
  scrollbar-color: rgba(140, 169, 111, 0.42) transparent;
}

.esc-header,
.section-heading,
.section-heading.split,
.confirm-actions {
  display: flex;
  align-items: center;
}

.esc-header {
  justify-content: space-between;
  gap: 16px;
  padding-bottom: 12px;
  border-bottom: 1px solid var(--panel-border);
}

.esc-kicker {
  margin: 0 0 4px;
  color: var(--accent);
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0;
  text-transform: uppercase;
}

.esc-header h2 {
  margin: 0;
  color: var(--text-main);
  font-size: 24px;
  font-weight: 700;
  line-height: 1.15;
}

.esc-state {
  max-width: 48%;
  padding: 5px 9px;
  border: 1px solid rgba(140, 169, 111, 0.42);
  border-radius: 6px;
  overflow: hidden;
  color: var(--accent-strong);
  background: rgba(140, 169, 111, 0.08);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.esc-state.offline {
  color: var(--warn);
  border-color: rgba(211, 170, 104, 0.42);
  background: rgba(211, 170, 104, 0.08);
}

.status-line {
  margin: 0;
  min-height: 28px;
  padding: 6px 9px;
  border: 1px solid var(--panel-border);
  border-radius: 6px;
  color: var(--text-main);
  background: rgba(28, 31, 28, 0.86);
  font-size: 12px;
  line-height: 1.35;
}

.status-line.success {
  color: var(--accent-strong);
  border-color: rgba(140, 169, 111, 0.45);
}

.status-line.warn {
  color: var(--warn);
  border-color: rgba(211, 170, 104, 0.45);
}

.status-line.error {
  color: #e2afa3;
  border-color: rgba(199, 130, 111, 0.5);
}

.settings-section {
  display: grid;
  gap: 9px;
  padding: 12px;
  border: 1px solid var(--panel-border);
  border-radius: 8px;
  background: rgba(28, 31, 28, 0.66);
}

.section-heading {
  justify-content: space-between;
  min-height: 20px;
  gap: 10px;
}

.section-heading h3 {
  margin: 0;
  color: var(--text-main);
  font-size: 13px;
  font-weight: 700;
  line-height: 1.2;
}

.section-heading span {
  overflow: hidden;
  color: var(--text-muted);
  font-size: 12px;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.locale-field {
  display: flex;
  align-items: center;
  justify-content: space-between;
  min-width: 0;
  gap: 10px;
  padding: 8px 10px;
  border: 1px solid var(--panel-border);
  border-radius: 7px;
  background: rgba(24, 27, 24, 0.58);
}

.viewport-settings {
  gap: 10px;
}

.current-scene {
  max-width: 58%;
}

.viewport-setting-row {
  min-width: 0;
  display: grid;
  grid-template-columns: minmax(76px, auto) minmax(0, 1fr) auto;
  align-items: center;
  gap: 10px;
  padding: 8px 10px;
  border: 1px solid var(--panel-border);
  border-radius: 7px;
  background: rgba(24, 27, 24, 0.58);
}

.setting-label {
  color: var(--text-muted);
  font-size: 12px;
  font-weight: 600;
  white-space: nowrap;
}

.viewport-mode-switch {
  grid-column: 2 / 4;
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 3px;
  padding: 2px;
  border: 1px solid var(--panel-border);
  border-radius: 6px;
  background: rgba(17, 19, 17, 0.96);
}

.viewport-mode-option {
  min-width: 0;
  height: 28px;
  border: 0;
  border-radius: 4px;
  color: var(--text-muted);
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 12px;
  font-weight: 600;
}

.viewport-mode-option:hover:not(:disabled) {
  color: var(--text-main);
  background: rgba(255, 255, 255, 0.06);
}

.viewport-mode-option.active {
  color: #f4f7ef;
  background: rgba(216, 184, 108, 0.32);
  box-shadow: inset 0 0 0 1px rgba(216, 184, 108, 0.35);
}

.viewport-mode-option:disabled {
  cursor: not-allowed;
  opacity: 0.44;
}

.speed-setting input[type='range'] {
  width: 100%;
  min-width: 0;
  accent-color: var(--accent-strong);
  cursor: pointer;
}

.speed-setting strong {
  width: 36px;
  color: var(--text-main);
  font-size: 12px;
  font-variant-numeric: tabular-nums;
  text-align: right;
}

.grid-setting {
  grid-template-columns: minmax(76px, auto) auto minmax(0, 1fr);
}

.grid-setting input[type='checkbox'] {
  width: 16px;
  height: 16px;
  accent-color: var(--accent-strong);
}

.setting-description {
  min-width: 0;
  overflow: hidden;
  color: var(--text-muted);
  font-size: 11px;
  line-height: 1.35;
  text-overflow: ellipsis;
  white-space: nowrap;
}


.locale-field span {
  min-width: 0;
  overflow: hidden;
  color: var(--text-muted);
  font-size: 12px;
  font-weight: 600;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.locale-switch {
  display: grid;
  grid-template-columns: minmax(0, 1fr) minmax(0, 1fr);
  min-width: 148px;
  max-width: 58%;
  padding: 2px;
  border: 1px solid var(--panel-border);
  border-radius: 6px;
  background: rgba(17, 19, 17, 0.96);
  gap: 2px;
}

.locale-option {
  min-width: 0;
  height: 26px;
  padding: 0 8px;
  border: 0;
  border-radius: 4px;
  color: var(--text-muted);
  background: transparent;
  cursor: pointer;
  font: inherit;
  font-size: 12px;
  font-weight: 600;
  line-height: 26px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.locale-option:hover {
  color: var(--text-main);
  background: rgba(255, 255, 255, 0.06);
}

.locale-option.active {
  color: #f4f7ef;
  background: rgba(216, 184, 108, 0.32);
  box-shadow: inset 0 0 0 1px rgba(216, 184, 108, 0.35);
}

.button-grid {
  display: grid;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  gap: 8px;
}

.button-grid.four,
.button-grid.tools {
  grid-template-columns: repeat(4, minmax(0, 1fr));
}

.button-grid.render {
  grid-template-columns: repeat(2, minmax(0, 1fr));
}

.home-button,
.small-button,
.text-button {
  border: 1px solid var(--panel-border);
  border-radius: 7px;
  font: inherit;
  cursor: pointer;
  transition:
    background-color 160ms ease,
    border-color 160ms ease,
    color 160ms ease,
    transform 160ms ease,
    box-shadow 160ms ease;
}


.apply-button {
  color: #eff7e8;
  border-color: rgba(140, 169, 111, 0.58);
  background: rgba(140, 169, 111, 0.18);
}


.home-button:hover:not(:disabled),
.small-button:hover:not(:disabled),
.text-button:hover:not(:disabled) {
  color: #f6f8f3;
  background: var(--panel-surface-hover);
  border-color: var(--panel-border-strong);
  transform: translateY(-1px);
}

.apply-button:hover:not(:disabled) {
  background: rgba(140, 169, 111, 0.24);
  border-color: var(--accent-strong);
}

.home-button:active:not(:disabled),
.small-button:active:not(:disabled),
.text-button:active:not(:disabled) {
  transform: translateY(1px) scale(0.99);
}

.home-button:focus-visible,
.small-button:focus-visible,
.text-button:focus-visible,
.field input:focus-visible {
  outline: 2px solid var(--accent-strong);
  outline-offset: 2px;
}

.home-button:disabled,
.small-button:disabled,
.text-button:disabled,
.field input:disabled {
  cursor: not-allowed;
  opacity: 0.44;
}

.text-button {
  min-height: 28px;
  padding: 4px 8px;
  color: var(--accent-strong);
  background: transparent;
  font-size: 12px;
}

.physics-grid {
  display: grid;
  grid-template-columns: repeat(3, minmax(0, 1fr));
  gap: 8px;
}

.field {
  display: grid;
  gap: 5px;
  min-width: 0;
}

.field span {
  color: var(--text-muted);
  font-size: 12px;
  line-height: 1.2;
}

.field input {
  width: 100%;
  min-width: 0;
  height: 34px;
  padding: 6px 8px;
  border: 1px solid var(--panel-border);
  border-radius: 6px;
  color: var(--text-main);
  background: rgba(17, 19, 17, 0.96);
  font: inherit;
  font-size: 13px;
  font-variant-numeric: tabular-nums;
}

.field input:hover:not(:disabled) {
  border-color: var(--panel-border-strong);
}

.apply-button {
  width: 100%;
}

.leave-section {
  margin-bottom: 2px;
}

.home-panel {
  min-height: 42px;
}

.home-panel.confirming {
  display: grid;
  gap: 10px;
  padding: 10px;
  border: 1px solid rgba(199, 130, 111, 0.38);
  border-radius: 8px;
  background: rgba(48, 32, 29, 0.66);
}

.home-panel p {
  margin: 0;
  color: #e8beb4;
  font-size: 13px;
  line-height: 1.4;
}

.home-button {
  width: 100%;
  min-height: 40px;
  color: #e8beb4;
  background: rgba(48, 32, 29, 0.42);
  border-color: rgba(199, 130, 111, 0.38);
}

.home-button:hover:not(:disabled) {
  background: var(--danger-surface);
  border-color: rgba(199, 130, 111, 0.7);
}

.confirm-actions {
  gap: 8px;
}

.small-button {
  flex: 1;
  min-height: 36px;
  padding: 7px 10px;
  color: var(--text-main);
  background: rgba(24, 27, 24, 0.94);
  font-size: 13px;
}

.small-button.danger {
  color: #f0d2cb;
  background: rgba(199, 130, 111, 0.12);
  border-color: rgba(199, 130, 111, 0.55);
}

.small-button.danger:hover:not(:disabled) {
  background: rgba(199, 130, 111, 0.2);
  border-color: rgba(199, 130, 111, 0.82);
}

@media (max-width: 620px) {
  .esc-body {
    padding: 16px;
  }

  .button-grid.four,
  .button-grid.tools {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .physics-grid {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 430px) {
  .esc-header {
    align-items: flex-start;
    flex-direction: column;
    gap: 8px;
  }

  .esc-state {
    max-width: 100%;
  }

  .locale-field {
    align-items: stretch;
    flex-direction: column;
  }

  .locale-switch {
    max-width: 100%;
    width: 100%;
  }

  .button-grid,
  .button-grid.four,
  .button-grid.tools,
  .button-grid.render,
  .physics-grid {
    grid-template-columns: 1fr;
  }
}
</style>
