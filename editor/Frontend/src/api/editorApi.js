/**
 * Bridge Utility for QWebChannel
 * 封装了与 C++ Editor API 的通信，支持 Promise 调用
 */

let editorApiMethodSpecs = null;
let editorApiManifestPromise = null;
let editorApiEventSpecs = null;
let editorApiEventManifestPromise = null;
const EDITOR_API_CALLER_CEF = 1;
// Default caller for the manifest-backed resource-search namespace.
const CURRENT_CALLER = 'SceneBar';
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
    onViewportGizmoPointerResult: (callback) => register_manifest_editor_api_callback('events.onViewportGizmoPointerResult', callback),
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
    sendAgentReply: (payload) =>
      call_manifest_editor_api('lanChat.sendAgentReply', [payload || {}]),
    sendSystemMessage: (payload) =>
      call_manifest_editor_api('lanChat.sendSystemMessage', [payload || {}]),
    sendSystemMessageToHost: (payload) =>
      call_manifest_editor_api('lanChat.sendSystemMessageToHost', [payload || {}]),
    sendSystemMessageToUser: (payload) =>
      call_manifest_editor_api('lanChat.sendSystemMessageToUser', [payload || {}]),
    pollAgentTrigger: () => call_manifest_editor_api('lanChat.pollAgentTrigger', []),
    pollCoordinatorSyncMessage: () =>
      call_manifest_editor_api('lanChat.pollCoordinatorSyncMessage', []),
    pollRoomEvent: () => call_manifest_editor_api('lanChat.pollRoomEvent', []),
    pollSyncEvent: () => call_manifest_editor_api('lanChat.pollSyncEvent', []),
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
    getDiscoveredPeers: () => call_manifest_editor_api('network.getDiscoveredPeers', []),
    clearDiscoveredPeers: () => call_manifest_editor_api('network.clearDiscoveredPeers', []),
    searchLan: () => call_manifest_editor_api('network.searchLan', []),
    connectToPeer: (ip, port, peerName) =>
      call_manifest_editor_api('network.connectToPeer', [ip, port, peerName]),
    lockObject: (objectId, userId, operation = 'modify') =>
      call_manifest_editor_api('network.lockObject', [objectId, userId, operation]),
    unlockObject: (objectId, userId) =>
      call_manifest_editor_api('network.unlockObject', [objectId, userId]),
    getLockOwner: (objectId) =>
      call_manifest_editor_api('network.getLockOwner', [objectId]),
    broadcastIntent: (userId, tooltip, position, status = 'placing_object') =>
      call_manifest_editor_api('network.broadcastIntent', [userId, tooltip, position, status]),
    checkPreviewCollision: (userId, position, delta = 0.5) =>
      call_manifest_editor_api('network.checkPreviewCollision', [userId, position, delta]),
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
    copyExistingToData: (payload) =>
      call_manifest_editor_api('project.copyExistingToData', [payload || {}]),
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
    getSnapshot: (sceneName = '') => call_manifest_editor_api('scene.getSnapshot', [sceneName]),
    getActor: async (sceneName, actorName) => {
      const result = await editorApi.scene.getSnapshot(sceneName);
      const snapshot = result?.data ?? result ?? {};
      const actor = Array.isArray(snapshot.actors)
        ? snapshot.actors.find((item) => item?.name === actorName)
        : null;
      if (!actor) {
        return {
          success: false,
          data: { status: 'error', message: `Actor not found: ${actorName}` },
        };
      }
      return { ...result, data: actor };
    },
    setActorTransform: (sceneName, actorName, transform) =>
      call_manifest_editor_api('scene.setActorTransform', [sceneName, actorName, transform]),
  },
  viewport: {
    capture: (sceneName, cameraName, camera, outputPath) =>
      call_manifest_editor_api('viewport.capture', [sceneName, cameraName, camera, outputPath]),
    setCameraPose: (sceneName, cameraName, camera) =>
      call_manifest_editor_api('viewport.setCameraPose', [sceneName, cameraName, camera]),
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
    selectActor: (sceneName, actorType, actorName, context = null) =>
      call_manifest_editor_api(
        'sceneTools.selectActor',
        context ? [sceneName, actorType, actorName, context] : [sceneName, actorType, actorName],
      ),
    focusActor: (sceneName, actorName, cameraName) =>
      call_manifest_editor_api('sceneTools.focusActor', [sceneName, actorName, cameraName]),
    setActorState: (sceneName, actorName, state) =>
      call_manifest_editor_api('sceneTools.setActorState', [sceneName, actorName, state]),
    saveActor: (sceneName, actorName) =>
      call_manifest_editor_api('sceneTools.saveActor', [sceneName, actorName]),
    selectModelFile: (sceneName, actorName, fileType = 'model') =>
      call_manifest_editor_api('sceneTools.selectModelFile', [sceneName, actorName, fileType]),
    setActorPhysics: (sceneName, actorName, physics) =>
      call_manifest_editor_api('sceneTools.setActorPhysics', [sceneName, actorName, physics]),
    setActorCameraLock: (sceneName, actorName, cameraLock) =>
      call_manifest_editor_api('sceneTools.setActorCameraLock', [sceneName, actorName, cameraLock]),
    setRenderBackend: (mode, sceneName = null, cameraId = null) =>
      call_manifest_editor_api('sceneTools.setRenderBackend', [mode, sceneName, cameraId]),
    getRenderBackend: (sceneName = null, cameraId = null) =>
      call_manifest_editor_api('sceneTools.getRenderBackend', [sceneName, cameraId]),
    setVisionRenderMode: (sceneName, cameraId = null, mode = 'path_tracing') =>
      call_manifest_editor_api('sceneTools.setVisionRenderMode', [sceneName, cameraId, mode]),
    getVisionRenderMode: (sceneName, cameraId = null) =>
      call_manifest_editor_api('sceneTools.getVisionRenderMode', [sceneName, cameraId]),
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
    createScene: (sceneName) => call_manifest_editor_api('main.createScene', [sceneName]),
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
};

export const editorApi = create_dynamic_editor_api_namespace('', editorApiStatic);

// 快捷访问
// 局域网聊天室：所有跨机传输在 C++ NetworkSystem 完成，前端只通过 cefQuery 调用。
// LANChat 主事件由 C++ Editor API registry 定义为 LANChat.event。

// End of manifest-backed editor API definitions.
