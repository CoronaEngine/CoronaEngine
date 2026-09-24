/** Project creation, opening, and legacy migration facade. */

import { nextTick } from 'vue';
import { appService } from './appService.js';
import { editorApi } from '../api/editorApi.js';
import { worldModeService, worldModeState } from './worldModeService.js';
import { drainWorldSession, withWorldTimeout } from './worldSessionLifecycle.js';
import lanchat from '../stores/lanchat.js';

let projectOpenQueue = Promise.resolve();
let selectionVersion = 0;
let runtimeCleanupRequired = false;
const pendingWorldSaves = new Set();
const superseded = () => ({ ok: false, status: 'superseded' });
export const projectOpenResult = (result) => result?.data ?? result;
export const isProjectOpenSuperseded = (result) => projectOpenResult(result)?.status === 'superseded';

export function cancelPendingProjectOpen() {
  ++selectionVersion;
  const token = worldModeService.beginOpen();
  worldModeService.finishOpen(token);
}

function worldRuntimeCleanupError(kind, result) {
  const details = [result?.restoreError, result?.restore_error, result?.message, result?.error]
    .filter(value => typeof value === 'string' && value.trim());
  if (Array.isArray(result?.pendingThreads) && result.pendingThreads.length) {
    details.push(`未停止的线程：${result.pendingThreads.join('、')}`);
  }
  if (result?.threadAlive) details.push('脚本线程仍在运行');
  if (result?.snapshotCaptured) details.push('运行快照尚未恢复');
  if (result?.restoreStatus && !['idle', 'restored'].includes(result.restoreStatus)) {
    details.push(`恢复状态：${result.restoreStatus}`);
  }
  if (!details.length && result?.status) details.push(`运行状态：${result.status}`);
  const reason = [...new Set(details)].join('；');
  return new Error(`旧世界${kind}尚未停止或恢复失败，已取消切换世界。${reason ? ` 原因：${reason}` : ''}`);
}

// Finish restoring the OLD scene before native open replaces it. Never run a
// stop/restore against the new story scene, and fail closed if workers persist.
export async function stopWorldRuntimeBeforeOpen(api, {
  wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms)), attempts = 40,
} = {}) {
  const unwrap = (result) => result?.data ?? result;
  let preview = unwrap(await api.stopGamePreview());
  const busy = (status) => status?.workerActive || status?.stopPending || status?.stop_pending
    || status?.hasSnapshot || status?.has_snapshot
    || ['starting', 'running', 'stopping'].includes(status?.status);
  for (let i = 0; busy(preview) && i < attempts; i++) {
    await wait(200);
    preview = unwrap(await api.getGamePreviewStatus());
  }
  if (busy(preview) || preview?.restoreError || preview?.restore_error) {
    throw worldRuntimeCleanupError('预览', preview);
  }
  const script = unwrap(await api.stopScriptExecution(true));
  // restored=false is normal when no script ever ran or a prior stop already
  // consumed the snapshot. Only live work, retained snapshots, and real errors block opening.
  if (['error', 'starting', 'running', 'stopping'].includes(script?.status)
    || script?.threadAlive || script?.pendingThreads?.length || script?.snapshotCaptured
    || ['error', 'restoring'].includes(script?.restoreStatus) || script?.restoreError) {
    throw worldRuntimeCleanupError('脚本', script);
  }
}

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
  openProject: (projectPath, options = {}) => {
    // Selection ownership starts at submission, NOT when the native queue runs.
    const selection = ++selectionVersion;
    const current = () => selection === selectionVersion;
    runtimeCleanupRequired ||= worldModeState.mode === 'creative';
    const saveOldWorld = window.__coronaNodeGraphFlushSave;
    if (saveOldWorld) pendingWorldSaves.add(saveOldWorld);
    const token = worldModeService.beginOpen(projectPath);
    const operation = projectOpenQueue.catch(() => {}).then(async () => {
      if (!current()) return superseded();
      try {
        // Fail closed: never restore/save an old world after replacing its scene.
        for (const save of pendingWorldSaves) {
          await withWorldTimeout(save(), '保存旧世界');
          pendingWorldSaves.delete(save);
        }
        await nextTick();
        if (typeof window.coronaBridge?.dockCommand === 'function') {
          await appService.setEditorUiEnabled(false);
        }
        await drainWorldSession();
        await withWorldTimeout(lanchat.finishWorldSession(), '关闭旧世界聊天室');
        if (runtimeCleanupRequired) {
          await withWorldTimeout(stopWorldRuntimeBeforeOpen(editorApi.scratch), '停止旧世界运行');
          runtimeCleanupRequired = false;
        }
        if (!current()) return superseded();
        const loadPolicy = options.loadPolicy || options.load_policy || 'prompt';
        // Native scene replacement is serialized even if this selection becomes stale.
        const result = await editorApi.project.openProject(projectPath, { load_policy: loadPolicy });
        if (!current()) return superseded();
        const opened = projectOpenResult(result);
        const activeProjectPath = opened?.path || projectPath;
        if (!opened?.ok) {
          worldModeService.fail(new Error(opened?.message || '打开世界失败'));
          return result;
        }
        const session = await worldModeService.resolve(activeProjectPath, { force: true });
        if (!current() || !session) return superseded();
        window.localStorage?.setItem('corona.activeProjectPath', activeProjectPath);
        window.localStorage?.setItem('corona.activeProjectLegacy', opened?.legacy ? 'true' : 'false');
        window.dispatchEvent(new CustomEvent('corona-active-project-changed', {
          detail: { projectPath: activeProjectPath },
        }));
        return result;
      } catch (error) {
        if (!current()) return superseded();
        worldModeService.fail(error);
        throw error;
      } finally {
        worldModeService.finishOpen(token);
      }
    });
    projectOpenQueue = operation;
    return operation;
  },
  setProjectMode: (mode, settings) => editorApi.project.setProjectMode(mode, settings),
  getAppVersion: () => editorApi.project.getAppVersion(),
  getProjectLoadStatus: () => editorApi.project.getProjectLoadStatus(),
  getRecentProjects: () => editorApi.project.getRecentProjects(),
};
