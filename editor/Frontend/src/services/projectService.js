/** Canonical project and main-view service facade. */

import { Bridge, editorApi } from '../api/editorApi.js';

export const projectService = {
  OnInit: (projectPath = window.localStorage?.getItem('corona.activeProjectPath') || '') =>
    editorApi.main.onInit(projectPath),
  importResourceFileByDialog: (sceneName, fileType) =>
    editorApi.main.importResourceFile(sceneName, fileType),
  sceneSave: (sceneName) => editorApi.main.sceneSave(sceneName),
  getMenuData: () => editorApi.main.getMenuData(),
  updateViewToolState: (toolId, enabled) => editorApi.main.updateViewToolState(toolId, enabled),
  runProject: (scenePath) => editorApi.main.runProject(scenePath),
  setDragRegions: (_routePath, x, y, w, h) =>
    Bridge.callDockCommand({ cmd: 'setDragRegions', tabId: null, regions: [{ x, y, w, h }] }),
  setCurrentTabDragRegions: (regions) =>
    Bridge.callDockCommand({
      cmd: 'setDragRegions',
      tabId: null,
      regions: Array.isArray(regions) ? regions : [],
    }),
};
