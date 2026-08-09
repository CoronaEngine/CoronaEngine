import { editorApi } from '../api/editorApi.js';

/** Compatibility facade for legacy project-settings panel imports. */
export const projectSettingsService = {
  getActiveProjectInfo: () => editorApi.projectSettings.getActiveProjectInfo(),
  saveActiveProjectInfo: (settings) => editorApi.projectSettings.saveActiveProjectInfo(settings),
  browseSceneFile: () => editorApi.projectSettings.browseSceneFile(),
};
