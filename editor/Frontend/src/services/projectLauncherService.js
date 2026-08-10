/** Project creation, opening, and legacy migration facade. */

import { editorApi } from '../api/editorApi.js';

export const projectLauncherService = {
  getDefaultProjectPath: () => editorApi.project.getDefaultProjectPath(),
  browseFolder: (default_path) => editorApi.project.browseFolder(default_path),
  choosePortableSceneTarget: () => editorApi.project.choosePortableSceneTarget(),
  validatePortableScene: (payload = {}) => editorApi.project.validatePortableScene(payload),
  importPortableAsset: (payload = {}) => editorApi.project.importPortableAsset(payload),
  cleanupPortableSceneAssets: (payload = {}) =>
    editorApi.project.cleanupPortableSceneAssets(payload),
  migrateLegacyScene: (payload) =>
    editorApi.project.migrateLegacyScene(payload).then((result) => {
      const migrated = result?.data ?? result;
      if (migrated?.ok && migrated?.path) {
        window.localStorage?.setItem('corona.activeProjectPath', migrated.path);
        window.localStorage?.setItem('corona.activeProjectLegacy', 'false');
      }
      return result;
    }),
  openProjectFile: () => editorApi.project.openProjectFile(),
  createProject: (projectData) => editorApi.project.createProject(projectData),
  createWorldProject: (worldData) => editorApi.project.createWorldProject(worldData),
  createMultiplayerProject: (projectData) =>
    editorApi.project.createMultiplayerProject(projectData),
  openProject: async (projectPath, options = {}) => {
    try {
      await window.__coronaNodeGraphFlushSave?.();
    } catch (error) {
      console.warn('切换项目之前保存节点图失败，继续打开目标项目:', error);
    }
    const loadPolicy = options.loadPolicy || options.load_policy || 'prompt';
    const result = await editorApi.project.openProject(projectPath, { load_policy: loadPolicy });
    const success = result?.data ?? result;
    const activeProjectPath = success?.path || projectPath;
    if (success?.ok && activeProjectPath) {
      window.localStorage?.setItem('corona.activeProjectPath', activeProjectPath);
      window.localStorage?.setItem('corona.activeProjectLegacy', success?.legacy ? 'true' : 'false');
      window.dispatchEvent(new CustomEvent('corona-active-project-changed', {
        detail: { projectPath: activeProjectPath },
      }));
    }
    return result;
  },
  setProjectMode: (mode, settings) => editorApi.project.setProjectMode(mode, settings),
  getAppVersion: () => editorApi.project.getAppVersion(),
  getProjectLoadStatus: () => editorApi.project.getProjectLoadStatus(),
  getRecentProjects: () => editorApi.project.getRecentProjects(),
};
