import { LOCALE_CHANGED_EVENT, LOCALE_STORAGE_KEY } from './index.js';
import enUS from './messages/en-US.js';
import zhCN from './messages/zh-CN.js';
import { CUSTOM_BLOCK_TRANSLATIONS } from './customBlockTranslations.js';

function buildLocaleTextTranslations(source, target, translations = {}) {
  for (const [key, sourceValue] of Object.entries(source || {})) {
    const targetValue = target?.[key];
    if (
      sourceValue &&
      targetValue &&
      typeof sourceValue === 'object' &&
      typeof targetValue === 'object' &&
      !Array.isArray(sourceValue) &&
      !Array.isArray(targetValue)
    ) {
      buildLocaleTextTranslations(sourceValue, targetValue, translations);
      continue;
    }
    if (
      typeof sourceValue === 'string' &&
      typeof targetValue === 'string' &&
      sourceValue !== targetValue &&
      !Object.hasOwn(translations, sourceValue)
    ) {
      translations[sourceValue] = targetValue;
    }
  }
  return translations;
}

const LOCALE_TEXT_TRANSLATIONS = buildLocaleTextTranslations(zhCN, enUS);

const EXPLICIT_DOM_TEXT_TRANSLATIONS = {
  房主: 'Host',
  无法连接到房主: 'Unable to Connect to the Host',
  '正在连接房主…': 'Connecting to Host...',
  'Actor 别名': 'Actor Alias',
  'Blockly 工作区加载中...': 'Loading Blockly workspace...',
  'Corona Editor': 'Corona Editor',
  'Corona Project Launcher': 'Corona Project Launcher',
  主窗口渲染模式: 'Main Window Render Mode',
  开始项目预览: 'Start Project Preview',
  结束项目预览: 'Stop Project Preview',
  输入场景名称: 'Enter scene name',
  项目: 'Project',
  保存项目: 'Save Project',
  视图: 'View',
  '重力 (X, Y, Z)': 'Gravity (X, Y, Z)',
  '地面高度 (Floor Y)': 'Floor Height (Floor Y)',
  地面弹性系数: 'Floor Restitution',
  '物理步长 (秒)': 'Physics Step (seconds)',
  应用物理参数: 'Apply Physics Settings',
  插件: 'Plugins',
  运行项目: 'Run Project',
  运行当前场景: 'Run Current Scene',
  帮助: 'Help',
  帮助文档: 'Documentation',
  关于: 'About',
  结束预览: 'Stop Preview',
  添加场景: 'Add Scene',
  创建场景: 'Create Scene',
  上次编辑: 'Last Edited',
  更多: 'More',
  'Python 代码': 'Python Code',
  一键清除所有内容: 'Clear all content',
  与同一网络下的伙伴一起创造: 'Create with people on the same network',
  下一步: 'Next',
  世界: 'world',
  个: '',
  中文: '中文',
  临时提示: 'Hint',
  为当前项目创建一个可编辑世界: 'Create an editable world for the current project',
  主机: 'Host',
  '主机 IP，如 192.168.1.42': 'Host IP, e.g. 192.168.1.42',
  '主机局域网 IP': 'Host LAN IP',
  主页: 'Home',
  事件: 'Events',
  二维码: 'QR Code',
  云海浮空城: 'Floating city above clouds',
  亮度: 'Brightness',
  代码区: 'Code Area',
  位置: 'Position',
  你想创造一个怎样的: 'What kind of',
  '例如：一座漂浮在云海之上的赛博朋克城市，永远是雨夜，霓虹倒映在湿漉漉的街道……':
    'Example: a cyberpunk city floating above the clouds, always in a rainy night, neon reflected on wet streets...',
  保存: 'Save',
  保存中: 'Saving',
  '保存中...': 'Saving...',
  信号: 'Signal',
  倒放: 'Reverse',
  停止: 'Stop',
  元素: 'Element',
  光场标定: 'Light Field Calibration',
  光照: 'Light',
  入口场景: 'Entry Scene',
  全局: 'Global',
  关闭: 'Close',
  创建: 'Create',
  '创建中...': 'Creating...',
  创建失败: 'Create failed',
  创建房间: 'Create Room',
  创建新项目: 'Create New Project',
  创建项目: 'Create Project',
  创建世界: 'Create World',
  创造世界: 'Create World',
  '创造中…': 'Creating...',
  剧情模式: 'Story Mode',
  加入: 'Join',
  加入房间: 'Join Room',
  '加载中...': 'Loading...',
  动画: 'Animation',
  单位: 'Actor',
  单位名称: 'Actor Name',
  取消: 'Cancel',
  发现的房间: 'Discovered Rooms',
  发送: 'Send',
  '变换 [v3]': 'Transform [v3]',
  口令: 'Password',
  右键菜单: 'Context Menu',
  名称: 'Name',
  启用: 'Enable',
  启用物理: 'Enable Physics',
  回复: 'Reply',
  回到主页: 'Back Home',
  团队: 'Team',
  固定步长: 'Fixed Step',
  图像: 'Image',
  场景: 'Scene',
  '场景为空，点击 + 添加对象': 'Scene is empty. Click + to add objects',
  场景管理: 'Scene Manager',
  坐标: 'Coordinates',
  垃圾桶: 'Trash',
  处理: 'Process',
  外观: 'Looks',
  复制: 'Copy',
  已复制: 'Copied',
  已连接: 'Connected',
  应用: 'Apply',
  底部: 'Bottom',
  开始: 'Start',
  开始预览: 'Start Preview',
  开放: 'Public',
  弹性: 'Restitution',
  弹性系数: 'Restitution',
  弹出为独立窗口: 'Pop out to separate window',
  当前: 'Current',
  当前文件: 'Current File',
  快速模板: 'Quick Templates',
  总帧: 'Total Frames',
  '恢复为100%': 'Reset to 100%',
  '悬浮 / 停靠': 'Float / Dock',
  房间名称: 'Room Name',
  房间号: 'Room ID',
  手动加入: 'Manual Join',
  打开: 'Open',
  '打开现有项目...': 'Open Existing Project...',
  打开项目: 'Open Project',
  扩展: 'Expand',
  '扫描中…': 'Scanning...',
  执行: 'Execute',
  拒绝: 'Reject',
  拖入文件: 'Drop file',
  提示词: 'Prompt',
  搜索: 'Search',
  操作: 'Actions',
  故事模式: 'Story Mode',
  文件: 'File',
  文件管理: 'File Manager',
  文件管理器: 'File Manager',
  新建单位: 'New Actor',
  新建场景: 'New Scene',
  新建文件夹: 'New Folder',
  新建游戏: 'New Game',
  新建项目: 'New Project',
  方向: 'Direction',
  旋转: 'Rotation',
  旋转锁定: 'Rotation Lock',
  无: 'None',
  无相机: 'No camera',
  暂无代码: 'No code yet',
  暂无文件或未打开项目: 'No files or project is open',
  暂无最近记录: 'No recent records',
  '更新偏移...': 'Updating offset...',
  最后打开: 'Last Opened',
  最大人数: 'Max Players',
  最近项目: 'Recent Projects',
  '本机局域网 IP': 'Local LAN IP',
  机型: 'Model',
  权限: 'Permissions',
  材质: 'Material',
  极光雪原: 'Aurora snowfield',
  查找: 'Find',
  模型: 'Model',
  '正在扫描局域网…': 'Scanning LAN...',
  水墨风的仙侠秘境: 'Ink-painting fantasy realm',
  没有选中对象: 'No object selected',
  深海下的远古遗迹: 'Ancient ruins under the sea',
  清除: 'Clear',
  渲染空间: 'Render Space',
  漂浮的群岛与天空之城: 'Floating islands and sky city',
  刷新: 'Refresh',
  物理: 'Physics',
  玩家名: 'Player name',
  '用一句话描述它，AI 会生成一步一步的专属搭建任务':
    'Describe it in one sentence and AI will generate a step-by-step personalized task plan',
  留空则公开: 'Leave empty for public',
  确定: 'OK',
  确认: 'Confirm',
  端口: 'Port',
  等待: 'Waiting',
  简介: 'Description',
  管理: 'Manage',
  网络协作: 'Network Collaboration',
  编辑: 'Edit',
  缩小: 'Zoom Out',
  缩放: 'Scale',
  缺少上下文: 'Missing context',
  继续: 'Continue',
  继续游戏: 'Continue',
  绘制: 'Draw',
  缓存: 'Cache',
  脚本: 'Script',
  自动: 'Auto',
  自定义: 'Custom',
  自定义名字: 'Custom name',
  菜单: 'Menu',
  '访问密码（可选）': 'Access Password (Optional)',
  详情: 'Details',
  请选择: 'Select',
  请先选择模型文件: 'Select a model file first',
  请先选中一个物体: 'Select an object first',
  资源: 'Resources',
  赛博朋克雨夜都市: 'Cyberpunk rainy-night city',
  质量: 'Mass',
  路径异常: 'Path error',
  返回: 'Back',
  返回主页: 'Back Home',
  运行: 'Run',
  连接: 'Connect',
  选择地形文件: 'Select terrain file',
  选择模型文件: 'Select model file',
  选择脚本文件: 'Select script file',
  通道: 'Channel',
  重命名: 'Rename',
  镜头: 'Camera',
  问一下: 'Ask',
  阻尼: 'Damping',
  隐藏: 'Hide',
  集成: 'Integrated',
  音效: 'Sound',
  项目名称: 'Project Name',
  项目模式: 'Project Mode',
  项目类型: 'Project Type',
  项目设置: 'Project Settings',
  高质量离线渲染: 'High Quality Offline Render',
  'AI 助手': 'AI Assistant',
  上一个: 'Previous',
  下一个: 'Next',
  无匹配: 'No match',
  积木盒宽度: 'Toolbox Width',
  适应: 'Fit',
  固定: 'Fixed',
  主题: 'Theme',
  白天模式: 'Light Mode',
  黑夜模式: 'Dark Mode',
  跟随系统: 'Follow System',
  拖至此处删除: 'Drag here to delete',
  '摄像机跟随 - 按住拖拽移动，点击展开': 'Camera Follow - hold to drag, click to expand',
  重置为默认: 'Reset to Default',
  '密码(可选)': 'Password (optional)',
  '给你的房间起个名字…': 'Name your room...',
  局域: 'LAN',
  联机: 'Online',
  昵称: 'Nickname',
  密码: 'Password',
  '创建房间后将以当前项目作为联机世界，房间会在同一局域网内自动广播，伙伴无需手动输入 IP 即可发现。':
    'After creating a room, the current project becomes the multiplayer world and is broadcast on the LAN so others can discover it without entering an IP manually.',
  '请输入项目名称...': 'Enter project name...',
  存储位置: 'Storage Location',
  浏览: 'Browse',
  移除: 'Remove',
  成员: 'Members',
  '角色职责 / 人设': 'Role duties / persona',
  '密码（可选）': 'Password (optional)',
  '添加 AI 助手': 'Add AI Assistant',
  '配置 AI 专家组': 'Configure AI Expert Group',
  '选择要加入本地聊天室的 Agent，也可以添加自定义角色':
    'Choose agents to join the local chat, or add a custom role',
  添加: 'Add',
  '＋助手': '+ Assistant',
  所有级别: 'All Levels',
  '搜索...': 'Search...',
  '输入名称...': 'Enter name...',
  可选: 'Optional',
  实例名称: 'Instance Name',
  '端口 (UDP)': 'Port (UDP)',
  停止会话: 'Stop Session',
  'IP 地址': 'IP Address',
  对方名称: 'Peer Name',
  '连接请求已发送，等待握手...': 'Connection request sent, waiting for handshake...',
  已连接用户数: 'Connected Users',
  使用说明: 'Instructions',
  '房主点击"创建房间"，客户端输入房主 IP 后点击"加入房间"':
    'The host clicks "Create Room"; clients enter the host IP and click "Join Room".',
  '两端端口需要一致，默认使用 27960/UDP':
    'Both sides must use the same port. Default is 27960/UDP.',
  '同时编辑同一物体时，最后写入者胜出 (LWW)':
    'When editing the same object, last writer wins (LWW).',
  跳到开始: 'Go to Start',
  跳到结尾: 'Go to End',
  模型别名: 'Model Alias',
  时间轴: 'Timeline',
  未打开文件: 'No File Open',
  显示: 'Show',
  地形: 'Terrain',
  平面: 'Plane',
  尺寸: 'Size',
  别名: 'Alias',
  '屏幕 UI': 'Screen UI',
  未选择模型: 'No model selected',
  相机锁定: 'Camera Lock',
  锁定偏移: 'Lock Offset',
  碰撞: 'Collision',
  包围盒: 'Bounding Box',
  平移锁定: 'Translation Lock',
  时间: 'Time',
  帧: 'Frame',
  '提示:': 'Tip:',
  '长按拖拽添加片段 · 点击添加关键帧 · 双击删除':
    'Long-press drag to add clips · click to add keyframes · double-click to delete',
  模型文件: 'Model File',
  默认变换: 'Default Transform',
  核心版本: 'Core Version',
  创建时间: 'Created At',
  '视口 UI 模式': 'Viewport UI Mode',
  摄像头移动速度: 'Camera Move Speed',
  '🔍 搜索资源(名称/中文/拼音,支持模糊)': 'Search resources (name/Chinese/Pinyin, fuzzy supported)',
  '以图搜索(本地 pHash)': 'Image Search (local pHash)',
  重建索引: 'Rebuild Index',
  定位到资源: 'Locate Resource',
  导入: 'Import',
  添加灯光: 'Add Light',
  添加摄像头: 'Add Camera',
  保存场景: 'Save Scene',
  截图: 'Screenshot',
  输出通道: 'Output Channel',
  '快速截图（保存当前输出模式到桌面）': 'Quick screenshot (save current output mode to desktop)',
  依次切换所有通道并逐一截图保存: 'Switch through all channels and save screenshots one by one',
  速度: 'Speed',
  '正在准备资源索引...': 'Preparing resource index...',
  找到: 'Found',
  项: 'items',
  暂无匹配结果: 'No matches',
  '📦 模型': 'Model',
  '👤 单位': 'Actor',
  '🎬 场景': 'Scene',
  '🎵 音频': 'Audio',
  '🖼 UI图片': 'UI Image',
  '📷 快速截图': 'Quick Screenshot',
  '📦 全部保存': 'Save All',
  搜索作品中的积木: 'Search blocks in workspace',
  设置: 'Settings',
  相机跟随: 'Camera Follow',
  偏移: 'Offset',
  删除: 'Delete',
  日志: 'Log',
  清空: 'Clear',
  局域网聊天: 'LAN Chat',
  手动连接: 'Manual Connect',
  网格: 'Grid',
  '浏览...': 'Browse...',
  重置: 'Reset',
  节点: 'Nodes',
  选择: 'Select',
  宏观节点: 'Macro Nodes',
  拖入中间: 'Drag to Center',
  态: 'State',
  状态节点: 'State Nodes',
  '开始 / 结束 / 自定义状态': 'Start / End / Custom State',
  节点编辑区: 'Node Editor',
  '空白处按住拖动画布 · 中键拖动 · 滚轮缩放':
    'Drag empty space to pan · middle-drag to pan · scroll to zoom',
  '恢复 100%': 'Reset to 100%',
  '退出全屏编辑（Esc）': 'Exit Fullscreen Editor (Esc)',
  全屏编辑节点图: 'Edit Node Graph in Fullscreen',
  拖动调整工具箱宽度: 'Drag to Resize Toolbox',
  空连接点: 'Open Connection Port',
  上一步: 'Previous',
  '仅演示操作位置，不会修改当前世界。按 Esc 可退出。':
    'This only demonstrates where to operate and will not modify the current world. Press Esc to exit.',
  操作展示: 'Operation Demo',
  'Dock 开关': 'Dock Toggle',
  包菜任务与答疑: 'Cabbage Tasks and Help',
  断开连接: 'Disconnect',
  '切断与 Corona 系统的连接后，所有未保存的宇宙演化进程将在后台处于休眠状态。':
    'After disconnecting from Corona, all unsaved world evolution will remain dormant in the background.',
  确认离开: 'Confirm Leave',
  'AI 创作助手': 'AI Creative Assistant',
  当前任务: 'Current Task',
  '开始使用 AI 创作助手': 'Start Creating with the AI Creative Assistant',
  '可以询问当前任务、让 AI 修改节点图，或根据你的描述生成节点逻辑。':
    'Ask about the current task, have AI modify the node graph, or generate node logic from your description.',
  展示: 'Show',
  包菜: 'Cabbage',
  'AI 正在查看当前世界与任务…': 'AI is reviewing the current world and task...',
  清空会话: 'Clear Conversation',
  停止等待: 'Stop Waiting',
  弹出为可拖动窗口: 'Pop Out as Draggable Window',
  '可以答疑，也可以让包菜生成、制作或编辑游戏节点逻辑…':
    'Ask questions or have Cabbage generate, build, or edit game node logic...',
  '正在读取对象属性…': 'Loading object properties...',
  还没有选中对象: 'No object selected',
  '在 3D 视口或场景管理中选择一个模型后，这里会显示可调整的属性。':
    'Select a model in the 3D viewport or Scene Manager to edit its properties here.',
  模型资源: 'Model Asset',
  变换: 'Transform',
  摄像机跟随: 'Camera Follow',
  位置偏移: 'Position Offset',
  碰撞形状: 'Collision Shape',
  物理模拟: 'Physics Simulation',
  锁定移动: 'Lock Movement',
  锁定旋转: 'Lock Rotation',
  对象: 'Object',
  对象名称: 'Object Name',
  未设置模型资源: 'No Model Asset',
  对象变换: 'Object Transform',
  碰撞设置: 'Collision Settings',
  物理设置: 'Physics Settings',
  分数: 'Score',
  生命: 'Lives',
  倒计时: 'Countdown',
};

// Runtime labels, status messages, and validation feedback assembled in Vue scripts.
// Placeholder patterns are matched against their rendered values by translateEnglishText().
const RUNTIME_DOM_TEXT_TRANSLATIONS = {
  退出全屏: 'Exit Fullscreen',
  全屏编辑: 'Fullscreen Editor',
  返回值积木: 'Value Blocks',
  微观积木: 'Micro Blocks',
  用于组合跳转条件: 'Used to Combine Transition Conditions',
  'Blockly 原生形状': 'Native Blockly Shapes',
  已连接: 'Connected',
  拖动调整内部编辑区宽度: 'Drag to Resize the Inner Editor',
  全局变量池: 'Global Variable Pool',
  节点类型: 'Node Type',
  拖动调整全局变量池高度: 'Drag to Resize the Global Variable Pool',
  自定义节点: 'Custom Node',
  全屏编辑节点图失败: 'Failed to Open the Node Graph in Fullscreen',
  退出全屏编辑失败: 'Failed to Exit Fullscreen Editing',
  未命名场景: 'Untitled Scene',
  未选择目标: 'No Target Selected',
  开始节点: 'Start Node',
  结束节点: 'End Node',
  '最外层使用“与 / 或 / 非”组合多个判断，最终只保留一个顶层布尔条件':
    'Use AND / OR / NOT at the outermost level to combine checks into one top-level Boolean condition',
  '未设置条件，当前连线会被视为始终成立':
    'No condition is set; this connection will always be treated as true',
  '节点内部编辑：{name}': 'Node Editor: {name}',
  '连线条件编辑：{name}': 'Connection Condition Editor: {name}',
  未命名连线: 'Untitled Connection',
  节点内部编辑: 'Node Editor',
  拖入微观积木编辑该节点: 'Drag micro blocks here to edit this node',
  '从左侧“返回值”中拖入积木，可使用“与 / 或 / 非”组合条件':
    'Drag blocks from Value Blocks on the left; use AND / OR / NOT to combine conditions',
  '状态{number}': 'State {number}',
  结束: 'End',
  未知节点: 'Unknown Node',
  '正在保存到当前世界...': 'Saving to the Current World...',
  '项目已切换，已跳过旧节点图保存':
    'The project changed; saving the previous node graph was skipped',
  保存本地节点图失败: 'Failed to Save the Local Node Graph',
  保存节点图失败: 'Failed to Save the Node Graph',
  '已实时保存到当前世界 {time}': 'Saved to the Current World at {time}',
  '已实时保存（不可运行：{error}）': 'Saved (Cannot Run: {error})',
  保存项目节点图失败: 'Failed to Save the Project Node Graph',
  '项目保存失败（本地副本已保留）': 'Project Save Failed (Local Copy Retained)',
  等待当前世界加载: 'Waiting for the Current World to Load',
  '{label}尚未挂载': '{label} Is Not Mounted',
  '{label}初始化失败：{error}': '{label} Initialization Failed: {error}',
  '{label}加载失败：{error}': '{label} Failed to Load: {error}',
  未知错误: 'Unknown Error',
  工作区尚未就绪: 'Workspace Is Not Ready',
  全局变量积木区: 'Global Variable Block Area',
  连线条件积木区: 'Connection Condition Block Area',
  节点内部积木区: 'Node Block Area',
  '生成的积木没有完整加载到编辑区（期望 {expected} 个，实际 {actual} 个）':
    'Generated blocks were not fully loaded into the editor (expected {expected}, actual {actual})',
  生成的目标积木没有出现在当前积木编辑区:
    'The generated target block did not appear in the current block editor',
  '可用物体：{objects}': 'Available Objects: {objects}',
  '可用物体：暂时无法读取场景对象列表': 'Available Objects: Unable to Read the Scene Object List',
  '请求目标：{scene} / {actor}': 'Requested Target: {scene} / {actor}',
  '绑定目标：{scene} / {actor}': 'Bound Target: {scene} / {actor}',
  '绑定模式：{mode}': 'Binding Mode: {mode}',
  'Python 场景：{scenes}': 'Python Scenes: {scenes}',
  '原生场景：{scene}': 'Native Scene: {scene}',
  '物体候选：{actors}': 'Object Candidates: {actors}',
  '(空场景)': '(No Scene)',
  '(空物体)': '(No Object)',
  '(空)': '(Empty)',
  关闭节点窗口前查询运行状态失败: 'Failed to Query Run Status Before Closing the Node Window',
  停止节点图失败: 'Failed to Stop the Node Graph',
  查询全局运行状态失败: 'Failed to Query Global Run Status',
  请先选择运行目标: 'Select a Run Target First',
  '生成失败：{error}': 'Generation Failed: {error}',
  生成节点图代码失败: 'Failed to Generate Node Graph Code',
  执行节点图失败: 'Failed to Run the Node Graph',

  '正在停止并恢复...': 'Stopping and Restoring...',
  更新场景光照失败: 'Failed to Update Scene Lighting',
  '正在启动脚本...': 'Starting Scripts...',
  '预览中 {count}': 'Previewing {count}',
  '已完成，{count} 个脚本错误': 'Completed with {count} Script Errors',
  脚本已完成: 'Scripts Completed',
  已停止并恢复: 'Stopped and Restored',
  '场景恢复失败：{error}': 'Failed to Restore Scene: {error}',
  没有可运行脚本: 'No Runnable Scripts',
  查询预览状态失败: 'Failed to Query Preview Status',
  '准备当前场景脚本...': 'Preparing Current Scene Scripts...',
  '准备项目预览...': 'Preparing Project Preview...',
  '启动前查询预览状态失败，将继续尝试启动':
    'Failed to Query Preview Status Before Starting; Continuing Anyway',
  开始预览失败: 'Failed to Start Preview',
  结束预览恢复失败: 'Failed to Restore the Scene After Preview',
  运行项目返回失败: 'The Run Project Command Failed',
  运行项目失败: 'Failed to Run the Project',
  没有当前场景: 'No Current Scene',
  运行当前场景返回失败: 'The Run Current Scene Command Failed',
  运行当前场景失败: 'Failed to Run the Current Scene',
  更新场景编辑网格失败: 'Failed to Update the Scene Editor Grid',

  '保存中…': 'Saving...',
  保存对象: 'Save Object',
  '修改后可用新的对象名称在节点积木中准确引用这个模型。':
    'After renaming, use the new object name to reference this model accurately in node blocks.',
  '修改模型在场景中的位置、旋转和大小。':
    'Change the model position, rotation, and scale in the scene.',
  '启用后模型会按照偏移值跟随编辑器或游戏摄像机。':
    'When enabled, the model follows the editor or game camera using the configured offset.',
  '选择模型参与碰撞检测时使用的形状。':
    'Choose the shape used when this model participates in collision detection.',
  '控制模型是否参与物理模拟，以及质量、弹性、阻尼和轴向锁定。':
    'Control physics simulation, mass, restitution, damping, and axis locks for the model.',
  模型网格: 'Model Mesh',
  无法读取对象属性: 'Unable to Read Object Properties',
  加载对象数据失败: 'Failed to Load Object Data',
  修改名称失败: 'Failed to Rename',
  修改对象名称失败: 'Failed to Rename the Object',
  更新对象变换失败: 'Failed to Update Object Transform',
  保存对象变换失败: 'Failed to Save Object Transform',
  更新对象渲染空间失败: 'Failed to Update Object Render Space',
  选择模型资源失败: 'Failed to Select a Model Asset',
  更新对象碰撞失败: 'Failed to Update Object Collision',
  更新对象物理属性失败: 'Failed to Update Object Physics Properties',
  更新对象轴锁失败: 'Failed to Update Object Axis Locks',
  更新摄像机跟随失败: 'Failed to Update Camera Follow',
  更新摄像机偏移失败: 'Failed to Update Camera Offset',
  保存对象失败: 'Failed to Save the Object',

  IP地址: 'IP Address',
  退出聊天: 'Leave Chat',
  '本地单人，内置专家默认启用': 'Local Single Player; Built-in Experts Enabled by Default',
  '房主开房，内置专家默认启用': 'Host Room; Built-in Experts Enabled by Default',
  正在同步房间: 'Syncing Room',
  '{seconds} 秒前': '{seconds} Seconds Ago',
  '{minutes} 分钟前': '{minutes} Minutes Ago',
  '{hours} 小时前': '{hours} Hours Ago',
  '输入@来指定AI助手~': 'Type @ to Select an AI Assistant',
  '{target} 仍在处理': '{target} Is Still Processing',
  '{target} 正在整理': '{target} Is Organizing the Response',
  '{target} 正在思考': '{target} Is Thinking',
  资源调度: 'Resource Coordination',
  全部AI助手: 'All AI Assistants',
  所有AI助手: 'All AI Assistants',
  '直接发送；输入 @ 指定AI助手': 'Send Directly; Type @ to Select an AI Assistant',
  '发送给 {target}': 'Send to {target}',
  '多人聊天室-{roomId}': 'Multiplayer Chat - {roomId}',
  系统: 'System',
  'GM 提案': 'GM Proposal',

  当前积木脚本正在由全局运行执行: 'The Current Block Script Is Running Globally',
  当前积木脚本正在由项目预览执行: 'The Current Block Script Is Running in Project Preview',
  'Blockly 模块加载失败': 'Failed to Load the Blockly Module',
  保存工作区状态失败: 'Failed to Save Workspace State',
  加载目标积木失败: 'Failed to Load the Target Blocks',
  切换工作区失败: 'Failed to Switch Workspace',
  '注册积木/生成器失败': 'Failed to Register Blocks or Generators',
  '刷新 Blockly 语言失败': 'Failed to Refresh the Blockly Language',
  'Blockly 容器未找到': 'Blockly Container Not Found',
  复制积木失败: 'Failed to Copy Blocks',
  粘贴积木失败: 'Failed to Paste Blocks',
  清空工作区失败: 'Failed to Clear the Workspace',
  初始化失败: 'Initialization Failed',

  '等待其他用户加入...': 'Waiting for Other Users to Join...',
  '创建房间或输入房主 IP 加入': 'Create a Room or Enter the Host IP to Join',
  客户端: 'Client',
  未加入: 'Not Joined',
  未知角色: 'Unknown Role',
  读取网络会话失败: 'Failed to Read the Network Session',
  '已有网络会话正在运行（当前角色：{role}），请先停止当前会话后再切换角色。':
    'A network session is already running (current role: {role}). Stop it before switching roles.',
  启动失败: 'Failed to Start',
  '已连接用户 {count}': 'Connected Users: {count}',
  本地会话启动失败: 'Failed to Start the Local Session',
  连接失败: 'Connection Failed',

  将左侧微观积木拖入这里: 'Drag Micro Blocks Here from the Left',
  读取子工作区状态失败: 'Failed to Read Sub-workspace State',
  积木工作区尚未初始化完成: 'The Block Workspace Has Not Finished Initializing',
  加载子工作区状态失败: 'Failed to Load Sub-workspace State',
  删除子工作区积木失败: 'Failed to Delete Blocks from the Sub-workspace',
  无法识别该积木: 'This Block Is Not Recognized',
  此积木应放入节点内部编辑区: 'This Block Belongs in the Node Editor',
  '跳转条件只能放入返回值积木；多个判断请使用“与 / 或”组合':
    'Transition conditions can only be placed in value blocks; combine multiple checks with AND / OR',
  全局变量池中的返回值积木必须连接到初始化积木:
    'Value blocks in the global variable pool must connect to an initialization block',
  '请使用“与 / 或”积木把多个判断连接成一个顶层条件':
    'Use AND / OR blocks to combine multiple checks into one top-level condition',
  跳转条件最外层必须是返回值积木: 'The outermost transition condition must be a value block',
  '跳转条件必须返回真或假；请将数字、坐标或文本连接到比较积木':
    'A transition condition must return true or false; connect numbers, coordinates, or text to a comparison block',
  '创建积木失败: {type}': 'Failed to Create Block: {type}',
  '加载积木工作区...': 'Loading Block Workspace...',
  '初始化子 Blockly 工作区失败': 'Failed to Initialize the Blockly Sub-workspace',
  积木工作区加载失败: 'Failed to Load the Block Workspace',

  '未发现房间，可点击刷新或在下方手动加入': 'No Rooms Found. Refresh or Join Manually Below.',
  玩家1: 'Player 1',
  玩家: 'Player',
  '正在准备联机存档…': 'Preparing Multiplayer Save...',
  创建联机存档失败: 'Failed to Create the Multiplayer Save',
  打开联机存档失败: 'Failed to Open the Multiplayer Save',
  设置联机项目目录失败: 'Failed to Set the Multiplayer Project Directory',
  '正在创建房间…': 'Creating Room...',
  '正在启动本地客户端…': 'Starting Local Client...',
  启动联机会话失败: 'Failed to Start the Multiplayer Session',
  '正在加入房间…': 'Joining Room...',
  连接房主失败: 'Failed to Connect to the Host',

  相似度: 'Similarity',
  'Vision PT · 实时': 'Vision PT · Realtime',
  'Vision PT · 渐进收敛': 'Vision PT · Progressive',
  'Vision ReSTIR · 实时': 'Vision ReSTIR · Realtime',
  '当前: Vision (路径追踪)，点击切换到 Native':
    'Current: Vision (Path Tracing). Click to Switch to Native',
  '当前: Native (光栅化)，点击切换到 Vision':
    'Current: Native (Rasterization). Click to Switch to Vision',
  网络错误: 'Network Error',
  '图片过大 ({size}MB),请使用 ≤ 2MB 的图片':
    'Image Too Large ({size} MB); Use an Image of 2 MB or Less',
  '[图] {name}': '[Image] {name}',
  图片读取失败: 'Failed to Read the Image',
  重建索引失败: 'Failed to Rebuild the Index',
  定位资源失败: 'Failed to Locate the Asset',
  创建对象: 'Create Object',
  资源索引预热失败: 'Failed to Warm Up the Asset Index',

  全部待处理任务: 'All Pending Tasks',
  当前没有待处理任务: 'No Pending Tasks',
  你: 'You',
  'AI 创作助手暂时不可用，请稍后再试。':
    'The AI Creative Assistant Is Temporarily Unavailable. Try Again Later.',
  'DeepSeek 没有返回可显示的内容。': 'DeepSeek Returned No Displayable Content.',
  '已停止等待本次回答。': 'Stopped Waiting for This Response.',

  游戏胜利: 'Victory',
  游戏失败: 'Defeat',
  '普通屏幕 UI': 'Standard Screen UI',
  '光场屏立体 UI': 'Light-field 3D UI',
  项目全局: 'Project-wide',
  正在重新开始: 'Restarting',

  创造模式: 'Creative Mode',
  废土上的最后绿洲: 'The Last Oasis in the Wasteland',
  霓虹蒸汽朋克工坊: 'Neon Steampunk Workshop',
  永夜极光下的雪原: 'Snowfield Beneath the Aurora of Eternal Night',
  '2D 平面设计': '2D Design',
  '3D 场景渲染': '3D Scene Rendering',

  记得保存当前项目: 'Remember to Save the Current Project',
  可以拖拽模型到场景里: 'You Can Drag Models into the Scene',
  右键对象查看更多操作: 'Right-click an Object for More Actions',
  调整相机看看构图效果: 'Adjust the Camera to Review the Composition',
  打开日志面板检查运行状态: 'Open the Log Panel to Check Run Status',
  '加载积木库...': 'Loading Block Library...',
  初始化积木库失败: 'Failed to Initialize the Block Library',
  积木库加载失败: 'Failed to Load the Block Library',
  '请查看高亮区域。': 'Check the Highlighted Area.',
  完成: 'Done',
  '确定要删除 "{name}" 吗？': 'Delete "{name}"?',
  '{nickname}（我）': '{nickname} (Me)',

  当前节点图正在由全局运行执行: 'The Current Node Graph Is Running Globally',
  当前节点图正在由项目预览执行: 'The Current Node Graph Is Running in Project Preview',
  '读取当前项目路径失败，将使用已有项目上下文:':
    'Failed to Read the Current Project Path; Using the Existing Project Context:',
  未选择: 'Not Selected',
  无法放入该积木: 'This Block Cannot Be Placed Here',
  '请先选择节点或连线；此积木不适用于全局变量池':
    'Select a node or connection first; this block cannot be used in the global variable pool',
  '项目加载中...': 'Loading Project...',
  节点图加载失败: 'Failed to Load the Node Graph',
  项目节点图已加载: 'Project Node Graph Loaded',
  '已加载当前项目的本地节点图，正在迁移...':
    'Loaded the Local Node Graph for the Current Project; Migrating...',
  新节点图: 'New Node Graph',
  加载项目节点图失败: 'Failed to Load the Project Node Graph',
  '项目加载失败，已使用当前项目本地副本':
    'Project Load Failed; Using the Local Copy for the Current Project',
  '内部 AI 结果只能应用到项目常驻节点图':
    'Internal AI Results Can Only Be Applied to the Persistent Project Node Graph',
  'AI 结果属于另一个世界，已拒绝应用': 'The AI Result Belongs to Another World and Was Not Applied',
  '生成期间节点逻辑已改变，迟到的 AI 结果未覆盖当前编辑':
    'Node Logic Changed During Generation; the Late AI Result Did Not Overwrite Current Edits',
  '内部 AI 节点图保存失败': 'Failed to Save the Internal AI Node Graph',
  '内部 AI 节点图已应用': 'Internal AI Node Graph Applied',
  '内部 AI 应用失败：{error}': 'Failed to Apply the Internal AI Result: {error}',
  '启动中...': 'Starting...',
  '等待连线条件：': 'Waiting for Connection Condition:',
  '运行中：': 'Running:',
  运行中: 'Running',
  已停止: 'Stopped',
  '执行失败：': 'Execution Failed:',
  状态查询失败: 'Failed to Query Status',
  查询节点图运行状态失败: 'Failed to Query Node Graph Run Status',
  '错误：{error}': 'Error: {error}',
  '运行作用域：当前场景（项目节点图）': 'Run Scope: Current Scene (Project Node Graph)',
  '运行场景：{scene}': 'Run Scene: {scene}',
  '对象来源：场景管理中已导入的物体，由各积木的对象参数指定':
    'Object Source: Objects Imported in Scene Manager, Selected by Each Block Object Parameter',
  已停止并恢复运行前状态: 'Stopped and Restored the Pre-run State',
  '已停止，但场景恢复失败：{error}': 'Stopped, but Failed to Restore the Scene: {error}',
  '停止节点图失败：{error}': 'Failed to Stop the Node Graph: {error}',
  '节点窗口已关闭，运行已停止': 'The Node Window Was Closed and Execution Stopped',
  后端拒绝执行节点图: 'The Backend Rejected Node Graph Execution',
  '运行中（{status}）': 'Running ({status})',
  '执行失败：{error}': 'Execution Failed: {error}',
  '运行成功，正在恢复运行前状态...': 'Run Succeeded. Restoring the Pre-run State...',
  '运行前状态恢复失败': 'Failed to Restore the Pre-run State',
  '运行已成功，但任务进度没有同步，请再点击一次“运行”':
    'The Run Succeeded, but Task Progress Was Not Synchronized. Click Run Again.',
  '运行成功，已恢复运行前状态': 'Run Succeeded. The Pre-run State Has Been Restored.',
  '运行验证失败：{error}': 'Run Validation Failed: {error}',
  '教程节点图运行验证失败': 'Tutorial Node Graph Run Validation Failed',
  '节点图保存失败，已取消全局运行': 'Node Graph Save Failed; Global Run Was Cancelled',

  'AI 正在读取积木文档并生成当前节点逻辑…':
    'AI Is Reading the Block Documentation and Generating the Current Node Logic...',
  '节点逻辑已经生成并保存。': 'Node Logic Was Generated and Saved.',
  '节点逻辑生成失败。': 'Failed to Generate Node Logic.',
  'AI 正在读取现有节点并补充逻辑…':
    'AI Is Reading Existing Nodes and Extending the Logic...',
  '已在现有节点图中补充并保存所需逻辑。':
    'The Required Logic Was Added to the Existing Node Graph and Saved.',
  '节点逻辑补充失败。': 'Failed to Extend Node Logic.',
  'AI 正在读取现有节点并进行局部修改…':
    'AI Is Reading Existing Nodes and Applying a Local Edit...',
  '已保留无关逻辑并完成局部修改。':
    'Unrelated Logic Was Preserved and the Local Edit Was Completed.',
  '节点逻辑修改失败。': 'Failed to Edit Node Logic.',
  'AI 正在定位并删除指定节点逻辑…':
    'AI Is Locating and Deleting the Requested Node Logic...',
  '已删除指定逻辑并保存节点图。': 'The Requested Logic Was Deleted and the Node Graph Was Saved.',
  '节点逻辑删除失败。': 'Failed to Delete Node Logic.',
  没有被修改: 'Was Not Modified',
  '{reason} 当前节点图没有被修改。': '{reason} The Current Node Graph Was Not Modified.',

  '收到远程 Actor 创建事件；SceneTools native 创建接口尚未接入':
    'Received a Remote Actor Creation Event; the SceneTools Native Creation API Is Not Connected Yet',
  '收到远程 Actor 状态事件: {actor}': 'Received Remote Actor State Event: {actor}',
  '收到远程 Actor Transform 事件: {actor}': 'Received Remote Actor Transform Event: {actor}',
  '收到远程 Actor 删除事件: {actor}': 'Received Remote Actor Deletion Event: {actor}',
  '远程 Actor 已创建: {actor}': 'Remote Actor Created: {actor}',
  'SceneTools native 快照接口尚未接入，跳过场景快照同步':
    'The SceneTools Native Snapshot API Is Not Connected Yet; Scene Snapshot Sync Was Skipped',
  '收到远程场景快照；SceneTools native 应用接口尚未接入':
    'Received a Remote Scene Snapshot; the SceneTools Native Apply API Is Not Connected Yet',
  'Actor 身份注册失败: {actor}': 'Failed to Register Actor Identity: {actor}',

  '[Blockly] 停止脚本失败:': '[Blockly] Failed to Stop Script:',
  '[Blockly] 查询全局运行状态失败，将继续尝试单物体运行:':
    '[Blockly] Failed to Query Global Run Status; Trying Single-object Run:',
  '[Blockly] 脚本执行超时，强制停止': '[Blockly] Script Timed Out and Was Force-stopped',
  '[Blockly] 脚本状态查询失败:': '[Blockly] Failed to Query Script Status:',
  '[Blockly] 保存项目积木镜像失败:': '[Blockly] Failed to Save the Project Block Snapshot:',
  'Blockly.inject 失败': 'Blockly.inject Failed',

  景: 'Scene',
  点: 'Node',
  '[Camera] coronaBridge 缺失或 cameraMove 不可用，':
    '[Camera] coronaBridge Is Missing or cameraMove Is Unavailable,',
  'CEF 子进程可能未运行。快速通道摄像头更新已禁用。':
    'The CEF Subprocess May Not Be Running. Fast-path Camera Updates Have Been Disabled.',
  'ProjectLauncher 初始化失败:': 'ProjectLauncher Initialization Failed:',
  '创建失败: ': 'Creation Failed: ',
  '创建失败: {error}': 'Creation Failed: {error}',
  '创建项目异常:': 'Unexpected Error While Creating the Project:',
  '打开项目失败:': 'Failed to Open the Project:',
  'RecentGames 初始化失败:': 'RecentGames Initialization Failed:',
  '打开现有项目失败:': 'Failed to Open an Existing Project:',
  '创造世界失败:': 'Failed to Create the World:',
  '渲染积木预览失败: {type}': 'Failed to Render Block Preview: {type}',
  '\u5df2\u81ea\u52a8\u6253\u5f00\u201c\u81ea\u5b9a\u4e49\u8282\u70b9\u201d\u8282\u70b9\uff0c\u5e76\u663e\u793a\u5176\u4e2d {count} \u4e2a\u53ef\u89c1\u79ef\u6728\u3002':
    'Automatically opened the "Custom Node" node and displayed its {count} visible blocks.',
  '\u5df2\u81ea\u52a8\u5b9a\u4f4d\u5230\u201c\u81ea\u5b9a\u4e49\u8282\u70b9\u201d\u8282\u70b9\uff1b\u8be5\u8282\u70b9\u5f53\u524d\u6ca1\u6709\u5185\u90e8\u79ef\u6728\u3002':
    'Automatically located the "Custom Node" node; it currently contains no blocks.',
  '\u5df2\u81ea\u52a8\u6253\u5f00\u201c{nodeName}\u201d\u8282\u70b9\uff0c\u5e76\u663e\u793a\u5176\u4e2d {count} \u4e2a\u53ef\u89c1\u79ef\u6728\u3002':
    'Automatically opened the "{nodeName}" node and displayed its {count} visible blocks.',
  '\u5df2\u81ea\u52a8\u5b9a\u4f4d\u5230\u201c{nodeName}\u201d\u8282\u70b9\uff1b\u8be5\u8282\u70b9\u5f53\u524d\u6ca1\u6709\u5185\u90e8\u79ef\u6728\u3002':
    'Automatically located the "{nodeName}" node; it currently contains no blocks.',
  '\u5df2\u81ea\u52a8\u6253\u5f00\u8fde\u7ebf\u6761\u4ef6\uff0c\u5e76\u663e\u793a\u5176\u4e2d {count} \u4e2a\u53ef\u89c1\u79ef\u6728\u3002':
    'Automatically opened the connection condition and displayed its {count} visible blocks.',
  '\u5df2\u81ea\u52a8\u5b9a\u4f4d\u5230\u672c\u6b21\u4fee\u6539\u7684\u8282\u70b9\u8fde\u7ebf\u3002':
    'Automatically located the node connection modified in this update.',
  'coronaBridge.actorTransform 不可用': 'coronaBridge.actorTransform Is Unavailable',
};
const CUSTOM_BLOCK_TEXT_TRANSLATIONS = {
  ...CUSTOM_BLOCK_TRANSLATIONS,
  无广播: 'No broadcasts',
  '新建广播...': 'New broadcast...',
  当游戏开始时: 'When the game starts',
  当按下: 'When',
  时: 'is pressed',
  当接收到广播: 'When I receive',
  发送广播: 'Broadcast',
  并等待: 'and wait',
  当按下组合键: 'When shortcut',
  当鼠标点击: 'When mouse button',
  左键: 'Left',
  右键: 'Right',
  中键: 'Middle',
  当鼠标移动时: 'When the mouse moves',
  当鼠标滚轮滚动时: 'When the mouse wheel scrolls',
  当鼠标右键菜单时: 'When the context menu opens',
  执行: 'Do',
  当进入当前节点时: 'When entering this node',
  进入节点时执行一次: 'Runs once when the node is entered',
  当前节点持续时: 'While this node is active',
  '节点激活期间每 0.05 秒执行一次': 'Runs every 0.05 seconds while the node is active',
  当离开当前节点时: 'When leaving this node',
  切换到下一节点前执行一次: 'Runs once before switching to the next node',
  创建入口触发节点: 'Create the Entry Trigger Node',
  创建核心接近检测节点: 'Create the Core Proximity Detection Node',
  '创建一个节点，当玩家进入或离开指定范围时触发提示。':
    'Create a node that shows a prompt when the player enters or leaves a specified area.',
  '创建一个节点，检测玩家是否接近游戏核心。':
    'Create a node that detects whether the player is near the game core.',
  '\u521b\u5efa\u4e00\u4e2a\u8282\u70b9\uff0c\u5f53\u73a9\u5bb6\u8fdb\u5165\u9057\u8ff9\u5165\u53e3\u65f6\u89e6\u53d1\u63d0\u793a\u3002':
    'Create a node that shows a prompt when the player enters the ruins entrance.',
  '\u521b\u5efa\u4e00\u4e2a\u8282\u70b9\uff0c\u5f53\u73a9\u5bb6\u8fdb\u5165\u6216\u8fdc\u79bb\u5165\u53e3\u65f6\u89e6\u53d1\u63d0\u793a\u3002':
    'Create a node that shows a prompt when the player enters or moves away from the entrance.',
  '\u521b\u5efa\u4e00\u4e2a\u8282\u70b9\uff0c\u5f53\u73a9\u5bb6\u8fdb\u5165\u6216\u8fdc\u79bb{location}\u65f6\u89e6\u53d1\u63d0\u793a\u3002':
    'Create a node that shows a prompt when the player enters or moves away from {location}.',
  '\u521b\u5efa\u4e00\u4e2a\u8282\u70b9\uff0c\u5f53\u73a9\u5bb6\u8fdb\u5165\u6216\u79bb\u5f00{location}\u65f6\u89e6\u53d1\u63d0\u793a\u3002':
    'Create a node that shows a prompt when the player enters or leaves {location}.',
  '\u521b\u5efa\u4e00\u4e2a\u8282\u70b9\uff0c\u5f53\u73a9\u5bb6\u8fdb\u5165{location}\u65f6\u89e6\u53d1\u63d0\u793a\u3002':
    'Create a node that shows a prompt when the player enters {location}.',
  '\u4f7f\u7528\u201c\u5f53\u8fdb\u5165\u5f53\u524d\u8282\u70b9\u65f6\u201d\u79ef\u6728\uff0c\u5e76\u6dfb\u52a0\u201c\u8f93\u51fa\u201d\u79ef\u6728\u663e\u793a\u63d0\u793a\u3002':
    'Use the "When entering this node" block and add an "Output" block to show the prompt.',
  '\u8282\u70b9\u5df2\u521b\u5efa\u5e76\u5305\u542b\u6240\u9700\u79ef\u6728':
    'The node has been created and contains the required blocks.',
};

export const DOM_TEXT_TRANSLATIONS = {
  ...LOCALE_TEXT_TRANSLATIONS,
  ...EXPLICIT_DOM_TEXT_TRANSLATIONS,
  ...RUNTIME_DOM_TEXT_TRANSLATIONS,
  ...CUSTOM_BLOCK_TEXT_TRANSLATIONS,
};

const REGEXP_SPECIAL_CHARACTERS = new Set('\\^$.*+?()[]{}|');

function escapeRegExp(value) {
  return [...value]
    .map((char) => (REGEXP_SPECIAL_CHARACTERS.has(char) ? `\\${char}` : char))
    .join('');
}

function buildPatternTranslations(translations) {
  const placeholderPattern = /\{([^{}]+)\}/g;
  const patterns = [];

  for (const [source, target] of Object.entries(translations)) {
    const matches = [...source.matchAll(placeholderPattern)];
    if (!matches.length) continue;

    const names = [];
    let cursor = 0;
    let expression = '^';
    for (const match of matches) {
      expression += escapeRegExp(source.slice(cursor, match.index));
      expression += '(.+?)';
      names.push(match[1]);
      cursor = match.index + match[0].length;
    }
    expression += `${escapeRegExp(source.slice(cursor))}$`;
    patterns.push({ regex: new RegExp(expression), names, target });
  }

  return patterns;
}

const DOM_PATTERN_TRANSLATIONS = buildPatternTranslations(DOM_TEXT_TRANSLATIONS);

function translateEnglishText(text) {
  if (Object.hasOwn(DOM_TEXT_TRANSLATIONS, text)) return DOM_TEXT_TRANSLATIONS[text];

  for (const pattern of DOM_PATTERN_TRANSLATIONS) {
    const match = text.match(pattern.regex);
    if (!match) continue;
    const values = Object.fromEntries(
      pattern.names.map((name, index) => [name, translateEnglishText(match[index + 1])])
    );
    return pattern.target.replace(/\{([^{}]+)\}/g, (placeholder, name) =>
      Object.hasOwn(values, name) ? values[name] : placeholder
    );
  }

  return text;
}

export function translateUiText(text) {
  const value = String(text ?? '');
  let locale = activeLocale;
  if (!locale && typeof localStorage !== 'undefined') {
    locale = localStorage.getItem(LOCALE_STORAGE_KEY) || '';
  }
  return locale === 'en-US' ? translateEnglishText(value) : value;
}

const ATTRIBUTE_NAMES = ['title', 'placeholder', 'aria-label', 'alt'];
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
  return translateEnglishText(text);
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
  const translated = translateEnglishText(trimmed);
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
  return translateEnglishText(trimmed) || original;
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
      if (
        current !== originals[attr] &&
        current !== renderedOriginal &&
        current !== renderedEnglish
      ) {
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

  activeLocale =
    localStorage.getItem(LOCALE_STORAGE_KEY) || document.documentElement.lang || 'zh-CN';
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
