import { inject } from 'vue';
import { useDockStore } from '@/stores/dockStore.js';
import { appService } from '@/services/appService.js';

/**
 * 面板组件在使用时的 composable
 * dock 模式（DockPanel 内）：isDocked=true，closePanel 关闭 dock 面板
 * standalone 模式（独立 CEF Tab）：isDocked=false，closePanel 调用 C++ close-this-tab 关闭浮窗
 */
export function useDockPanel() {
  const dockPanelId = inject('dockPanelId', null);
  const inDock = inject('inDock', false);
  const dockStore = inDock ? useDockStore() : null;

  function closePanel() {
    if (inDock && dockPanelId && dockStore) {
      dockStore.closePanel(dockPanelId);
    } else if (!inDock) {
      // 独立浮窗 Tab：通知 C++ 关闭自身并广播 panel-closed
      const id = dockPanelId || getPanelIdFromRoute();
      if (id) {
        appService.closeThisTab(id).catch(() => {});
      }
    }
  }

  return {
    isDocked: inDock,
    dockPanelId,
    closePanel,
  };
}

function getPanelIdFromRoute() {
  const hash = window.location.hash || '';
  const path = hash.replace('#', '').split('?')[0];
  const map = {
    '/SceneBar': 'SceneTools',
    '/Object': 'Object',
    '/Pet': 'AITool',
    '/LogView': 'LogTool',
    '/FileManager': 'FileManager',
    '/ProjectSettings': 'ProjectSettings',
    '/ScratchTool': 'ScratchTool',
    '/NodeGraph': 'NodeGraphPanel',
    '/CabbageChat': 'CabbageChatPanel',
    '/Network': 'Network',
    '/SetUp': 'EditorSettings',
  };
  return map[path] || null;
}
