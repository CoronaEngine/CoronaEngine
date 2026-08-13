import SceneBar from '@/views/sidebar/SceneBar.vue';
import ObjectPanel from '@/views/sidebar/Object.vue';
import Pet from '@/views/tools/Pet.vue';
import LogView from '@/views/sidebar/LogView.vue';
import FileManager from '@/views/sidebar/FileManager.vue';
import ProjectSettings from '@/views/sidebar/ProjectSettings.vue';
import NodeGraphPanel from '@/views/sidebar/NodeGraphPanel.vue';
import CabbageChatPanel from '@/views/sidebar/CabbageChatPanel.vue';
import EditorSettings from '@/views/sidebar/EditorSettings.vue';
import LightFieldCalibrationPanel from '@/components/panels/LightFieldCalibrationPanel.vue';

/**
 * Vue component owner for panels declared by config/pluginManifest.js.
 *
 * Keep page-level component imports in the view composition layer. The keys
 * intentionally match the manifest IDs.
 */
export const PANEL_COMPONENTS = Object.freeze({
  SceneTools: SceneBar,
  LightFieldCalibration: LightFieldCalibrationPanel,
  Object: ObjectPanel,
  AITool: Pet,
  LogTool: LogView,
  FileManager,
  ProjectSettings,
  NodeGraphPanel,
  CabbageChatPanel,
  EditorSettings,
});

export function getPluginComponent(id) {
  return PANEL_COMPONENTS[id] ?? null;
}
