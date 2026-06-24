<template>
  <div class="dock-panel">
    <div class="dock-panel-header">
      <span class="dock-panel-title" :title="panelTitle">{{ panelTitle }}</span>
      <div class="dock-panel-actions">
        <button class="dock-action-btn" :title="t('dock.popOut')" @click="handlePopOut">&#x29C9;</button>
        <button class="dock-action-btn dock-action-close" :title="t('dock.close')" @click="handleClose">&times;</button>
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
import { getPluginDisplayName, getPluginManifest } from '@/config/pluginManifest.js';
import { appService } from '@/utils/bridge.js';

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
  return getPluginDisplayName(manifest.value) || props.panelId;
});

function handleClose() {
  dockStore.closePanel(props.panelId);
}

async function handlePopOut() {
  const m = manifest.value;
  if (!m) return;
  try {
    const result = await appService.createPanelTab(
      props.panelId,
      '#' + (m.routePath || ''),
      m.defaultWidth || 400,
      m.defaultHeight || 600,
      m.defaultFloatPosition || 'right_top'
    );
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
  border-bottom: 1px solid #3c3c3c;
  background: #1e1e1e;
  contain: layout style;
}
.dock-panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 2px 6px;
  background: #2d2d2d;
  border-bottom: 1px solid #3c3c3c;
  flex-shrink: 0;
  user-select: none;
}
.dock-panel-title {
  min-width: 0;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  color: #c0c0c0;
  font-size: 12px;
  font-weight: 500;
}
.dock-panel-actions {
  display: flex;
  gap: 2px;
}
.dock-action-btn {
  background: transparent;
  border: none;
  color: #909090;
  cursor: pointer;
  font-size: 14px;
  padding: 0 4px;
  border-radius: 3px;
  line-height: 1;
}
.dock-action-btn:hover {
  background: #3c3c3c;
  color: #e0e0e0;
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
}
.dock-panel-loading {
  padding: 1rem;
  color: #ff6b6b;
}
</style>
