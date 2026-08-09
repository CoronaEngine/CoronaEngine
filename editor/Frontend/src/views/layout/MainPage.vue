<template>
  <div class="relative flex flex-col flex-1 min-h-0 h-full w-full" tabindex="0">
    <!-- 顶部菜单栏 -->
    <div
      v-if="false"
      class="w-full bg-[#2d2d2d] text-gray-200 border-b border-gray-700 h-10 flex items-center px-4 space-x-6 text-sm shadow-md"
    >
      <!-- 项目菜单 -->
      <div class="relative">
        <button
          class="hover:bg-[#3d3d3d] px-3 py-1.5 rounded transition-colors duration-200"
          :class="{ 'bg-[#3d3d3d]': activeMenu === 'project' }"
          @click="toggleMenu('project')"
        >
          项目
        </button>
        <div
          v-if="activeMenu === 'project'"
          class="absolute top-full left-0 mt-1 w-48 bg-[#2d2d2d] border border-gray-700 rounded shadow-lg z-50"
        >
          <div class="py-1">
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleNewProject"
            >
              新建项目
            </a>
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleOpenProject"
            >
              打开项目
            </a>
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleProjectSettings"
            >
              项目设置
            </a>
            <hr class="border-gray-700 my-1" />
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleSaveProject"
            >
              保存项目
            </a>
          </div>
        </div>
      </div>

      <!-- 视图菜单 - 修改为动态渲染 -->
      <div class="relative">
        <button
          class="hover:bg-[#3d3d3d] px-3 py-1.5 rounded transition-colors duration-200"
          :class="{ 'bg-[#3d3d3d]': activeMenu === 'view' }"
          @click="toggleMenu('view')"
        >
          视图
        </button>
        <div
          v-if="activeMenu === 'view'"
          class="absolute top-full left-0 mt-1 w-56 bg-[#2d2d2d] border border-gray-700 rounded shadow-lg z-50"
        >
          <div class="py-1">
            <a
              v-for="tool in viewStates"
              :key="tool.id"
              href="#"
              class="flex items-center justify-between px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="toggleViewTool(tool)"
            >
              <span>{{ tool.name }}</span>
              <span class="text-sm" :class="tool.open ? 'text-green-400' : 'text-red-400'">
                {{ tool.open ? '√' : '×' }}
              </span>
            </a>
          </div>
        </div>
      </div>

      <!-- 物理参数菜单 -->
      <div class="relative">
        <button
          class="hover:bg-[#3d3d3d] px-3 py-1.5 rounded transition-colors duration-200"
          :class="{ 'bg-[#3d3d3d]': activeMenu === 'physics' }"
          @click="toggleMenu('physics')"
        >
          物理
        </button>
        <div
          v-if="activeMenu === 'physics'"
          class="absolute top-full left-0 mt-1 w-72 bg-[#2d2d2d] border border-gray-700 rounded shadow-lg z-50 p-3 space-y-3"
          @click.stop
        >
          <!-- 重力 -->
          <div>
            <label class="block text-xs text-gray-400 mb-1">重力 (X, Y, Z)</label>
            <div class="flex gap-1">
              <input
                v-model.number="physicsParams.gravityX"
                type="number"
                step="0.1"
                class="w-1/3 bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1 text-xs text-white"
                placeholder="X"
              />
              <input
                v-model.number="physicsParams.gravityY"
                type="number"
                step="0.1"
                class="w-1/3 bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1 text-xs text-white"
                placeholder="Y"
              />
              <input
                v-model.number="physicsParams.gravityZ"
                type="number"
                step="0.1"
                class="w-1/3 bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1 text-xs text-white"
                placeholder="Z"
              />
            </div>
          </div>
          <!-- 地面高度 -->
          <div>
            <label class="block text-xs text-gray-400 mb-1">地面高度 (Floor Y)</label>
            <input
              v-model.number="physicsParams.floorY"
              type="number"
              step="0.1"
              class="w-full bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1 text-xs text-white"
            />
          </div>
          <!-- 地面弹性 -->
          <div>
            <label class="block text-xs text-gray-400 mb-1">地面弹性系数</label>
            <input
              v-model.number="physicsParams.floorRestitution"
              type="number"
              step="0.05"
              min="0"
              max="1"
              class="w-full bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1 text-xs text-white"
            />
          </div>
          <!-- 时间步长 -->
          <div>
            <label class="block text-xs text-gray-400 mb-1">物理步长 (秒)</label>
            <input
              v-model.number="physicsParams.fixedDt"
              type="number"
              step="0.001"
              min="0.001"
              class="w-full bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1 text-xs text-white"
            />
          </div>
          <!-- 应用按钮 -->
          <button
            class="w-full bg-[#d8b86c] hover:bg-[#aa8727] text-white text-xs py-1.5 rounded transition-colors duration-200"
            @click="handleApplyPhysics"
          >
            应用物理参数
          </button>
        </div>
      </div>

      <!-- 插件菜单 - 修改为动态渲染 -->
      <div class="relative">
        <button
          class="hover:bg-[#3d3d3d] px-3 py-1.5 rounded transition-colors duration-200"
          :class="{ 'bg-[#3d3d3d]': activeMenu === 'plugin' }"
          @click="toggleMenu('plugin')"
        >
          插件
        </button>
        <div
          v-if="activeMenu === 'plugin'"
          class="absolute top-full left-0 mt-1 w-48 bg-[#2d2d2d] border border-gray-700 rounded shadow-lg z-50"
        >
          <div class="py-1">
            <a
              v-for="plugin in pluginStates"
              :key="plugin.id"
              href="#"
              class="flex items-center justify-between px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="toggleViewTool(plugin)"
            >
              <span>{{ plugin.name }}</span>
              <span class="text-sm" :class="plugin.open ? 'text-green-400' : 'text-red-400'">
                {{ plugin.open ? '√' : '×' }}
              </span>
            </a>
          </div>
        </div>
      </div>

      <!-- 运行菜单 -->
      <div class="relative">
        <button
          class="hover:bg-[#3d3d3d] px-3 py-1.5 rounded transition-colors duration-200"
          :class="{ 'bg-[#3d3d3d]': activeMenu === 'run' }"
          @click="toggleMenu('run')"
        >
          运行
        </button>
        <div
          v-if="activeMenu === 'run'"
          class="absolute top-full left-0 mt-1 w-48 bg-[#2d2d2d] border border-gray-700 rounded shadow-lg z-50"
        >
          <div class="py-1">
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleRunProject"
            >
              运行项目
            </a>
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleRunCurrentScene"
            >
              运行当前场景
            </a>
          </div>
        </div>
      </div>

      <!-- 帮助菜单 -->
      <div class="relative">
        <button
          class="hover:bg-[#3d3d3d] px-3 py-1.5 rounded transition-colors duration-200"
          :class="{ 'bg-[#3d3d3d]': activeMenu === 'help' }"
          @click="toggleMenu('help')"
        >
          帮助
        </button>
        <div
          v-if="activeMenu === 'help'"
          class="absolute top-full left-0 mt-1 w-48 bg-[#2d2d2d] border border-gray-700 rounded shadow-lg z-50"
        >
          <div class="py-1">
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleHelpDocs"
            >
              帮助文档
            </a>
            <a
              href="#"
              class="block px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200"
              @click.prevent="handleAbout"
            >
              关于
            </a>
          </div>
        </div>
      </div>

      <div class="ml-auto flex items-center gap-2">
        <div class="relative">
          <button
            class="px-2.5 py-1 rounded border border-gray-600 text-gray-200 bg-[#252525] hover:bg-[#3d3d3d] transition-colors duration-200 whitespace-nowrap"
            :class="{ 'bg-[#3d3d3d]': activeMenu === 'mainRenderMode' }"
            title="主窗口渲染模式"
            @click="toggleMenu('mainRenderMode')"
          >
            {{ mainRenderModeLabel }}
          </button>
          <div
            v-if="activeMenu === 'mainRenderMode'"
            class="absolute top-full right-0 mt-1 w-52 bg-[#2d2d2d] border border-gray-700 rounded shadow-lg z-50"
          >
            <div class="py-1">
              <button
                v-for="mode in mainRenderModeOptions"
                :key="mode.value"
                class="block w-full text-left px-4 py-2 hover:bg-[#3d3d3d] transition-colors duration-200 disabled:text-gray-600 disabled:hover:bg-transparent"
                :disabled="mode.backend === 'vision' && !visionAvailable"
                @click="selectMainRenderMode(mode.value)"
              >
                {{ mode.label }}
              </button>
            </div>
          </div>
        </div>
        <button
          class="px-2.5 py-1 rounded border transition-colors duration-200 whitespace-nowrap"
          :class="previewRunning || previewBusy
            ? 'border-gray-600 text-gray-500 bg-[#252525] cursor-not-allowed'
            : 'border-green-500/50 text-green-200 bg-green-700/20 hover:bg-green-600/30'"
          :disabled="previewRunning || previewBusy"
          data-guidance="preview-start"
          title="开始项目预览"
          @click="handleStartGamePreview"
        >
          开始预览
        </button>
        <button
          class="px-2.5 py-1 rounded border transition-colors duration-200 whitespace-nowrap"
          :class="!previewRunning || previewBusy
            ? 'border-gray-600 text-gray-500 bg-[#252525] cursor-not-allowed'
            : 'border-red-500/50 text-red-200 bg-red-700/20 hover:bg-red-600/30'"
          :disabled="!previewRunning || previewBusy"
          data-guidance="preview-stop"
          title="结束项目预览"
          @click="handleStopGamePreview"
        >
          结束预览
        </button>
        <span v-if="previewStatusText" class="text-xs text-[#b8c7b0] whitespace-nowrap">
          {{ previewStatusText }}
        </span>
      </div>
    </div>

    <div
      ref="viewportPickSurfaceRef"
      tabindex="0"
      class="relative flex-1 min-h-0 w-full"
      :class="{ 'viewport-cursor-hidden': nativeViewportCursorEnabled && viewportUiMode === 'stereo3d' }"
      :style="nativeViewportCursorEnabled && viewportUiMode === 'stereo3d' ? { cursor: 'none' } : null"
      data-viewport-pick-surface
      data-guidance="main-viewport"
      @focus="handleViewportFocus"
      @pointermove="handleViewportPointer"
      @pointerdown="handleViewportPointerDown"
      @pointerup="handleViewportPointer"
      @pointercancel="handleViewportPointerCancel"
      @pointerleave="handleViewportPointerLeave"
      @click="handleViewportClick"
      @wheel.prevent="handleWheel"
    ></div>

    <aside
      class="scene-quick-controls"
      :aria-label="translate('layout.sceneControls')"
      @mousedown.stop
      @pointerdown.stop
      @click.stop
      @wheel.stop
    >
      <section
        class="scene-quick-lighting"
        data-guidance="scene-lighting"
        :data-assistant-title="translate('layout.sceneLighting')"
        :data-assistant-description="translate('layout.sceneLightingDescription')"
      >
        <div class="scene-quick-lighting-header">
          <span>{{ translate('layout.sceneLighting') }}</span>
          <label class="scene-quick-light-toggle">
            <input
              v-model="sceneLightSettings.enabled"
              type="checkbox"
              :disabled="sceneLightBusy"
              @change="updateSceneLight"
            />
            <span>{{ sceneLightSettings.enabled ? translate('common.yes') : translate('common.no') }}</span>
          </label>
        </div>
        <div class="scene-quick-direction" :class="{ disabled: !sceneLightSettings.enabled || sceneLightBusy }">
          <span class="scene-quick-direction-label">{{ translate('layout.lightDirection') }}</span>
          <label v-for="axis in ['x', 'y', 'z']" :key="axis">
            <span>{{ axis.toUpperCase() }}</span>
            <input
              v-model.number="sceneLightSettings.direction[axis]"
              type="number"
              step="0.1"
              :disabled="!sceneLightSettings.enabled || sceneLightBusy"
              :data-guidance="axis === 'x' ? 'scene-light-x' : undefined"
              @change="updateSceneLight(axis)"
            />
          </label>
        </div>
      </section>
    </aside>

    <!-- 自定义弹窗 -->
    <div
      v-if="showDialog"
      class="fixed top-0 left-0 w-full h-full flex items-center justify-center bg-black/50 backdrop-blur-sm transition-opacity duration-300"
    >
      <div
        class="bg-white rounded-lg shadow-xl w-96 transform transition-all duration-300 ease-out scale-100"
      >
        <div class="p-6">
          <div>
            <label for="new-tab-name" class="block text-sm font-medium text-gray-700 mb-2">
              添加场景
            </label>
            <input
              id="new-tab-name"
              ref="nameInput"
              v-model="inputState.newTabName"
              type="text"
              class="mt-1 px-3 py-2 bg-gray-50 border border-gray-300 rounded-md w-full focus:ring-2 focus:ring-teal-500 focus:border-transparent transition-colors duration-200 outline-none"
              autofocus
              placeholder="输入场景名称"
              @keyup.enter="confirmAddTab"
            />
          </div>
          <div class="flex justify-end gap-3 mt-5">
            <button
              class="px-4 py-2 text-gray-600 bg-gray-100 rounded-md hover:bg-gray-200 transition-colors duration-200 focus:ring-2 focus:ring-teal-500 focus:ring-offset-2"
              @click="cancelAddTab"
            >
              取消
            </button>
            <button
              class="px-4 py-2 text-white bg-teal-600 rounded-md hover:bg-teal-700 transition-colors duration-200 shadow-sm hover:shadow-md focus:ring-2 focus:ring-teal-500 focus:ring-offset-2"
              @click="confirmAddTab"
            >
              创建场景
            </button>
          </div>
        </div>
      </div>
    </div>

    <div
      v-if="projectResourceLoadStatus?.loading"
      class="fixed left-1/2 top-14 z-[90] w-[360px] max-w-[calc(100vw-2rem)] -translate-x-1/2 pointer-events-none rounded-lg border border-[#84a65b]/40 bg-[#151a16]/90 px-4 py-3 shadow-xl backdrop-blur-sm"
    >
      <div class="flex items-center justify-between gap-3 text-xs">
        <span class="text-[#d9e6cf]">场景已打开，资源后台加载中</span>
        <span class="font-mono text-[#9fc276]">
          {{ Math.round(projectResourceLoadStatus.progress || 0) }}%
        </span>
      </div>
      <div class="mt-2 h-1.5 overflow-hidden rounded-full bg-black/40">
        <div
          class="h-full rounded-full bg-[#84a65b] transition-all duration-300"
          :style="{ width: `${projectResourceLoadStatus.progress || 0}%` }"
        ></div>
      </div>
      <div class="mt-1.5 text-[11px] text-[#8f9b8a]">
        已就绪 {{ projectResourceLoadStatus.ready || 0 }} /
        {{ projectResourceLoadStatus.total || 0 }}
      </div>
    </div>

    <div
      v-if="showLocalModal"
      class="fixed inset-0 bg-black/70 flex items-center justify-center z-[9999]"
    >
      <div class="bg-[#2a2a2a] rounded-lg p-6 min-w-[300px] border border-[#d8b86c]/30 shadow-2xl">
        <div class="flex items-center gap-3 mb-4">
          <div class="w-6 h-6 animate-spin">
            <svg
              class="w-full h-full text-[#d8b86c]"
              fill="none"
              stroke="currentColor"
              viewBox="0 0 24 24"
            >
              <path
                stroke-linecap="round"
                stroke-linejoin="round"
                stroke-width="2"
                d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15"
              ></path>
            </svg>
          </div>
          <span class="text-[#e0e0e0] font-medium">{{ localModalTitle }}</span>
        </div>
        <div class="text-sm text-[#909090] mb-2">{{ localModalMessage }}</div>
        <div v-if="localModalProgress > 0" class="w-full bg-[#1a1a1a] rounded-full h-2 mt-4">
          <div
            class="bg-[#d8b86c] h-full rounded-full transition-all duration-300"
            :style="{ width: localModalProgress + '%' }"
          ></div>
        </div>
        <div v-if="localModalProgress > 0" class="text-xs text-[#d8b86c] text-center mt-2">
          {{ Math.round(localModalProgress) }}%
        </div>
      </div>
    </div>

    <div
      class="absolute right-5 top-20 z-[80] h-24 w-24 pointer-events-none select-none rounded-md border border-white/10 bg-[#101418]/70 shadow-xl backdrop-blur-sm"
      aria-hidden="true"
    >
      <svg class="h-full w-full" viewBox="0 0 90 90">
        <circle cx="45" cy="45" r="3" fill="#dbeafe" opacity="0.85" />
        <g v-for="axis in sceneAxisVectors" :key="axis.name">
          <line
            x1="45"
            y1="45"
            :x2="axis.x"
            :y2="axis.y"
            :stroke="axis.color"
            :stroke-width="axis.width"
            stroke-linecap="round"
            :opacity="axis.opacity"
          />
          <circle :cx="axis.x" :cy="axis.y" r="4" :fill="axis.color" :opacity="axis.opacity" />
          <text
            :x="axis.labelX"
            :y="axis.labelY"
            text-anchor="middle"
            dominant-baseline="middle"
            class="fill-white text-[10px] font-semibold"
            :opacity="axis.opacity"
          >
            {{ axis.name }}
          </text>
        </g>
      </svg>
    </div>

    <nav class="dock-shortcut-bar" aria-label="Dock 开关">
      <button
        v-for="shortcut in dockShortcuts"
        :key="shortcut.id"
        type="button"
        class="dock-shortcut-button"
        :class="{
          active: isShortcutOpen(shortcut.id),
          pending: dockShortcutPending.has(shortcut.id),
        }"
        :aria-busy="dockShortcutPending.has(shortcut.id)"
        :data-guidance="shortcut.id === 'SceneTools' ? 'scene-shortcut' : 'node-shortcut'"
        :title="`${isShortcutOpen(shortcut.id) ? '关闭' : '打开'}${shortcut.label}`"
        @click.stop="toggleDockShortcut(shortcut.id, { source: 'user' })"
      >
        <span class="dock-shortcut-icon">{{ shortcut.icon }}</span>
        <span>{{ shortcut.label }}</span>
      </button>
    </nav>

    <aside
      class="cabbage-resident-stack"
      aria-label="包菜任务与答疑"
      @mousedown.stop
      @pointerdown.stop
      @click.stop
      @wheel.stop
    >
      <CabbageReviewAssistant
        resident
        class="cabbage-resident-tasks"
        :tasks="cabbageAssistant.tasks"
        :attention-token="cabbageAssistant.attentionToken"
      />
      <div v-if="!cabbageChatDetached" class="cabbage-resident-chat">
        <CabbageChatPanel resident @detach="detachResidentCabbageChat" />
      </div>
    </aside>
    <CabbageGuidanceOverlay />
  </div>
</template>

<script setup>
import { computed, ref, onMounted, onUnmounted, reactive, watch, nextTick } from 'vue';
import { useRouter } from 'vue-router';
import { useI18n } from 'vue-i18n';
import { DEFAULT_SCENE_NAME } from '@/utils/constants.js';
import {
  Bridge,
  editorApi,
} from '@/api/editorApi.js';
import { appService } from '@/services/appService.js';
import { projectLauncherService } from '@/services/projectLauncherService.js';
import { useErrorHandler } from '@/composables/useErrorHandler.js';
import { useDockStore } from '@/stores/dockStore.js';
import { PLUGIN_MANIFEST } from '@/config/pluginManifest.js';
import { coronaEventBus } from '@/utils/eventBus.js';
import {
  closeFloatingPanel,
  consumeExpectedPanelClosed,
  isFloatingPanel,
  openFloatingPanel,
  toggleFloatingPanel,
} from '@/utils/panelWindows.js';
import { createViewportPickController, indexActorsByHandle } from '@/utils/viewportPick.js';
import {
  createViewportGizmoController,
  isViewportGizmoSelectionOwner,
  resolveViewportGizmoTarget,
} from '@/utils/viewportGizmo.js';
import {
  createViewportUiModeStore,
  createViewportUiCalibrationStore,
  createViewportUiPointerController,
  isNativeViewportCursorEnabled,
} from '@/utils/viewportUiMode.js';
import { createServiceInitializationRetry } from '@/utils/serviceInitialization.js';
import CabbageReviewAssistant from '@/components/ui/CabbageReviewAssistant.vue';
import CabbageChatPanel from '@/views/sidebar/CabbageChatPanel.vue';
import CabbageGuidanceOverlay from '@/components/ui/CabbageGuidanceOverlay.vue';
import { reviewScopeId, subscribeNodeGraphReviews } from '@/services/nodeGraphReviewService.js';
import { useCabbageAssistantStore } from '@/stores/cabbageAssistantStore.js';
import {
  cabbageContextService,
  cancelPendingTransformEvents,
  initializeWorldTasks,
  publishCabbageAssistantContext,
  readCabbageAssistantContext,
  subscribeCabbageAssistantContext,
  subscribeCabbagePreWarnings,
} from '@/services/cabbageAssistantContextService.js';
import { flushProjectNodeGraphBeforeRun } from '@/services/nodeGraphRuntimeService.js';
import { cancelActiveNodeGraphGeneration } from '@/services/nodeGraphGenerationService.js';
import { setActorContext } from '@/blockly/composables/useActorContext.js';
import { closeTutorialSessionChannel } from '@/services/cabbageTutorialSessionService.js';

const { error: logError, warn: logWarn } = useErrorHandler('MainPage');

const router = useRouter();
const { t: translate } = useI18n();
const dockStore = useDockStore();
const cabbageAssistant = useCabbageAssistantStore();
const dockShortcuts = [
  { id: 'SceneTools', label: '场景管理', icon: '景' },
  { id: 'NodeGraphPanel', label: '节点', icon: '点' },
];
const dockShortcutPending = reactive(new Set());
const isShortcutOpen = (id) => Boolean(dockStore.panels[id]?.open);
const cabbageChatDetached = computed(() => {
  const panel = dockStore.panels.CabbageChatPanel;
  return Boolean(panel?.open && panel.mode === 'external');
});
const openDockedPanel = (id, { preserveZone = false } = {}) => {
  const panel = dockStore.panels[id];
  const manifest = PLUGIN_MANIFEST.find((item) => item.id === id);
  if (!panel || !manifest) return;
  if (!preserveZone || !panel.dockZone) {
    dockStore.setDockZone(id, manifest.defaultDock);
  }
  dockStore.popIn(id);
  dockStore.openPanel(id);
  nextTick(() => window.dispatchEvent(new Event('resize')));
};
const detachResidentCabbageChat = async () => {
  if (cabbageAssistant.chatBusy || cabbageChatDetached.value) return;
  const panelId = 'CabbageChatPanel';
  const manifest = PLUGIN_MANIFEST.find((item) => item.id === panelId);
  if (!manifest) return;
  try {
    const result = await appService.createDetachedPanel({
      panelId,
      routePath: '#' + (manifest.routePath || ''),
      width: Math.max(420, Number(manifest.defaultWidth) || 420),
      height: Math.max(640, Number(manifest.defaultHeight) || 640),
      x: 120,
      y: 120,
    });
    const tabId = result?.tab_id ?? result?.data?.tab_id;
    dockStore.setExternal(panelId, tabId);
  } catch (error) {
    logError('Failed to detach resident Cabbage chat', error);
  }
};

const recordPanelOpened = (id, source = 'user') => cabbageContextService.recordEvent({
  type: 'panel_opened',
  category: 'panel',
  success: true,
  details: { panelId: id, source },
});

const toggleDockShortcut = async (id, { source = 'user' } = {}) => {
  const wasOpen = Boolean(dockStore.panels[id]?.open);
  if (id === 'NodeGraphPanel') {
    dockShortcutPending.add(id);
    try {
      await toggleFloatingPanel(dockStore, id);
      if (!wasOpen && dockStore.panels[id]?.open) void recordPanelOpened(id, source);
    } finally {
      dockShortcutPending.delete(id);
    }
    return;
  }

  const panel = dockStore.panels[id];
  if (!panel) return;
  if (panel.open && panel.mode === 'external') {
    await closeFloatingPanel(dockStore, id);
    return;
  }
  if (panel.open) {
    dockStore.closePanel(id);
    nextTick(() => window.dispatchEvent(new Event('resize')));
    return;
  }
  openDockedPanel(id);
  if (!wasOpen && dockStore.panels[id]?.open) void recordPanelOpened(id, source);
};
const handleNodeGraphPanelOpenRequest = async () => {
  // AI generation must use the same centered in-editor floating surface as the
  // Node shortcut. Opening it as a normal dock would honor defaultDock=bottom
  // and shrink the main viewport. openFloatingPanel also reuses an existing tab.
  await openFloatingPanel(dockStore, 'NodeGraphPanel');
};

const goToHome = () => {
  router.push('/');
};

const showLocalModal = ref(false);
const localModalTitle = ref('');
const localModalMessage = ref('');
const localModalProgress = ref(0);
const projectResourceLoadStatus = ref(null);
let projectResourceLoadPollTimer = null;

const activeTab = ref(0); // 当前激活的标签页

const cameraState = ref({
  position: [0.0, 5.0, 10.0],
  forward: [0.0, 1.5, 0.0],
  up: [0.0, 1.0, 0.0],
  fov: 45.0,
});

const cameraBindingState = ref({
  sceneId: DEFAULT_SCENE_NAME,
  cameraId: null,
  cameraName: null,
  cameraHandle: null,
});
let actorPickIndex = new Map();
let actorPickResultCallbackToken = null;
let actorSelectionCallbackToken = null;
let gizmoPointerResultCallbackToken = null;
let actorTransformCallbackToken = null;
let sceneAddedCallbackToken = null;
const actorTransformBaselines = new Map();
const ACTOR_TRANSFORM_EPSILON = 1e-5;
let sceneRenamedCallbackToken = null;
let gizmoDownRequestId = '';
let gizmoDownPointerId = null;
let gizmoDownConsumed = false;
let gizmoClickTimer = 0;
const viewportPickSurfaceRef = ref(null);
const viewportLayoutVersion = ref(0);
const viewportUiMode = ref('flat2d');
const viewportUiModeStore = createViewportUiModeStore();
const nativeViewportCursorEnabled = isNativeViewportCursorEnabled();
const viewportUiModeItems = [
  { mode: 'flat2d', label: '2D UI', title: '普通屏幕 UI' },
  { mode: 'stereo3d', label: '3D UI', title: '光场屏立体 UI' },
];

// 光场 3D UI 标定：dock 面板编辑后经 coronaEventBus 通知，这里用活动相机句柄下发；
// 切到 stereo3d 自动打开标定 dock 面板并推送当前值，切回 flat2d 关闭。
const viewportUiCalibrationStore = createViewportUiCalibrationStore();
const VIEWPORT_UI_CALIBRATION_PANEL = 'LightFieldCalibration';
const applyViewportUiCalibration = (calibration) => {
  viewportUiCalibrationStore.applyToBridge({
    bridge: window.coronaBridge,
    cameraHandle: cameraBindingState.value.cameraHandle,
    calibration: calibration ?? viewportUiCalibrationStore.get({}),
  });
};
const syncViewportUiCalibrationPanel = (mode) => {
  if (mode === 'stereo3d') {
    dockStore.openPanel(VIEWPORT_UI_CALIBRATION_PANEL);
    applyViewportUiCalibration();
  } else {
    dockStore.closePanel(VIEWPORT_UI_CALIBRATION_PANEL);
  }
};

// 摄像头移动速度（可调节）
const cameraSpeed = ref(0.2);
const sceneGridEnabled = ref(true);
const sceneLightSettings = reactive({
  enabled: true,
  direction: { x: 1, y: 1, z: 1 },
});
const sceneLightBusy = ref(false);
const mouseSensitivity = ref(0.15);

// 鼠标旋转状态
const mouseRotate = reactive({
  active: false,
  lastX: 0,
  lastY: 0,
  startForward: null,
  moved: false,
});
const cameraMovementGestures = new Map();
const movementAxisGroups = Object.freeze({
  w: 'forward_back',
  s: 'forward_back',
  a: 'left_right',
  d: 'left_right',
  q: 'up_down',
  e: 'up_down',
});

const MAX_CAMERA_VIEWPORT_RENDER_PIXELS = 1920 * 1080;

const computeCameraViewportRenderSize = (width, height, scale) => {
  const physicalWidth = Math.max(Math.round(width * scale), 1);
  const physicalHeight = Math.max(Math.round(height * scale), 1);
  const pixels = physicalWidth * physicalHeight;
  if (pixels <= MAX_CAMERA_VIEWPORT_RENDER_PIXELS) {
    return { width: physicalWidth, height: physicalHeight };
  }

  const ratio = Math.sqrt(MAX_CAMERA_VIEWPORT_RENDER_PIXELS / pixels);
  return {
    width: Math.max(Math.floor(physicalWidth * ratio), 1),
    height: Math.max(Math.floor(physicalHeight * ratio), 1),
  };
};

const getViewportHitRect = () => viewportPickSurfaceRef.value?.getBoundingClientRect?.() ?? null;

const getViewportRenderRect = () => {
  const rect = getViewportHitRect();
  const width = Math.max(Number(rect?.width || 0), 0);
  const height = Math.max(Number(rect?.height || 0), 0);
  const scale = Math.max(Number(window.devicePixelRatio || 1), 0.01);
  const renderSize = computeCameraViewportRenderSize(width, height, scale);
  return {
    left: Number(rect?.left || 0),
    top: Number(rect?.top || 0),
    width,
    height,
    renderWidth: renderSize.width,
    renderHeight: renderSize.height,
  };
};

const currentViewportUiDescriptor = () => ({
  scope: 'main',
  sceneId: cameraBindingState.value.sceneId || tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME,
  cameraHandle: cameraBindingState.value.cameraHandle || '',
});

const normalizeTransformVector = (value, fallback = { x: 0, y: 0, z: 0 }) => ({
  x: Number(value?.x ?? value?.[0] ?? fallback.x) || 0,
  y: Number(value?.y ?? value?.[1] ?? fallback.y) || 0,
  z: Number(value?.z ?? value?.[2] ?? fallback.z) || 0,
});

const actorTransformKey = (sceneId, actorName) => `${String(sceneId || '')}::${String(actorName || '')}`;

const normalizeActorTransform = (source = {}, fallback = {}) => ({
  position: normalizeTransformVector(source.position, fallback.position || { x: 0, y: 0, z: 0 }),
  rotation: normalizeTransformVector(source.rotation, fallback.rotation || { x: 0, y: 0, z: 0 }),
  scale: normalizeTransformVector(source.scale, fallback.scale || { x: 1, y: 1, z: 1 }),
});

const transformVectorChanged = (previous, next) => ['x', 'y', 'z'].some((axis) => (
  Math.abs(Number(previous?.[axis] || 0) - Number(next?.[axis] || 0)) > ACTOR_TRANSFORM_EPSILON
));

const seedActorTransformBaseline = async (sceneId, actorName) => {
  const scene = String(sceneId || '').trim();
  const actor = String(actorName || '').trim();
  if (!scene || !actor) return;
  try {
    const result = await editorApi.scene.getActor(scene, actor);
    const data = result?.data ?? result ?? {};
    if (!data || data.status === 'error') return;
    actorTransformBaselines.set(
      actorTransformKey(scene, actor),
      normalizeActorTransform(data.geometry || {}),
    );
  } catch (_) {
    // Selection should still open the Object Dock when a baseline cannot be read.
  }
};

const handleActorTransformForCabbage = (payload = {}) => {
  const sceneId = String(payload?.scene || payload?.scene_id || '').trim();
  const actorName = String(payload?.actor || payload?.actor_name || '').trim();
  if (!sceneId || !actorName) return;
  const key = actorTransformKey(sceneId, actorName);
  const previous = actorTransformBaselines.get(key);
  const next = normalizeActorTransform(payload, previous || {});
  actorTransformBaselines.set(key, next);
  // The first callback may be an initial state echo. Only compare after a baseline exists.
  if (!previous) return;

  for (const transformKey of ['position', 'rotation', 'scale']) {
    if (!transformVectorChanged(previous[transformKey], next[transformKey])) continue;
    void cabbageContextService.recordEvent({
      type: transformKey === 'position'
        ? 'transform_position'
        : transformKey === 'rotation'
          ? 'transform_rotation'
          : 'transform_scale',
      category: 'scene',
      success: true,
      details: {
        sceneName: sceneId,
        actorName,
        source: 'viewport',
      },
    });
  }
};

const emitActorChangeFast = (type, sceneId, actorName) => {
  editorApi.sceneTools.selectActor(sceneId, type, actorName, {
    sourceViewport: 'main',
    sourceCameraHandle: Number(cameraBindingState.value.cameraHandle || 0),
  }).catch((error) => {
    logError('Failed to publish actor selection', error);
  });
};

const handleActorSelectionForObjectDock = async (payload = {}, maybeSceneId = '', maybeActorName = '') => {
  const actorType = String(payload?.actor_type || payload?.type || '').trim().toLowerCase();
  const sceneId = String(payload?.scene || payload?.scene_id || maybeSceneId || '').trim();
  const actorName = String(payload?.actor || payload?.actor_name || maybeActorName || '').trim();
  if (!actorName || actorType === 'scene') {
    viewportGizmoController.clearTarget();
    return;
  }

  const resolvedSceneId = sceneId || tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME;
  setActorContext(resolvedSceneId, actorName);
  void seedActorTransformBaseline(resolvedSceneId, actorName);
  const ownsGizmo = isViewportGizmoSelectionOwner({
    viewportScope: 'main',
    cameraHandle: cameraBindingState.value.cameraHandle,
    selection: payload,
  });
  if (ownsGizmo) {
    await syncMainViewportGizmoSelection({
      ...payload,
      scene: resolvedSceneId,
      actor: actorName,
      actor_type: actorType,
    });
  } else {
    viewportGizmoController.clearTarget();
  }
  if (!dockStore.panels.Object?.open) {
    openDockedPanel('Object');
  }
};

const viewportPickController = createViewportPickController({
  getBridge: () => window.coronaBridge,
  getCameraBinding: () => cameraBindingState.value,
  getHitRect: getViewportHitRect,
  getRenderRect: getViewportRenderRect,
  getActorIndex: () => actorPickIndex,
  emitActorChange: (type, sceneId, actorName) => emitActorChangeFast(type, sceneId, actorName),
});

const viewportGizmoController = createViewportGizmoController({
  getBridge: () => window.coronaBridge,
  getCameraBinding: () => cameraBindingState.value,
  getHitRect: getViewportHitRect,
  getRenderRect: getViewportRenderRect,
  onDragEnd: (payload) => {
    const sceneId =
      String(payload?.sceneId || cameraBindingState.value.sceneId || '').trim();
    const actorName = String(payload?.actor || '').trim();
    if (sceneId && actorName) {
      editorApi.sceneTools.saveActor(sceneId, actorName).catch((error) => {
        logError('Failed to save gizmo actor transform', error);
      });
    }
  },
});

const syncMainViewportGizmoSelection = async (selection = {}, pickResult = null) => {
  const sceneId =
    String(cameraBindingState.value.sceneId || tabs.value[activeTab.value]?.id || '').trim();
  let target = resolveViewportGizmoTarget({
    sceneId,
    selection,
    pickResult,
    actorIndex: actorPickIndex,
  });
  if (!target && selection?.actor && String(selection.actor_type || selection.type) !== 'scene') {
    await refreshActorPickIndex(sceneId).catch(() => false);
    target = resolveViewportGizmoTarget({
      sceneId,
      selection,
      pickResult,
      actorIndex: actorPickIndex,
    });
  }
  if (target) {
    viewportGizmoController.setTarget(target);
  } else {
    viewportGizmoController.clearTarget();
  }
  return target;
};

const viewportUiPointerController = createViewportUiPointerController({
  getBridge: () => window.coronaBridge,
  getCameraHandle: () => cameraBindingState.value.cameraHandle,
  getEnabled: () => viewportUiMode.value === 'stereo3d',
  getNativeCursorEnabled: () => nativeViewportCursorEnabled,
  getHitRect: getViewportHitRect,
  getRenderRect: getViewportRenderRect,
});

let cameraViewportSyncRafId = null;
let cameraViewportResizeObserver = null;
let lastCameraViewportSignature = '';

const syncCameraViewportRect = () => {
  const bridge = window.coronaBridge;
  const cameraHandle = Number(cameraBindingState.value.cameraHandle || 0);
  const rect = getViewportHitRect();
  if (!bridge || typeof bridge.setCameraViewport !== 'function' || !cameraHandle || !rect) {
    return false;
  }

  const scale = Math.max(Number(window.devicePixelRatio || 1), 0.01);
  const x = Math.max(Math.round(Number(rect.left || 0) * scale), 0);
  const y = Math.max(Math.round(Number(rect.top || 0) * scale), 0);
  const width = Math.max(Math.round(Number(rect.width || 0) * scale), 1);
  const height = Math.max(Math.round(Number(rect.height || 0) * scale), 1);
  const renderSize = computeCameraViewportRenderSize(
    Math.max(Number(rect.width || 0), 0),
    Math.max(Number(rect.height || 0), 0),
    scale,
  );
  const signature = `${cameraHandle}:${x}:${y}:${width}:${height}:${renderSize.width}:${renderSize.height}`;
  if (signature === lastCameraViewportSignature) {
    return true;
  }

  if (bridge.setCameraViewport(
    cameraHandle,
    x,
    y,
    width,
    height,
    renderSize.width,
    renderSize.height,
  )) {
    lastCameraViewportSignature = signature;
    return true;
  }
  return false;
};

const scheduleCameraViewportSync = () => {
  if (cameraViewportSyncRafId != null) return;
  cameraViewportSyncRafId = requestAnimationFrame(() => {
    cameraViewportSyncRafId = null;
    syncCameraViewportRect();
  });
};

const handleViewportLayoutChange = () => {
  viewportLayoutVersion.value += 1;
  scheduleCameraViewportSync();
};

const syncViewportUiMode = () => {
  const mode = viewportUiModeStore.get(currentViewportUiDescriptor());
  viewportUiMode.value = mode;
  viewportUiModeStore.applyToBridge({
    bridge: window.coronaBridge,
    cameraHandle: cameraBindingState.value.cameraHandle,
    mode,
  });
  if (mode !== 'stereo3d') {
    viewportUiPointerController.hide();
  }
  syncViewportUiCalibrationPanel(mode);
};

const selectViewportUiMode = (mode) => {
  viewportUiMode.value = viewportUiModeStore.set(currentViewportUiDescriptor(), mode);
  viewportUiModeStore.applyToBridge({
    bridge: window.coronaBridge,
    cameraHandle: cameraBindingState.value.cameraHandle,
    mode: viewportUiMode.value,
  });
  if (viewportUiMode.value !== 'stereo3d') {
    viewportUiPointerController.hide();
  }
  syncViewportUiCalibrationPanel(viewportUiMode.value);
};

const hasActiveMovementKeys = () => Object.values(movementKeys).some((value) => value);

const isRealtimeCameraInputActive = () => mouseRotate.active || hasActiveMovementKeys();

// 摄像头更新节流：用 rAF 合并高频输入，每帧最多发送一次
let cameraDirty = false;
let cameraRafId = null;

const scheduleCameraUpdate = () => {
  cameraDirty = true;
  if (cameraRafId != null) return;
  cameraRafId = requestAnimationFrame(() => {
    cameraRafId = null;
    if (cameraDirty) {
      cameraDirty = false;
      if (!sendCameraUpdateFast()) {
        const sceneId = tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME;
        syncSceneCameraBinding(sceneId);
      }
    }
  });
};

// 标签页数据
const tabs = ref([]);

// 添加新标签页
const showDialog = ref(false);
const inputState = reactive({
  newTabName: '',
});

// 新增：菜单状态
const activeMenu = ref(null);
const previewRunning = ref(false);
const previewBusy = ref(false);
const previewStatusText = ref('');
const previewDetails = ref({});
let tutorialPreviewObservedRunning = false;
const visionAvailable = ref(false);
const mainRenderBackend = ref('native');
const mainVisionRenderMode = ref('path_tracing');
let previewPollTimer = null;
window.__coronaEditorInputLocks = window.__coronaEditorInputLocks instanceof Set
  ? window.__coronaEditorInputLocks
  : new Set();
window.__coronaGamePreviewInputLocked = window.__coronaEditorInputLocks.size > 0;
const EDITOR_CONTROLS_KEY = '__coronaEditorControls';

// 物理参数状态
const physicsParams = ref({
  gravityX: 0.0,
  gravityY: -9.8,
  gravityZ: 0.0,
  floorY: 0.0,
  floorRestitution: 0.6,
  fixedDt: 1.0 / 60.0,
});

// 视图/插件菜单状态：从 Pinia dockStore + pluginManifest 派生
const viewStates = computed(() =>
  PLUGIN_MANIFEST.filter((p) => p.pageType === 'view').map((p) => ({
    id: p.id,
    name: p.displayName,
    open: dockStore.panels[p.id]?.open ?? false,
  }))
);
const pluginStates = computed(() =>
  PLUGIN_MANIFEST.filter((p) => p.pageType === 'plugin').map((p) => ({
    id: p.id,
    name: p.displayName,
    open: dockStore.panels[p.id]?.open ?? false,
  }))
);
const mainRenderModeOptions = [
  { value: 'native', backend: 'native', label: 'Native' },
  { value: 'path_tracing', backend: 'vision', label: 'Vision Path Tracing' },
  { value: 'svgf', backend: 'vision', label: 'Vision SVGF' },
  { value: 'ssat', backend: 'vision', label: 'Vision SSAT' },
];
const mainRenderModeLabel = computed(() => {
  if (mainRenderBackend.value !== 'vision') {
    return 'Native';
  }
  return mainRenderModeOptions.find((mode) => mode.value === mainVisionRenderMode.value)?.label
    || 'Vision Path Tracing';
});
let pendingMainRenderSelection = null;
const currentMainCameraId = () =>
  cameraBindingState.value.cameraId || cameraBindingState.value.cameraName || null;

// Cabbage assistant: world-scoped tutorial and node-logic tasks.
let unsubscribeNodeGraphReview = null;
let unsubscribeCabbageContext = null;
let unsubscribeCabbagePreWarnings = null;
let cabbageCandidateTimer = null;
let cabbageWorldLoadGeneration = 0;
const cabbageWorldInitializationRetry = createServiceInitializationRetry();
const ACTIVE_PROJECT_PATH_KEY = 'corona.activeProjectPath';

function normalizeActiveProjectPath(value) {
  return String(value || '')
    .trim()
    .replace(/[\\/]+$/, '')
    .replace(/\\/g, '/')
    .toLowerCase();
}

function readActiveProjectPath() {
  return String(window.localStorage?.getItem(ACTIVE_PROJECT_PATH_KEY) || '').trim();
}

let activeProjectPathSnapshot = normalizeActiveProjectPath(readActiveProjectPath());

function currentProjectReviewScopeId() {
  return reviewScopeId(readActiveProjectPath());
}

async function persistCabbageTaskActions(actions = []) {
  const validActions = Array.isArray(actions) ? actions.filter((item) => item?.task) : [];
  for (const action of validActions) {
    try {
      await cabbageContextService.updateTask(action);
    } catch (error) {
      console.warn('[CabbageContext] failed to persist task', error?.message || error);
    }
  }
  if (validActions.length) {
    void cabbageContextService.requestProfileScoreUpdate().catch(() => {});
  }
}

async function loadCabbageWorldContext({ reset = true } = {}) {
  const generation = ++cabbageWorldLoadGeneration;
  const expectedProjectPath = normalizeActiveProjectPath(readActiveProjectPath());
  const scopeId = currentProjectReviewScopeId();
  // Keep the last snapshot for this same world while the backend context is loading.
  // Publishing an empty reset here used to overwrite the cached task list, which made
  // the task board disappear whenever loading was slow or temporarily failed.
  const cachedSnapshot = readCabbageAssistantContext(scopeId);
  if (reset) {
    cabbageWorldInitializationRetry.cancel();
    cabbageAssistant.clearForProjectChange(scopeId);
    if (cachedSnapshot) cabbageAssistant.hydrateContext(cachedSnapshot);
    publishCabbageAssistantContext(cabbageAssistant);
  }
  try {
    const snapshot = await cabbageContextService.loadCurrentWorld();
    if (generation !== cabbageWorldLoadGeneration
      || normalizeActiveProjectPath(readActiveProjectPath()) !== expectedProjectPath) {
      return null;
    }
    cabbageWorldInitializationRetry.cancel();
    cabbageAssistant.hydrateContext(snapshot);
    void cabbageContextService.requestProfileScoreUpdate().catch(() => {});
    const goal = snapshot?.worldGoal || {};
    if (goal.source === 'ai'
      && goal.status === 'generating'
      && String(goal.prompt || '').trim()) {
      void initializeWorldTasks({
        prompt: String(goal.prompt || '').trim(),
        mode: String(goal.mode || 'story'),
        waitForCompletion: false,
      }).catch((error) => {
        console.warn(
          '[CabbageContext] failed to resume world task generation',
          error?.message || error,
        );
      });
    }
    return snapshot;
  } catch (error) {
    if (generation === cabbageWorldLoadGeneration) {
      if (cachedSnapshot) cabbageAssistant.hydrateContext(cachedSnapshot);
      if (error?.retryable) {
        cabbageWorldInitializationRetry.schedule(() => {
          if (generation !== cabbageWorldLoadGeneration) return;
          void loadCabbageWorldContext({ reset: false });
        });
      } else {
        console.warn('[CabbageContext] failed to load world context', error?.message || error);
      }
    }
  }
  return null;
}

function clearNodeReviewForProjectChange() {
  actorTransformBaselines.clear();
  cancelPendingTransformEvents();
  void loadCabbageWorldContext({ reset: true });
}

function refreshCameraAfterProjectChange() {
  window.setTimeout(() => {
    void refreshSceneCameraBinding({
      force: true,
      preservePose: false,
    });
  }, 0);
}

function applyActualProjectChange(projectPath) {
  const normalizedPath = normalizeActiveProjectPath(projectPath);
  if (normalizedPath === activeProjectPathSnapshot) return false;

  activeProjectPathSnapshot = normalizedPath;
  // A node-generation request belongs to the world where it started. Cancel it
  // before loading the next world's context so a late DeepSeek response cannot
  // modify the newly opened world's node graph.
  void cancelActiveNodeGraphGeneration();
  clearNodeReviewForProjectChange();
  clearKnownEditorCameraInputLocks();
  void reconcileEditorCameraInputLocks();
  refreshCameraAfterProjectChange();
  return true;
}

function onActiveProjectChanged(event) {
  const detail = event?.detail;
  const projectPath = typeof detail === 'string'
    ? detail
    : detail?.projectPath || detail?.path || readActiveProjectPath();
  applyActualProjectChange(projectPath);
}

function onActiveProjectStorageChanged(event) {
  if (event?.key !== ACTIVE_PROJECT_PATH_KEY) return;
  applyActualProjectChange(event?.newValue);
}

async function handleNodeGraphReview(result) {
  if (result?.projectScopeId && result.projectScopeId !== currentProjectReviewScopeId()) return;
  if (result?.success !== true || result?.status !== 'ok') {
    if (result?.error) console.warn('[NodeGraphReview]', result.error);
    return;
  }
  const actions = cabbageAssistant.applyReview(result);
  publishCabbageAssistantContext(cabbageAssistant);
  await persistCabbageTaskActions(actions);
}

function promoteDueCabbageTasks(options = {}) {
  const promoted = cabbageAssistant.promoteDueCandidates(options);
  if (!promoted.length) return;
  publishCabbageAssistantContext(cabbageAssistant);
  void persistCabbageTaskActions(promoted.map((task) => ({ action: 'upsert', task })));
}

function onCabbageRunFailed(event) {
  promoteDueCabbageTasks({ runtimeFailed: true });
  if (event?.detail?.contextRecorded) return;
  void cabbageContextService.recordEvent({
    type: 'run_failed',
    category: 'runtime',
    success: false,
    details: {
      source: String(event?.detail?.source || 'editor'),
      error: String(event?.detail?.error || '').slice(0, 500),
    },
  });
}

// Assistant state has been initialized above.
const isLoadingMenu = ref(false);

watch(showDialog, (newVal) => {
  if (newVal) {
    nextTick(() => {
      const input = document.getElementById('new-tab-name');
      if (input) {
        input.select();
      }
    });
  }
});

// 新增：切换菜单显示
const toggleMenu = (menu) => {
  if (activeMenu.value === menu) {
    activeMenu.value = null;
  } else {
    activeMenu.value = menu;
    if (menu === 'physics') {
      loadPhysicsParams();
    }
  }
};

const selectMainRenderMode = async (mode) => {
  const sceneId = tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME;
  const cameraId = currentMainCameraId();
  activeMenu.value = null;
  try {
    if (mode === 'native') {
      pendingMainRenderSelection = {
        sceneId,
        backend: 'native',
        visionMode: mainVisionRenderMode.value,
        expiresAt: Date.now() + 3000,
      };
      mainRenderBackend.value = 'native';
      const result = unwrapBridgeData(
        await editorApi.sceneTools.setRenderBackend('native', sceneId, cameraId),
      );
      mainRenderBackend.value = result?.mode || 'native';
      await syncSceneCameraBinding(sceneId);
      return true;
    }

    pendingMainRenderSelection = {
      sceneId,
      backend: 'vision',
      visionMode: mode,
      expiresAt: Date.now() + 3000,
    };
    mainRenderBackend.value = 'vision';
    mainVisionRenderMode.value = mode;

    const modeResult = unwrapBridgeData(
      await editorApi.sceneTools.setVisionRenderMode(sceneId, cameraId, mode),
    );
    mainVisionRenderMode.value = modeResult?.mode || mode;

    await editorApi.sceneTools.setOutputMode(sceneId, cameraId, 'final_color');

    const backendResult = unwrapBridgeData(
      await editorApi.sceneTools.setRenderBackend('vision', sceneId, cameraId),
    );
    mainRenderBackend.value = backendResult?.mode || 'native';
    if (mainRenderBackend.value !== 'vision') {
      pendingMainRenderSelection = null;
      return false;
    }
    await syncSceneCameraBinding(sceneId);
    return true;
  } catch (error) {
    pendingMainRenderSelection = null;
    logError('Failed to set main viewport render mode', error);
    return false;
  }
};

// 新增：点击其他地方关闭菜单
const handleClickOutside = (event) => {
  const menuBar = document.querySelector('.bg-\\[\\#2d2d2d\\]');
  if (menuBar && !menuBar.contains(event.target)) {
    activeMenu.value = null;
  }
};

const addNewTab = async () => {
  logError('Single-scene editor mode does not support creating extra scenes');
};

const confirmAddTab = async () => {
  showDialog.value = false;
  inputState.newTabName = '';
};

// 清空输入框
const cancelAddTab = () => {
  showDialog.value = false;
  inputState.newTabName = '';
};

const isVector3 = (value) => Array.isArray(value) && value.length === 3;

let sceneCameraBindingRequestRevision = 0;
let sceneCameraBindingRefreshPromise = null;
let sceneCameraBindingLastRefreshAt = 0;
const SCENE_CAMERA_BINDING_REFRESH_INTERVAL_MS = 1000;

const sceneGridEnabledFromSnapshot = (snapshot = {}) => {
  if (typeof snapshot?.grid?.enabled === 'boolean') return snapshot.grid.enabled;
  if (typeof snapshot?.floor_grid_enabled === 'boolean') return snapshot.floor_grid_enabled;
  return null;
};

const applySceneSnapshot = (sceneId, payload, { preservePose = false } = {}) => {
  const snapshot = payload?.scene ?? payload?.data?.scene ?? payload?.data ?? payload;
  if (!snapshot || typeof snapshot !== 'object') {
    cameraBindingState.value = {
      ...cameraBindingState.value,
      sceneId: sceneId ?? cameraBindingState.value.sceneId,
    };
    return;
  }

  const normalizedSceneId =
    snapshot.scene_id ?? snapshot.sceneId ?? snapshot.id ?? sceneId ?? DEFAULT_SCENE_NAME;
  const gridEnabled = sceneGridEnabledFromSnapshot(snapshot);
  if (gridEnabled !== null) sceneGridEnabled.value = gridEnabled;
  const sun = snapshot.sun && typeof snapshot.sun === 'object' ? snapshot.sun : {};
  const lightDirection = Array.isArray(sun.direction) ? sun.direction : [1, 1, 1];
  sceneLightSettings.enabled = sun.enabled !== false;
  sceneLightSettings.direction.x = Number(lightDirection[0] ?? 1);
  sceneLightSettings.direction.y = Number(lightDirection[1] ?? 1);
  sceneLightSettings.direction.z = Number(lightDirection[2] ?? 1);
  const cameras = Array.isArray(snapshot.cameras) ? snapshot.cameras : [];
  actorPickIndex = indexActorsByHandle(Array.isArray(snapshot.actors) ? snapshot.actors : []);
  const activeCameraName =
    snapshot.active_camera_name ?? snapshot.activeCameraName ?? cameras[0]?.name ?? null;
  const activeCamera =
    cameras.find((cam) => cam?.name === activeCameraName) ?? cameras[0] ?? snapshot.camera ?? null;

  cameraBindingState.value = {
    sceneId: normalizedSceneId,
    cameraId: activeCamera?.camera_id ?? activeCamera?.id ?? null,
    cameraName: activeCameraName,
    cameraHandle: activeCamera?.handle ?? activeCamera?.camera_handle ?? null,
  };
  lastCameraViewportSignature = '';
  scheduleCameraViewportSync();
  syncViewportUiMode();
  if (
    pendingMainRenderSelection &&
    pendingMainRenderSelection.sceneId === normalizedSceneId &&
    Date.now() < pendingMainRenderSelection.expiresAt
  ) {
    mainRenderBackend.value = pendingMainRenderSelection.backend;
    mainVisionRenderMode.value = pendingMainRenderSelection.visionMode;
  } else {
    pendingMainRenderSelection = null;
    mainRenderBackend.value = activeCamera?.render_backend || 'native';
    mainVisionRenderMode.value = activeCamera?.vision_render_mode || 'path_tracing';
  }

  if (
    activeCamera &&
    !preservePose &&
    !isRealtimeCameraInputActive() &&
    (isVector3(activeCamera.position) ||
      isVector3(activeCamera.forward) ||
      isVector3(activeCamera.world_up))
  ) {
    cameraState.value = {
      position: isVector3(activeCamera.position)
        ? [...activeCamera.position]
        : [...cameraState.value.position],
      forward: isVector3(activeCamera.forward)
        ? [...activeCamera.forward]
        : [...cameraState.value.forward],
      up: isVector3(activeCamera.world_up) ? [...activeCamera.world_up] : [...cameraState.value.up],
      fov: Number.isFinite(Number(activeCamera.fov))
        ? Number(activeCamera.fov)
        : cameraState.value.fov,
    };
    // Keep the editor main viewport aligned with the scene's active camera on load.
    scheduleCameraUpdate();
  }
};

const syncSceneCameraBinding = async (sceneId, { preservePose = false } = {}) => {
  if (!sceneId) {
    return false;
  }

  const requestRevision = ++sceneCameraBindingRequestRevision;
  try {
    const result = await editorApi.scene.getSnapshot(sceneId);
    if (requestRevision !== sceneCameraBindingRequestRevision) return false;
    applySceneSnapshot(sceneId, result, { preservePose });
    broadcastViewportControlsState();
    return true;
  } catch (e) {
    if (requestRevision === sceneCameraBindingRequestRevision) {
      logError('Failed to sync scene camera binding', e);
    }
    return false;
  }
};

const updateSceneLight = async (axis = '') => {
  if (sceneLightBusy.value) return false;
  const sceneId = cameraBindingState.value.sceneId
    || tabs.value[activeTab.value]?.id
    || DEFAULT_SCENE_NAME;
  const direction = sceneLightSettings.direction;
  sceneLightBusy.value = true;
  try {
    await editorApi.sceneTools.sunDirection(sceneId, sceneLightSettings.enabled, [
      Number(direction.x) || 0,
      Number(direction.y) || 0,
      Number(direction.z) || 0,
    ]);
    void cabbageContextService.recordEvent({
      type: 'lighting_changed',
      category: 'lighting',
      success: true,
      details: {
        sceneName: sceneId,
        axis: String(axis || '').toLowerCase(),
        value: axis ? Number(direction[axis]) || 0 : undefined,
        source: 'property_panel',
      },
    });
    return true;
  } catch (error) {
    logError('更新场景光照失败', error);
    await syncSceneCameraBinding(sceneId, { preservePose: true });
    return false;
  } finally {
    sceneLightBusy.value = false;
  }
};

const refreshSceneCameraBinding = ({ force = false, preservePose = true } = {}) => {
  const sceneId = tabs.value[activeTab.value]?.id || cameraBindingState.value.sceneId || DEFAULT_SCENE_NAME;
  const now = Date.now();
  if (!force && sceneCameraBindingRefreshPromise) return sceneCameraBindingRefreshPromise;
  if (!force && now - sceneCameraBindingLastRefreshAt < SCENE_CAMERA_BINDING_REFRESH_INTERVAL_MS) {
    return Promise.resolve(true);
  }

  sceneCameraBindingLastRefreshAt = now;
  const refreshPromise = syncSceneCameraBinding(sceneId, { preservePose });
  const trackedPromise = refreshPromise.finally(() => {
    if (sceneCameraBindingRefreshPromise === trackedPromise) {
      sceneCameraBindingRefreshPromise = null;
    }
  });
  sceneCameraBindingRefreshPromise = trackedPromise;
  return trackedPromise;
};

const restoreCameraViews = async (sceneId) => {
  if (!sceneId) return;
  try {
    const result = await editorApi.sceneTools.listCameraViews(sceneId);
    const payload = result?.data ?? result;
    const openCameras = Array.isArray(payload?.cameras)
      ? payload.cameras.filter((camera) => camera.view_open)
      : [];
    for (const camera of openCameras) {
      await appService.createCameraView({ ...camera, scene_id: sceneId });
    }
  } catch (e) {
    logError('Failed to restore camera views', e);
  }
};

const scratchMouseButton = (button) => ({ 0: 'LeftButton', 1: 'MiddleButton', 2: 'RightButton' }[button] || '');
const sendScratchPointerEvent = (type, event, pickedActor = '') => {
  const renderRect = getViewportRenderRect();
  const localX = Number(event.clientX || 0) - Number(renderRect.left || 0);
  const localY = Number(event.clientY || 0) - Number(renderRect.top || 0);
  editorApi.scratch.sendMouseEvent(
    type,
    scratchMouseButton(event.button),
    event.clientX || 0,
    event.clientY || 0,
    localX,
    localY,
    renderRect.width || 0,
    renderRect.height || 0,
    pickedActor || ''
  ).catch(() => {});
};
const vectorDistance = (left = [], right = []) => Math.sqrt(
  left.reduce((sum, value, index) => sum + ((Number(value) || 0) - (Number(right[index]) || 0)) ** 2, 0)
);

const handleWheel = (event) => {
  sendScratchPointerEvent('wheel', event);
  if (isGamePreviewInputLocked()) return;
  if (event.shiftKey) {
    // Shift+滚轮：调节摄像头速度
    const delta = event.deltaY > 0 ? -0.02 : 0.02;
    cameraSpeed.value =
      Math.round(Math.max(0.01, Math.min(2, cameraSpeed.value + delta)) * 100) / 100;
    event.preventDefault();
    return;
  }
  const before = [...cameraState.value.position];
  const direction = event.deltaY > 0 ? 'backward' : 'forward';
  handleCameraMove(direction);
  const actualDelta = vectorDistance(before, cameraState.value.position);
  if (actualDelta > 1e-6) {
    void cabbageContextService.recordEvent({
      type: 'camera_moved',
      category: 'camera',
      success: true,
      details: { interaction: 'wheel', actualDelta },
    });
  }
};

const handleViewportFocus = () => {
  void cabbageContextService.recordEvent({
    type: 'viewport_focused',
    category: 'viewport',
    success: true,
    details: { source: 'user' },
  });
};

const handleKeyDown = (event) => {
  // 检查输入框是否聚焦
  const inputElement = document.getElementById('new-tab-name');
  if (inputElement && inputElement === document.activeElement) {
    return;
  }
  const tag = event.target?.tagName;
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
  if (
    (event.key === 'Escape' || event.code === 'Escape')
    && viewportGizmoController.isDragging()
  ) {
    event.preventDefault();
    viewportGizmoController.cancel('escape');
    return;
  }
  const modifiers = [
    event.ctrlKey ? 'Ctrl' : '',
    event.altKey ? 'Alt' : '',
    event.shiftKey ? 'Shift' : '',
    event.metaKey ? 'Meta' : '',
  ].filter(Boolean).join(',');
  // Native SDL is authoritative in the editor; this is browser-dev fallback.
  if (!window.coronaBridge && !event.__coronaScratchKeyForwarded) {
    event.__coronaScratchKeyForwarded = true;
    editorApi.scratch.sendKeyEvent(
      event.code || event.key || '',
      modifiers,
      event.key || event.code || ''
    ).catch(() => {});
  }
  if (isGamePreviewInputLocked()) {
    resetRealtimeCameraInput();
    return;
  }

  const key = event.key.toLowerCase();
  if (movementKeys[key] !== undefined) {
    event.preventDefault();
    if (!movementKeys[key]) {
      if (movementAxisGroups[key]) {
        cameraMovementGestures.set(key, [...cameraState.value.position]);
      }
      // A project/scene reload recreates native cameras and invalidates their old
      // handles. Refresh once when a new movement gesture starts instead of
      // continuing to publish WASD/QE updates to a released camera.
      void refreshSceneCameraBinding({ preservePose: true });
    }
    movementKeys[key] = true;
    startMoveLoop();
  }
};

const handleKeyUp = (event) => {
  const tag = event.target?.tagName;
  if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;
  if (!window.coronaBridge && !event.__coronaScratchKeyForwarded) {
    event.__coronaScratchKeyForwarded = true;
    editorApi.scratch.sendKeyUpEvent(
      event.code || event.key || '',
      event.key || event.code || ''
    ).catch(() => {});
  }
  if (isGamePreviewInputLocked()) {
    resetRealtimeCameraInput();
    return;
  }

  const key = event.key.toLowerCase();
  if (movementKeys[key] !== undefined) {
    movementKeys[key] = false;
    const gestureStart = cameraMovementGestures.get(key);
    cameraMovementGestures.delete(key);
    const axisGroup = movementAxisGroups[key];
    if (gestureStart && axisGroup) {
      const actualDelta = vectorDistance(gestureStart, cameraState.value.position);
      if (actualDelta > 1e-6) {
        void cabbageContextService.recordEvent({
          type: 'camera_moved',
          category: 'camera',
          success: true,
          details: { key: key.toUpperCase(), axisGroup, actualDelta },
        });
      }
    }
    if (!hasActiveMovementKeys()) {
      stopMoveLoop();
      scheduleCameraUpdate();
    }
  }
};

// ---- 平滑移动系统 ----
const movementKeys = reactive({
  w: false,
  s: false,
  a: false,
  d: false,
  q: false,
  e: false,
  arrowleft: false,
  arrowright: false,
  arrowup: false,
  arrowdown: false,
});
let moveLoopId = null;
let lastMoveTime = 0;

const startMoveLoop = () => {
  if (moveLoopId !== null) return;
  lastMoveTime = performance.now();
  moveLoopId = requestAnimationFrame(moveLoop);
};

const stopMoveLoop = () => {
  if (moveLoopId !== null) {
    cancelAnimationFrame(moveLoopId);
    moveLoopId = null;
  }
};

const isGamePreviewInputLocked = () => {
  const locks = window.__coronaEditorInputLocks;
  return Boolean(
    window.__coronaGamePreviewInputLocked ||
    (locks instanceof Set && locks.size > 0)
  );
};

const resetRealtimeCameraInput = () => {
  Object.keys(movementKeys).forEach((key) => {
    movementKeys[key] = false;
  });
  stopMoveLoop();
  cameraMovementGestures.clear();
  mouseRotate.active = false;
  mouseRotate.startForward = null;
  mouseRotate.moved = false;
};

const setEditorCameraInputLock = (reason, locked) => {
  const locks = window.__coronaEditorInputLocks instanceof Set
    ? window.__coronaEditorInputLocks
    : new Set();
  window.__coronaEditorInputLocks = locks;
  if (locked) locks.add(reason);
  else locks.delete(reason);
  window.__coronaGamePreviewInputLocked = locks.size > 0;
  if (window.__coronaGamePreviewInputLocked) {
    resetRealtimeCameraInput();
  }
};

const setGamePreviewInputLocked = (locked) => {
  setEditorCameraInputLock('game_preview', Boolean(locked));
};

function clearKnownEditorCameraInputLocks() {
  setEditorCameraInputLock('node_graph', false);
  setEditorCameraInputLock('game_preview', false);
}

let cameraInputLockReconcileToken = 0;
async function reconcileEditorCameraInputLocks() {
  const token = ++cameraInputLockReconcileToken;
  const [scriptResult, previewResult] = await Promise.allSettled([
    editorApi.scratch.getScriptStatus(),
    editorApi.scratch.getGamePreviewStatus(),
  ]);
  if (token !== cameraInputLockReconcileToken) return;

  if (scriptResult.status === 'fulfilled') {
    const scriptStatus = unwrapBridgeData(scriptResult.value) || {};
    const scriptWorkerActive = Boolean(scriptStatus.threadAlive)
      && ['starting', 'running', 'stopping'].includes(String(scriptStatus.status || ''));
    setEditorCameraInputLock(
      'node_graph',
      scriptWorkerActive && Boolean(scriptStatus.inputLocked)
    );
  }
  if (previewResult.status === 'fulfilled') {
    const previewStatus = normalizePreviewDetails(unwrapBridgeData(previewResult.value) || {});
    const previewWorkerActive = Boolean(previewStatus.workerActive || previewStatus.stopPending);
    setGamePreviewInputLocked(
      previewWorkerActive && Boolean(previewStatus.inputLocked)
    );
  }
}

const moveLoop = (now) => {
  if (isGamePreviewInputLocked()) {
    resetRealtimeCameraInput();
    return;
  }

  const dt = Math.min((now - lastMoveTime) / 1000, 0.1); // 秒，上限 0.1s
  lastMoveTime = now;

  const anyActive = hasActiveMovementKeys();
  if (!anyActive) {
    moveLoopId = null;
    return;
  }

  const speed = cameraSpeed.value * 60 * dt; // 归一化到帧率无关
  const rotSpeed = 2.0 * 60 * dt;
  const { position, forward, up } = cameraState.value;
  const fwd = vec3.normalize(forward);
  const worldUp = vec3.normalize(up);
  const right = vec3.normalize(vec3.cross(worldUp, fwd));
  let moved = false;

  if (movementKeys.w) {
    position[0] += fwd[0] * speed;
    position[1] += fwd[1] * speed;
    position[2] += fwd[2] * speed;
    moved = true;
  }
  if (movementKeys.s) {
    position[0] -= fwd[0] * speed;
    position[1] -= fwd[1] * speed;
    position[2] -= fwd[2] * speed;
    moved = true;
  }
  if (movementKeys.a) {
    position[0] -= right[0] * speed;
    position[1] -= right[1] * speed;
    position[2] -= right[2] * speed;
    moved = true;
  }
  if (movementKeys.d) {
    position[0] += right[0] * speed;
    position[1] += right[1] * speed;
    position[2] += right[2] * speed;
    moved = true;
  }
  if (movementKeys.q) {
    position[0] += worldUp[0] * speed;
    position[1] += worldUp[1] * speed;
    position[2] += worldUp[2] * speed;
    moved = true;
  }
  if (movementKeys.e) {
    position[0] -= worldUp[0] * speed;
    position[1] -= worldUp[1] * speed;
    position[2] -= worldUp[2] * speed;
    moved = true;
  }

  if (movementKeys.arrowleft) {
    rotateCameraView('rotateLeft', rotSpeed);
    moved = true;
  }
  if (movementKeys.arrowright) {
    rotateCameraView('rotateRight', rotSpeed);
    moved = true;
  }
  if (movementKeys.arrowup) {
    rotateCameraView('rotateUp', rotSpeed);
    moved = true;
  }
  if (movementKeys.arrowdown) {
    rotateCameraView('rotateDown', rotSpeed);
    moved = true;
  }

  if (moved && !sendCameraUpdateFast()) {
    scheduleCameraUpdate();
  }

  moveLoopId = requestAnimationFrame(moveLoop);
};

// ---- 向量工具 ----
const vec3 = {
  length(v) {
    return Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  },
  normalize(v) {
    const len = vec3.length(v);
    return len > 1e-8 ? [v[0] / len, v[1] / len, v[2] / len] : [0, 0, 1];
  },
  cross(a, b) {
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  },
  dot(a, b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  },
};

const sceneAxisVectors = computed(() => {
  const fwd = vec3.normalize(cameraState.value.forward);
  let up = vec3.normalize(cameraState.value.up);
  let right = vec3.cross(up, fwd);

  if (vec3.length(right) <= 1e-6) {
    up = Math.abs(fwd[1]) < 0.95 ? [0, 1, 0] : [1, 0, 0];
    right = vec3.cross(up, fwd);
  }

  right = vec3.normalize(right);
  up = vec3.normalize(vec3.cross(fwd, right));

  const center = 45;
  const radius = 29;
  const axes = [
    { name: 'X', color: '#ef4444', vector: [1, 0, 0] },
    { name: 'Y', color: '#22c55e', vector: [0, 1, 0] },
    { name: 'Z', color: '#3b82f6', vector: [0, 0, 1] },
  ];

  return axes
    .map((axis) => {
      const screenX = vec3.dot(axis.vector, right);
      const screenY = -vec3.dot(axis.vector, up);
      const depth = vec3.dot(axis.vector, fwd);
      const x = center + screenX * radius;
      const y = center + screenY * radius;
      const labelOffset = 9;
      const labelLength = Math.max(1, Math.sqrt(screenX * screenX + screenY * screenY));

      return {
        ...axis,
        x,
        y,
        labelX: x + (screenX / labelLength) * labelOffset,
        labelY: y + (screenY / labelLength) * labelOffset,
        opacity: 0.58 + Math.max(0, depth) * 0.35,
        width: 2.5 + Math.max(0, depth) * 1.2,
        depth,
      };
    })
    .sort((a, b) => a.depth - b.depth);
});

/**
 * 绕任意轴旋转向量 v（罗德里格斯公式）
 * @param {number[]} v   待旋转向量
 * @param {number[]} axis 旋转轴（需归一化）
 * @param {number} angle  旋转角度（弧度）
 */
const rotateVecAroundAxis = (v, axis, angle) => {
  const c = Math.cos(angle);
  const s = Math.sin(angle);
  const k = axis;
  const dot = vec3.dot(k, v);
  const cross = vec3.cross(k, v);
  return [
    v[0] * c + cross[0] * s + k[0] * dot * (1 - c),
    v[1] * c + cross[1] * s + k[1] * dot * (1 - c),
    v[2] * c + cross[2] * s + k[2] * dot * (1 - c),
  ];
};

/**
 * 旋转摄像头 forward 向量
 * @param {'rotateLeft'|'rotateRight'|'rotateUp'|'rotateDown'} direction
 * @param {number} [angleDeg=2] 每步旋转角度
 */
const rotateCameraView = (direction, angleDeg = 2) => {
  const { forward, up } = cameraState.value;
  const fwd = vec3.normalize(forward);
  const worldUp = vec3.normalize(up);
  const angleRad = (angleDeg * Math.PI) / 180;

  let newFwd;
  if (direction === 'rotateLeft' || direction === 'rotateRight') {
    // 水平旋转（绕 world_up 轴）
    const yawAngle = direction === 'rotateLeft' ? -angleRad : angleRad;
    newFwd = rotateVecAroundAxis(fwd, worldUp, yawAngle);
  } else {
    // 垂直旋转（绕 right 轴，即 forward × up）
    const right = vec3.normalize(vec3.cross(fwd, worldUp));
    const pitchAngle = direction === 'rotateUp' ? angleRad : -angleRad;
    newFwd = rotateVecAroundAxis(fwd, right, pitchAngle);
    // 限制俯仰角，防止翻转（与 world_up 夹角保持在 10°~170°）
    const dotUp = vec3.dot(vec3.normalize(newFwd), worldUp);
    if (Math.abs(dotUp) > 0.985) return; // cos(10°) ≈ 0.985
  }

  cameraState.value.forward = vec3.normalize(newFwd);
};

/**
 * 鼠标拖拽旋转摄像头（灵敏度与分辨率无关）
 */
const handleMouseRotate = (dx, dy) => {
  const sensitivity = mouseSensitivity.value; // 度/像素
  const { forward, up } = cameraState.value;
  const fwd = vec3.normalize(forward);
  const worldUp = vec3.normalize(up);

  // 水平 yaw
  const yawRad = (dx * sensitivity * Math.PI) / 180;
  let newFwd = rotateVecAroundAxis(fwd, worldUp, yawRad);

  // 垂直 pitch
  const right = vec3.normalize(vec3.cross(newFwd, worldUp));
  const pitchRad = (-dy * sensitivity * Math.PI) / 180;
  const pitched = rotateVecAroundAxis(newFwd, right, pitchRad);

  const dotUp = vec3.dot(vec3.normalize(pitched), worldUp);
  if (Math.abs(dotUp) < 0.985) {
    newFwd = pitched;
  }

  cameraState.value.forward = vec3.normalize(newFwd);
};

const viewportCursorShape = () => (mouseRotate.active ? 'grabbing' : 'arrow');

const handleViewportPointer = (event) => {
  viewportGizmoController.pointer(event, event.type);
  if (event.type === 'pointerup') {
    try {
      viewportPickSurfaceRef.value?.releasePointerCapture?.(event.pointerId);
    } catch (_) {
      // Pointer capture may already have been released.
    }
  }
  sendScratchPointerEvent(event.type === 'pointerup' ? 'mouseup' : 'move', event);
  viewportUiPointerController.send(event, event.type, viewportCursorShape());
};

const handleViewportPointerDown = (event) => {
  try {
    viewportPickSurfaceRef.value?.setPointerCapture?.(event.pointerId);
  } catch (_) {
    // Pointer capture is best effort on embedded browser surfaces.
  }
  gizmoDownConsumed = false;
  gizmoDownPointerId = event.pointerId;
  gizmoDownRequestId = viewportGizmoController.pointer(event, event.type) || '';
  sendScratchPointerEvent('mousedown', event);
  viewportUiPointerController.send(
    event,
    event.type,
    event.button === 2 ? 'grabbing' : viewportCursorShape(),
  );
};

const handleViewportPointerCancel = (event) => {
  if (event.pointerId !== gizmoDownPointerId) return;
  try {
    viewportPickSurfaceRef.value?.releasePointerCapture?.(event.pointerId);
  } catch (_) {
    // Pointer capture may already have been released.
  }
  viewportGizmoController.cancel('pointercancel');
};

const handleViewportPointerLeave = () => {
  viewportUiPointerController.hide();
};

const handleViewportGizmoPointerResult = (payload = {}) => {
  const result = viewportGizmoController.handleResult(payload);
  if (payload.requestId === gizmoDownRequestId && payload.consumed) {
    gizmoDownConsumed = true;
  }
  if (result.status === 'ended' || result.status === 'cancelled') {
    gizmoDownRequestId = '';
    gizmoDownPointerId = null;
  }
};

const handleMainViewportBlur = () => viewportGizmoController.cancel('blur');

let pendingScratchClick = null;

const finishPendingScratchClick = (pickedActor = '') => {
  const pending = pendingScratchClick;
  if (!pending) return;
  pendingScratchClick = null;
  sendScratchPointerEvent('click', pending.event, pickedActor);
};

const handleViewportScratchClick = (event) => {
  const eventSnapshot = {
    clientX: Number(event?.clientX || 0),
    clientY: Number(event?.clientY || 0),
    button: Number(event?.button || 0),
  };
  const requestId = viewportPickController.pickAt(event);
  if (!requestId) {
    sendScratchPointerEvent('click', eventSnapshot, '');
    return;
  }

  // A newer click supersedes an unfinished request. The engine may still
  // complete the old request, but viewportPickController filters it by the
  // latest requestId and finishScratchClickFromPick ignores its old pending
  // entry. This keeps a later blank click from being swallowed by a slow pick.
  pendingScratchClick = {
    requestId,
    event: eventSnapshot,
  };
};

const handleViewportClick = (event) => {
  // A single click owns selection/clearing. Ignore the synthetic second
  // click from a rapid double-click so it cannot reset an active Gizmo drag.
  if (Number(event?.detail || 0) > 1) return;
  if (gizmoClickTimer) window.clearTimeout(gizmoClickTimer);
  const eventSnapshot = {
    clientX: Number(event?.clientX || 0),
    clientY: Number(event?.clientY || 0),
    button: Number(event?.button || 0),
  };
  gizmoClickTimer = window.setTimeout(() => {
    gizmoClickTimer = 0;
    if (!gizmoDownConsumed) handleViewportScratchClick(eventSnapshot);
  }, 45);
};

const finishScratchClickFromPick = (payload, result) => {
  if (!pendingScratchClick || payload?.requestId !== pendingScratchClick.requestId) return;
  if (result?.status === 'stale') return;
  const pickedActor =
    result?.actor?.name ||
    payload?.actorName ||
    payload?.name ||
    payload?.actor?.name ||
    '';
  finishPendingScratchClick(pickedActor);
};

const onMouseDown = (event) => {
  // 右键拖拽旋转（原有逻辑不变）
  if (event.button === 2) {
    if (isGamePreviewInputLocked()) return;
    mouseRotate.active = true;
    mouseRotate.lastX = event.clientX;
    mouseRotate.lastY = event.clientY;
    mouseRotate.startForward = [...cameraState.value.forward];
    mouseRotate.moved = false;
    event.preventDefault();
    return;
  }

  // 左键拾取只由 viewportPickSurfaceRef 对应的视口层触发。
};

const onMouseMove = (event) => {
  if (isGamePreviewInputLocked()) {
    mouseRotate.active = false;
    return;
  }
  if (!mouseRotate.active) return;
  const dx = event.clientX - mouseRotate.lastX;
  const dy = event.clientY - mouseRotate.lastY;
  mouseRotate.lastX = event.clientX;
  mouseRotate.lastY = event.clientY;

  if (dx === 0 && dy === 0) return;
  handleMouseRotate(dx, dy);
  mouseRotate.moved = true;
  scheduleCameraUpdate();
};

const onMouseUp = (event) => {
  if (isGamePreviewInputLocked()) {
    mouseRotate.active = false;
    return;
  }
  if (event.button === 2 && mouseRotate.active) {
    mouseRotate.active = false;
    const actualDelta = mouseRotate.startForward
      ? vectorDistance(mouseRotate.startForward, cameraState.value.forward)
      : 0;
    if (mouseRotate.moved && actualDelta > 1e-6) {
      void cabbageContextService.recordEvent({
        type: 'camera_rotated',
        category: 'camera',
        success: true,
        details: { interaction: 'right_mouse_drag', actualDelta },
      });
    }
    mouseRotate.startForward = null;
    mouseRotate.moved = false;
    if (!sendCameraUpdateFast()) {
      const sceneId = tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME;
      syncSceneCameraBinding(sceneId);
    }
  }
};

const onContextMenu = (event) => {
  if (isGamePreviewInputLocked()) return;
  event.preventDefault();
};

const refreshActorPickIndex = async (sceneId) => {
  if (!sceneId) return false;
  const result = await editorApi.sceneTools.listSceneTree(sceneId);
  const snapshot = result?.data ?? result;
  const actors = Array.isArray(snapshot?.actors) ? snapshot.actors : [];
  actorPickIndex = indexActorsByHandle(actors);
  return actorPickIndex.size > 0;
};

const applyActorPickResult = (result, payload = result?.payload) => {
  if (result.status === 'pending' || result.status === 'stale') return;

  if (result.status !== 'selected') {
    const fallbackActorHandle = Number(payload?.actorHandle || 0);
    if (
      result.status === 'unknown' &&
      payload?.status === 'success' &&
      Number.isFinite(fallbackActorHandle) &&
      fallbackActorHandle > 0
    ) {
      emitActorChangeFast(
        payload?.actorType || 'actor',
        payload?.sceneId || cameraBindingState.value.sceneId || DEFAULT_SCENE_NAME,
        payload?.actorName || `Actor ${fallbackActorHandle}`
      );
      return;
    }
    return;
  }

  // 选中结果已由 viewportPickController 内部 emitActorChange 驱动属性面板，
  // 此处无需额外处理。
};

const handleActorPickResult = (payload) => {
  const result = viewportPickController.handlePickResult(payload);
  applyActorPickResult(result, payload);
  // Gizmo synchronization is owned exclusively by actorSelectionChanged.
  // A miss still publishes the empty scene selection so that the same
  // selection path clears the property panel and Gizmo exactly once.
  if (result.status === 'miss') {
    emitActorChangeFast(
      'scene',
      payload?.sceneId || cameraBindingState.value.sceneId || DEFAULT_SCENE_NAME,
      '',
    );
  }
  finishScratchClickFromPick(payload, result);
};

const sendCameraUpdateFast = () => {
  // During Blockly/node-graph execution the editor must not publish camera
  // poses. Keyboard/mouse events are still sent to Scratch by their handlers.
  if (isGamePreviewInputLocked()) return true;
  const handle = cameraBindingState.value.cameraHandle;
  if (!handle) return false;
  const bridge = window.coronaBridge;
  if (!bridge || typeof bridge.cameraMove !== 'function') {
    if (!window._coronaBridgeWarned) {
      window._coronaBridgeWarned = true;
      console.warn(
        '[Camera] coronaBridge 缺失或 cameraMove 不可用，' +
        'CEF 子进程可能未运行。快速通道摄像头更新已禁用。'
      );
    }
    return false;
  }
  try {
    bridge.cameraMove(
      handle,
      [...cameraState.value.position],
      [...cameraState.value.forward],
      [...cameraState.value.up],
      cameraState.value.fov
    );
    return true;
  } catch (e) {
    return false;
  }
};

/** 发送当前 cameraState 到引擎——已移除，全部走快速通道 */

const handleCameraMove = (direction) => {
  if (isGamePreviewInputLocked()) return;

  const speed = cameraSpeed.value;
  const { position, forward, up } = cameraState.value;

  // 基于摄像头朝向计算移动方向（左手坐标系：right = up × forward）
  const fwd = vec3.normalize(forward);
  const worldUp = vec3.normalize(up);
  const right = vec3.normalize(vec3.cross(worldUp, fwd));

  switch (direction) {
    case 'up':
      position[0] += worldUp[0] * speed;
      position[1] += worldUp[1] * speed;
      position[2] += worldUp[2] * speed;
      break;
    case 'down':
      position[0] -= worldUp[0] * speed;
      position[1] -= worldUp[1] * speed;
      position[2] -= worldUp[2] * speed;
      break;
    case 'left':
      position[0] -= right[0] * speed;
      position[1] -= right[1] * speed;
      position[2] -= right[2] * speed;
      break;
    case 'right':
      position[0] += right[0] * speed;
      position[1] += right[1] * speed;
      position[2] += right[2] * speed;
      break;
    case 'forward':
      position[0] += fwd[0] * speed;
      position[1] += fwd[1] * speed;
      position[2] += fwd[2] * speed;
      break;
    case 'backward':
      position[0] -= fwd[0] * speed;
      position[1] -= fwd[1] * speed;
      position[2] -= fwd[2] * speed;
      break;
    case 'rotateRight':
    case 'rotateLeft':
    case 'rotateUp':
    case 'rotateDown':
      rotateCameraView(direction);
      break;
  }

  scheduleCameraUpdate();
};

const handleApplyPhysics = async () => {
  const sceneId = tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME;
  activeMenu.value = null;
  try {
    const result = await editorApi.sceneTools.setPhysicsParams(sceneId, {
      gravity: [
        physicsParams.value.gravityX,
        physicsParams.value.gravityY,
        physicsParams.value.gravityZ,
      ],
      floor_y: physicsParams.value.floorY,
      floor_restitution: physicsParams.value.floorRestitution,
      fixed_dt: physicsParams.value.fixedDt,
    });
    const data = unwrapBridgeData(result);
    if (data?.status === 'error' || data?.success === false) {
      logError('Apply physics params failed', data?.message || data?.error);
      return false;
    }
    return true;
  } catch (e) {
    logError('Apply physics params failed', e);
    return false;
  }
};

const loadPhysicsParams = async () => {
  const sceneId = tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME;
  try {
    const result = await editorApi.sceneTools.getPhysicsParams(sceneId);
    const data = result?.data ?? result;
    if (data && data.status !== 'error') {
      const g = data.gravity || [0, -9.8, 0];
      physicsParams.value.gravityX = g[0];
      physicsParams.value.gravityY = g[1];
      physicsParams.value.gravityZ = g[2];
      physicsParams.value.floorY = data.floor_y ?? 0.0;
      physicsParams.value.floorRestitution = data.floor_restitution ?? 0.6;
      physicsParams.value.fixedDt = data.fixed_dt ?? 1.0 / 60.0;
      return true;
    }
    return false;
  } catch (e) {
    logError('Load physics params failed', e);
    return false;
  }
};

// 关闭标签页
const closeTab = async (index) => {
  activeTab.value = 0;
};

// 切换标签页
const switchTab = async (index, if_new) => {
  activeTab.value = 0;
  const sceneId = tabs.value[0]?.id || DEFAULT_SCENE_NAME;
  await syncSceneCameraBinding(sceneId);
  await restoreCameraViews(sceneId);
};

const startEngine = () => {
  dockStore.initDefaultLayout();
};

const createScene = async () => {
  await syncSceneCameraBinding(tabs.value[0]?.id || DEFAULT_SCENE_NAME);
};

// ========== 预留的空函数 ==========

// 项目菜单
const handleNewProject = () => {
  console.log('新建项目');
  activeMenu.value = null;
  // TODO: 实现新建项目逻辑
};

const handleOpenProject = () => {
  console.log('打开项目');
  activeMenu.value = null;
  // TODO: 实现打开项目逻辑
};

const handleProjectSettings = () => {
  dockStore.openPanel('ProjectSettings');
  activeMenu.value = null;
};

const handleSaveProject = () => {
  console.log('保存项目');
  activeMenu.value = null;
  // TODO: 实现保存项目逻辑
};

// 视图工具/插件切换：由 Pinia dockStore 管理
const toggleViewTool = (tool) => {
  dockStore.togglePanel(tool.id);
};

const unwrapBridgeData = (result) => result?.data ?? result;

const stopProjectResourceLoadPolling = () => {
  if (projectResourceLoadPollTimer !== null) {
    window.clearTimeout(projectResourceLoadPollTimer);
    projectResourceLoadPollTimer = null;
  }
};

const pollProjectResourceLoadStatus = async () => {
  stopProjectResourceLoadPolling();
  try {
    const status = unwrapBridgeData(
      await editorApi.project.getProjectLoadStatus(),
    );
    projectResourceLoadStatus.value = status?.active ? status : null;
    if (status?.loading) {
      projectResourceLoadPollTimer = window.setTimeout(
        pollProjectResourceLoadStatus,
        250,
      );
    } else {
      projectResourceLoadStatus.value = null;
    }
  } catch (error) {
    projectResourceLoadStatus.value = null;
    logWarn('Failed to query project resource load status', error);
  }
};

const clearPreviewPoll = () => {
  if (previewPollTimer) {
    clearTimeout(previewPollTimer);
    previewPollTimer = null;
  }
};

const normalizePreviewDetails = (payload = {}) => ({
  ...payload,
  scope: payload.scope || 'project',
  sceneName: payload.sceneName ?? payload.scene_name ?? '',
  startedCount: Number(payload.startedCount ?? payload.started_count ?? 0),
  runningCount: Number(payload.runningCount ?? payload.running_count ?? 0),
  completedCount: Number(payload.completedCount ?? payload.completed_count ?? 0),
  errorCount: Number(payload.errorCount ?? payload.error_count ?? 0),
  blocklyCount: Number(payload.blocklyCount ?? payload.blockly_count ?? 0),
  nodeGraphCount: Number(payload.nodeGraphCount ?? payload.node_graph_count ?? 0),
  inputLocked: Boolean(payload.inputLocked ?? payload.input_locked),
  hasSnapshot: Boolean(payload.hasSnapshot ?? payload.has_snapshot),
  restoreStatus: payload.restoreStatus ?? payload.restore_status ?? 'idle',
  restoreError: payload.restoreError ?? payload.restore_error ?? '',
  restored: Boolean(
    payload.restored
    ?? ['restored', 'completed', 'success'].includes(String(payload.restoreStatus ?? payload.restore_status ?? '').toLowerCase())
  ),
  stopPending: Boolean(payload.stopPending ?? payload.stop_pending),
  workerActive: Boolean(payload.workerActive ?? payload.worker_active),
  errors: Array.isArray(payload.errors) ? payload.errors : [],
  warnings: Array.isArray(payload.warnings) ? payload.warnings : [],
  targets: Array.isArray(payload.targets) ? payload.targets : [],
});

const publishGamePreviewStatus = (details = {}) => {
  if (typeof window === 'undefined') return;
  window.__coronaGamePreviewState = details;
  window.dispatchEvent(new CustomEvent('corona-game-preview-status', { detail: details }));
};

const applyPreviewStatus = (payload = {}) => {
  const details = normalizePreviewDetails(payload);
  previewDetails.value = details;
  const state = details.status || 'idle';
  previewRunning.value = ['starting', 'running', 'stopping'].includes(state)
    || details.runningCount > 0
    || details.hasSnapshot;
  setGamePreviewInputLocked(Boolean(details.inputLocked));
  if (state === 'starting') previewStatusText.value = '正在启动脚本...';
  else if (state === 'running') previewStatusText.value = `预览中 ${details.runningCount || details.startedCount}`;
  else if (state === 'stopping') previewStatusText.value = '正在停止并恢复...';
  else if (state === 'completed') previewStatusText.value = details.errorCount ? `已完成，${details.errorCount} 个脚本错误` : '脚本已完成';
  else if (state === 'stopped') previewStatusText.value = '已停止并恢复';
  else if (state === 'error') previewStatusText.value = details.restoreError ? `场景恢复失败：${details.restoreError}` : (details.errors[0] || details.message || '预览出错');
  else previewStatusText.value = details.startedCount === 0 && details.warnings.length ? '没有可运行脚本' : '';
  publishGamePreviewStatus(details);
  if (state === 'running' && !tutorialPreviewObservedRunning) {
    tutorialPreviewObservedRunning = true;
    void cabbageContextService.recordEvent({
      type: 'preview_started',
      category: 'preview',
      success: true,
      details: { status: 'running' },
    });
  } else if (
    state === 'stopped'
    && tutorialPreviewObservedRunning
    && details.restored
    && !details.restoreError
  ) {
    tutorialPreviewObservedRunning = false;
    void cabbageContextService.recordEvent({
      type: 'preview_stopped',
      category: 'preview',
      success: true,
      details: { status: 'stopped', restored: true, restoreError: '' },
    });
  }
  return details;
};

const pollGamePreviewStatus = () => {
  clearPreviewPoll();
  const poll = async () => {
    try {
      const result = await editorApi.scratch.getGamePreviewStatus();
      const details = applyPreviewStatus(unwrapBridgeData(result));
      broadcastViewportControlsState();
      if (previewRunning.value) previewPollTimer = setTimeout(poll, 700);
      return details;
    } catch (error) {
      previewRunning.value = false;
      previewDetails.value = {};
      setGamePreviewInputLocked(false);
      previewStatusText.value = '预览状态异常';
      logError('查询预览状态失败', error);
      return null;
    }
  };
  previewPollTimer = setTimeout(poll, 300);
};

const normalizePreviewRequest = (request) => {
  if (!request || typeof request !== 'object' || !['project', 'scene'].includes(request.scope)) {
    return { scope: 'project' };
  }
  return {
    scope: request.scope,
    ...(request.scope === 'scene' ? { scene_name: request.scene_name || request.sceneName || '' } : {}),
  };
};

const handleStartGamePreview = async (request = { scope: 'project' }) => {
  if (previewBusy.value) return false;
  const previewRequest = normalizePreviewRequest(request);
  window.__coronaPreviewActionPendingCount = Number(window.__coronaPreviewActionPendingCount || 0) + 1;
  window.__coronaPreviewActionPending = true;
  window.__coronaPreviewPendingScope = previewRequest.scope;
  previewBusy.value = true;
  previewStatusText.value = previewRequest.scope === 'scene' ? '准备当前场景脚本...' : '准备项目预览...';
  try {
    // 本地 previewRunning 可能因跨面板广播延迟而滞后。启动前重新读取
    // 后端真值，避免把一次有效点击误判为重复启动。
    try {
    const live = applyPreviewStatus(unwrapBridgeData(await editorApi.scratch.getGamePreviewStatus()));
      const liveActive = ['starting', 'running', 'stopping'].includes(live.status)
        || live.runningCount > 0
        || live.hasSnapshot;
      if (liveActive) {
        broadcastViewportControlsState();
        return live;
      }
    } catch (statusError) {
      logWarn('启动前查询预览状态失败，将继续尝试启动', statusError);
    }
    publishGamePreviewStatus(normalizePreviewDetails({
      status: 'starting',
      scope: previewRequest.scope,
      sceneName: previewRequest.scene_name || '',
    }));
    if (previewRequest.scope === 'project') {
      // Global run executes only node_graph:project:global.
      await flushProjectNodeGraphBeforeRun();
    } else {
      if (typeof window.__coronaBlocklyFlushSave === 'function') {
        await window.__coronaBlocklyFlushSave();
      }
      if (typeof window.__coronaNodeGraphFlushSave === 'function') {
        await window.__coronaNodeGraphFlushSave();
      }
    }
    const result = await editorApi.scratch.startGamePreview(previewRequest);
    const payload = unwrapBridgeData(result);
    const details = applyPreviewStatus(payload);
    if (payload?.status === 'error') {
      logError('开始预览失败', payload.message || details.errors[0]);
      broadcastViewportControlsState();
      return false;
    }
    if (previewRunning.value) pollGamePreviewStatus();
    broadcastViewportControlsState();
    return details;
  } catch (error) {
    previewRunning.value = false;
    previewDetails.value = {};
    setGamePreviewInputLocked(false);
    previewStatusText.value = '预览启动失败';
    logError('开始预览失败', error);
    broadcastViewportControlsState();
    return false;
  } finally {
    window.__coronaPreviewActionPendingCount = Math.max(0, Number(window.__coronaPreviewActionPendingCount || 1) - 1);
    window.__coronaPreviewActionPending = window.__coronaPreviewActionPendingCount > 0;
    if (!window.__coronaPreviewActionPending) window.__coronaPreviewPendingScope = '';
    previewBusy.value = false;
    activeMenu.value = null;
    broadcastViewportControlsState();
  }
};

const handleStopGamePreview = async () => {
  // 后端停止接口是幂等的。不要因为前端状态同步稍慢而挡住停止请求，
  // 否则 Scene ID 不一致或跨窗口状态延迟时会出现“只能运行、不能停止”。
  if (previewBusy.value) return false;
  previewBusy.value = true;
  previewStatusText.value = '正在停止并恢复...';
  try {
    const result = await editorApi.scratch.stopGamePreview();
    const payload = unwrapBridgeData(result);
    const details = applyPreviewStatus(payload);
    if (details.status === 'stopping' || details.stopPending) {
      pollGamePreviewStatus();
    } else {
      clearPreviewPoll();
    }
    if (details.restoreError && details.status !== 'stopping') {
      logError('结束预览恢复失败', details.restoreError);
      broadcastViewportControlsState();
      return false;
    }
    broadcastViewportControlsState();
    return details;
  } catch (error) {
    previewStatusText.value = '结束预览失败';
    logError('结束预览失败', error);
    // 即使本次 RPC 报错也继续轮询，后端可能已经收到停止请求。
    pollGamePreviewStatus();
    broadcastViewportControlsState();
    return false;
  } finally {
    previewBusy.value = false;
    activeMenu.value = null;
    broadcastViewportControlsState();
  }
};

const handleRunProject = async () => {
  try {
    console.log('运行项目');
    // 不传参数，运行整个项目
    const result = await editorApi.main.runProject();

    if (result.success) {
      // TODO: 可以显示一个成功提示
      return true;
    } else {
      logError('运行项目返回失败', result?.message);
      return false;
    }
  } catch (error) {
    logError('运行项目失败', error);
    return false;
  } finally {
    activeMenu.value = null;
  }
};

const handleRunCurrentScene = async () => {
  try {
    console.log('运行当前场景');
    const currentSceneId = tabs.value[activeTab.value]?.id;

    if (!currentSceneId) {
      logError('没有当前场景');
      return false;
    }

    // 传入场景路径，运行指定场景
    const result = await editorApi.main.runProject(currentSceneId);

    if (result.success) {
      // TODO: 可以显示一个成功提示
      return true;
    } else {
      logError('运行当前场景返回失败', result?.message);
      return false;
    }
  } catch (error) {
    logError('运行当前场景失败', error);
    return false;
  } finally {
    activeMenu.value = null;
  }
};

const getPhysicsSnapshot = () => ({
  gravityX: Number(physicsParams.value.gravityX),
  gravityY: Number(physicsParams.value.gravityY),
  gravityZ: Number(physicsParams.value.gravityZ),
  floorY: Number(physicsParams.value.floorY),
  floorRestitution: Number(physicsParams.value.floorRestitution),
  fixedDt: Number(physicsParams.value.fixedDt),
});

const coerceNumber = (value, fallback) => {
  const next = Number(value);
  return Number.isFinite(next) ? next : fallback;
};

const setCameraSpeedFromPanel = (value) => {
  const next = Math.min(2, Math.max(0.01, coerceNumber(value, cameraSpeed.value)));
  cameraSpeed.value = next;
  broadcastViewportControlsState();
  return getEditorControlsState();
};

const setViewportUiModeFromPanel = (mode) => {
  if (!viewportUiModeItems.some((item) => item.mode === mode)) {
    return false;
  }
  selectViewportUiMode(mode);
  broadcastViewportControlsState();
  return getEditorControlsState();
};

const setSceneGridEnabledFromPanel = async (enabled, requestedSceneId = '') => {
  const currentSceneId = tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME;
  const requested = String(requestedSceneId || '').trim();
  if (requested && requested !== currentSceneId) {
    await syncSceneCameraBinding(currentSceneId, { preservePose: true });
    broadcastViewportControlsState();
    return getEditorControlsState();
  }

  const nextEnabled = Boolean(enabled);
  try {
    await editorApi.sceneTools.floorGrid(currentSceneId, nextEnabled);
    sceneGridEnabled.value = nextEnabled;
    broadcastViewportControlsState();
    return getEditorControlsState();
  } catch (error) {
    logError('更新场景编辑网格失败', error);
    return false;
  }
};

const applyPhysicsFromSettings = async (nextParams = {}) => {
  const current = getPhysicsSnapshot();
  physicsParams.value = {
    gravityX: coerceNumber(nextParams.gravityX, current.gravityX),
    gravityY: coerceNumber(nextParams.gravityY, current.gravityY),
    gravityZ: coerceNumber(nextParams.gravityZ, current.gravityZ),
    floorY: coerceNumber(nextParams.floorY, current.floorY),
    floorRestitution: coerceNumber(nextParams.floorRestitution, current.floorRestitution),
    fixedDt: Math.max(0.001, coerceNumber(nextParams.fixedDt, current.fixedDt)),
  };
  const applied = await handleApplyPhysics();
  if (applied === false) return false;
  return getEditorControlsState();
};

const currentMainRenderMode = () =>
  mainRenderBackend.value === 'vision' ? mainVisionRenderMode.value : 'native';

const getEditorControlsState = () => ({
  available: true,
  sceneId: tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME,
  previewRunning: previewRunning.value,
  previewBusy: previewBusy.value,
  previewStatusText: previewStatusText.value,
  preview: previewDetails.value,
  visionAvailable: visionAvailable.value,
  renderMode: currentMainRenderMode(),
  renderLabel: mainRenderModeLabel.value,
  renderModes: mainRenderModeOptions.map((mode) => ({
    ...mode,
    active: currentMainRenderMode() === mode.value,
    disabled: mode.backend === 'vision' && !visionAvailable.value,
  })),
  viewportUiMode: viewportUiMode.value,
  viewportUiModes: viewportUiModeItems.map((item) => ({
    ...item,
    active: item.mode === viewportUiMode.value,
  })),
  cameraSpeed: Number(cameraSpeed.value),
  gridEnabled: Boolean(sceneGridEnabled.value),
  physics: getPhysicsSnapshot(),
});

const broadcastViewportControlsState = () => {
  appService
    .crossTabBroadcast('viewport-controls-state', getEditorControlsState())
    .catch(() => {});
};

const handleViewportControlsRequest = async (payload = {}) => {
  if (!payload || typeof payload !== 'object') {
    broadcastViewportControlsState();
    return;
  }

  if (payload.action === 'setViewportUiMode') {
    setViewportUiModeFromPanel(payload.mode);
    return;
  }

  if (payload.action === 'setCameraSpeed') {
    setCameraSpeedFromPanel(payload.value);
    return;
  }

  if (payload.action === 'setGridEnabled') {
    await setSceneGridEnabledFromPanel(payload.enabled, payload.sceneId);
    return;
  }

  if (payload.action === 'startPreview') {
    await handleStartGamePreview(payload.request || payload);
    broadcastViewportControlsState();
    return;
  }

  if (payload.action === 'stopPreview') {
    await handleStopGamePreview();
    broadcastViewportControlsState();
    return;
  }

  broadcastViewportControlsState();
};

const registerEditorControls = () => {
  window[EDITOR_CONTROLS_KEY] = {
    getState: getEditorControlsState,
    refreshPhysics: async () => {
      const loaded = await loadPhysicsParams();
      if (loaded === false) return false;
      return getEditorControlsState();
    },
    applyPhysics: applyPhysicsFromSettings,
    startPreview: handleStartGamePreview,
    stopPreview: handleStopGamePreview,
    runProject: handleRunProject,
    runCurrentScene: handleRunCurrentScene,
    selectRenderMode: selectMainRenderMode,
    setViewportUiMode: setViewportUiModeFromPanel,
    setCameraSpeed: setCameraSpeedFromPanel,
    setGridEnabled: setSceneGridEnabledFromPanel,
  };
};

const unregisterEditorControls = () => {
  if (window[EDITOR_CONTROLS_KEY]?.getState === getEditorControlsState) {
    delete window[EDITOR_CONTROLS_KEY];
  }
};

// 帮助菜单
const handleHelpDocs = () => {
  console.log('帮助文档');
  activeMenu.value = null;
  // TODO: 实现打开帮助文档逻辑
};

const handleAbout = () => {
  console.log('关于');
  activeMenu.value = null;
  // TODO: 实现显示关于信息逻辑
};

const showLoading = ({ title = '加载中', message = '请稍候...', progress = 0 } = {}) => {
  localModalTitle.value = title;
  localModalMessage.value = message;
  localModalProgress.value = progress;
  showLocalModal.value = true;
};

const updateLoading = ({ message, progress } = {}) => {
  if (message !== undefined) localModalMessage.value = message;
  if (progress !== undefined) localModalProgress.value = progress;
};

const hideLoading = () => {
  showLocalModal.value = false;
  setTimeout(() => {
    localModalTitle.value = '';
    localModalMessage.value = '';
    localModalProgress.value = 0;
  }, 300);
};

const applyCameraPose = (pose = {}) => {
  const toVector3 = (value) => {
    if (!isVector3(value)) return null;
    const next = value.map((item) => Number(item));
    return next.every((item) => Number.isFinite(item)) ? next : null;
  };

  const position = toVector3(pose.position);
  const forward = toVector3(pose.forward);
  const up = toVector3(pose.up);
  if (!position || !forward || !up) {
    return false;
  }

  cameraState.value = {
    position,
    forward,
    up,
    fov: Number.isFinite(Number(pose.fov)) ? Number(pose.fov) : cameraState.value.fov,
  };

  if (Number.isFinite(Number(pose.cameraHandle))) {
    cameraBindingState.value = {
      ...cameraBindingState.value,
      cameraHandle: Number(pose.cameraHandle),
      cameraId: pose.cameraId ?? cameraBindingState.value.cameraId,
      cameraName: pose.cameraName ?? cameraBindingState.value.cameraName,
    };
  }

  scheduleCameraUpdate();
  return true;
};

const addSceneTab = (name, id) => {
  tabs.value = [{ name, id }];
  activeTab.value = 0;
  syncSceneCameraBinding(id);
  return true;
};

const renameSceneTab = (oldId, newId, newName) => {
  if (!tabs.value[0] || tabs.value[0].id !== oldId) {
    return false;
  }

  tabs.value[0] = {
    ...tabs.value[0],
    id: newId,
    name: newName || tabs.value[0].name,
  };

  cameraBindingState.value.sceneId = newId;
  syncSceneCameraBinding(newId);

  return true;
};

const onSceneAddedEvent = (payload) => addSceneTab(payload?.name, payload?.route);

const onSceneRenamedEvent = (payload) => renameSceneTab(payload?.old_path, payload?.new_path, payload?.name);

const pendingPanelRedocks = new Map();
const PANEL_REDOCK_TTL_MS = 5000;

const handlePanelRedockRequest = (payload) => {
  const panelId = payload?.panelId;
  if (!panelId || !dockStore.panels[panelId]) return;
  const previousTimer = pendingPanelRedocks.get(panelId);
  if (previousTimer) window.clearTimeout(previousTimer);
  const timer = window.setTimeout(() => pendingPanelRedocks.delete(panelId), PANEL_REDOCK_TTL_MS);
  pendingPanelRedocks.set(panelId, timer);
  if (panelId === 'CabbageChatPanel') {
    dockStore.popIn(panelId);
    dockStore.closePanel(panelId);
    return;
  }
  openDockedPanel(panelId, { preserveZone: true });
};

const handlePanelClosed = (payload) => {
  const panelId = payload?.panelId;
  if (!panelId) return;
  const redockTimer = pendingPanelRedocks.get(panelId);
  if (redockTimer) {
    window.clearTimeout(redockTimer);
    pendingPanelRedocks.delete(panelId);
    if (panelId === 'CabbageChatPanel') {
      dockStore.popIn(panelId);
      dockStore.closePanel(panelId);
    } else {
      openDockedPanel(panelId, { preserveZone: true });
    }
    return;
  }
  if (isFloatingPanel(panelId)) {
    // Ignore the delayed native event produced by our own close request. The
    // queued shortcut operation may already have opened a replacement tab.
    if (consumeExpectedPanelClosed(panelId)) return;
    dockStore.markExternalClosed(panelId);
    return;
  }
  dockStore.popIn(panelId);
};


onMounted(async () => {
  // A reused CEF page must not inherit camera locks from a previous world.
  // Real active workers are added back by reconcileEditorCameraInputLocks().
  clearKnownEditorCameraInputLocks();
  const result = await editorApi.main.onInit();
  const initData = result?.data ?? result;
  const scenes = initData?.scenes ?? [];
  const activeIndex = Number(initData?.active_index ?? 0);
  await pollProjectResourceLoadStatus();
  try {
    const visionResult = unwrapBridgeData(await editorApi.sceneTools.isVisionAvailable());
    visionAvailable.value = !!visionResult?.available;
  } catch (error) {
    visionAvailable.value = false;
  }

  if (scenes.length > 0) {
    for (const s of scenes) {
      tabs.value.push({ name: s.name, id: s.path });
    }
  } else {
    // 兼容旧格式
    tabs.value.push({
      name: initData?.name ?? DEFAULT_SCENE_NAME,
      id: initData?.path ?? DEFAULT_SCENE_NAME,
    });
  }
  const resolvedActiveIndex =
    tabs.value.length > 0
      ? Math.min(Math.max(Number.isFinite(activeIndex) ? activeIndex : 0, 0), tabs.value.length - 1)
      : 0;
  activeTab.value = resolvedActiveIndex;

  await startEngine();
  // 等待 Vue 渲染 dock 面板（SceneBar/Object 等），确保 eventBus 监听就绪
  await nextTick();
  const initialSceneId = tabs.value[resolvedActiveIndex]?.id || DEFAULT_SCENE_NAME;
  await syncSceneCameraBinding(tabs.value[activeTab.value]?.id || DEFAULT_SCENE_NAME);
  syncViewportUiMode();
  scheduleCameraViewportSync();
  if (typeof ResizeObserver !== 'undefined' && viewportPickSurfaceRef.value) {
    cameraViewportResizeObserver = new ResizeObserver(handleViewportLayoutChange);
    cameraViewportResizeObserver.observe(viewportPickSurfaceRef.value);
  }
  await restoreCameraViews(initialSceneId);
  // Restoring detached camera windows can overlap native scene initialization.
  // Always take a second snapshot after that phase so the main viewport owns the
  // current camera handle rather than the camera object that was just destroyed.
  await syncSceneCameraBinding(initialSceneId);
  // The CEF window can survive project creation/switching, so discard stale
  // frontend lock reasons using the backend's self-healed runtime truth.
  await reconcileEditorCameraInputLocks();

  document.addEventListener('keydown', handleKeyDown);
  document.addEventListener('keyup', handleKeyUp);
  document.addEventListener('click', handleClickOutside);
  document.addEventListener('mousedown', onMouseDown);
  document.addEventListener('mousemove', onMouseMove);
  document.addEventListener('mouseup', onMouseUp);
  document.addEventListener('contextmenu', onContextMenu);
  window.addEventListener('resize', handleViewportLayoutChange);
  window.addEventListener('blur', handleMainViewportBlur);
  registerEditorControls();

  // 跨窗口事件监听：panel / loading / viewport 等 UI 本地通道
  coronaEventBus.on('panel-redock-request', handlePanelRedockRequest);
  coronaEventBus.on('panel-closed', handlePanelClosed);
  coronaEventBus.on('loading-show', showLoading);
  coronaEventBus.on('loading-update', updateLoading);
  coronaEventBus.on('loading-hide', hideLoading);
  coronaEventBus.on('camera-pose-request', applyCameraPose);
  coronaEventBus.on('viewport-controls-request', handleViewportControlsRequest);
  coronaEventBus.on('node-graph-panel-open-request', handleNodeGraphPanelOpenRequest);
  sceneAddedCallbackToken = await editorApi.events.onSceneAdded(onSceneAddedEvent);
  sceneRenamedCallbackToken = await editorApi.events.onSceneRenamed(onSceneRenamedEvent);
  actorSelectionCallbackToken = await editorApi.events.onActorSelectionChanged(handleActorSelectionForObjectDock);
  actorTransformCallbackToken = await editorApi.events.onActorTransformUpdated(handleActorTransformForCabbage);
  actorPickResultCallbackToken = await editorApi.events.onActorPickResult(handleActorPickResult);
  gizmoPointerResultCallbackToken =
    await editorApi.events.onViewportGizmoPointerResult(handleViewportGizmoPointerResult);
  coronaEventBus.on('viewport-ui-calibration-changed', applyViewportUiCalibration);

  // Primary work docks start hidden. If this main CEF page is reused, close any native
  // floating tab left by the previous project before resetting the shortcut state.
  for (const panelId of [
    ...dockShortcuts.map((item) => item.id),
    'Object',
    'CabbageChatPanel',
  ]) {
    const panelState = dockStore.panels[panelId];
    if (panelState?.mode === 'external') {
      await closeFloatingPanel(dockStore, panelId);
    } else {
      dockStore.closePanel(panelId);
    }
  }
  await syncSceneCameraBinding(initialSceneId);
  broadcastViewportControlsState();

  // Subscribe to proactive DeepSeek reviews and the world-scoped assistant context.
  unsubscribeNodeGraphReview = subscribeNodeGraphReviews(handleNodeGraphReview);
  unsubscribeCabbageContext = subscribeCabbageAssistantContext(
    (snapshot) => cabbageAssistant.hydrateContext(snapshot),
    { projectScopeId: currentProjectReviewScopeId, emitCurrent: true }
  );
  unsubscribeCabbagePreWarnings = subscribeCabbagePreWarnings(
    (warning) => cabbageAssistant.showPreWarning(warning),
    { projectScopeId: currentProjectReviewScopeId }
  );
  await loadCabbageWorldContext({ reset: true });
  cabbageCandidateTimer = window.setInterval(() => promoteDueCabbageTasks(), 1000);
  window.addEventListener('cabbage-run-failed', onCabbageRunFailed);
  window.addEventListener('corona-active-project-changed', onActiveProjectChanged);
  window.addEventListener('storage', onActiveProjectStorageChanged);
});

onUnmounted(() => {
  cabbageWorldInitializationRetry.cancel();
  stopProjectResourceLoadPolling();
  clearPreviewPoll();
  clearKnownEditorCameraInputLocks();
  unregisterEditorControls();
  unsubscribeNodeGraphReview?.();
  unsubscribeNodeGraphReview = null;
  unsubscribeCabbageContext?.();
  unsubscribeCabbageContext = null;
  unsubscribeCabbagePreWarnings?.();
  unsubscribeCabbagePreWarnings = null;
  if (cabbageCandidateTimer) window.clearInterval(cabbageCandidateTimer);
  cabbageCandidateTimer = null;
  window.removeEventListener('cabbage-run-failed', onCabbageRunFailed);
  void cabbageContextService.flush();
  window.removeEventListener('corona-active-project-changed', onActiveProjectChanged);
  window.removeEventListener('storage', onActiveProjectStorageChanged);
  closeTutorialSessionChannel();
  coronaEventBus.off('panel-redock-request', handlePanelRedockRequest);
  coronaEventBus.off('panel-closed', handlePanelClosed);
  for (const timer of pendingPanelRedocks.values()) window.clearTimeout(timer);
  pendingPanelRedocks.clear();
  coronaEventBus.off('loading-show', showLoading);
  coronaEventBus.off('loading-update', updateLoading);
  coronaEventBus.off('loading-hide', hideLoading);
  coronaEventBus.off('camera-pose-request', applyCameraPose);
  coronaEventBus.off('viewport-controls-request', handleViewportControlsRequest);
  coronaEventBus.off('node-graph-panel-open-request', handleNodeGraphPanelOpenRequest);
  if (gizmoClickTimer) window.clearTimeout(gizmoClickTimer);
  gizmoClickTimer = 0;
  viewportGizmoController.cancel('cancel');
  viewportGizmoController.clearTarget();
  if (actorPickResultCallbackToken) {
    editorApi.off(actorPickResultCallbackToken).catch((error) => {
      logError('Failed to unregister actor pick result callback', error);
    });
    actorPickResultCallbackToken = null;
  }
  if (actorSelectionCallbackToken) {
    editorApi.off(actorSelectionCallbackToken).catch((error) => {
      logError('Failed to unregister actor selection callback', error);
    });
    actorSelectionCallbackToken = null;
  }
  if (gizmoPointerResultCallbackToken) {
    editorApi.off(gizmoPointerResultCallbackToken).catch((error) => {
      logError('Failed to unregister viewport gizmo callback', error);
    });
    gizmoPointerResultCallbackToken = null;
  }
  if (actorTransformCallbackToken) {
    editorApi.off(actorTransformCallbackToken).catch((error) => {
      logError('Failed to unregister actor transform callback', error);
    });
    actorTransformCallbackToken = null;
  }
  actorTransformBaselines.clear();
  if (sceneAddedCallbackToken) {
    editorApi.off(sceneAddedCallbackToken).catch((error) => {
      logError('Failed to unregister scene added callback', error);
    });
    sceneAddedCallbackToken = null;
  }
  if (sceneRenamedCallbackToken) {
    editorApi.off(sceneRenamedCallbackToken).catch((error) => {
      logError('Failed to unregister scene renamed callback', error);
    });
    sceneRenamedCallbackToken = null;
  }
  window.removeEventListener('resize', handleViewportLayoutChange);
  window.removeEventListener('blur', handleMainViewportBlur);
  sceneCameraBindingRequestRevision += 1;
  sceneCameraBindingRefreshPromise = null;
  stopMoveLoop();
  if (cameraViewportSyncRafId != null) {
    cancelAnimationFrame(cameraViewportSyncRafId);
    cameraViewportSyncRafId = null;
  }
  cameraViewportResizeObserver?.disconnect?.();
  cameraViewportResizeObserver = null;
  viewportPickController.dispose();
  viewportUiPointerController.dispose();
  document.removeEventListener('keydown', handleKeyDown);
  document.removeEventListener('keyup', handleKeyUp);
  document.removeEventListener('click', handleClickOutside);
  document.removeEventListener('mousedown', onMouseDown);
  document.removeEventListener('mousemove', onMouseMove);
  document.removeEventListener('mouseup', onMouseUp);
  document.removeEventListener('contextmenu', onContextMenu);
});
</script>

<style scoped>
[data-viewport-pick-surface] {
  touch-action: none;
}

.viewport-cursor-hidden,
.viewport-cursor-hidden * {
  cursor: none !important;
}

.scene-quick-controls {
  position: absolute;
  top: 12px;
  left: 12px;
  z-index: 2147483000;
  width: min(356px, calc(100% - 24px));
  padding: 9px;
  border: 1px solid rgba(216, 184, 108, 0.48);
  border-radius: 9px;
  background: rgba(13, 12, 8, 0.94);
  box-shadow: 0 10px 30px rgba(0, 0, 0, 0.45);
  backdrop-filter: blur(8px);
  color: #e9dfc5;
  pointer-events: auto;
}

.scene-quick-lighting-header,
.scene-quick-direction,
.scene-quick-light-toggle,
.scene-quick-direction label {
  display: flex;
  align-items: center;
}

.scene-quick-lighting {
  margin: 0;
}

.scene-quick-lighting-header {
  justify-content: space-between;
  color: #eadfbd;
  font-size: 11px;
  font-weight: 700;
}

.scene-quick-light-toggle {
  gap: 5px;
  color: #bfb493;
  font-size: 10px;
  font-weight: 500;
}

.scene-quick-light-toggle input {
  accent-color: #d8b86c;
}

.scene-quick-direction {
  min-width: 0;
  margin-top: 7px;
  gap: 5px;
}

.scene-quick-direction.disabled {
  opacity: 0.5;
}

.scene-quick-direction-label {
  flex: 0 0 auto;
  margin-right: 2px;
  color: #a99d80;
  font-size: 10px;
}

.scene-quick-direction label {
  min-width: 0;
  flex: 1 1 0;
  gap: 3px;
  color: #91876e;
  font-size: 9px;
}

.scene-quick-direction input {
  width: 100%;
  min-width: 0;
  padding: 4px 5px;
  border: 1px solid #55431f;
  border-radius: 4px;
  outline: none;
  background: #0f0e0a;
  color: #e5e7eb;
  font-size: 10px;
}

.scene-quick-direction input:focus {
  border-color: #d8b86c;
}

.dock-shortcut-bar {
  position: absolute; right: 16px; bottom: 16px; z-index: 2147483000;
  display: flex; align-items: center; gap: 7px; padding: 7px;
  border: 1px solid rgba(216, 184, 108, .48); border-radius: 9px;
  background: rgba(13, 12, 8, .96); box-shadow: 0 10px 30px rgba(0, 0, 0, .45);
  backdrop-filter: blur(8px);
}
.dock-shortcut-button {
  display: inline-flex; align-items: center; gap: 6px; border: 1px solid #56491f;
  border-radius: 6px; background: #18150d; color: #d8cfb7; padding: 7px 10px;
  font-size: 12px; line-height: 1; transition: border-color .15s ease, background .15s ease, color .15s ease;
}
.dock-shortcut-button:hover { border-color: #b79232; color: #fff4cd; }
.dock-shortcut-button.active { border-color: #d8b86c; background: #332712; color: #fff4cd; box-shadow: inset 0 0 0 1px rgba(216, 184, 108, .18); }
.dock-shortcut-button.pending { cursor: progress; border-color: #9f8132; }
.dock-shortcut-button.pending .dock-shortcut-icon { animation: dock-shortcut-pulse .7s ease-in-out infinite alternate; }
@keyframes dock-shortcut-pulse { from { opacity: .48; } to { opacity: 1; } }
.dock-shortcut-icon { display:grid; place-items:center; width:19px; height:19px; border-radius:4px; background:#0e0d09; color:#d6b66b; font-size:10px; font-weight:700; }

.cabbage-resident-stack {
  position: absolute;
  left: 12px;
  bottom: 12px;
  z-index: 2147482500;
  width: min(418px, calc(100% - 24px));
  max-height: calc(100% - 24px);
  display: flex;
  flex-direction: column;
  gap: 9px;
  pointer-events: auto;
}
.cabbage-resident-tasks {
  min-height: 0;
  flex: 1 1 auto;
}
.cabbage-resident-chat {
  height: clamp(340px, 52vh, 500px);
  min-height: 320px;
  flex: 0 1 auto;
  overflow: hidden;
}
@media (max-width: 720px) {
  .scene-quick-controls { width: min(330px, calc(100% - 24px)); }
  .dock-shortcut-bar { max-width: calc(100vw - 32px); overflow-x: auto; }
  .dock-shortcut-button { padding: 7px 8px; white-space: nowrap; }
  .cabbage-resident-stack { width: min(390px, calc(100% - 24px)); }
}
@media (max-height: 680px) {
  .cabbage-resident-stack { max-height: calc(100% - 16px); bottom: 8px; }
  .cabbage-resident-chat { height: clamp(270px, 48vh, 370px); min-height: 250px; }
}
</style>
