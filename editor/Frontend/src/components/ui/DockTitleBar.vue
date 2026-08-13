<template>
  <div
    ref="titleBarRef"
    class="titlebar"
    :class="extraClass"
    style="touch-action: none; -webkit-user-select: none; user-select: none"
  >
    <div class="titlebar-accent"></div>
    <div class="titlebar-title">{{ title }}</div>
    <div class="titlebar-actions">
      <slot name="actions"></slot>

      <div class="titlebar-button-group">
        <button
          v-if="showFloatToggle"
          type="button"
          :title="t('dock.toggleFloat')"
          class="titlebar-button"
          @pointerdown.stop
          @mousedown.stop
          @click.stop.prevent="onToggleFloat"
        >
          ⤢
        </button>

        <button
          type="button"
          :title="t('dock.close')"
          class="titlebar-button titlebar-close"
          @pointerdown.stop
          @mousedown.stop
          @click.stop.prevent="onClose"
        >
          ×
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { onMounted, onUnmounted, ref, defineProps, defineEmits } from 'vue';
import { useI18n } from 'vue-i18n';
import { appService } from '@/services/appService.js';

const props = defineProps({
  title: { type: String, default: '' },
  extraClass: { type: String, default: '' },
  // 默认显示浮动切换按钮：DockTitleBar 仅在 standalone 模式渲染（独立 CEF Tab），
  // 此时点击可把面板 detach 成独立无边框 OS 窗口、或从独立窗口收回主窗口。
  showFloatToggle: { type: Boolean, default: true },
  // 必须传入当前页面的 routePath，以便后端 Python 查找对应的 tab-Id
  routePath: { type: String, required: true },
});

const emit = defineEmits(['close', 'toggleFloat']);
const { t } = useI18n();

// DOM 引用
const titleBarRef = ref(null);

// ============================================================================
// 内部坐标同步逻辑 (原 regionReporter.js 内容合并)
// ============================================================================

// 共享的节流函数，防止 Resize 期间发送请求过快
const THROTTLE_MS = 16; // 约 60fps

function throttle(fn, delay) {
  let lastCall = 0;
  return function (...args) {
    const now = new Date().getTime();
    if (now - lastCall < delay) return;
    lastCall = now;
    return fn(...args);
  };
}

// 实际发送坐标的函数
const sendRegionsToNative = (routePath, element) => {
  if (!routePath || !element) return;

  // 获取相对于 CEF 视口的坐标
  const rect = element.getBoundingClientRect();

  // 转换成后端需要的整数 (x, y, w, h)
  // 假设整个 TitleBar 区域都是可拖拽区域
  const x = Math.floor(rect.x);
  const y = Math.floor(rect.y);
  const w = Math.floor(rect.width);
  const h = Math.floor(rect.height);

  appService.setDragRegions(routePath, x, y, w, h);
};

// 创建节流版本的发送函数
const throttledSend = throttle(sendRegionsToNative, THROTTLE_MS);

// 用于监听 DOM 尺寸变化的 Observer
let resizeObserver = null;

// ============================================================================
// 生命周期钩子
// ============================================================================

onMounted(() => {
  if (titleBarRef.value && props.routePath) {
    // 1. 初始化时发送一次坐标
    sendRegionsToNative(props.routePath, titleBarRef.value);

    // 2. 绑定 ResizeObserver，监听后续变化（如窗口缩放、布局调整）
    resizeObserver = new ResizeObserver(() => {
      // 变化时调用节流发送
      throttledSend(props.routePath, titleBarRef.value);
    });
    resizeObserver.observe(titleBarRef.value);
  } else {
    console.error('[DockTitleBar] Missing titleBarRef or routePath prop.');
  }
});

onUnmounted(() => {
  // 资源清理，防止内存泄漏
  if (resizeObserver) {
    resizeObserver.disconnect();
    resizeObserver = null;
  }
});

// ============================================================================
// 按钮事件处理
// ============================================================================

// routePath -> panelId（与 useDockPanel.getPanelIdFromRoute 保持一致）。redock 需要把
// panelId 带进 panel-closed 广播，主窗口据此用 dockStore.popIn 恢复对应 DOM 面板。
const ROUTE_TO_PANEL_ID = {
  '/SceneBar': 'SceneTools',
  '/Object': 'Object',
  '/Pet': 'AITool',
  '/LogView': 'LogTool',
  '/FileManager': 'FileManager',
  '/ProjectSettings': 'ProjectSettings',
  '/ScratchTool': 'ScratchTool',
  '/NodeGraph': 'NodeGraphPanel',
  '/CabbageChat': 'CabbageChatPanel',
  '/SetUp': 'EditorSettings',
};

function panelIdFromRoute() {
  const path = (props.routePath || '').split('?')[0];
  return ROUTE_TO_PANEL_ID[path] || null;
}

async function onToggleFloat() {
  const panelId = panelIdFromRoute();
  if (!panelId) return;
  try {
    // createPanelTab starts as a draggable floating surface inside the main editor.
    // Native code keeps the same tab alive while toggling it between that surface and
    // a detachable OS window, so the panel state and editor contents are preserved.
    await appService.togglePanelWindowMode({ panelId });
  } catch (e) {
    console.error('[DockTitleBar] toggle window mode failed:', e);
  }
  emit('toggleFloat');
}

function onClose() {
  emit('close');
}
</script>

<style scoped>
.titlebar {
  position: relative;
  display: flex;
  align-items: center;
  width: 100%;
  min-height: 32px;
  padding: 0 6px 0 10px;
  border-bottom: 1px solid rgba(216, 184, 108, 0.28);
  background: linear-gradient(180deg, rgba(35, 30, 17, 0.98) 0%, rgba(14, 13, 9, 0.98) 100%);
  color: #f2ead5;
  cursor: default;
  user-select: none;
}

.titlebar-accent {
  width: 3px;
  height: 16px;
  margin-right: 8px;
  border-radius: 999px;
  background: #d8b86c;
  box-shadow: 0 0 10px rgba(216, 184, 108, 0.28);
  flex: 0 0 auto;
}

.titlebar-title {
  min-width: 0;
  flex: 1;
  overflow: hidden;
  white-space: nowrap;
  text-overflow: ellipsis;
  font-size: 12px;
  font-weight: 600;
  line-height: 1;
}

.titlebar-actions,
.titlebar-button-group {
  display: flex;
  align-items: center;
  gap: 4px;
  flex: 0 0 auto;
}

.titlebar-button {
  width: 22px;
  height: 22px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 4px;
  background: rgba(255, 255, 255, 0.06);
  color: #d7cba9;
  font-size: 13px;
  line-height: 1;
  transition:
    background-color 140ms ease,
    border-color 140ms ease,
    color 140ms ease,
    transform 140ms ease;
}

.titlebar-button:hover {
  background: rgba(216, 184, 108, 0.18);
  border-color: rgba(216, 184, 108, 0.36);
  color: #ffffff;
}

.titlebar-button:active {
  transform: translateY(1px);
}

.titlebar-button:focus-visible {
  outline: 2px solid rgba(216, 184, 108, 0.7);
  outline-offset: 1px;
}

.titlebar-close:hover {
  background: rgba(178, 92, 78, 0.28);
  border-color: rgba(203, 116, 98, 0.5);
}
</style>
