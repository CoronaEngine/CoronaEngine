import { LOCALE_CHANGED_EVENT, LOCALE_STORAGE_KEY } from './index.js';

export const DOM_TEXT_TRANSLATIONS = {
  'AI 对话': 'AI Chat',
  'Actor 别名': 'Actor Alias',
  'Blockly 工作区加载中...': 'Loading Blockly workspace...',
  'Corona Editor': 'Corona Editor',
  'Corona Project Launcher': 'Corona Project Launcher',
  '主窗口渲染模式': 'Main Window Render Mode',
  '开始项目预览': 'Start Project Preview',
  '结束项目预览': 'Stop Project Preview',
  '输入场景名称': 'Enter scene name',
  '项目': 'Project',
  '保存项目': 'Save Project',
  '视图': 'View',
  '重力 (X, Y, Z)': 'Gravity (X, Y, Z)',
  '地面高度 (Floor Y)': 'Floor Height (Floor Y)',
  '地面弹性系数': 'Floor Restitution',
  '物理步长 (秒)': 'Physics Step (seconds)',
  '应用物理参数': 'Apply Physics Settings',
  '插件': 'Plugins',
  '运行项目': 'Run Project',
  '运行当前场景': 'Run Current Scene',
  '帮助': 'Help',
  '帮助文档': 'Documentation',
  '关于': 'About',
  '结束预览': 'Stop Preview',
  '添加场景': 'Add Scene',
  '创建场景': 'Create Scene',
  '上次编辑': 'Last Edited',
  '添加助手': 'Add Assistant',
  '指定 AI 助手': 'Choose AI Assistant',
  '更多': 'More',
  '全部历史记录': 'All History',
  'Python 代码': 'Python Code',
  'VLM 外观检查': 'VLM Appearance Review',
  '一键清除所有内容': 'Clear all content',
  '与同一网络下的伙伴一起创造': 'Create with people on the same network',
  '下一步': 'Next',
  '世界': 'world',
  '个': '',
  '中文': '中文',
  '临时提示': 'Hint',
  '为当前项目创建一个可编辑世界': 'Create an editable world for the current project',
  '主机': 'Host',
  '主机 IP，如 192.168.1.42': 'Host IP, e.g. 192.168.1.42',
  '主机局域网 IP': 'Host LAN IP',
  '主页': 'Home',
  '事件': 'Events',
  '二维码': 'QR Code',
  '云海浮空城': 'Floating city above clouds',
  '亮度': 'Brightness',
  '代码区': 'Code Area',
  '位置': 'Position',
  '你的昵称': 'Your nickname',
  '你想创造一个怎样的': 'What kind of',
  '例如：一座漂浮在云海之上的赛博朋克城市，永远是雨夜，霓虹倒映在湿漉漉的街道……':
    'Example: a cyberpunk city floating above the clouds, always in a rainy night, neon reflected on wet streets...',
  '保存': 'Save',
  '保存中': 'Saving',
  '保存中...': 'Saving...',
  '信号': 'Signal',
  '倒放': 'Reverse',
  '停止': 'Stop',
  '元素': 'Element',
  '光场标定': 'Light Field Calibration',
  '光照': 'Light',
  '入口场景': 'Entry Scene',
  '全局': 'Global',
  '共创': 'Co-create',
  '关闭': 'Close',
  '关闭房间': 'Close Room',
  '创建': 'Create',
  '创建中...': 'Creating...',
  '创建多人房间': 'Create Multiplayer Room',
  '创建失败': 'Create failed',
  '创建房间': 'Create Room',
  '创建新项目': 'Create New Project',
  '创建项目': 'Create Project',
  '创建世界': 'Create World',
  '创造世界': 'Create World',
  '创造中…': 'Creating...',
  '剧情模式': 'Story Mode',
  '加入': 'Join',
  '加入房间': 'Join Room',
  '加载中...': 'Loading...',
  '动画': 'Animation',
  '单位': 'Actor',
  '单位名称': 'Actor Name',
  '历史记录': 'History',
  '取消': 'Cancel',
  '发现的房间': 'Discovered Rooms',
  '发送': 'Send',
  '变换 [v3]': 'Transform [v3]',
  '口令': 'Password',
  '可以继续补充想法': 'You can keep adding ideas',
  '右键菜单': 'Context Menu',
  '同步中': 'Syncing',
  '名称': 'Name',
  '启用': 'Enable',
  '启用物理': 'Enable Physics',
  '回复': 'Reply',
  '回到主页': 'Back Home',
  '团队': 'Team',
  '固定步长': 'Fixed Step',
  '图像': 'Image',
  '场景': 'Scene',
  '场景为空，点击 + 添加对象': 'Scene is empty. Click + to add objects',
  '场景管理': 'Scene Manager',
  '坐标': 'Coordinates',
  '垃圾桶': 'Trash',
  '处理': 'Process',
  '外观': 'Looks',
  '复制': 'Copy',
  '已关闭': 'Closed',
  '已复制': 'Copied',
  '已连接': 'Connected',
  '应用': 'Apply',
  '底部': 'Bottom',
  '开始': 'Start',
  '开始预览': 'Start Preview',
  '开放': 'Public',
  '弹性': 'Restitution',
  '弹性系数': 'Restitution',
  '弹出为独立窗口': 'Pop out to separate window',
  '当前': 'Current',
  '当前文件': 'Current File',
  '快速模板': 'Quick Templates',
  '总帧': 'Total Frames',
  '恢复为100%': 'Reset to 100%',
  '悬浮 / 停靠': 'Float / Dock',
  '房主在线': 'Host Online',
  '房主端口': 'Host Port',
  '房间不存在': 'Room not found',
  '房间名称': 'Room Name',
  '房间号': 'Room ID',
  '手动加入': 'Manual Join',
  '打开': 'Open',
  '打开现有项目...': 'Open Existing Project...',
  '打开项目': 'Open Project',
  '扩展': 'Expand',
  '扫描中…': 'Scanning...',
  '执行': 'Execute',
  '拒绝': 'Reject',
  '拖入文件': 'Drop file',
  '提示词': 'Prompt',
  '搜索': 'Search',
  '操作': 'Actions',
  '故事模式': 'Story Mode',
  '文件': 'File',
  '文件管理': 'File Manager',
  '文件管理器': 'File Manager',
  '新建单位': 'New Actor',
  '新建场景': 'New Scene',
  '新建文件夹': 'New Folder',
  '新建游戏': 'New Game',
  '新建项目': 'New Project',
  '方向': 'Direction',
  '旋转': 'Rotation',
  '旋转锁定': 'Rotation Lock',
  '无': 'None',
  '无历史记录': 'No history',
  '无相机': 'No camera',
  '暂无代码': 'No code yet',
  '暂无历史记录': 'No history',
  '暂无文件或未打开项目': 'No files or project is open',
  '暂无最近记录': 'No recent records',
  '更新偏移...': 'Updating offset...',
  '最后打开': 'Last Opened',
  '最大人数': 'Max Players',
  '最近项目': 'Recent Projects',
  '本地单人协作': 'Local Solo Collaboration',
  '本机局域网 IP': 'Local LAN IP',
  '机型': 'Model',
  '权限': 'Permissions',
  '材质': 'Material',
  '极光雪原': 'Aurora snowfield',
  '查找': 'Find',
  '模型': 'Model',
  '正在加载历史记录…': 'Loading history...',
  '正在扫描局域网…': 'Scanning LAN...',
  '水墨风的仙侠秘境': 'Ink-painting fantasy realm',
  '没有选中对象': 'No object selected',
  '深海下的远古遗迹': 'Ancient ruins under the sea',
  '清除': 'Clear',
  '渲染空间': 'Render Space',
  '漂浮的群岛与天空之城': 'Floating islands and sky city',
  '刷新': 'Refresh',
  '物理': 'Physics',
  '玩家名': 'Player name',
  '用一句话描述它，AI 会替你把它构建出来': 'Describe it in one sentence and AI will build it for you',
  '生成方案': 'Generate Plan',
  '留空则公开': 'Leave empty for public',
  '确定': 'OK',
  '确认': 'Confirm',
  '确认生成': 'Confirm Generate',
  '端口': 'Port',
  '等待': 'Waiting',
  '简介': 'Description',
  '管理': 'Manage',
  '网络协作': 'Network Collaboration',
  '编辑': 'Edit',
  '缩小': 'Zoom Out',
  '缩放': 'Scale',
  '缺少上下文': 'Missing context',
  '继续': 'Continue',
  '继续游戏': 'Continue',
  '绘制': 'Draw',
  '缓存': 'Cache',
  '脚本': 'Script',
  '自动': 'Auto',
  '自定义': 'Custom',
  '自定义名字': 'Custom name',
  '自己设计': 'Design Myself',
  '节奏': 'Timing',
  '菜单': 'Menu',
  '访问密码（可选）': 'Access Password (Optional)',
  '详情': 'Details',
  '请选择': 'Select',
  '请先选择模型文件': 'Select a model file first',
  '请先选中一个物体': 'Select an object first',
  '资源': 'Resources',
  '赛博朋克雨夜都市': 'Cyberpunk rainy-night city',
  '质量': 'Mass',
  '路径异常': 'Path error',
  '返回': 'Back',
  '返回主页': 'Back Home',
  '运行': 'Run',
  '连接': 'Connect',
  '连接中': 'Connecting',
  '选择地形文件': 'Select terrain file',
  '选择模型文件': 'Select model file',
  '选择脚本文件': 'Select script file',
  '通道': 'Channel',
  '重命名': 'Rename',
  '镜头': 'Camera',
  '长者': 'Elder',
  '问一下': 'Ask',
  '阻尼': 'Damping',
  '隐藏': 'Hide',
  '集成': 'Integrated',
  '音效': 'Sound',
  '项目名称': 'Project Name',
  '项目模式': 'Project Mode',
  '项目类型': 'Project Type',
  '项目设置': 'Project Settings',
  '高质量离线渲染': 'High Quality Offline Render',
  'AI 助手': 'AI Assistant',
  '上一个': 'Previous',
  '下一个': 'Next',
  '无匹配': 'No match',
  '积木盒宽度': 'Toolbox Width',
  '适应': 'Fit',
  '固定': 'Fixed',
  '主题': 'Theme',
  '白天模式': 'Light Mode',
  '黑夜模式': 'Dark Mode',
  '跟随系统': 'Follow System',
  '拖至此处删除': 'Drag here to delete',
  '摄像机跟随 - 按住拖拽移动，点击展开': 'Camera Follow - hold to drag, click to expand',
  '重置为默认': 'Reset to Default',
  '密码(可选)': 'Password (optional)',
  '给你的房间起个名字…': 'Name your room...',
  '局域': 'LAN',
  '联机': 'Online',
  '昵称': 'Nickname',
  '密码': 'Password',
  '创建房间后将以当前项目作为联机世界，房间会在同一局域网内自动广播，伙伴无需手动输入 IP 即可发现。':
    'After creating a room, the current project becomes the multiplayer world and is broadcast on the LAN so others can discover it without entering an IP manually.',
  '请输入项目名称...': 'Enter project name...',
  '存储位置': 'Storage Location',
  '浏览': 'Browse',
  '移除': 'Remove',
  '成员': 'Members',
  '角色职责 / 人设': 'Role duties / persona',
  '密码（可选）': 'Password (optional)',
  '房主 IP（如 192.168.1.5）': 'Host IP, e.g. 192.168.1.5',
  '添加 AI 助手': 'Add AI Assistant',
  '助手名字（如 小策）': 'Assistant name, e.g. Planner',
  '人设提示词（可选，也可直接写自定义角色）': 'Persona prompt (optional, or write a custom role)',
  '作为单人聊天室继续': 'Continue as solo chat',
  '配置 AI 专家组': 'Configure AI Expert Group',
  '选择要加入本地聊天室的 Agent，也可以添加自定义角色':
    'Choose agents to join the local chat, or add a custom role',
  '添加': 'Add',
  '＋助手': '+ Assistant',
  '连接已断开': 'Disconnected',
  '所有级别': 'All Levels',
  '搜索...': 'Search...',
  '输入名称...': 'Enter name...',
  '可选': 'Optional',
  '实例名称': 'Instance Name',
  '端口 (UDP)': 'Port (UDP)',
  '停止会话': 'Stop Session',
  'IP 地址': 'IP Address',
  '对方名称': 'Peer Name',
  '连接请求已发送，等待握手...': 'Connection request sent, waiting for handshake...',
  '已连接用户数': 'Connected Users',
  '使用说明': 'Instructions',
  '房主点击"创建房间"，客户端输入房主 IP 后点击"加入房间"':
    'The host clicks "Create Room"; clients enter the host IP and click "Join Room".',
  '两端端口需要一致，默认使用 27960/UDP': 'Both sides must use the same port. Default is 27960/UDP.',
  '同时编辑同一物体时，最后写入者胜出 (LWW)': 'When editing the same object, last writer wins (LWW).',
  '跳到开始': 'Go to Start',
  '跳到结尾': 'Go to End',
  '模型别名': 'Model Alias',
  '时间轴': 'Timeline',
  '未打开文件': 'No File Open',
  '显示': 'Show',
  '地形': 'Terrain',
  '平面': 'Plane',
  '尺寸': 'Size',
  '别名': 'Alias',
  '屏幕 UI': 'Screen UI',
  '未选择模型': 'No model selected',
  '相机锁定': 'Camera Lock',
  '锁定偏移': 'Lock Offset',
  '碰撞': 'Collision',
  '包围盒': 'Bounding Box',
  '平移锁定': 'Translation Lock',
  '时间': 'Time',
  '帧': 'Frame',
  '提示:': 'Tip:',
  '长按拖拽添加片段 · 点击添加关键帧 · 双击删除':
    'Long-press drag to add clips · click to add keyframes · double-click to delete',
  '模型文件': 'Model File',
  '默认变换': 'Default Transform',
  '核心版本': 'Core Version',
  '创建时间': 'Created At',
  '视口 UI 模式': 'Viewport UI Mode',
  '摄像头移动速度': 'Camera Move Speed',
  '🔍 搜索资源(名称/中文/拼音,支持模糊)': 'Search resources (name/Chinese/Pinyin, fuzzy supported)',
  '以图搜索(本地 pHash)': 'Image Search (local pHash)',
  '重建索引': 'Rebuild Index',
  '定位到资源': 'Locate Resource',
  '导入': 'Import',
  '添加灯光': 'Add Light',
  '添加摄像头': 'Add Camera',
  '保存场景': 'Save Scene',
  '截图': 'Screenshot',
  '输出通道': 'Output Channel',
  '快速截图（保存当前输出模式到桌面）': 'Quick screenshot (save current output mode to desktop)',
  '依次切换所有通道并逐一截图保存': 'Switch through all channels and save screenshots one by one',
  '速度': 'Speed',
  '正在准备资源索引...': 'Preparing resource index...',
  '找到': 'Found',
  '项': 'items',
  '暂无匹配结果': 'No matches',
  '📦 模型': 'Model',
  '👤 单位': 'Actor',
  '🎬 场景': 'Scene',
  '🎵 多媒体': 'Media',
  '🖼 UI图片': 'UI Image',
  '📷 快速截图': 'Quick Screenshot',
  '📦 全部保存': 'Save All',
  '搜索作品中的积木': 'Search blocks in workspace',
  '设置': 'Settings',
  '相机跟随': 'Camera Follow',
  '偏移': 'Offset',
  '删除': 'Delete',
  '日志': 'Log',
  '清空': 'Clear',
  '局域网聊天': 'LAN Chat',
  '手动连接': 'Manual Connect',
  '网格': 'Grid',
  '浏览...': 'Browse...',
  '重置': 'Reset',
};

const ATTRIBUTE_NAMES = ['title', 'placeholder', 'aria-label'];
const SKIP_TAGS = new Set(['SCRIPT', 'STYLE', 'TEXTAREA']);
const textOriginals = new WeakMap();
const attrOriginals = new WeakMap();

let observer = null;
let activeLocale = '';
let translatingDocument = false;
let localeChangeHandler = null;

const OBSERVER_OPTIONS = {
  childList: true,
  subtree: true,
  characterData: true,
  attributes: true,
  attributeFilter: ATTRIBUTE_NAMES,
};

function translateText(text) {
  if (activeLocale !== 'en-US') return text;
  return DOM_TEXT_TRANSLATIONS[text] || text;
}

function preserveWhitespace(original, translated) {
  const prefix = original.match(/^\s*/)?.[0] || '';
  const suffix = original.match(/\s*$/)?.[0] || '';
  return `${prefix}${translated}${suffix}`;
}

function renderText(original) {
  const trimmed = original.trim();
  if (!trimmed) return original;
  const translated = translateText(trimmed);
  return translated === trimmed ? original : preserveWhitespace(original, translated);
}

function renderEnglishText(original) {
  const trimmed = original.trim();
  if (!trimmed) return original;
  const translated = DOM_TEXT_TRANSLATIONS[trimmed] || trimmed;
  return translated === trimmed ? original : preserveWhitespace(original, translated);
}

function translateTextNode(node) {
  let original = textOriginals.get(node);
  const current = node.nodeValue || '';

  if (original === undefined) {
    original = current;
    textOriginals.set(node, original);
  } else {
    const renderedOriginal = renderText(original);
    const renderedEnglish = renderEnglishText(original);
    if (current !== original && current !== renderedOriginal && current !== renderedEnglish) {
      original = current;
      textOriginals.set(node, original);
    }
  }

  const trimmed = original.trim();
  if (!trimmed) return;

  const nextValue = renderText(original);
  if (node.nodeValue !== nextValue) {
    node.nodeValue = nextValue;
  }
}

function renderAttributeValue(original) {
  const trimmed = original.trim();
  return translateText(trimmed) || original;
}

function renderEnglishAttributeValue(original) {
  const trimmed = original.trim();
  return DOM_TEXT_TRANSLATIONS[trimmed] || original;
}

function translateElementAttributes(element) {
  let originals = attrOriginals.get(element);
  if (!originals) {
    originals = {};
    attrOriginals.set(element, originals);
  }
  for (const attr of ATTRIBUTE_NAMES) {
    if (!element.hasAttribute(attr)) continue;
    if (!Object.hasOwn(originals, attr)) {
      originals[attr] = element.getAttribute(attr);
    } else {
      const current = element.getAttribute(attr) || '';
      const renderedOriginal = renderAttributeValue(originals[attr] || '');
      const renderedEnglish = renderEnglishAttributeValue(originals[attr] || '');
      if (current !== originals[attr] && current !== renderedOriginal && current !== renderedEnglish) {
        originals[attr] = current;
      }
    }
    const original = originals[attr] || '';
    const nextValue = renderAttributeValue(original);
    if (element.getAttribute(attr) !== nextValue) {
      element.setAttribute(attr, nextValue);
    }
  }
}

function translateNode(node) {
  if (!node) return;
  if (node.nodeType === Node.TEXT_NODE) {
    translateTextNode(node);
    return;
  }
  if (node.nodeType !== Node.ELEMENT_NODE || SKIP_TAGS.has(node.tagName)) return;

  translateElementAttributes(node);
  for (const child of node.childNodes) {
    translateNode(child);
  }
}

function translateDocument() {
  if (typeof document === 'undefined' || !document.body || translatingDocument) return;

  translatingDocument = true;
  if (observer) observer.disconnect();
  try {
    translateNode(document.body);
  } finally {
    translatingDocument = false;
    observeDocument();
  }
}

function observeDocument() {
  if (!observer || typeof document === 'undefined' || !document.body) return;
  observer.observe(document.body, OBSERVER_OPTIONS);
}

export function setupDomTranslation() {
  if (typeof window === 'undefined' || typeof document === 'undefined') return;
  stopDomTranslation();

  activeLocale = localStorage.getItem(LOCALE_STORAGE_KEY) || document.documentElement.lang || 'zh-CN';
  translateDocument();

  observer = new MutationObserver((mutations) => {
    if (translatingDocument) return;
    translatingDocument = true;
    observer.disconnect();
    try {
      for (const mutation of mutations) {
        for (const node of mutation.addedNodes) {
          translateNode(node);
        }
        if (mutation.type === 'characterData') {
          translateNode(mutation.target);
        }
        if (mutation.type === 'attributes') {
          translateElementAttributes(mutation.target);
        }
      }
    } finally {
      translatingDocument = false;
      observeDocument();
    }
  });

  observeDocument();

  localeChangeHandler = (event) => {
    activeLocale = event.detail?.locale || 'zh-CN';
    translateDocument();
  };
  window.addEventListener(LOCALE_CHANGED_EVENT, localeChangeHandler);
}

export function stopDomTranslation() {
  observer?.disconnect();
  observer = null;
  if (localeChangeHandler) {
    window.removeEventListener(LOCALE_CHANGED_EVENT, localeChangeHandler);
    localeChangeHandler = null;
  }
}
