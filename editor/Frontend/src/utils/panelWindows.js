import { appService } from '@/utils/bridge.js';
import { getPluginManifest, PLUGIN_MANIFEST } from '@/config/pluginManifest.js';

export const FLOATING_PANEL_IDS = ['SceneTools', 'SceneDatas', 'AITalkBar'];

const POSITION_WHITELIST = new Set(['right_top', 'right_bottom', 'left_bottom', 'center']);

export function isFloatingPanel(panelId) {
  return FLOATING_PANEL_IDS.includes(panelId);
}

export function floatingPanelManifests() {
  return PLUGIN_MANIFEST.filter(
    (panel) => panel.defaultOpenMode === 'external' && isFloatingPanel(panel.id)
  );
}

function normalizeFloatPosition(position) {
  return POSITION_WHITELIST.has(position) ? position : 'right_top';
}

export async function openFloatingPanel(dockStore, panelId) {
  const manifest = getPluginManifest(panelId);
  if (!manifest || !dockStore?.panels?.[panelId]) {
    console.error('[panelWindows] Unknown panel:', panelId);
    return false;
  }

  const panelState = dockStore.panels[panelId];
  if (panelState.open && panelState.mode === 'external' && panelState.externalTabId) {
    return true;
  }

  const routePath = `#${manifest.routePath || ''}`;
  const width = manifest.defaultWidth || 400;
  const height = manifest.defaultHeight || 600;
  const dockingPos = normalizeFloatPosition(manifest.defaultFloatPosition);

  try {
    const result = await appService.createPanelTab(panelId, routePath, width, height, dockingPos);
    const tabId = result?.tab_id ?? result?.data?.tab_id;
    if (!Number.isInteger(tabId)) {
      throw new Error(`Invalid external tab id for ${panelId}`);
    }
    dockStore.setExternal(panelId, tabId);
    return true;
  } catch (error) {
    console.error(`[panelWindows] Failed to open floating panel ${panelId}:`, error);
    return false;
  }
}
