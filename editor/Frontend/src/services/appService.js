/**
 * Canonical application/Dock service for Vue callers.
 *
 * The manifest transport remains owned by `src/api/editorApi.js`; this service
 * only assembles Dock/window commands and application lifecycle operations.
 */

import { Bridge, editorApi } from '../api/editorApi.js';

export const appService = {
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
    Bridge.callDockCommand({
      cmd: 'createPanelTab',
      panelId,
      routePath,
      width,
      height,
      dockingPos,
      zPriority,
    }),
  createDetachedPanel: ({ panelId, routePath, width, height, x, y }) =>
    Bridge.callDockCommand({ cmd: 'createDetachedPanel', panelId, routePath, width, height, x, y }),
  closeThisTab: (panelId) => Bridge.callDockCommand({ cmd: 'closeThisTab', panelId }),
  closePanelTab: (tabId, panelId) =>
    Bridge.callDockCommand({ cmd: 'closePanelTab', tabId, panelId }),
  detachPanel: (opts = {}) => Bridge.callDockCommand({ cmd: 'detachPanel', ...opts }),
  togglePanelWindowMode: (opts = {}) =>
    Bridge.callDockCommand({ cmd: 'togglePanelWindowMode', ...opts }),
  redockPanel: (opts = {}) => Bridge.callDockCommand({ cmd: 'redockPanel', ...opts }),
  toggleMaximizeThisCameraView: (sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'toggleMaximizeThisCameraView', sceneId, cameraId }),
  cycleThisCameraViewWindowMode: (sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'cycleThisCameraViewWindowMode', sceneId, cameraId }),
  toggleBorderlessThisCameraView: (sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'toggleBorderlessThisCameraView', sceneId, cameraId }),
  resizeThisCameraView: (width, height, sceneId = '', cameraId = '') =>
    Bridge.callDockCommand({ cmd: 'resizeThisCameraView', width, height, sceneId, cameraId }),
  createCameraView: (camera) =>
    Bridge.callDockCommand({
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
