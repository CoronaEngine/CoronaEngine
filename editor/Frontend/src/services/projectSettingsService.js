import { editorApi } from '../api/editorApi.js';

/** Canonical project-settings service facade. */
export const projectSettingsService = {
  getActiveProjectInfo: () => editorApi.projectSettings.getActiveProjectInfo(),
  saveActiveProjectInfo: (settings) => editorApi.projectSettings.saveActiveProjectInfo(settings),
  browseSceneFile: () => editorApi.projectSettings.browseSceneFile(),
};
