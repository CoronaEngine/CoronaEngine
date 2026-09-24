/**
 * Canonical application/Dock service for Vue callers.
 *
 * The manifest transport remains owned by `src/api/editorApi.js`; this service
 * only assembles Dock/window commands and application lifecycle operations.
 */

import { Bridge, editorApi } from '../api/editorApi.js';
import { editorUiAllowed, worldModeState } from './worldModeService.js';

const surfaceSession = `${Date.now()}_${Math.random().toString(36).slice(2)}`;
let nativeUiGeneration = null;
export const isCurrentWindowEvent = (payload) => editorUiAllowed()
  && (payload?.uiGeneration === undefined || payload.uiGeneration === nativeUiGeneration);

async function openEditorWindow(command) {
  if (!editorUiAllowed()) throw new Error('当前世界不允许打开编辑器窗口');
  const revision = worldModeState.revision;
  const result = await Bridge.callDockCommand(command);
  if (!editorUiAllowed() || revision !== worldModeState.revision) {
    const tabId = result?.tab_id ?? result?.data?.tab_id;
    if (Number.isInteger(tabId)) {
      await Bridge.callDockCommand({ cmd: 'closePanelTab', tabId, panelId: command.panelId || '' });
    }
    throw new Error('世界已切换，已丢弃旧窗口请求');
  }
  return result;
}

export const appService = {
  readEditorUiPolicy: async () => {
    const result = await Bridge.callDockCommand({ cmd: 'getEditorUiPolicy' });
    nativeUiGeneration = result?.ui_generation ?? null;
    return result;
  },
  setEditorUiEnabled: async (enabled) => {
    const revision = worldModeState.revision;
    const result = await Bridge.callDockCommand({ cmd: 'setEditorUiEnabled', enabled,
      session: `${surfaceSession}:${revision}` });
    if (revision === worldModeState.revision) nativeUiGeneration = result?.ui_generation ?? null;
    return result;
  },
  setDragRegions: (_routePath, x, y, w, h) =>
    Bridge.callDockCommand({
      cmd: 'setDragRegions',
      tabId: null,
      regions: [{ x, y, w, h }],
    }),
  setCurrentTabDragRegions: (regions) =>
    Bridge.callDockCommand({
      cmd: 'setDragRegions',
      tabId: null,
      regions: Array.isArray(regions) ? regions : [],
    }),
  createPanelTab: (panelId, routePath, width, height, dockingPos, zPriority = 0) =>
    openEditorWindow({
      cmd: 'createPanelTab',
      panelId,
      routePath,
      width,
      height,
      dockingPos,
      zPriority,
    }),
  createDetachedPanel: ({ panelId, routePath, width, height, x, y }) =>
    openEditorWindow({ cmd: 'createDetachedPanel', panelId, routePath, width, height, x, y }),
  closeThisTab: (panelId) => Bridge.callDockCommand({ cmd: 'closeThisTab', panelId }),
  closePanelTab: (tabId, panelId) =>
    Bridge.callDockCommand({ cmd: 'closePanelTab', tabId, panelId }),
  detachPanel: (opts = {}) => openEditorWindow({ cmd: 'detachPanel', ...opts }),
  togglePanelWindowMode: (opts = {}) =>
    openEditorWindow({ cmd: 'togglePanelWindowMode', ...opts }),
  redockPanel: (opts = {}) => openEditorWindow({ cmd: 'redockPanel', ...opts }),
  toggleMaximizeThisCameraView: (sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'toggleMaximizeThisCameraView', sceneId, cameraId }),
  cycleThisCameraViewWindowMode: (sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'cycleThisCameraViewWindowMode', sceneId, cameraId }),
  toggleBorderlessThisCameraView: (sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'toggleBorderlessThisCameraView', sceneId, cameraId }),
  resizeThisCameraView: (width, height, sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'resizeThisCameraView', width, height, sceneId, cameraId }),
  createCameraView: (camera) =>
    openEditorWindow({
      cmd: 'createCameraView',
      sceneId: camera.scene_id,
      cameraId: camera.camera_id || camera.id,
      cameraHandle: camera.handle,
      routePath: `/CameraView?scene=${encodeURIComponent(camera.scene_id)}&camera=${encodeURIComponent(camera.camera_id || camera.id)}`,
      width: camera.view_width || 960,
      height: camera.view_height || 540,
      x: camera.view_x || 120,
      y: camera.view_y || 120,
    }),
  closeCameraView: (sceneId, cameraId) =>
    Bridge.callDockCommand({ cmd: 'closeCameraView', sceneId, cameraId }),
  suspendCameraViews: (sceneId) => Bridge.callDockCommand({ cmd: 'suspendCameraViews', sceneId }),
  crossTabBroadcast: (event, payload) =>
    Bridge.callDockCommand({ cmd: 'broadcast', event, payload }),
  closeProcess: () => editorApi.app.closeProcess(),
};
