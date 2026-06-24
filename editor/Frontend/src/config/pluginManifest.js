/**
 * 面板静态注册表 - 替代 Python register_web 装饰器中的 UI 元数据
 * 每个面板的 id 必须与 Python 端 module_name 一致（用于 cefQuery 路由）
 */
import SceneBar from '@/views/sidebar/SceneBar.vue';
import ObjectPanel from '@/views/sidebar/Object.vue';
import Pet from '@/views/tools/Pet.vue';
import LogView from '@/views/sidebar/LogView.vue';
import FileManager from '@/views/sidebar/FileManager.vue';
import ProjectSettings from '@/views/sidebar/ProjectSettings.vue';
import AITalkBar from '@/views/sidebar/AITalkBar.vue';
import EditorSettings from '@/views/sidebar/EditorSettings.vue';
import NetworkPanel from '@/views/sidebar/Network.vue';
import LightFieldCalibrationPanel from '@/components/panels/LightFieldCalibrationPanel.vue';
import { translate } from '@/i18n/index.js';

export const PLUGIN_MANIFEST = [
  {
    id: 'SceneTools',
    routePath: '/SceneBar',
    displayNameKey: 'plugins.SceneTools',
    displayName: '场景管理',
    pageType: 'view',
    defaultDock: 'right',
    defaultWidth: 460,
    defaultHeight: 560,
    autoInit: false,
    defaultOpenMode: 'external',
    defaultFloatPosition: 'right_top',
    minFloatWidth: 340,
    minFloatHeight: 360,
    component: SceneBar,
  },
  {
    id: 'LightFieldCalibration',
    routePath: '/LightFieldCalibration',
    displayNameKey: 'plugins.LightFieldCalibration',
    displayName: '光场3D UI标定',
    pageType: 'view',
    defaultDock: 'right',
    defaultWidth: 300,
    defaultHeight: 300,
    autoInit: false,
    component: LightFieldCalibrationPanel,
  },
  {
    id: 'SceneDatas',
    routePath: '/Object',
    displayNameKey: 'plugins.SceneDatas',
    displayName: '详情',
    pageType: 'view',
    defaultDock: 'right',
    defaultWidth: 420,
    defaultHeight: 460,
    autoInit: false,
    defaultOpenMode: 'external',
    defaultFloatPosition: 'right_bottom',
    minFloatWidth: 320,
    minFloatHeight: 320,
    component: ObjectPanel,
  },
  {
    id: 'AITool',
    routePath: '/Pet',
    displayNameKey: 'plugins.AITool',
    displayName: '白菜助手',
    pageType: 'plugin',
    defaultDock: 'bottom',
    defaultWidth: 200,
    defaultHeight: 200,
    autoInit: false,
    component: Pet,
  },
  {
    id: 'LogTool',
    routePath: '/LogView',
    displayNameKey: 'plugins.LogTool',
    displayName: '日志工具',
    pageType: 'view',
    defaultDock: 'bottom',
    defaultWidth: 1100,
    defaultHeight: 200,
    autoInit: false,
    component: LogView,
  },
  {
    id: 'FileManager',
    routePath: '/FileManager',
    displayNameKey: 'plugins.FileManager',
    displayName: '文件管理器',
    pageType: 'view',
    defaultDock: 'left',
    defaultWidth: 300,
    defaultHeight: 600,
    autoInit: false,
    component: FileManager,
  },
  {
    id: 'ProjectSettings',
    routePath: '/ProjectSettings',
    displayNameKey: 'plugins.ProjectSettings',
    displayName: '项目设置',
    pageType: 'special',
    defaultDock: 'center',
    defaultWidth: 600,
    defaultHeight: 800,
    autoInit: false,
    component: ProjectSettings,
  },
  {
    id: 'AITalkBar',
    routePath: '/AITalkBar',
    displayNameKey: 'plugins.AITalkBar',
    displayName: 'AI 对话',
    pageType: 'plugin',
    defaultDock: 'right',
    defaultWidth: 420,
    defaultHeight: 560,
    autoInit: false,
    defaultOpenMode: 'external',
    defaultFloatPosition: 'left_bottom',
    minFloatWidth: 320,
    minFloatHeight: 360,
    component: AITalkBar,
  },
  {
    id: 'EditorSettings',
    routePath: '/SetUp',
    displayNameKey: 'plugins.EditorSettings',
    displayName: '暂停菜单',
    pageType: 'special',
    defaultDock: 'center',
    defaultWidth: 620,
    defaultHeight: 720,
    autoInit: false,
    component: EditorSettings,
  },
  {
    id: 'Network',
    routePath: '/Network',
    displayNameKey: 'plugins.Network',
    displayName: '局域网聊天',
    pageType: 'plugin',
    defaultDock: 'right',
    defaultWidth: 360,
    defaultHeight: 430,
    autoInit: false,
    component: NetworkPanel,
  },
];

/** 按 id 快速查找 */
export function getPluginManifest(id) {
  return PLUGIN_MANIFEST.find((p) => p.id === id);
}

export function getPluginDisplayName(plugin) {
  if (!plugin) return '';
  return plugin.displayNameKey ? translate(plugin.displayNameKey) : plugin.displayName;
}
