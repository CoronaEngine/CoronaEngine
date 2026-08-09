/** Canonical Blockly and role-script runtime service facade. */

import { editorApi } from '../api/editorApi.js';

export const scriptingService = {
  executePythonCode: (code, mode, sceneName, actorName, targetType = 'actor') =>
    editorApi.scratch.executePythonCode(code, mode, sceneName, actorName, targetType),
  saveBlocklyTarget: (payload) => editorApi.scratch.saveBlocklyTarget(payload),
  loadBlocklyTarget: (payload) => editorApi.scratch.loadBlocklyTarget(payload),
  startGamePreview: (payload = { scope: 'project' }) => editorApi.scratch.startGamePreview(payload),
  stopGamePreview: () => editorApi.scratch.stopGamePreview(),
  getGamePreviewStatus: () => editorApi.scratch.getGamePreviewStatus(),
  stopScriptExecution: (restoreState = false) =>
    editorApi.scratch.stopScriptExecution(restoreState),
  getScriptStatus: () => editorApi.scratch.getScriptStatus(),
  sendKeyEvent: (key, modifiers, displayKey) =>
    editorApi.scratch.sendKeyEvent(key, modifiers, displayKey),
  sendKeyUpEvent: (key, displayKey) => editorApi.scratch.sendKeyUpEvent(key, displayKey),
  sendMouseEvent: (
    eventType,
    button,
    x,
    y,
    viewportX,
    viewportY,
    viewportWidth,
    viewportHeight,
    pickedActor = ''
  ) =>
    editorApi.scratch.sendMouseEvent(
      eventType,
      button,
      x,
      y,
      viewportX,
      viewportY,
      viewportWidth,
      viewportHeight,
      pickedActor
    ),
};
