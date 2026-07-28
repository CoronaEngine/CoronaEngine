/**
 * Bridge Utility for QWebChannel
 * 封装了与 C++ Editor API 的通信，支持 Promise 调用
 */

let editorApiMethodSpecs = null;
let editorApiManifestPromise = null;
let editorApiEventSpecs = null;
let editorApiEventManifestPromise = null;
const EDITOR_API_CALLER_CEF = 1;
export class Bridge {
  static async ensureEditorApiManifest() {
    if (!editorApiMethodSpecs) {
      if (!editorApiManifestPromise) {
        editorApiManifestPromise = call_editor_api('EditorApi.list_methods', [])
          .then((response) => {
            const methods = response?.data?.methods ?? response?.methods ?? [];
            editorApiMethodSpecs = new Map(
              methods
                .filter((method) => typeof method?.api === 'string')
                .map((method) => [method.api, method]),
            );
            Bridge.validateEditorApiWrapperMethods();
            return editorApiMethodSpecs;
          })
          .finally(() => {
            editorApiManifestPromise = null;
          });
      }
      await editorApiManifestPromise;
    }
    return editorApiMethodSpecs;
  }

  static async ensureEditorApiMethod(apiName) {
    if (apiName === 'EditorApi.list_methods') {
      return null;
    }
    await Bridge.ensureEditorApiManifest();
    const spec = editorApiMethodSpecs?.get(apiName);
    if (!spec) {
      throw new Error(`Editor API method is not defined by C++ manifest: ${apiName}`);
    }
    return spec;
  }

  static editorApiValueMatchesType(value, type) {
    if (type === 'any') return true;
    if (type === 'null') return value === null;
    if (type === 'boolean') return typeof value === 'boolean';
    if (type === 'integer') return Number.isInteger(value);
    if (type === 'number') return typeof value === 'number' && Number.isFinite(value);
    if (type === 'string') return typeof value === 'string';
    if (type === 'object') return value !== null && typeof value === 'object' && !Array.isArray(value);
    if (type === 'array') return Array.isArray(value);
    return false;
  }

  static resolveEditorApiWrapperPath(wrapperPath) {
    if (!wrapperPath || typeof wrapperPath !== 'string') return null;
    return wrapperPath
      .split('.')
      .reduce((current, segment) => (current ? current[segment] : null), editorApi);
  }

  static validateEditorApiWrapperMethods() {
    if (!editorApiMethodSpecs) return;
    const missing = [];
    for (const spec of editorApiMethodSpecs.values()) {
      const wrapperPath = spec?.js_wrapper;
      if (!wrapperPath) continue;
      if (typeof Bridge.resolveEditorApiWrapperPath(wrapperPath) !== 'function') {
        missing.push(`${spec.api} -> ${wrapperPath}`);
      }
    }
    if (missing.length > 0) {
      throw new Error(`Frontend wrapper path is not implemented: ${missing.join(', ')}`);
    }
  }

  static validateEditorApiEventWrapperMethods() {
    if (!editorApiEventSpecs) return;
    const missing = [];
    for (const spec of editorApiEventSpecs.values()) {
      const wrapperPath = spec?.js_wrapper;
      if (!wrapperPath) continue;
      if (typeof Bridge.resolveEditorApiWrapperPath(wrapperPath) !== 'function') {
        missing.push(`${spec.event} -> ${wrapperPath}`);
      }
    }
    if (missing.length > 0) {
      throw new Error(`Frontend event wrapper path is not implemented: ${missing.join(', ')}`);
    }
  }

  static validateEditorApiWrapperPath(apiName, spec, wrapperPath) {
    if (spec?.js_wrapper && spec.js_wrapper !== wrapperPath) {
      throw new Error(`Frontend wrapper path is not defined by C++ manifest: ${wrapperPath} for ${apiName}`);
    }
  }

  static validateEditorApiCaller(apiName, spec, callerMask, callerName) {
    const allowedCallers = Number(spec?.allowed_callers ?? 0);
    if ((allowedCallers & callerMask) === 0) {
      throw new Error(`Editor API caller is not allowed by C++ manifest: ${callerName} cannot call ${apiName}`);
    }
  }

  static async validateEditorApiArgs(apiName, args) {
    const spec = await Bridge.ensureEditorApiMethod(apiName);
    if (!spec) return null;
    Bridge.validateEditorApiCaller(apiName, spec, EDITOR_API_CALLER_CEF, 'CEF');
    if (!Array.isArray(args)) {
      throw new Error(`Editor API argument schema mismatch for ${apiName}: args must be an array`);
    }
    const params = Array.isArray(spec.params) ? spec.params : [];
    if (args.length > params.length) {
      throw new Error(`Editor API argument schema mismatch for ${apiName}: too many arguments`);
    }
    for (let index = 0; index < params.length; index += 1) {
      const param = params[index] || {};
      const value = args[index];
      if (index >= args.length || value === undefined || value === null) {
        if (param.optional) continue;
        throw new Error(`Editor API argument schema mismatch for ${apiName}: missing ${param.name || index}`);
      }
      if (!Bridge.editorApiValueMatchesType(value, param.type)) {
        throw new Error(`Editor API argument schema mismatch for ${apiName}: ${param.name || index} must be ${param.type}`);
      }
    }
    return spec;
  }

  static validateEditorApiReturn(apiName, data, spec) {
    if (!spec) return;
    const returnType = spec.return || 'any';
    if (!Bridge.editorApiValueMatchesType(data, returnType)) {
      throw new Error(`Editor API return schema mismatch for ${apiName}: data must be ${returnType}`);
    }
  }

  static async ensureEditorApiEventManifest() {
    if (!editorApiEventSpecs) {
      if (!editorApiEventManifestPromise) {
        editorApiEventManifestPromise = call_editor_api('EditorApi.list_events', [])
          .then((response) => {
            const events = response?.data?.events ?? response?.events ?? [];
            editorApiEventSpecs = new Map(
              events
                .filter((event) => typeof event?.event === 'string')
                .map((event) => [event.event, event]),
            );
            Bridge.validateEditorApiEventWrapperMethods();
            return editorApiEventSpecs;
          })
          .finally(() => {
            editorApiEventManifestPromise = null;
          });
      }
      await editorApiEventManifestPromise;
    }
    return editorApiEventSpecs;
  }

  static async ensureEditorApiEvent(eventName) {
    await Bridge.ensureEditorApiEventManifest();
    const eventSpec = editorApiEventSpecs?.get(eventName);
    if (!eventSpec) {
      throw new Error(`Editor API event is not defined by C++ manifest: ${eventName}`);
    }
    Bridge.validateEditorApiCaller(eventName, eventSpec, EDITOR_API_CALLER_CEF, 'CEF');
    return eventSpec;
  }

  static validateEditorApiEventPayload(eventName, payload, eventSpec) {
    if (!eventSpec) return;
    const payloadType = eventSpec.payload || 'any';
    if (!Bridge.editorApiValueMatchesType(payload, payloadType)) {
      throw new Error(`Editor API event payload schema mismatch for ${eventName}: payload must be ${payloadType}`);
    }
  }

  static async callDockCommand(params) {
    const requestId = `dock_${Date.now()}_${Math.random().toString(36).slice(2)}`;
    const payload = {
      ...params,
      requestId,
    };

    return new Promise((resolve, reject) => {
      if (!window.coronaBridge || typeof window.coronaBridge.dockCommand !== 'function') {
        reject(new Error('coronaBridge.dockCommand is unavailable'));
        return;
      }

      const previousCallback = window.__dockCallback;
      window.__dockCallback = (id, error, result) => {
        if (id !== requestId) {
          if (typeof previousCallback === 'function') {
            previousCallback(id, error, result);
          }
          return;
        }

        window.__dockCallback = previousCallback;
        if (error) {
          reject(new Error(error.message || String(error)));
        } else {
          resolve(result);
        }
      };

      try {
        window.coronaBridge.dockCommand(JSON.stringify(payload));
      } catch (error) {
        window.__dockCallback = previousCallback;
        reject(error);
      }
    });
  }
}

const call_editor_api = async (apiName, args) => {
  const normalizedArgs = args || [];
  const spec = await Bridge.validateEditorApiArgs(apiName, normalizedArgs);
  const request = {
    api: apiName,
    args: normalizedArgs,
  };

  return new Promise((resolve, reject) => {
    try {
      window.cefQuery({
        request: JSON.stringify(request),
        persistent: false,
        onSuccess: (response) => {
          try {
            const jsonResponse = typeof response === 'string' ? JSON.parse(response) : response;
            if (
              jsonResponse &&
              (jsonResponse.success === false ||
                jsonResponse.status === 'error' ||
                jsonResponse.type === 'error' ||
                jsonResponse.error)
            ) {
              reject(new Error(jsonResponse.error || jsonResponse.message || 'Editor API error'));
            } else {
              Bridge.validateEditorApiReturn(apiName, jsonResponse?.data, spec);
              resolve(jsonResponse);
            }
          } catch (e) {
            reject(e);
          }
        },
        onFailure: (error_code, error_message) => {
          reject(new Error(`Editor API Error (${error_code}): ${error_message}`));
        },
      });
    } catch (error) {
      reject(error);
    }
  });
};

const call_typed_editor_api = async (apiName, wrapperPath, args) => {
  const spec = await Bridge.ensureEditorApiMethod(apiName);
  Bridge.validateEditorApiWrapperPath(apiName, spec, wrapperPath);
  return call_editor_api(apiName, args);
};

const editorApiCallbacks = new Map();

if (typeof window !== 'undefined') {
  window.__coronaEditorApiDispatch = (event) => {
    const envelope = typeof event === 'string' ? JSON.parse(event) : event;
    const token = envelope?.token ?? envelope?.callback_token;
    const entry = editorApiCallbacks.get(token);
    const callback = entry?.callback;
    if (typeof callback === 'function') {
      Bridge.validateEditorApiEventPayload(envelope?.event, envelope?.payload, entry.eventSpec);
      callback(envelope?.payload, envelope?.event);
    }
  };
}

const register_editor_api_callback = async (eventName, callback) => {
  const eventSpec = await Bridge.ensureEditorApiEvent(eventName);
  const response = await call_editor_api('EditorApi.register_callback', [
    eventName,
    { transport: 'cef-js' },
  ]);
  const callbackToken = response?.data?.callback_token ?? response?.callback_token;
  if (!callbackToken) {
    throw new Error(`Editor API event registration failed: ${eventName}`);
  }
  editorApiCallbacks.set(callbackToken, { callback, eventName, eventSpec });
  return callbackToken;
};

const register_typed_editor_api_callback = async (eventName, wrapperName, callback) => {
  const eventSpec = await Bridge.ensureEditorApiEvent(eventName);
  if (eventSpec?.js_wrapper !== wrapperName) {
    throw new Error(`Editor API event wrapper is not defined by C++ manifest: ${wrapperName}`);
  }
  return register_editor_api_callback(eventName, callback);
};

const unregister_callback = async (callbackToken) => {
  return call_editor_api('EditorApi.unregister_callback', [callbackToken])
    .then((response) => {
      editorApiCallbacks.delete(callbackToken);
      return response;
    });
};

const find_editor_api_method_by_js_wrapper = async (wrapperPath) => {
  const specs = await Bridge.ensureEditorApiManifest();
  for (const spec of specs.values()) {
    if (spec?.js_wrapper === wrapperPath) {
      return spec;
    }
  }
  return null;
};

const find_editor_api_event_by_js_wrapper = async (wrapperPath) => {
  const specs = await Bridge.ensureEditorApiEventManifest();
  for (const spec of specs.values()) {
    if (spec?.js_wrapper === wrapperPath) {
      return spec;
    }
  }
  return null;
};

const register_manifest_editor_api_callback = async (wrapperPath, callback) => {
  const eventSpec = await find_editor_api_event_by_js_wrapper(wrapperPath);
  if (!eventSpec?.event) {
    throw new Error(`Editor API event wrapper path is not defined by C++ manifest: ${wrapperPath}`);
  }
  return register_typed_editor_api_callback(eventSpec.event, wrapperPath, callback);
};

const call_manifest_editor_api = async (wrapperPath, args) => {
  const methodSpec = await find_editor_api_method_by_js_wrapper(wrapperPath);
  if (!methodSpec?.api) {
    throw new Error(`Editor API wrapper path is not defined by C++ manifest: ${wrapperPath}`);
  }
  return call_typed_editor_api(methodSpec.api, wrapperPath, args || []);
};

const create_dynamic_editor_api_function = (wrapperPath) => async (...args) => {
  const methodSpec = await find_editor_api_method_by_js_wrapper(wrapperPath);
  if (methodSpec?.api) {
    return call_typed_editor_api(methodSpec.api, wrapperPath, args);
  }
  const eventSpec = await find_editor_api_event_by_js_wrapper(wrapperPath);
  if (eventSpec?.event) {
    return register_typed_editor_api_callback(eventSpec.event, wrapperPath, args[0]);
  }
  throw new Error(`Editor API wrapper path is not defined by C++ manifest: ${wrapperPath}`);
};

const create_dynamic_editor_api_namespace = (wrapperPath, target = {}) => {
  return new Proxy(target, {
    get(current, property) {
      if (typeof property !== 'string') {
        return current[property];
      }
      if (property === 'then') {
        return undefined;
      }
      if (property in current) {
        return current[property];
      }
      const childPath = wrapperPath ? `${wrapperPath}.${property}` : property;
      const dynamicMember = create_dynamic_editor_api_namespace(
        childPath,
        create_dynamic_editor_api_function(childPath),
      );
      current[property] = dynamicMember;
      return dynamicMember;
    },
    apply(current, thisArg, args) {
      return current.apply(thisArg, args);
    },
  });
};

const editorApiStatic = {
  listMethods: () => call_editor_api('EditorApi.list_methods', []),
  listEvents: () => call_editor_api('EditorApi.list_events', []),
  off: (callbackToken) => unregister_callback(callbackToken),
  editor: {
    listMethods: () => call_editor_api('EditorApi.list_methods', []),
    listEvents: () => call_editor_api('EditorApi.list_events', []),
    registerCallback: (eventName, callbackSpec = {}) =>
      call_editor_api('EditorApi.register_callback', [eventName, callbackSpec || {}]),
    unregisterCallback: (callbackToken) => unregister_callback(callbackToken),
  },
  events: {
    onAiChunk: (callback) => register_manifest_editor_api_callback('events.onAiChunk', callback),
    onLogBatch: (callback) => register_manifest_editor_api_callback('events.onLogBatch', callback),
    onActorChanged: (callback) => register_manifest_editor_api_callback('events.onActorChanged', callback),
    onActorSelectionChanged: (callback) => register_manifest_editor_api_callback('events.onActorSelectionChanged', callback),
    onActorTransformUpdated: (callback) => register_manifest_editor_api_callback('events.onActorTransformUpdated', callback),
    onActorPickResult: (callback) => register_manifest_editor_api_callback('events.onActorPickResult', callback),
    onFocusPoseResult: (callback) => register_manifest_editor_api_callback('events.onFocusPoseResult', callback),
    onNetworkActorDeleteSyncBroadcastRequested: (callback) => register_manifest_editor_api_callback('events.onNetworkActorDeleteSyncBroadcastRequested', callback),
    onNetworkActorOwnershipClaimed: (callback) => register_manifest_editor_api_callback('events.onNetworkActorOwnershipClaimed', callback),
    onNetworkActorStateSyncBroadcastRequested: (callback) => register_manifest_editor_api_callback('events.onNetworkActorStateSyncBroadcastRequested', callback),
    onNetworkActorSyncBroadcastRequested: (callback) => register_manifest_editor_api_callback('events.onNetworkActorSyncBroadcastRequested', callback),
    onNetworkActorTransformSyncBroadcastRequested: (callback) => register_manifest_editor_api_callback('events.onNetworkActorTransformSyncBroadcastRequested', callback),
    onNetworkAssetImportCompleted: (callback) => register_manifest_editor_api_callback('events.onNetworkAssetImportCompleted', callback),
    onNetworkFileSyncStatusChanged: (callback) => register_manifest_editor_api_callback('events.onNetworkFileSyncStatusChanged', callback),
    onNetworkSyncPauseRequested: (callback) => register_manifest_editor_api_callback('events.onNetworkSyncPauseRequested', callback),
    onSceneAdded: (callback) => register_manifest_editor_api_callback('events.onSceneAdded', callback),
    onSceneRenamed: (callback) => register_manifest_editor_api_callback('events.onSceneRenamed', callback),
    onSceneTreeChanged: (callback) => register_manifest_editor_api_callback('events.onSceneTreeChanged', callback),
    onProjectOpened: (callback) => register_manifest_editor_api_callback('events.onProjectOpened', callback),
    onLanChatEvent: (callback) => register_manifest_editor_api_callback('events.onLanChatEvent', callback),
  },
  app: {
    closeProcess: () => call_manifest_editor_api('app.closeProcess', []),
  },
  ai: {
    sendMessageToAIStream: (payload) => call_manifest_editor_api('ai.sendMessageToAIStream', [payload]),
    readLocalFileAsBase64: (filePath) => call_manifest_editor_api('ai.readLocalFileAsBase64', [filePath]),
    generateHint: (elementType, context = {}) =>
      call_manifest_editor_api('ai.generateHint', [elementType, context || {}]),
    submitRequest: (payload) => call_manifest_editor_api('ai.submitRequest', [payload || {}]),
    chatStream: (request) => editorApi.ai.submitRequest(request || {}),
    cancelRequest: (requestId) =>
      editorApi.ai.submitRequest(
        {
          operation: 'request.cancel',
          request_id: requestId,
        },
      ),
    getRequestStatus: (requestId) =>
      editorApi.ai.submitRequest(
        {
          operation: 'request.status',
          request_id: requestId,
        },
      ),
  },
  files: {
    getProjectInfo: () => call_manifest_editor_api('files.getProjectInfo', []),
    getFiles: (relPath = '') => call_manifest_editor_api('files.getFiles', [relPath || '']),
    getFileTree: (relPath = '') => call_manifest_editor_api('files.getFileTree', [relPath || '']),
    createFolder: (path, folderName) =>
      call_manifest_editor_api('files.createFolder', [path, folderName]),
    createFile: (path, fileName, type) =>
      call_manifest_editor_api('files.createFile', [path, fileName, type]),
    deleteItem: (path) => call_manifest_editor_api('files.deleteItem', [path]),
    renameItem: (oldPath, newName) =>
      call_manifest_editor_api('files.renameItem', [oldPath, newName]),
    openFile: (filePath, fileType) =>
      call_manifest_editor_api('files.openFile', [filePath, fileType]),
  },
  lanChat: {
    startRoom: (payload) => call_manifest_editor_api('lanChat.startRoom', [payload || {}]),
    startLocalRoom: (payload) => call_manifest_editor_api('lanChat.startLocalRoom', [payload || {}]),
    stopRoom: () => call_manifest_editor_api('lanChat.stopRoom', []),
    stopLocalRoom: () => call_manifest_editor_api('lanChat.stopLocalRoom', []),
    joinRoom: (payload) => call_manifest_editor_api('lanChat.joinRoom', [payload || {}]),
    getHistory: () => call_manifest_editor_api('lanChat.getHistory', []),
    listHistoryRooms: () => call_manifest_editor_api('lanChat.listHistoryRooms', []),
    loadHistoryRoom: (room) => call_manifest_editor_api('lanChat.loadHistoryRoom', [{ room }]),
    leaveRoom: () => call_manifest_editor_api('lanChat.leaveRoom', []),
    sendMessage: (text, options = {}) =>
      call_manifest_editor_api('lanChat.sendMessage', [{ text, ...(options || {}) }]),
    getLocalIp: () => call_manifest_editor_api('lanChat.getLocalIp', []),
    addAgent: (payload) => call_manifest_editor_api('lanChat.addAgent', [payload || {}]),
    removeAgent: (agentId) => call_manifest_editor_api('lanChat.removeAgent', [{ agent_id: agentId }]),
    listAgents: () => call_manifest_editor_api('lanChat.listAgents', []),
  },
  network: {
    startSession: (instanceName, projectId, port = 27960, role = 'host') =>
      call_manifest_editor_api('network.startSession', [instanceName, projectId, port, role]),
    stopSession: () => call_manifest_editor_api('network.stopSession', []),
    getPeerCount: () => call_manifest_editor_api('network.getPeerCount', []),
    getSessionInfo: () => call_manifest_editor_api('network.getSessionInfo', []),
    connectToPeer: (ip, port, peerName) =>
      call_manifest_editor_api('network.connectToPeer', [ip, port, peerName]),
    setProjectRoot: (projectRoot) =>
      call_manifest_editor_api('network.setProjectRoot', [projectRoot]),
    broadcastActorCreate: (actorGuid, sceneName, modelPath, actorData) =>
      call_manifest_editor_api('network.broadcastActorCreate', [actorGuid, sceneName, modelPath, actorData]),
    broadcastActorTransform: (actorGuid, sceneName, actorData) =>
      call_manifest_editor_api('network.broadcastActorTransform', [actorGuid, sceneName, actorData]),
    broadcastActorDelete: (actorGuid, sceneName, actorName) =>
      call_manifest_editor_api('network.broadcastActorDelete', [actorGuid, sceneName, actorName]),
    requestSceneSnapshot: (sceneName) =>
      call_manifest_editor_api('network.requestSceneSnapshot', [sceneName]),
    broadcastSceneSnapshot: (sceneName, snapshot) =>
      call_manifest_editor_api('network.broadcastSceneSnapshot', [sceneName, snapshot]),
    broadcastActorStateUpdate: (actorGuid, sceneName, actorData) =>
      call_manifest_editor_api('network.broadcastActorStateUpdate', [actorGuid, sceneName, actorData]),
    pollPendingActorCreate: () => call_manifest_editor_api('network.pollPendingActorCreate', []),
    pollPendingActorTransform: () => call_manifest_editor_api('network.pollPendingActorTransform', []),
    pollPendingActorDelete: () => call_manifest_editor_api('network.pollPendingActorDelete', []),
    pollPendingSceneSnapshotRequest: () =>
      call_manifest_editor_api('network.pollPendingSceneSnapshotRequest', []),
    pollPendingSceneSnapshot: () => call_manifest_editor_api('network.pollPendingSceneSnapshot', []),
    pollPendingActorStateUpdate: () => call_manifest_editor_api('network.pollPendingActorStateUpdate', []),
    setSyncPaused: (paused) => call_manifest_editor_api('network.setSyncPaused', [!!paused]),
    registerActorIdentity: (actorGuid, actorHandle, locallyOwned = true) =>
      call_manifest_editor_api('network.registerActorIdentity', [actorGuid, String(actorHandle || ''), !!locallyOwned]),
    claimActorOwnership: (actorGuid) =>
      call_manifest_editor_api('network.claimActorOwnership', [actorGuid]),
  },
  project: {
    browseFolder: (defaultPath = '') =>
      call_manifest_editor_api('project.browseFolder', defaultPath ? [defaultPath] : []),
    createMultiplayerProject: (projectData) =>
      call_manifest_editor_api('project.createMultiplayerProject', [projectData || {}]),
    createProject: (projectData) =>
      call_manifest_editor_api('project.createProject', [projectData || {}]),
    createWorldProject: (worldData) =>
      call_manifest_editor_api('project.createWorldProject', [worldData || {}]),
    choosePortableSceneTarget: () =>
      call_manifest_editor_api('project.choosePortableSceneTarget', []),
    validatePortableScene: (payload) =>
      call_manifest_editor_api('project.validatePortableScene', [payload || {}]),
    importPortableAsset: (payload) =>
      call_manifest_editor_api('project.importPortableAsset', [payload || {}]),
    cleanupPortableSceneAssets: (payload) =>
      call_manifest_editor_api('project.cleanupPortableSceneAssets', [payload || {}]),
    getAppVersion: () => call_manifest_editor_api('project.getAppVersion', []),
    getDefaultProjectPath: () => call_manifest_editor_api('project.getDefaultProjectPath', []),
    getProjectLoadStatus: () => call_manifest_editor_api('project.getProjectLoadStatus', []),
    getRecentProjects: () => call_manifest_editor_api('project.getRecentProjects', []),
    migrateLegacyScene: (payload) =>
      call_manifest_editor_api('project.migrateLegacyScene', [payload || {}]),
    openProject: (projectPath, options = {}) =>
      call_manifest_editor_api('project.openProject', [projectPath, options]),
    openProjectFile: () => call_manifest_editor_api('project.openProjectFile', []),
    setProjectMode: (mode, settings) =>
      call_manifest_editor_api('project.setProjectMode', [{ mode, settings }]),
  },
  scene: {
    listActorTree: (sceneName) => call_manifest_editor_api('scene.listActorTree', [sceneName]),
  },
  scratch: {
    executePythonCode: (code, mode, sceneName, actorName, targetType = 'actor') =>
      call_manifest_editor_api('scratch.executePythonCode', [
        code,
        mode ?? 0,
        sceneName ?? '',
        actorName ?? '',
        targetType || 'actor',
      ]),
    saveBlocklyTarget: (payload) => call_manifest_editor_api('scratch.saveBlocklyTarget', [payload || {}]),
    loadBlocklyTarget: (payload) => call_manifest_editor_api('scratch.loadBlocklyTarget', [payload || {}]),
    startGamePreview: (payload = { scope: 'project' }) =>
      call_manifest_editor_api('scratch.startGamePreview', [payload || { scope: 'project' }]),
    stopGamePreview: () => call_manifest_editor_api('scratch.stopGamePreview', []),
    getGamePreviewStatus: () => call_manifest_editor_api('scratch.getGamePreviewStatus', []),
    stopScriptExecution: (restoreState = false) => call_manifest_editor_api('scratch.stopScriptExecution', [Boolean(restoreState)]),
    getScriptStatus: () => call_manifest_editor_api('scratch.getScriptStatus', []),
    sendKeyEvent: (key, modifiers, displayKey) =>
      call_manifest_editor_api('scratch.sendKeyEvent', [key, modifiers || '', displayKey || key]),
    sendKeyUpEvent: (key, displayKey) =>
      call_manifest_editor_api('scratch.sendKeyUpEvent', [key, displayKey || key]),
    sendMouseEvent: (eventType, button, x, y, viewportX, viewportY, viewportWidth, viewportHeight, pickedActor = '') =>
      call_manifest_editor_api('scratch.sendMouseEvent', [
        eventType, button || '', x || 0, y || 0,
        viewportX ?? x ?? 0, viewportY ?? y ?? 0,
        viewportWidth ?? 0, viewportHeight ?? 0, pickedActor || '',
      ]),
  },
  sceneTools: {
    createScene: (sceneName) => call_manifest_editor_api('sceneTools.createScene', [sceneName]),
    listSceneTree: (sceneName) => call_manifest_editor_api('sceneTools.listSceneTree', [sceneName]),
    reloadScene: (sceneName, projectPath = '') =>
      call_manifest_editor_api('sceneTools.reloadScene', projectPath ? [sceneName, projectPath] : [sceneName]),
    rebindActorResource: (sceneName, actorGuid, path) =>
      call_manifest_editor_api('sceneTools.rebindActorResource', [sceneName, actorGuid, path]),
    createActor: (sceneName, objPath, actorType = 'model', actorData = null) =>
      call_manifest_editor_api('sceneTools.createActor',
        actorData ? [sceneName, objPath, actorType, actorData] : [sceneName, objPath, actorType],
      ),
    removeActor: (sceneName, actorName) =>
      call_manifest_editor_api('sceneTools.removeActor', [sceneName, actorName]),
    renameActor: (sceneName, actorName, name) =>
      call_manifest_editor_api('sceneTools.renameActor', [sceneName, actorName, name]),
    openActor: (sceneName, actorName) =>
      call_manifest_editor_api('sceneTools.openActor', [sceneName, actorName]),
    selectActor: (sceneName, actorType, actorName) =>
      call_manifest_editor_api('sceneTools.selectActor', [sceneName, actorType, actorName]),
    focusActor: (sceneName, actorName, cameraName) =>
      call_manifest_editor_api('sceneTools.focusActor', [sceneName, actorName, cameraName]),
    setRenderBackend: (mode, sceneName = null, cameraId = null) =>
      call_manifest_editor_api('sceneTools.setRenderBackend', [mode, sceneName, cameraId]),
    getRenderBackend: (sceneName = null, cameraId = null) =>
      call_manifest_editor_api('sceneTools.getRenderBackend', [sceneName, cameraId]),
    setVisionRenderMode: (sceneName, cameraId = null, mode = 'path_tracing') =>
      call_manifest_editor_api('sceneTools.setVisionRenderMode', [sceneName, cameraId, mode]),
    getVisionRenderMode: (sceneName, cameraId = null) =>
      call_manifest_editor_api('sceneTools.getVisionRenderMode', [sceneName, cameraId]),
    setSsatViewViewer: (sceneName, cameraId = null, mode = 'interlaced', viewIndex = 0) =>
      call_manifest_editor_api('sceneTools.setSsatViewViewer', [sceneName, cameraId, mode, viewIndex]),
    getSsatViewViewer: (sceneName, cameraId = null) =>
      call_manifest_editor_api('sceneTools.getSsatViewViewer', [sceneName, cameraId]),
    createCameraView: (sceneName, name = null) =>
      call_manifest_editor_api('sceneTools.createCameraView', [sceneName, name]),
    openCameraView: (sceneName, cameraId) =>
      call_manifest_editor_api('sceneTools.openCameraView', [sceneName, cameraId]),
    closeCameraView: (sceneName, cameraId) =>
      call_manifest_editor_api('sceneTools.closeCameraView', [sceneName, cameraId]),
    renameCameraView: (sceneName, cameraId, name) =>
      call_manifest_editor_api('sceneTools.renameCameraView', [sceneName, cameraId, name]),
    listCameraViews: (sceneName) =>
      call_manifest_editor_api('sceneTools.listCameraViews', [sceneName]),
    updateCameraView: (sceneName, cameraId, state) =>
      call_manifest_editor_api('sceneTools.updateCameraView', [sceneName, cameraId, state]),
    deleteCamera: (sceneName, cameraId) =>
      call_manifest_editor_api('sceneTools.deleteCamera', [sceneName, cameraId]),
    sunDirection: (sceneName, enable, direction) =>
      call_manifest_editor_api('sceneTools.sunDirection', [sceneName, enable, direction]),
    floorGrid: (sceneName, enabled) =>
      call_manifest_editor_api('sceneTools.floorGrid', [sceneName, enabled]),
    setPhysicsParams: (sceneName, params) =>
      call_manifest_editor_api('sceneTools.setPhysicsParams', [
        sceneName,
        params.gravity,
        params.floor_y,
        params.floor_restitution,
        params.fixed_dt,
      ]),
    getPhysicsParams: (sceneName) => call_manifest_editor_api('sceneTools.getPhysicsParams', [sceneName]),
    selectScreenshotPath: (sceneName, cameraName) =>
      call_manifest_editor_api('sceneTools.selectScreenshotPath', [sceneName, cameraName]),
    saveScreenshot: (sceneName, path, cameraName) =>
      call_manifest_editor_api('sceneTools.saveScreenshot', [sceneName, path, cameraName]),
    setOutputMode: (sceneName, cameraName, mode) =>
      call_manifest_editor_api('sceneTools.setOutputMode', [sceneName, cameraName, mode]),
    getOutputMode: (sceneName, cameraName) =>
      call_manifest_editor_api('sceneTools.getOutputMode', [sceneName, cameraName]),
    setShadowCascadeDebug: (sceneName, cameraName, enabled) =>
      call_manifest_editor_api('sceneTools.setShadowCascadeDebug', [sceneName, cameraName, !!enabled]),
    getShadowCascadeDebug: (sceneName, cameraName) =>
      call_manifest_editor_api('sceneTools.getShadowCascadeDebug', [sceneName, cameraName]),
    setSsaoEnabled: (sceneName, cameraName, enabled) =>
      call_manifest_editor_api('sceneTools.setSsaoEnabled', [sceneName, cameraName, !!enabled]),
    getSsaoEnabled: (sceneName, cameraName) =>
      call_manifest_editor_api('sceneTools.getSsaoEnabled', [sceneName, cameraName]),
    isVisionAvailable: () => call_manifest_editor_api('sceneTools.isVisionAvailable', []),
    loadVisionScene: (path) => call_manifest_editor_api('sceneTools.loadVisionScene', [path]),
    pickActor: (sceneName, x, y, vpWidth, vpHeight) =>
      call_manifest_editor_api('sceneTools.pickActor', [sceneName, x, y, vpWidth, vpHeight]),
    playAudio: (resourceId, loop) =>
      call_manifest_editor_api('sceneTools.playAudio', [resourceId, loop]),
    stopAudio: (resourceId) =>
      call_manifest_editor_api('sceneTools.stopAudio', [resourceId]),
    actorPlayAudio: (actorName, loop = false) =>
      call_manifest_editor_api('sceneTools.actorPlayAudio', [actorName, loop]),
    actorStopAudio: (actorName) =>
      call_manifest_editor_api('sceneTools.actorStopAudio', [actorName]),
  },
  main: {
    getMenuData: () => call_manifest_editor_api('main.getMenuData', []),
    importResourceFile: (sceneName, fileType) =>
      call_manifest_editor_api('main.importResourceFile', [sceneName, fileType]),
    onInit: (projectPath = '') =>
      call_manifest_editor_api('main.onInit', projectPath ? [projectPath] : []),
    runProject: (scenePath = '') =>
      call_manifest_editor_api('main.runProject', scenePath ? [scenePath] : []),
    sceneSave: (sceneName) => call_manifest_editor_api('main.sceneSave', [sceneName]),
    updateViewToolState: (toolId, enabled) =>
      call_manifest_editor_api('main.updateViewToolState', [toolId, !!enabled]),
  },
  projectSettings: {
    getActiveProjectInfo: () => call_manifest_editor_api('projectSettings.getActiveProjectInfo', []),
    saveActiveProjectInfo: (settings) =>
      call_manifest_editor_api('projectSettings.saveActiveProjectInfo', [settings || {}]),
    browseSceneFile: () => call_manifest_editor_api('projectSettings.browseSceneFile', []),
  },
  resourceSearch: {
    prepareIndex: (caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.prepareIndex', [caller]),
    fuzzySearch: (query, topK = 20, typeFilter = null, caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.fuzzySearch', [query, topK, typeFilter, caller]),
    imageSearch: (imageB64, topK = 20, threshold = 10, caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.imageSearch', [imageB64, topK, threshold, caller]),
    listTypes: (caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.listTypes', [caller]),
    rebuildIndex: (caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.rebuildIndex', [caller]),
    getStats: (caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.getStats', [caller]),
    markIndexDirty: (reason = 'frontend', caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.markIndexDirty', [reason, caller]),
    focusActor: (sceneName, actorName, caller = CURRENT_CALLER) =>
      call_manifest_editor_api('resourceSearch.focusActor', [sceneName, actorName, caller]),
  },
  sceneDatas: {
    getScene: (sceneId) => call_manifest_editor_api('sceneDatas.getScene', [sceneId]),
    getActor: (sceneId, actorId) => call_manifest_editor_api('sceneDatas.getActor', [sceneId, actorId]),
    actorOperation: (sceneName, actorName, operation, vector) =>
      call_manifest_editor_api('sceneDatas.actorOperation', [sceneName, actorName, operation, vector]),
    saveActor: (sceneName, actorName) =>
      call_manifest_editor_api('sceneDatas.saveActor', [sceneName, actorName]),
    selectModelFile: (sceneId, actorId, fileType) =>
      call_manifest_editor_api('sceneDatas.selectModelFile', [sceneId, actorId, fileType]),
  },
};

export const editorApi = create_dynamic_editor_api_namespace('', editorApiStatic);

// 快捷访问
export const sceneService = {
  createActor: (sceneName, objPath, actorType = 'model', actorData = null) =>
    editorApi.sceneTools.createActor(sceneName, objPath, actorType, actorData),
  removeActor: (sceneName, actorName) =>
    editorApi.sceneTools.removeActor(sceneName, actorName),
  renameActor: (sceneName, actorName, name) =>
    editorApi.sceneTools.renameActor(sceneName, actorName, name),
  createScene: (sceneName) =>
    editorApi.sceneTools.createScene(sceneName),

  sunDirection: (sceneName, enable, direction) =>
    editorApi.sceneTools.sunDirection(sceneName, enable, direction),
  floorGrid: (sceneName, enabled) =>
    editorApi.sceneTools.floorGrid(sceneName, enabled),
  setPhysicsParams: (sceneName, params) =>
    editorApi.sceneTools.setPhysicsParams(sceneName, params),
  getPhysicsParams: (sceneName) => editorApi.sceneTools.getPhysicsParams(sceneName),
  selectScreenshotPath: (sceneName, cameraName) =>
    editorApi.sceneTools.selectScreenshotPath(sceneName, cameraName),
  saveScreenshot: (sceneName, path, cameraName) =>
    editorApi.sceneTools.saveScreenshot(sceneName, path, cameraName),
  setOutputMode: (sceneName, cameraName, mode) =>
    editorApi.sceneTools.setOutputMode(sceneName, cameraName, mode),
  getOutputMode: (sceneName, cameraName) =>
    editorApi.sceneTools.getOutputMode(sceneName, cameraName),
  setShadowCascadeDebug: (sceneName, cameraName, enabled) =>
    editorApi.sceneTools.setShadowCascadeDebug(sceneName, cameraName, enabled),
  getShadowCascadeDebug: (sceneName, cameraName) =>
    editorApi.sceneTools.getShadowCascadeDebug(sceneName, cameraName),
  setSsaoEnabled: (sceneName, cameraName, enabled) =>
    editorApi.sceneTools.setSsaoEnabled(sceneName, cameraName, enabled),
  getSsaoEnabled: (sceneName, cameraName) =>
    editorApi.sceneTools.getSsaoEnabled(sceneName, cameraName),
  isVisionAvailable: () => editorApi.sceneTools.isVisionAvailable(),
  setRenderBackend: (mode, sceneName = null, cameraId = null) =>
    editorApi.sceneTools.setRenderBackend(mode, sceneName, cameraId),
  getRenderBackend: (sceneName = null, cameraId = null) =>
    editorApi.sceneTools.getRenderBackend(sceneName, cameraId),
  setVisionRenderMode: (sceneName, cameraId = null, mode = 'path_tracing') =>
    editorApi.sceneTools.setVisionRenderMode(sceneName, cameraId, mode),
  getVisionRenderMode: (sceneName, cameraId = null) =>
    editorApi.sceneTools.getVisionRenderMode(sceneName, cameraId),
  setSsatViewViewer: (sceneName, cameraId = null, mode = 'interlaced', viewIndex = 0) =>
    editorApi.sceneTools.setSsatViewViewer(sceneName, cameraId, mode, viewIndex),
  getSsatViewViewer: (sceneName, cameraId = null) =>
    editorApi.sceneTools.getSsatViewViewer(sceneName, cameraId),
  createCameraView: (sceneName, name = null) =>
    editorApi.sceneTools.createCameraView(sceneName, name),
  openCameraView: (sceneName, cameraId) =>
    editorApi.sceneTools.openCameraView(sceneName, cameraId),
  closeCameraView: (sceneName, cameraId) =>
    editorApi.sceneTools.closeCameraView(sceneName, cameraId),
  renameCameraView: (sceneName, cameraId, name) =>
    editorApi.sceneTools.renameCameraView(sceneName, cameraId, name),
  listCameraViews: (sceneName) =>
    editorApi.sceneTools.listCameraViews(sceneName),
  updateCameraView: (sceneName, cameraId, state) =>
    editorApi.sceneTools.updateCameraView(sceneName, cameraId, state),
  deleteCamera: (sceneName, cameraId) =>
    editorApi.sceneTools.deleteCamera(sceneName, cameraId),
  loadVisionScene: (path) => editorApi.sceneTools.loadVisionScene(path),
  reloadScene: (sceneName, projectPath = '') =>
    editorApi.sceneTools.reloadScene(sceneName, projectPath),
  rebindActorResource: (sceneName, actorGuid, path) =>
    editorApi.sceneTools.rebindActorResource(sceneName, actorGuid, path),
  listActorTree: (sceneName) => editorApi.scene.listActorTree(sceneName),
  listSceneTree: (sceneName) => editorApi.sceneTools.listSceneTree(sceneName),
  openSceneActor: (sceneName, actorName) =>
    editorApi.sceneTools.openActor(sceneName, actorName),
  focusActor: (sceneName, actorName, cameraName) =>
    editorApi.sceneTools.focusActor(sceneName, actorName, cameraName),
  /** 鼠标在3D视口中拾取物体（异步：首次调用设置拾取，~50ms后重试获取结果） */
  pickActor: (sceneName, x, y, vpWidth, vpHeight) =>
    editorApi.sceneTools.pickActor(sceneName, x, y, vpWidth, vpHeight),
  /** 播放已导入的音频资源 */
  playAudio: (resourceId, loop) =>
    editorApi.sceneTools.playAudio(resourceId, loop),
  /** 停止播放音频资源 */
  stopAudio: (resourceId) =>
    editorApi.sceneTools.stopAudio(resourceId),
  /** 在 audio Actor 的世界位置播放其绑定音频（空间音频） */
  actorPlayAudio: (actorName, loop = false) =>
    editorApi.sceneTools.actorPlayAudio(actorName, loop),
  /** 停止 audio Actor 的空间音频播放 */
  actorStopAudio: (actorName) =>
    editorApi.sceneTools.actorStopAudio(actorName),

  getScene: (sceneId) => editorApi.sceneDatas.getScene(sceneId),
  getActor: (sceneId, actorId) => editorApi.sceneDatas.getActor(sceneId, actorId),
  actorOperation: (scene_name, actor_name, operation, vector) =>
    editorApi.sceneDatas.actorOperation(scene_name, actor_name, operation, vector),
  /** 仅触发写盘：Transform 已由快速通道写入 SharedDataHub */
  saveActor: (sceneName, actorName) =>
    editorApi.sceneDatas.saveActor(sceneName, actorName),
  selectModelFileDialog: (sceneId, actorId, fileType) =>
    editorApi.sceneDatas.selectModelFile(sceneId, actorId, fileType),
  setCameraLock: (sceneName, actorName, enabled) =>
    editorApi.sceneDatas.actorOperation(sceneName, actorName, 'SetCameraLock', [enabled]),
  setCameraLockOffset: (sceneName, actorName, offset) =>
    editorApi.sceneDatas.actorOperation(sceneName, actorName, 'SetCameraLockOffset', offset),
  setCameraLockRotation: (sceneName, actorName, rotation) =>
    editorApi.sceneDatas.actorOperation(sceneName, actorName, 'SetCameraLockRotation', rotation),
};

export const projectService = {
  OnInit: (projectPath = window.localStorage?.getItem('corona.activeProjectPath') || '') =>
    editorApi.main.onInit(projectPath),
  importResourceFileByDialog: (sceneName, fileType) =>
    editorApi.main.importResourceFile(sceneName, fileType),
  sceneSave: (sceneName) => editorApi.main.sceneSave(sceneName),

  // 菜单数据接口
  getMenuData: () => editorApi.main.getMenuData(),
  updateViewToolState: (toolId, enabled) =>
    editorApi.main.updateViewToolState(toolId, enabled),

  runProject: (scenePath) =>
    editorApi.main.runProject(scenePath),

  setDragRegions: (Path, x, y, w, h) =>
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
};

export const appService = {
  createPanelTab: (panelId, routePath, width, height, dockingPos, zPriority = 0) =>
    Bridge.callDockCommand({ cmd: 'createPanelTab', panelId, routePath, width, height, dockingPos, zPriority }),
  // Create a panel that is born directly as its own borderless OS window (skips the
  // main-window docked-rectangle stage, so no 1-frame flash). x/y/width/height are the
  // desired initial geometry in logical px. Returns { tab_id, panel_id }.
  createDetachedPanel: ({ panelId, routePath, width, height, x, y }) =>
    Bridge.callDockCommand({ cmd: 'createDetachedPanel', panelId, routePath, width, height, x, y }),
  closeThisTab: (panelId) =>
    Bridge.callDockCommand({ cmd: 'closeThisTab', panelId }),
  closePanelTab: (tabId, panelId) =>
    Bridge.callDockCommand({ cmd: 'closePanelTab', tabId, panelId }),
  // Detach the calling panel into its own borderless OS window (tabId omitted ⇒ C++
  // resolves it from the calling browser). x/y/width/height are optional desired geometry
  // in logical px; width/height default to the panel's current size on the C++ side.
  detachPanel: (opts = {}) =>
    Bridge.callDockCommand({ cmd: 'detachPanel', ...opts }),
  togglePanelWindowMode: (opts = {}) =>
    Bridge.callDockCommand({ cmd: 'togglePanelWindowMode', ...opts }),
  // Re-dock the calling panel back into the main window (destroys its secondary window).
  redockPanel: (opts = {}) =>
    Bridge.callDockCommand({ cmd: 'redockPanel', ...opts }),
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
  suspendCameraViews: (sceneId) =>
    Bridge.callDockCommand({ cmd: 'suspendCameraViews', sceneId }),
  crossTabBroadcast: (event, payload) =>
    Bridge.callDockCommand({ cmd: 'broadcast', event, payload }),
  closeProcess: () => editorApi.app.closeProcess(),
};

export const aiService = {
  sendMessageToAIStream: (payload) => editorApi.ai.sendMessageToAIStream(payload),
  readLocalFileAsBase64: (filePath) => editorApi.ai.readLocalFileAsBase64(filePath),
  generateHint: (elementType, context = {}) => editorApi.ai.generateHint(elementType, context),
  startNodeGraphReview: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.start',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  getNodeGraphReviewStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  chatAboutNodeGraph: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  startNodeGraphReviewChat: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat.start',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  getNodeGraphReviewChatStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  cancelNodeGraphReviewChat: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'node_graph.review.chat.cancel',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
  loadCabbageContext: async () => {
    const response = await editorApi.ai.submitRequest({ operation: 'cabbage.context.load' });
    return response?.data ?? response;
  },
  recordCabbageEvent: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.context.record_event',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  updateCabbageTask: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.context.update_task',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  appendCabbageMessage: async (payload) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.context.append_message',
      payload: payload || {},
    });
    return response?.data ?? response;
  },
  startCabbageProfileScoreUpdate: async (payload = {}) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.profile.score.start',
      payload,
    });
    return response?.data ?? response;
  },
  getCabbageProfileScoreStatus: async (taskId) => {
    const response = await editorApi.ai.submitRequest({
      operation: 'cabbage.profile.score.status',
      taskId: String(taskId || ''),
    });
    return response?.data ?? response;
  },
};

export const aiClient = {
  chatStream: (request) => editorApi.ai.chatStream(request),
  cancelRequest: (requestId) => editorApi.ai.cancelRequest(requestId),
  getRequestStatus: (requestId) => editorApi.ai.getRequestStatus(requestId),
};

// 局域网聊天室：所有跨机传输在 C++ NetworkSystem 完成，前端只通过 cefQuery 调用。
// LANChat 主事件由 C++ Editor API registry 定义为 LANChat.event。
//
// 注意：C++ 脚本服务会用 create_success_response 把返回值包成
// { success, data, timestamp }，业务结果在 .data 里。这里统一解包，
// 让 store 直接拿到 { ok, ip, ... } 业务对象（约定同 SceneBar：result?.data ?? result）。
const _unwrap = (res) => (res && res.data !== undefined ? res.data : res);

export const lanChatService = {
  // 房主开房：{ room, password, port? } -> { ok, ip, port, room } | { ok:false, error }
  startRoom: (payload) => editorApi.lanChat.startRoom(payload).then(_unwrap),
  // 单人本地房：不启动 NetworkSystem 协作会话
  startLocalRoom: (payload) => editorApi.lanChat.startLocalRoom(payload).then(_unwrap),
  // 房主关房 -> { ok }
  stopRoom: () => editorApi.lanChat.stopRoom().then(_unwrap),
  // 关闭单人本地房，不停止 NetworkSystem 协作会话
  stopLocalRoom: () => editorApi.lanChat.stopLocalRoom().then(_unwrap),
  // 加入房间：{ ip, port, room, password, nickname } -> { ok, members, history } | { ok:false, code }
  joinRoom: (payload) => editorApi.lanChat.joinRoom(payload).then(_unwrap),
  // 显式读取当前房间历史，用于开房后兜底恢复持久化记录
  getHistory: () => editorApi.lanChat.getHistory().then(_unwrap),
  // 读取持久化历史房间列表，打开 Dock 时展示给用户选择
  listHistoryRooms: () => editorApi.lanChat.listHistoryRooms().then(_unwrap),
  // 读取指定持久化房间历史，不自动进入该房间
  loadHistoryRoom: (room) => editorApi.lanChat.loadHistoryRoom(room).then(_unwrap),
  // 离开房间 -> { ok }
  leaveRoom: () => editorApi.lanChat.leaveRoom().then(_unwrap),
  // 发送消息：{ text } -> { ok } | { ok:false, error }
  sendMessage: (text, options = {}) =>
    editorApi.lanChat.sendMessage(text, options).then(_unwrap),
  // 获取本机局域网 IP -> { ok, ip, port }
  getLocalIp: () => editorApi.lanChat.getLocalIp().then(_unwrap),
  // 添加 AI 助手：{ name, persona } -> { ok, agent_id, name } | { ok:false, error }
  addAgent: (payload) => editorApi.lanChat.addAgent(payload).then(_unwrap),
  // 移除 AI 助手：{ agent_id } -> { ok }
  removeAgent: (agentId) => editorApi.lanChat.removeAgent(agentId).then(_unwrap),
  // 列出 agent 名册 -> { ok, agents:[{agent_id,name,owner}] }
  listAgents: () => editorApi.lanChat.listAgents().then(_unwrap),
};

export const scriptingService = {
  /**
   * 执行 Blockly 生成的 Python 代码
   * @param {string} code - Python 代码
   * @param {number} mode - 执行模式（0 = 编辑模式）
   * @param {string} sceneName - 目标场景名称（可选）
   * @param {string} actorName - 目标 Actor 名称（可选）
   */
  executePythonCode: (code, mode, sceneName, actorName, targetType = 'actor') =>
    editorApi.scratch.executePythonCode(code, mode, sceneName, actorName, targetType),

  saveBlocklyTarget: (payload) => editorApi.scratch.saveBlocklyTarget(payload),

  loadBlocklyTarget: (payload) => editorApi.scratch.loadBlocklyTarget(payload),
  startGamePreview: (payload = { scope: 'project' }) => editorApi.scratch.startGamePreview(payload),

  stopGamePreview: () => editorApi.scratch.stopGamePreview(),

  getGamePreviewStatus: () => editorApi.scratch.getGamePreviewStatus(),

  /**
   * 停止当前正在执行的脚本
   */
  stopScriptExecution: (restoreState = false) => editorApi.scratch.stopScriptExecution(restoreState),

  /**
   * Query the current script state and node-graph execution trace.
   * @returns {Promise<{
   *   status: 'starting'|'running'|'completed'|'stopped'|'error',
   *   outcome: string,
   *   error: string,
   *   contextId: string,
   *   sceneName: string,
   *   actorName: string,
   *   targetType: 'actor'|'project',
   *   currentNodeId: string,
   *   currentNodeName: string,
   *   waitingEdgeId: string,
   *   waitingEdgeName: string,
   *   startedAt: number,
   *   finishedAt: number
   * }>}
   */
  getScriptStatus: () => editorApi.scratch.getScriptStatus(),

  /**
   * 发送键盘事件到积木脚本
   * @param {string} key - 按键名 (如 'KeyA', 'Space', 'ArrowUp')
   * @param {string} modifiers - 修饰键 (如 'Ctrl,Shift')
   */
  sendKeyEvent: (key, modifiers, displayKey) =>
    editorApi.scratch.sendKeyEvent(key, modifiers, displayKey),

  /**
   * 发送键盘释放事件到积木脚本
   */
  sendKeyUpEvent: (key, displayKey) =>
    editorApi.scratch.sendKeyUpEvent(key, displayKey),

  /**
   * 发送鼠标事件到积木脚本
   */
  sendMouseEvent: (eventType, button, x, y, viewportX, viewportY, viewportWidth, viewportHeight, pickedActor = '') =>
    editorApi.scratch.sendMouseEvent(
      eventType, button, x, y, viewportX, viewportY, viewportWidth, viewportHeight, pickedActor
    ),
};

export const projectLauncherService = {
  // 获取默认项目路径
  getDefaultProjectPath: () => editorApi.project.getDefaultProjectPath(),
  // 浏览文件夹
  browseFolder: (default_path) =>
    editorApi.project.browseFolder(default_path),
  choosePortableSceneTarget: () =>
    editorApi.project.choosePortableSceneTarget(),
  validatePortableScene: (payload = {}) =>
    editorApi.project.validatePortableScene(payload),
  importPortableAsset: (payload = {}) =>
    editorApi.project.importPortableAsset(payload),
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
  // 浏览并选择项目文件 (.ini)
  openProjectFile: () => editorApi.project.openProjectFile(),
  // 创建项目
  createProject: (projectData) =>
    editorApi.project.createProject(projectData),
  // 创建 AI 世界项目：自动命名 + 存到引擎 data 目录，无需 name/path
  // worldData: { mode: 'story'|'creative', prompt: string } -> { name, path }
  createWorldProject: (worldData) =>
    editorApi.project.createWorldProject(worldData),
  // 创建首页联机入口使用的临时项目：{ role: 'host'|'guest' } -> { name, path, role }
  createMultiplayerProject: (projectData) =>
    editorApi.project.createMultiplayerProject(projectData),
  // 打开项目并让原生场景同步到该项目。
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
  // 设置项目模式 (2D/3D/渲染)
  setProjectMode: (mode, settings) =>
    editorApi.project.setProjectMode(mode, settings),
  // 获取版本信息
  getAppVersion: () => editorApi.project.getAppVersion(),
  // 获取当前项目异步资源加载进度
  getProjectLoadStatus: () => editorApi.project.getProjectLoadStatus(),
  // 获取最近项目列表
  getRecentProjects: () => editorApi.project.getRecentProjects(),
};

export const fileService = {
  getProjectInfo: () => editorApi.files.getProjectInfo(),
  getFiles: (relPath) => editorApi.files.getFiles(relPath),
  getFileTree: (relPath) => editorApi.files.getFileTree(relPath),
  createFolder: (path, folderName) =>
    editorApi.files.createFolder(path, folderName),
  createFile: (path, fileName, type) =>
    editorApi.files.createFile(path, fileName, type),
  deleteItem: (path) => editorApi.files.deleteItem(path),
  renameItem: (oldPath, newName) =>
    editorApi.files.renameItem(oldPath, newName),
  openFile: (filePath, fileType) =>
    editorApi.files.openFile(filePath, fileType),
};

export const logService = {
  setLogReady: () => Promise.resolve({ success: true, disabled: true }),
  setLogClose: () => Promise.resolve({ success: true, disabled: true }),
};

/**
 * 场景栏资源智能搜索
 * - fuzzy_search: 模糊文本搜索(支持中文分词/拼音/编辑距离)
 * - image_search: 以图搜索(本地 pHash,无网络依赖)
 * - list_types / rebuild_index / get_stats: 索引元操作
 * - focus_actor: 搜索结果"定位"按钮 → 桥接 SceneTools
 */
// 当前模块的"调用方"标识(必须出现在后端 ALLOWED_CALLERS 白名单内)
// 任何后端接口调用都会自动附带此标识,供权限控制
const CURRENT_CALLER = 'SceneBar';
const RESOURCE_SEARCH_ENABLED = false;
const resourceSearchDisabled = () => Promise.resolve({
  success: true,
  data: {
    status: 'disabled',
    code: 'resource_search_disabled',
    message: 'ResourceSearch is disabled',
    items: [],
    total: 0,
  },
});

export const resourceService = {
  prepareIndex: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.prepareIndex()
      : resourceSearchDisabled(),
  fuzzySearch: (query, topK = 20, typeFilter = null) =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.fuzzySearch(query, topK, typeFilter)
      : resourceSearchDisabled(),
  imageSearch: (imageB64, topK = 20, threshold = 10) =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.imageSearch(imageB64, topK, threshold)
      : resourceSearchDisabled(),
  listTypes: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.listTypes()
      : resourceSearchDisabled(),
  rebuildIndex: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.rebuildIndex()
      : resourceSearchDisabled(),
  getStats: () =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.getStats()
      : resourceSearchDisabled(),
  markIndexDirty: (reason = 'frontend') =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.markIndexDirty(reason)
      : resourceSearchDisabled(),
  focusActor: (sceneName, actorName) =>
    RESOURCE_SEARCH_ENABLED
      ? editorApi.resourceSearch.focusActor(sceneName, actorName)
      : resourceSearchDisabled(),
};

export const projectSettingsService = {
  // 获取当前激活项目的配置
  getActiveProjectInfo: () => editorApi.projectSettings.getActiveProjectInfo(),
  // 保存当前激活项目的配置
  saveActiveProjectInfo: (settings) =>
    editorApi.projectSettings.saveActiveProjectInfo(settings),
  // 浏览当前项目中的场景文件
  browseSceneFile: () => editorApi.projectSettings.browseSceneFile(),
};

export const networkService = {
  startSession: (instanceName, projectId, port = 27960, role = 'host') =>
    editorApi.network.startSession(instanceName, projectId, port, role).then(_unwrap),
  stopSession: () => editorApi.network.stopSession().then(_unwrap),
  getPeerCount: () => editorApi.network.getPeerCount().then(_unwrap),
  getSessionInfo: () => editorApi.network.getSessionInfo().then(_unwrap),
  connectToPeer: (ip, port, peerName) =>
    editorApi.network.connectToPeer(ip, port, peerName).then(_unwrap),
  setProjectRoot: (projectRoot) =>
    editorApi.network.setProjectRoot(projectRoot).then(_unwrap),
  broadcastActorCreate: (actorGuid, sceneName, modelPath, actorData) =>
    editorApi.network.broadcastActorCreate(actorGuid, sceneName, modelPath, actorData).then(_unwrap),
  broadcastActorTransform: (actorGuid, sceneName, actorData) =>
    editorApi.network.broadcastActorTransform(actorGuid, sceneName, actorData).then(_unwrap),
  broadcastActorDelete: (actorGuid, sceneName, actorName) =>
    editorApi.network.broadcastActorDelete(actorGuid, sceneName, actorName).then(_unwrap),
  requestSceneSnapshot: (sceneName) =>
    editorApi.network.requestSceneSnapshot(sceneName).then(_unwrap),
  broadcastSceneSnapshot: (sceneName, snapshot) =>
    editorApi.network.broadcastSceneSnapshot(sceneName, snapshot).then(_unwrap),
  broadcastActorStateUpdate: (actorGuid, sceneName, actorData) =>
    editorApi.network.broadcastActorStateUpdate(actorGuid, sceneName, actorData).then(_unwrap),
  /** 轮询待创建的远程 Actor（文件传输完成后触发创建） */
  pollPendingActorCreate: () =>
    editorApi.network.pollPendingActorCreate().then(_unwrap),
  /** 轮询远程 Actor transform delta */
  pollPendingActorTransform: () =>
    editorApi.network.pollPendingActorTransform().then(_unwrap),
  pollPendingActorDelete: () =>
    editorApi.network.pollPendingActorDelete().then(_unwrap),
  pollPendingSceneSnapshotRequest: () =>
    editorApi.network.pollPendingSceneSnapshotRequest().then(_unwrap),
  pollPendingSceneSnapshot: () =>
    editorApi.network.pollPendingSceneSnapshot().then(_unwrap),
  pollPendingActorStateUpdate: () =>
    editorApi.network.pollPendingActorStateUpdate().then(_unwrap),
  /** 暂停/恢复同步（Actor 创建期间避免 seq_id 碰撞） */
  setSyncPaused: (paused) =>
    editorApi.network.setSyncPaused(paused).then(_unwrap),
  /** 注册 actor_guid -> 本地 Actor handle 映射，作为后续稳定同步的锚点 */
  registerActorIdentity: (actorGuid, actorHandle, locallyOwned = true) =>
    editorApi.network.registerActorIdentity(actorGuid, actorHandle, locallyOwned).then(_unwrap),
  claimActorOwnership: (actorGuid) =>
    editorApi.network.claimActorOwnership(actorGuid).then(_unwrap),
};
