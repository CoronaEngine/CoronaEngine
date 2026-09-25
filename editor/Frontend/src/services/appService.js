/**
 * Canonical application/Dock service for Vue callers.
 *
 * The manifest transport remains owned by `src/api/editorApi.js`; this service
 * only assembles Dock/window commands and application lifecycle operations.
 */

import { Bridge, editorApi } from '../api/editorApi.js';
import { editorUiAllowed, worldModeState } from './worldModeService.js';

import { createEditorWindowSession } from './editorWindowSession.js';

const windowSessions = new WeakMap();
function session() {
  let controller = windowSessions.get(window);
  if (!controller) {
    let owner = window.sessionStorage?.getItem('corona.editorUi.owner');
    if (!owner) {
      owner = `${Date.now()}_${Math.random().toString(36).slice(2)}`;
      window.sessionStorage?.setItem('corona.editorUi.owner', owner);
    }
    controller = createEditorWindowSession({
      owner,
      storage: window.localStorage,
      send: (...args) => Bridge.callDockCommand(...args),
      getWorld: () => worldModeState,
      getQuery: () => new URLSearchParams((window.location?.hash || '').split('?')[1] || ''),
    });
    windowSessions.set(window, controller);
  }
  return controller;
}
export const isCurrentWindowEvent = payload => editorUiAllowed() && session().accepts(payload);
const openEditorWindow = command => session().open(command);

export const appService = {
  readEditorUiPolicy: async () => session().policy(),
  setEditorUiEnabled: enabled => session().prepare(enabled),
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
  closeThisTab: (panelId) => session().closeThis(panelId),
  closePanelTab: (tabId, panelId) =>
    session().closeTab(tabId, panelId),
  detachPanel: (opts = {}) => openEditorWindow({ cmd: 'detachPanel', ...opts }),
  togglePanelWindowMode: (opts = {}) =>
    openEditorWindow({ cmd: 'togglePanelWindowMode', ...opts }),
  redockPanel: (opts = {}) => openEditorWindow({ cmd: 'redockPanel', ...opts }),
  toggleMaximizeThisCameraView: (sceneId = '', cameraId = '') =>
    openEditorWindow({ cmd: 'toggleMaximizeThisCameraView', sceneId, cameraId }),
  cycleThisCameraViewWindowMode: (sceneId = '', cameraId = '') =>
    openEditorWindow({ cmd: 'cycleThisCameraViewWindowMode', sceneId, cameraId }),
  toggleBorderlessThisCameraView: (sceneId = '', cameraId = '') =>
    openEditorWindow({ cmd: 'toggleBorderlessThisCameraView', sceneId, cameraId }),
  resizeThisCameraView: (width, height, sceneId = '', cameraId = '') =>
    openEditorWindow({ cmd: 'resizeThisCameraView', width, height, sceneId, cameraId }),
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
    openEditorWindow({ cmd: 'closeCameraView', sceneId, cameraId }),
  suspendCameraViews: (sceneId) => Bridge.callDockCommand({ cmd: 'suspendCameraViews', sceneId }),
  crossTabBroadcast: (event, payload) =>
    session().broadcast(event, payload),
  closeProcess: () => editorApi.app.closeProcess(),
};
