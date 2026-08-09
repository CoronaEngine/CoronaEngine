<template>
  <div class="dock-panel" :data-dock-panel-id="panelId">
    <div class="dock-panel-header" @pointerdown="onHeaderPointerDown">
      <span class="dock-panel-title" :title="panelTitle">{{ panelTitle }}</span>
      <div class="dock-panel-actions" @pointerdown.stop @mousedown.stop>
        <button
          type="button"
          class="dock-action-btn"
          :title="t('dock.popOut')"
          @click.stop.prevent="handlePopOut"
        >&#x29C9;</button>
        <button
          type="button"
          class="dock-action-btn dock-action-close"
          :title="t('dock.close')"
          @click.stop.prevent="handleClose"
        >&times;</button>
      </div>
    </div>
    <div class="dock-panel-body">
      <component :is="component" v-if="component" />
      <div v-else class="dock-panel-loading">{{ t('dock.componentMissing', { panelId }) }}</div>
    </div>
  </div>
</template>

<script setup>
import { computed, provide } from 'vue';
import { useI18n } from 'vue-i18n';
import { useDockStore } from '@/stores/dockStore.js';
import { getPluginManifest } from '@/config/pluginManifest.js';
import { appService } from '@/services/appService.js';

const props = defineProps({
  panelId: { type: String, required: true },
  component: { type: Object, default: null },
});

// 向下传递 panelId，子组件可通过 inject('dockPanelId') 获取
provide('dockPanelId', props.panelId);
provide('inDock', true);

const { t, locale } = useI18n();
const dockStore = useDockStore();
const manifest = computed(() => getPluginManifest(props.panelId));
const panelTitle = computed(() => {
  locale.value;
  const plugin = manifest.value;
  return plugin?.displayNameKey ? t(plugin.displayNameKey) : plugin?.displayName || props.panelId;
});

// ============================================================================
// 标题栏拖动重排 / 跨区（Pointer Events 自管，刻意不用 HTML5 原生 DnD）。
//
// 为什么用 Pointer Events 而非 draggable+dragstart：docked 面板内部可能嵌入 Blockly
// 等大量使用原生 HTML5 拖放的组件，原生 DnD 会与之共用同一套全局 drag 事件通道而互相
// 干扰。Pointer Events 走独立通道，作用域严格限定在 header，零冲突。全程在主窗口这一个
// CEF 页面 / 一个 JS 上下文内完成，不跨 CEF、不发 IPC、不碰 C++。
// ============================================================================

// 起拖阈值（px）：超过才视为拖动，否则当作普通点击（保留 header 上的点击语义）。
const DRAG_THRESHOLD = 5;

let drag = null; // { startX, startY, active, pointerId } | null

function onHeaderPointerDown(e) {
  // 只接管鼠标左键 / 单指触摸。
  if (e.button !== undefined && e.button !== 0) return;
  // 护栏：从按钮或任何 Blockly 元素上起的指针，绝不拦截（让其原生行为生效）。
  if (e.target.closest('.dock-panel-actions')) return;
  if (e.target.closest('[class*="blockly"]')) return;

  e.preventDefault();
  if (e.currentTarget?.setPointerCapture && e.pointerId !== undefined) {
    e.currentTarget.setPointerCapture(e.pointerId);
  }

  drag = {
    startX: e.clientX,
    startY: e.clientY,
    active: false,
    pointerId: e.pointerId,
    captureTarget: e.currentTarget,
  };
  window.addEventListener('pointermove', onPointerMove);
  window.addEventListener('pointerup', onPointerUp);
  window.addEventListener('pointercancel', onPointerUp);
}

function onPointerMove(e) {
  if (!drag) return;
  if (!drag.active) {
    const dx = e.clientX - drag.startX;
    const dy = e.clientY - drag.startY;
    if (dx * dx + dy * dy < DRAG_THRESHOLD * DRAG_THRESHOLD) return;
    // 越过阈值：正式进入拖动。
    drag.active = true;
    dockStore.setDraggingId(props.panelId);
  }
  // 实时高亮指针所在的放置区。
  const target = resolveDropTarget(e.clientX, e.clientY);
  dockStore.setDragOverZone(target ? target.zone : null);
}

function onPointerUp(e) {
  window.removeEventListener('pointermove', onPointerMove);
  window.removeEventListener('pointerup', onPointerUp);
  window.removeEventListener('pointercancel', onPointerUp);

  if (drag?.captureTarget?.releasePointerCapture && e.pointerId !== undefined) {
    try {
      drag.captureTarget.releasePointerCapture(e.pointerId);
    } catch {
      // The pointer may already have been released by the browser/CEF host.
    }
  }

  const wasActive = drag && drag.active;
  drag = null;

  if (!wasActive) {
    dockStore.setDraggingId(null);
    dockStore.setDragOverZone(null);
    return; // 未越过阈值：视为点击，不做任何移动。
  }

  const target = resolveDropTarget(e.clientX, e.clientY);
  if (target && target.zone) {
    dockStore.movePanel(props.panelId, target.zone, target.beforeId);
  }
  dockStore.setDraggingId(null);
  dockStore.setDragOverZone(null);
}

// 用 elementFromPoint 命中落点：找到目标 zone，并在 zone 内确定插入到哪个面板之前
// （指针位于某面板上半部 ⇒ 插其前；下半部 ⇒ 插其后）。返回 { zone, beforeId|null }。
function resolveDropTarget(x, y) {
  const el = document.elementFromPoint(x, y);
  if (!el) return null;

  const zoneEl = el.closest('[data-dock-zone]');
  if (!zoneEl) return null;
  const zone = zoneEl.getAttribute('data-dock-zone');

  const panelEl = el.closest('[data-dock-panel-id]');
  if (!panelEl) {
    // 落在 zone 空白处：追加到末尾。
    return { zone, beforeId: null };
  }
  const overId = panelEl.getAttribute('data-dock-panel-id');
  if (overId === props.panelId) {
    return { zone, beforeId: null }; // 落在自己身上：无操作意图。
  }
  // 上半 ⇒ 插在该面板之前；下半 ⇒ 插在其之后（即下一个面板之前）。
  const rect = panelEl.getBoundingClientRect();
  const isVerticalZone = zone === 'left' || zone === 'right';
  const before = isVerticalZone
    ? y < rect.top + rect.height / 2
    : x < rect.left + rect.width / 2;
  return { zone, beforeId: before ? overId : nextPanelId(zone, overId, props.panelId) };
}

// 返回 zone 内排在 afterId 之后的面板 id（用于“插在 afterId 之后”=“插在其后继之前”）。
function nextPanelId(zone, afterId, excludeId = null) {
  const list = dockStore.panelsByZone(zone)
    .map((p) => p.id)
    .filter((id) => id !== excludeId);
  const idx = list.indexOf(afterId);
  return idx >= 0 && idx + 1 < list.length ? list[idx + 1] : null;
}

function handleClose() {
  dockStore.closePanel(props.panelId);
}

async function handlePopOut() {
  const m = manifest.value;
  if (!m) return;
  try {
    const result = await appService.createDetachedPanel({
      panelId: props.panelId,
      routePath: '#' + (m.routePath || ''),
      width: m.defaultWidth || 400,
      height: m.defaultHeight || 600,
      x: 120,
      y: 120,
    });
    const tabId = result?.tab_id ?? result?.data?.tab_id;
    dockStore.setExternal(props.panelId, tabId);
  } catch (e) {
    console.error('[DockPanel] pop-out failed:', e);
  }
}
</script>

<style scoped>
.dock-panel {
  display: flex;
  flex-direction: column;
  overflow: hidden;
  flex: 1;
  min-height: 0;
  border: 1px solid #30281c;
  background: rgba(17, 16, 13, 0.72);
  box-shadow: 0 12px 32px rgba(0, 0, 0, 0.32);
  contain: layout style;
}
.dock-panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  min-height: 30px;
  padding: 3px 7px 3px 9px;
  background: linear-gradient(180deg, rgba(33, 29, 18, 0.9) 0%, rgba(21, 19, 13, 0.84) 100%);
  border-bottom: 1px solid #4a3d1d;
  flex-shrink: 0;
  user-select: none;
}
.dock-panel-title {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: #f2ead5;
  font-size: 12px;
  font-weight: 600;
}
.dock-panel-actions {
  display: flex;
  gap: 2px;
}
.dock-action-btn {
  background: transparent;
  border: none;
  color: #b9ad8f;
  cursor: pointer;
  font-size: 14px;
  padding: 0 4px;
  border-radius: 3px;
  line-height: 1;
}
.dock-action-btn:hover {
  background: #3f3018;
  color: #fff4cd;
}
.dock-action-close:hover {
  background: #c0392b;
  color: #fff;
}
.dock-panel-body {
  flex: 1;
  overflow: hidden;
  min-height: 0;
  display: flex;
  flex-direction: column;
  background: transparent;
}
.dock-panel-body > :deep(*) {
  background: transparent !important;
}
.dock-panel-loading {
  padding: 1rem;
  color: #ff6b6b;
}
</style>
