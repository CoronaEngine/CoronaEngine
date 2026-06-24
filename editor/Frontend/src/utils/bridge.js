/**
 * Bridge Utility for QWebChannel
 * 封装了与 Python 后端的通信，支持 Promise 调用
 */

export class Bridge {
  static async callCEF(moduleName, methodName, args) {
    const request = {
      function: methodName,
      module: moduleName,
      args: args || [],
    };

    return new Promise((resolve, reject) => {
      try {
        window.cefQuery({
          request: JSON.stringify(request),
          persistent: false,
          onSuccess: (response) => {
            try {
              const jsonResponse = typeof response === 'string' ? JSON.parse(response) : response;
              console.log('CEF Response:', JSON.stringify(jsonResponse, null, 2));
              if (
                jsonResponse &&
                (jsonResponse.success === false ||
                  jsonResponse.status === 'error' ||
                  jsonResponse.type === 'error' ||
                  jsonResponse.error)
              ) {
                reject(new Error(jsonResponse.error || jsonResponse.message || 'Backend error'));
              } else {
                resolve(jsonResponse);
              }
            } catch (e) {
              resolve(response);
            }
          },
          onFailure: (error_code, error_message) => {
            reject(new Error(`CEF Error (${error_code}): ${error_message}`));
          },
        });
      } catch (error) {
        reject(error);
      }
    });
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

// 快捷访问
export const sceneService = {
  createActor: (sceneName, objPath) =>
    Bridge.callCEF('SceneTools', 'create_actor', [sceneName, objPath]),
  removeActor: (sceneName, actorName) =>
    Bridge.callCEF('SceneTools', 'remove_actor', [sceneName, actorName]),
  renameActor: (sceneName, actorName, name) =>
    Bridge.callCEF('SceneTools', 'rename_actor', [sceneName, actorName, name]),
  createScene: (sceneName) => Bridge.callCEF('SceneTools', 'create_scene', [sceneName]),

  sunDirection: (sceneName, enable, direction) =>
    Bridge.callCEF('SceneTools', 'sun_direction', [sceneName, enable, direction]),
  floorGrid: (sceneName, enabled) =>
    Bridge.callCEF('SceneTools', 'floor_grid', [sceneName, enabled]),
  setPhysicsParams: (sceneName, params) =>
    Bridge.callCEF('SceneTools', 'set_physics_params', [
      sceneName,
      params.gravity,
      params.floor_y,
      params.floor_restitution,
      params.fixed_dt,
    ]),
  getPhysicsParams: (sceneName) => Bridge.callCEF('SceneTools', 'get_physics_params', [sceneName]),
  selectScreenshotPath: (sceneName, cameraName) =>
    Bridge.callCEF('SceneTools', 'select_screenshot_path', [sceneName, cameraName]),
  saveScreenshot: (sceneName, path, cameraName) =>
    Bridge.callCEF('SceneTools', 'save_screenshot', [sceneName, path, cameraName]),
  setOutputMode: (sceneName, cameraName, mode) =>
    Bridge.callCEF('SceneTools', 'set_output_mode', [sceneName, cameraName, mode]),
  getOutputMode: (sceneName, cameraName) =>
    Bridge.callCEF('SceneTools', 'get_output_mode', [sceneName, cameraName]),
  isVisionAvailable: () => Bridge.callCEF('SceneTools', 'is_vision_available', []),
  setRenderBackend: (mode, sceneName = null, cameraId = null) =>
    Bridge.callCEF('SceneTools', 'set_render_backend', [mode, sceneName, cameraId]),
  getRenderBackend: (sceneName = null, cameraId = null) =>
    Bridge.callCEF('SceneTools', 'get_render_backend', [sceneName, cameraId]),
  setVisionRenderMode: (sceneName, cameraId = null, mode = 'path_tracing') =>
    Bridge.callCEF('SceneTools', 'set_vision_render_mode', [sceneName, cameraId, mode]),
  getVisionRenderMode: (sceneName, cameraId = null) =>
    Bridge.callCEF('SceneTools', 'get_vision_render_mode', [sceneName, cameraId]),
  createCameraView: (sceneName, name = null) =>
    Bridge.callCEF('SceneTools', 'create_camera_view', [sceneName, name]),
  openCameraView: (sceneName, cameraId) =>
    Bridge.callCEF('SceneTools', 'open_camera_view', [sceneName, cameraId]),
  closeCameraView: (sceneName, cameraId) =>
    Bridge.callCEF('SceneTools', 'close_camera_view', [sceneName, cameraId]),
  renameCameraView: (sceneName, cameraId, name) =>
    Bridge.callCEF('SceneTools', 'rename_camera_view', [sceneName, cameraId, name]),
  listCameraViews: (sceneName) =>
    Bridge.callCEF('SceneTools', 'list_camera_views', [sceneName]),
  updateCameraView: (sceneName, cameraId, state) =>
    Bridge.callCEF('SceneTools', 'update_camera_view', [sceneName, cameraId, state]),
  deleteCamera: (sceneName, cameraId) =>
    Bridge.callCEF('SceneTools', 'delete_camera', [sceneName, cameraId]),
  loadVisionScene: (path) => Bridge.callCEF('SceneTools', 'load_vision_scene', [path]),
  listActorTree: (sceneName) => Bridge.callCEF('SceneTools', 'list_actor_tree', [sceneName]),
  listSceneTree: (sceneName) => Bridge.callCEF('SceneTools', 'list_scene_tree', [sceneName]),
  openSceneActor: (sceneName, actorName) =>
    Bridge.callCEF('SceneTools', 'open_actor', [sceneName, actorName]),
  focusActor: (sceneName, actorName, cameraName) =>
    Bridge.callCEF('SceneTools', 'focus_actor', [sceneName, actorName, cameraName]),
  /** 鼠标在3D视口中拾取物体（异步：首次调用设置拾取，~50ms后重试获取结果） */
  pickActor: (sceneName, x, y, vpWidth, vpHeight) =>
    Bridge.callCEF('SceneTools', 'pick_actor_at_pixel', [sceneName, x, y, vpWidth, vpHeight]),
  /** 播放已导入的音频资源 */
  playAudio: (resourceId, loop) =>
    Bridge.callCEF('SceneTools', 'play_audio', [resourceId, loop]),
  /** 停止播放音频资源 */
  stopAudio: (resourceId) =>
    Bridge.callCEF('SceneTools', 'stop_audio', [resourceId]),

  getScene: (sceneId) => Bridge.callCEF('SceneDatas', 'get_scene', [sceneId]),
  getActor: (sceneId, actorId) => Bridge.callCEF('SceneDatas', 'get_actor', [sceneId, actorId]),
  actorOperation: (scene_name, actor_name, operation, vector) =>
    Bridge.callCEF('SceneDatas', 'actor_operation', [scene_name, actor_name, operation, vector]),
  /** 仅触发写盘：Transform 已由快速通道写入 SharedDataHub */
  saveActor: (sceneName, actorName) =>
    Bridge.callCEF('SceneDatas', 'save_actor', [sceneName, actorName]),
  selectModelFileDialog: (sceneId, actorId, fileType) =>
    Bridge.callCEF('SceneDatas', 'select_model_file', [sceneId, actorId, fileType]),
  setCameraLock: (sceneName, actorName, enabled) =>
    Bridge.callCEF('SceneDatas', 'actor_operation', [sceneName, actorName, 'SetCameraLock', [enabled]]),
  setCameraLockOffset: (sceneName, actorName, offset) =>
    Bridge.callCEF('SceneDatas', 'actor_operation', [sceneName, actorName, 'SetCameraLockOffset', offset]),
  setCameraLockRotation: (sceneName, actorName, rotation) =>
    Bridge.callCEF('SceneDatas', 'actor_operation', [sceneName, actorName, 'SetCameraLockRotation', rotation]),
};

export const projectService = {
  OnInit: () => Bridge.callCEF('MainView', 'on_init', []),
  importResourceFileByDialog: (sceneName, fileType) =>
    Bridge.callCEF('MainView', 'import_resource_file', [sceneName, fileType]),
  sceneSave: (sceneName) => Bridge.callCEF('MainView', 'scene_save', [sceneName]),
  sceneSwitch: (currentName, toName) =>
    Bridge.callCEF('MainView', 'switch_scene', [currentName, toName]),
  createNewScene: (sceneName) => Bridge.callCEF('MainView', 'create_new_scene', [sceneName]),
  removeScene: (scenePath) => Bridge.callCEF('MainView', 'remove_scene', [scenePath]),

  // 菜单数据接口
  getMenuData: () => Bridge.callCEF('MainView', 'get_menu_data', []),
  updateViewToolState: (toolId, enabled) =>
    Bridge.callCEF('MainView', 'update_view_tool_state', [toolId, enabled]),

  runProject: (scenePath) =>
    Bridge.callCEF('MainView', 'run_project', scenePath ? [scenePath] : []),

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
  createPanelTab: (panelId, routePath, width, height, dockingPos) =>
    Bridge.callDockCommand({ cmd: 'createPanelTab', panelId, routePath, width, height, dockingPos }),
  closeThisTab: (panelId) =>
    Bridge.callDockCommand({ cmd: 'closeThisTab', panelId }),
  closePanelTab: (tabId, panelId) =>
    Bridge.callDockCommand({ cmd: 'closePanelTab', tabId, panelId }),
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
  closeProcess: () => Bridge.callCEF('CoronaEditor', 'close_process'),
  callDockFunction: (routename, functionname, args) => {
    // 单 CEF Tab 架构：直接调 window.xxx，不需要 Python 中转
    const fn = window[functionname];
    if (typeof fn === 'function') {
      try { fn(...(args || [])); } catch (e) { /* ignore */ }
    }
    return Promise.resolve({ success: true });
  },
  start_engine: () => Bridge.callCEF('CoronaEditor', 'start_corona_engine', []),
};

export const aiService = {
  sendMessageToAIStream: (payload) =>
    Bridge.callCEF('AITool', 'send_message_to_ai_stream', [payload]),
  readLocalFileAsBase64: (filePath) =>
    Bridge.callCEF('AITool', 'read_local_file_as_base64', [filePath]),
  generateHint: (elementType, context = {}) =>
    Bridge.callCEF('AITool', 'generate_hint', [elementType, context]),
};

export const aiClient = {
  chatStream: (request) => Bridge.callCEF('AITool', 'ai_rpc', [request]),
  cancelRequest: (requestId) =>
    Bridge.callCEF('AITool', 'ai_rpc', [
      {
        operation: 'request.cancel',
        request_id: requestId,
      },
    ]),
  getRequestStatus: (requestId) =>
    Bridge.callCEF('AITool', 'ai_rpc', [
      {
        operation: 'request.status',
        request_id: requestId,
      },
    ]),
};

// 局域网聊天室：所有跨机传输在 C++ NetworkSystem 完成，前端只通过 cefQuery 调用。
// C++ 侧通过 __coronaEmit('lanchat-event', event) 把房间消息推回前端
// （coronaEventBus.on('lanchat-event')），事件信封带 channel: 'lanchat'。
//
// 注意：deal_func_from_js 用 create_success_response 把返回值包成
// { success, data, timestamp }，业务结果在 .data 里。这里统一解包，
// 让 store 直接拿到 { ok, ip, ... } 业务对象（约定同 SceneBar：result?.data ?? result）。
const _unwrap = (res) => (res && res.data !== undefined ? res.data : res);

export const lanChatService = {
  // 房主开房：{ room, password, port? } -> { ok, ip, port, room } | { ok:false, error }
  startRoom: (payload) =>
    Bridge.callCEF('LANChat', 'start_room', [payload]).then(_unwrap),
  // 单人本地房：不启动 NetworkSystem 协作会话
  startLocalRoom: (payload) =>
    Bridge.callCEF('LANChat', 'start_local_room', [payload]).then(_unwrap),
  // 房主关房 -> { ok }
  stopRoom: () => Bridge.callCEF('LANChat', 'stop_room', [{}]).then(_unwrap),
  // 关闭单人本地房，不停止 NetworkSystem 协作会话
  stopLocalRoom: () => Bridge.callCEF('LANChat', 'stop_local_room', [{}]).then(_unwrap),
  // 加入房间：{ ip, port, room, password, nickname } -> { ok, members, history } | { ok:false, code }
  joinRoom: (payload) =>
    Bridge.callCEF('LANChat', 'join_room', [payload]).then(_unwrap),
  // 显式读取当前房间历史，用于开房后兜底恢复持久化记录
  getHistory: () => Bridge.callCEF('LANChat', 'get_history', [{}]).then(_unwrap),
  // 读取持久化历史房间列表，打开 Dock 时展示给用户选择
  listHistoryRooms: () => Bridge.callCEF('LANChat', 'list_history_rooms', [{}]).then(_unwrap),
  // 读取指定持久化房间历史，不自动进入该房间
  loadHistoryRoom: (room) =>
    Bridge.callCEF('LANChat', 'load_history_room', [{ room }]).then(_unwrap),
  // 离开房间 -> { ok }
  leaveRoom: () => Bridge.callCEF('LANChat', 'leave_room', [{}]).then(_unwrap),
  // 发送消息：{ text } -> { ok } | { ok:false, error }
  sendMessage: (text, options = {}) =>
    Bridge.callCEF('LANChat', 'send_message', [{ text, ...(options || {}) }]).then(_unwrap),
  // 获取本机局域网 IP -> { ok, ip, port }
  getLocalIp: () => Bridge.callCEF('LANChat', 'get_local_ip', [{}]).then(_unwrap),
  // 添加 AI 助手：{ name, persona } -> { ok, agent_id, name } | { ok:false, error }
  addAgent: (payload) =>
    Bridge.callCEF('LANChat', 'add_agent', [payload]).then(_unwrap),
  // 移除 AI 助手：{ agent_id } -> { ok }
  removeAgent: (agentId) =>
    Bridge.callCEF('LANChat', 'remove_agent', [{ agent_id: agentId }]).then(_unwrap),
  // 列出 agent 名册 -> { ok, agents:[{agent_id,name,owner}] }
  listAgents: () => Bridge.callCEF('LANChat', 'list_agents', [{}]).then(_unwrap),
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
    Bridge.callCEF('ScratchTool', 'execute_python_code', [
      code,
      mode ?? 0,
      sceneName ?? '',
      actorName ?? '',
      targetType || 'actor',
    ]),

  saveBlocklyTarget: (payload) =>
    Bridge.callCEF('ScratchTool', 'save_blockly_target', [payload || {}]),

  loadBlocklyTarget: (payload) =>
    Bridge.callCEF('ScratchTool', 'load_blockly_target', [payload || {}]),

  startGamePreview: (payload = { scope: 'project' }) =>
    Bridge.callCEF('ScratchTool', 'start_game_preview', [payload]),

  stopGamePreview: () =>
    Bridge.callCEF('ScratchTool', 'stop_game_preview', []),

  getGamePreviewStatus: () =>
    Bridge.callCEF('ScratchTool', 'get_game_preview_status', []),

  /**
   * 停止当前正在执行的脚本
   */
  stopScriptExecution: () =>
    Bridge.callCEF('ScratchTool', 'stop_script_execution', []),

  /**
   * 查询当前脚本执行状态
   * @returns {Promise<{status: 'running'|'idle'}>}
   */
  getScriptStatus: () =>
    Bridge.callCEF('ScratchTool', 'get_script_status', []),

  /**
   * 发送键盘事件到积木脚本
   * @param {string} key - 按键名 (如 'KeyA', 'Space', 'ArrowUp')
   * @param {string} modifiers - 修饰键 (如 'Ctrl,Shift')
   */
  sendKeyEvent: (key, modifiers, displayKey) =>
    Bridge.callCEF('ScratchTool', 'key_event', [key, modifiers || '', displayKey || key]),

  /**
   * 发送键盘释放事件到积木脚本
   */
  sendKeyUpEvent: (key, displayKey) =>
    Bridge.callCEF('ScratchTool', 'key_release', [key, displayKey || key]),

  /**
   * 发送鼠标事件到积木脚本
   */
  sendMouseEvent: (eventType, button, x, y) =>
    Bridge.callCEF('ScratchTool', 'mouse_event', [eventType, button || '', x || 0, y || 0]),
};

export const projectLauncherService = {
  // 获取默认项目路径
  getDefaultProjectPath: () => Bridge.callCEF('ProjectLauncher', 'get_default_project_path', []),
  // 浏览文件夹
  browseFolder: (default_path) =>
    Bridge.callCEF('ProjectLauncher', 'browse_folder', [default_path]),
  // 浏览并选择项目文件 (.ini)
  openProjectFile: () => Bridge.callCEF('ProjectLauncher', 'open_project_file', []),
  // 创建项目
  createProject: (projectData) =>
    Bridge.callCEF('ProjectLauncher', 'create_project', [projectData]),
  // 创建 AI 世界项目：自动命名 + 存到引擎 data 目录，无需 name/path
  // worldData: { mode: 'story'|'creative', prompt: string } -> { name, path }
  createWorldProject: (worldData) =>
    Bridge.callCEF('ProjectLauncher', 'create_world_project', [worldData]),
  // 打开项目（执行加载逻辑）
  openProject: (projectPath) => Bridge.callCEF('ProjectLauncher', 'open_project', [projectPath]),
  // 设置项目模式 (2D/3D/渲染)
  setProjectMode: (mode, settings) =>
    Bridge.callCEF('ProjectLauncher', 'set_project_mode', [{ mode, settings }]),
  // 获取版本信息
  getAppVersion: () => Bridge.callCEF('ProjectLauncher', 'get_app_version', []),
  // 获取最近项目列表
  getRecentProjects: () => Bridge.callCEF('ProjectLauncher', 'get_recent_projects', []),
};

export const fileService = {
  getProjectInfo: () => Bridge.callCEF('FileManager', 'get_project_info', []),
  getFiles: (relPath) => Bridge.callCEF('FileManager', 'get_files', [relPath]),
  getFileTree: (relPath) => Bridge.callCEF('FileManager', 'get_file_tree', [relPath]),
  createFolder: (path, folderName) =>
    Bridge.callCEF('FileManager', 'create_folder', [path, folderName]),
  createFile: (path, fileName, type) =>
    Bridge.callCEF('FileManager', 'create_file', [path, fileName, type]),
  deleteItem: (path) => Bridge.callCEF('FileManager', 'delete_item', [path]),
  renameItem: (oldPath, newName) =>
    Bridge.callCEF('FileManager', 'rename_item', [oldPath, newName]),
  openFile: (filePath, fileType) =>
    Bridge.callCEF('FileManager', 'open_file', [filePath, fileType]),
};

export const logService = {
  // 对应 Python 中的 LogTool.set_log_ready
  setLogReady: () => Bridge.callCEF('LogTool', 'set_log_ready', []),
  // 如果需要，也可以添加关闭接口
  setLogClose: () => Bridge.callCEF('LogTool', 'set_log_close', []),
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
      ? Bridge.callCEF('ResourceSearch', 'prepare_index', [CURRENT_CALLER])
      : resourceSearchDisabled(),
  fuzzySearch: (query, topK = 20, typeFilter = null) =>
    RESOURCE_SEARCH_ENABLED
      ? Bridge.callCEF('ResourceSearch', 'fuzzy_search',
        [query, topK, typeFilter, CURRENT_CALLER])
      : resourceSearchDisabled(),
  imageSearch: (imageB64, topK = 20, threshold = 10) =>
    RESOURCE_SEARCH_ENABLED
      ? Bridge.callCEF('ResourceSearch', 'image_search',
        [imageB64, topK, threshold, CURRENT_CALLER])
      : resourceSearchDisabled(),
  listTypes: () =>
    RESOURCE_SEARCH_ENABLED
      ? Bridge.callCEF('ResourceSearch', 'list_types', [CURRENT_CALLER])
      : resourceSearchDisabled(),
  rebuildIndex: () =>
    RESOURCE_SEARCH_ENABLED
      ? Bridge.callCEF('ResourceSearch', 'rebuild_index', [CURRENT_CALLER])
      : resourceSearchDisabled(),
  getStats: () =>
    RESOURCE_SEARCH_ENABLED
      ? Bridge.callCEF('ResourceSearch', 'get_stats', [CURRENT_CALLER])
      : resourceSearchDisabled(),
  markIndexDirty: (reason = 'frontend') =>
    RESOURCE_SEARCH_ENABLED
      ? Bridge.callCEF('ResourceSearch', 'mark_index_dirty',
        [reason, CURRENT_CALLER])
      : resourceSearchDisabled(),
  focusActor: (sceneName, actorName) =>
    RESOURCE_SEARCH_ENABLED
      ? Bridge.callCEF('ResourceSearch', 'focus_actor',
        [sceneName, actorName, CURRENT_CALLER])
      : resourceSearchDisabled(),
};

export const projectSettingsService = {
  // 获取当前激活项目的配置
  getActiveProjectInfo: () => Bridge.callCEF('ProjectSettings', 'get_active_project_info', []),
  // 保存当前激活项目的配置
  saveActiveProjectInfo: (settings) =>
    Bridge.callCEF('ProjectSettings', 'save_active_project_info', [settings]),
  // 浏览当前项目中的场景文件
  browseSceneFile: () => Bridge.callCEF('ProjectSettings', 'browse_scene_file', []),
};

export const networkService = {
  startSession: (instanceName, projectId, port = 27960, role = 'host') =>
    Bridge.callCEF('Network', 'start_session', [instanceName, projectId, port, role]).then(_unwrap),
  stopSession: () => Bridge.callCEF('Network', 'stop_session').then(_unwrap),
  getPeerCount: () => Bridge.callCEF('Network', 'get_peer_count').then(_unwrap),
  getSessionInfo: () => Bridge.callCEF('Network', 'get_session_info').then(_unwrap),
  connectToPeer: (ip, port, peerName) =>
    Bridge.callCEF('Network', 'connect_to_peer', [ip, port, peerName]).then(_unwrap),
  setProjectRoot: (projectRoot) =>
    Bridge.callCEF('Network', 'set_project_root', [projectRoot]).then(_unwrap),
  broadcastActorCreate: (actorGuid, sceneName, modelPath, actorData) =>
    Bridge.callCEF('Network', 'broadcast_actor_create', [actorGuid, sceneName, modelPath, actorData]).then(_unwrap),
  broadcastActorTransform: (actorGuid, sceneName, actorData) =>
    Bridge.callCEF('Network', 'broadcast_actor_transform', [actorGuid, sceneName, actorData]).then(_unwrap),
  broadcastActorDelete: (actorGuid, sceneName, actorName) =>
    Bridge.callCEF('Network', 'broadcast_actor_delete', [actorGuid, sceneName, actorName]).then(_unwrap),
  requestSceneSnapshot: (sceneName) =>
    Bridge.callCEF('Network', 'request_actor_scene_snapshot', [sceneName]).then(_unwrap),
  broadcastSceneSnapshot: (sceneName, snapshot) =>
    Bridge.callCEF('Network', 'broadcast_actor_scene_snapshot', [sceneName, snapshot]).then(_unwrap),
  broadcastActorStateUpdate: (actorGuid, sceneName, actorData) =>
    Bridge.callCEF('Network', 'broadcast_actor_state_update',
      [actorGuid, sceneName, actorData]).then(_unwrap),
  /** 轮询待创建的远程 Actor（文件传输完成后触发创建） */
  pollPendingActorCreate: () =>
    Bridge.callCEF('Network', 'poll_pending_actor_create', []).then(_unwrap),
  /** 轮询远程 Actor transform delta */
  pollPendingActorTransform: () =>
    Bridge.callCEF('Network', 'poll_pending_actor_transform', []).then(_unwrap),
  pollPendingActorDelete: () =>
    Bridge.callCEF('Network', 'poll_pending_actor_delete', []).then(_unwrap),
  pollPendingSceneSnapshotRequest: () =>
    Bridge.callCEF('Network', 'poll_pending_actor_scene_snapshot_request', []).then(_unwrap),
  pollPendingSceneSnapshot: () =>
    Bridge.callCEF('Network', 'poll_pending_actor_scene_snapshot', []).then(_unwrap),
  pollPendingActorStateUpdate: () =>
    Bridge.callCEF('Network', 'poll_pending_actor_state_update', []).then(_unwrap),
  /** 暂停/恢复同步（Actor 创建期间避免 seq_id 碰撞） */
  setSyncPaused: (paused) =>
    Bridge.callCEF('Network', 'set_sync_paused', [paused]).then(_unwrap),
  /** 注册 actor_guid -> 本地 Actor handle 映射，作为后续稳定同步的锚点 */
  registerActorIdentity: (actorGuid, actorHandle, locallyOwned = true) =>
    Bridge.callCEF('Network', 'register_actor_identity',
      [actorGuid, String(actorHandle || ''), Boolean(locallyOwned)]).then(_unwrap),
  claimActorOwnership: (actorGuid) =>
    Bridge.callCEF('Network', 'claim_actor_ownership', [actorGuid]).then(_unwrap),
};
