<template>
  <div class="dock-root">
    <div class="dock-workspace">
      <section class="dock-viewport-region" :style="overlayStyles.viewport">
        <router-view />
      </section>

      <template v-if="leftVisible">
        <aside
          class="dock-zone dock-zone-side dock-zone-left"
          data-dock-zone="left"
          :class="{ 'dock-zone-dragover': dragOverZone === 'left', 'dock-zone-empty': leftPanels.length === 0 }"
          :style="overlayStyles.left"
        >
          <DockPanel
            v-for="p in leftPanels"
            :key="p.id"
            :panel-id="p.id"
            :component="getComponent(p.id)"
          />
          <div v-if="leftPanels.length === 0" class="dock-zone-placeholder">{{ t('dock.dropHere') }}</div>
        </aside>
        <div
          class="dock-sep dock-sep-v"
          :style="overlayStyles.leftSeparator"
          @mousedown="startResize('left', $event)"
        ></div>
      </template>

      <template v-if="bottomVisible">
        <div
          class="dock-sep dock-sep-h"
          :style="overlayStyles.bottomSeparator"
          @mousedown="startResize('bottom', $event)"
        ></div>
        <section
          class="dock-zone dock-zone-bottom"
          data-dock-zone="bottom"
          :class="{ 'dock-zone-dragover': dragOverZone === 'bottom', 'dock-zone-empty': bottomPanels.length === 0 }"
          :style="overlayStyles.bottom"
        >
          <div class="dock-bottom-row">
            <DockPanel
              v-for="p in bottomPanels"
              :key="p.id"
              :panel-id="p.id"
              :component="getComponent(p.id)"
            />
            <div v-if="bottomPanels.length === 0" class="dock-zone-placeholder">{{ t('dock.dropHere') }}</div>
          </div>
        </section>
      </template>

      <template v-if="rightVisible">
        <div
          class="dock-sep dock-sep-v"
          :style="overlayStyles.rightSeparator"
          @mousedown="startResize('right', $event)"
        ></div>
        <aside
          class="dock-zone dock-zone-side dock-zone-right"
          data-dock-zone="right"
          :class="{ 'dock-zone-dragover': dragOverZone === 'right', 'dock-zone-empty': rightPanels.length === 0 }"
          :style="overlayStyles.right"
        >
          <DockPanel
            v-for="p in rightPanels"
            :key="p.id"
            :panel-id="p.id"
            :component="getComponent(p.id)"
          />
          <div v-if="rightPanels.length === 0" class="dock-zone-placeholder">{{ t('dock.dropHere') }}</div>
        </aside>
      </template>
    </div>
  </div>
</template>

<script setup>
import { computed, ref, onMounted, onUnmounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { useDockStore } from '@/stores/dockStore.js';
import DockPanel from './DockPanel.vue';
import { createDockOverlayStyles } from './dockOverlayLayout.js';

const props = defineProps({
  componentResolver: { type: Function, default: null },
});

const { t } = useI18n();
const dockStore = useDockStore();

const leftPanels = computed(() => dockStore.panelsByZone('left'));
const rightPanels = computed(() => dockStore.panelsByZone('right'));
const bottomPanels = computed(() => dockStore.panelsByZone('bottom'));
const isDragging = computed(() => dockStore.draggingId !== null);
const dragOverZone = computed(() => dockStore.dragOverZone);
const leftVisible = computed(() => leftPanels.value.length > 0 || isDragging.value);
const rightVisible = computed(() => rightPanels.value.length > 0 || isDragging.value);
const bottomVisible = computed(() => bottomPanels.value.length > 0 || isDragging.value);

const leftWidth = ref(360);
const rightWidth = ref(400);
const bottomHeight = ref(320);

const MIN_SIDE = 260;
const MIN_CENTER = 520;
const MIN_BOTTOM = 180;
const MIN_VIEWPORT_HEIGHT = 260;
const DOCK_SEPARATOR_SIZE = 4;

const overlayStyles = computed(() => createDockOverlayStyles({
  leftVisible: leftVisible.value,
  rightVisible: rightVisible.value,
  leftWidth: leftWidth.value,
  rightWidth: rightWidth.value,
  bottomHeight: bottomHeight.value,
  separatorSize: DOCK_SEPARATOR_SIZE,
}));

function getComponent(panelId) {
  return props.componentResolver?.(panelId) ?? null;
}

function clampSideWidth(value, oppositeWidth) {
  const available = Math.max(window.innerWidth - oppositeWidth - MIN_CENTER - 8, MIN_SIDE);
  return Math.min(Math.max(value, MIN_SIDE), available);
}

function clampBottomHeight(value) {
  const available = Math.max(window.innerHeight - MIN_VIEWPORT_HEIGHT - 4, MIN_BOTTOM);
  return Math.min(Math.max(value, MIN_BOTTOM), available);
}

function clampLayout() {
  leftWidth.value = clampSideWidth(leftWidth.value, rightPanels.value.length ? rightWidth.value : 0);
  rightWidth.value = clampSideWidth(rightWidth.value, leftPanels.value.length ? leftWidth.value : 0);
  bottomHeight.value = clampBottomHeight(bottomHeight.value);
}

let resizing = null;

function startResize(zone, event) {
  resizing = { zone, startX: event.clientX, startY: event.clientY };
  document.body.classList.add(zone === 'bottom' ? 'dock-resizing-row' : 'dock-resizing-column');
  event.preventDefault();
}

function onMouseMove(event) {
  if (!resizing) return;
  const dx = event.clientX - resizing.startX;
  const dy = event.clientY - resizing.startY;

  if (resizing.zone === 'left') {
    leftWidth.value = clampSideWidth(leftWidth.value + dx, rightPanels.value.length ? rightWidth.value : 0);
  } else if (resizing.zone === 'right') {
    rightWidth.value = clampSideWidth(rightWidth.value - dx, leftPanels.value.length ? leftWidth.value : 0);
  } else if (resizing.zone === 'bottom') {
    bottomHeight.value = clampBottomHeight(bottomHeight.value - dy);
  }

  resizing.startX = event.clientX;
  resizing.startY = event.clientY;
}

function onMouseUp() {
  resizing = null;
  document.body.classList.remove('dock-resizing-row', 'dock-resizing-column');
}

onMounted(() => {
  clampLayout();
  window.addEventListener('resize', clampLayout);
  window.addEventListener('mousemove', onMouseMove);
  window.addEventListener('mouseup', onMouseUp);
});

onUnmounted(() => {
  document.body.classList.remove('dock-resizing-row', 'dock-resizing-column');
  window.removeEventListener('resize', clampLayout);
  window.removeEventListener('mousemove', onMouseMove);
  window.removeEventListener('mouseup', onMouseUp);
});
</script>

<style scoped>
.dock-root {
  width: 100vw;
  height: 100vh;
  overflow: hidden;
  background: transparent;
  contain: layout style;
}

.dock-workspace {
  position: relative;
  width: 100%;
  height: 100%;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
}

.dock-viewport-region {
  position: absolute;
  z-index: 0;
  background: transparent;
  display: flex;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
  contain: layout style;
}

.dock-zone {
  position: absolute;
  z-index: 20;
  min-width: 0;
  min-height: 0;
  overflow: hidden;
  background: transparent;
  contain: layout style;
}

.dock-zone-side {
  display: flex;
  flex-direction: column;
}

.dock-zone-left {
  border-right: 1px solid #30281c;
}

.dock-zone-right {
  border-left: 1px solid #30281c;
}

.dock-zone-bottom {
  border-top: 1px solid #30281c;
}

.dock-bottom-row {
  position: relative;
  display: flex;
  width: 100%;
  height: 100%;
  min-width: 0;
  min-height: 0;
}

.dock-zone-dragover {
  outline: 2px solid #d8b86c;
  outline-offset: -2px;
  background: #29220f;
}

.dock-zone-empty {
  min-width: 120px;
  min-height: 80px;
}

.dock-zone-placeholder {
  display: flex;
  flex: 1;
  align-items: center;
  justify-content: center;
  margin: 6px;
  border: 1px dashed #665420;
  border-radius: 5px;
  color: #a3936c;
  font-size: 12px;
  pointer-events: none;
}

.dock-sep {
  position: absolute;
  z-index: 30;
  flex: 0 0 auto;
  background: #30281c;
  transition: background-color 120ms ease;
}

.dock-sep::after {
  position: absolute;
  content: '';
}

.dock-sep-v {
  width: 4px;
  cursor: col-resize;
}

.dock-sep-v::after {
  inset: 0 -3px;
}

.dock-sep-h {
  height: 4px;
  cursor: row-resize;
}

.dock-sep-h::after {
  inset: -3px 0;
}

.dock-sep:hover {
  background: #b8924a;
}

:global(body.dock-resizing-column),
:global(body.dock-resizing-column *) {
  cursor: col-resize !important;
  user-select: none !important;
}

:global(body.dock-resizing-row),
:global(body.dock-resizing-row *) {
  cursor: row-resize !important;
  user-select: none !important;
}
</style>
