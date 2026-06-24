// 路由文件
import { createRouter, createWebHashHistory } from 'vue-router';

// dock 面板组件（已在 pluginManifest.js 中静态导入，此处复用避免双模式 import 警告）
import SceneBar from '../views/sidebar/SceneBar.vue';
import ObjectPanel from '../views/sidebar/Object.vue';
import LogView from '../views/sidebar/LogView.vue';
import FileManager from '../views/sidebar/FileManager.vue';
import AITalkBar from '../views/sidebar/AITalkBar.vue';
import Pet from '../views/tools/Pet.vue';
import ProjectSettings from '../views/sidebar/ProjectSettings.vue';
import BlocklyWorkspace from '../blockly/components/BlocklyWorkspace.vue';
import EditorSettings from '../views/sidebar/EditorSettings.vue';
import NetworkPanel from '../views/sidebar/Network.vue';
import LightFieldCalibrationPanel from '../components/panels/LightFieldCalibrationPanel.vue';

const routes = [
  {
    path: '/',
    name: 'MainPage',
    component: () => import('../views/layout/MainPage.vue'),
  },
  {
    path: '/StartScreen',
    name: 'StartScreen',
    component: () => import('../views/layout/StartScreen.vue'),
  },
  {
    path: '/ProjectLauncher',
    name: 'ProjectLauncher',
    component: () => import('../views/layout/ProjectLauncher.vue'),
  },
  {
    path: '/NewGame',
    name: 'NewGame',
    component: () => import('../views/layout/NewGame.vue'),
  },
  {
    path: '/JoinGame',
    name: 'JoinGame',
    component: () => import('../views/layout/JoinGame.vue'),
  },
  {
    path: '/RecentGames',
    name: 'RecentGames',
    component: () => import('../views/layout/RecentGames.vue'),
  },
  {
    path: '/AITalkBar',
    name: 'AITalkBar',
    component: AITalkBar,
  },
  {
    path: '/SceneBar',
    name: 'SceneBar',
    component: SceneBar,
  },
  {
    path: '/Object',
    name: 'Object',
    component: ObjectPanel,
  },
  {
    path: '/Pet',
    name: 'Pet',
    component: Pet,
  },
  {
    path: '/LogView',
    name: 'LogView',
    component: LogView,
  },
  {
    path: '/FileManager',
    name: 'FileManager',
    component: FileManager,
  },
  {
    path: '/SetUp',
    name: 'SetUp',
    component: EditorSettings,
  },
  {
    path: '/Network',
    name: 'Network',
    component: NetworkPanel,
  },
  {
    path: '/ProjectSettings',
    name: 'ProjectSettings',
    component: ProjectSettings,
  },
  {
    path: '/ScratchTool',
    name: 'ScratchTool',
    component: BlocklyWorkspace,
  },
  {
    path: '/CameraView',
    name: 'CameraView',
    component: () => import('../views/tools/CameraView.vue'),
  },
  {
    path: '/LightFieldCalibration',
    name: 'LightFieldCalibration',
    component: LightFieldCalibrationPanel,
  },
];

const router = createRouter({
  history: createWebHashHistory(),
  routes,
});

router.beforeEach((to, from) => {});

window.__ROUTES__ = routes;

export default router;
