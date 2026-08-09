// 路由文件
import { createRouter, createWebHashHistory } from 'vue-router';

import BlocklyWorkspace from '../blockly/components/BlocklyWorkspace.vue';
import { getPluginComponent } from '../views/panelRegistry.js';

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
    path: '/SceneBar',
    name: 'SceneBar',
    component: getPluginComponent('SceneTools'),
  },
  {
    path: '/NodeGraph',
    name: 'NodeGraphPanel',
    component: getPluginComponent('NodeGraphPanel'),
  },
  {
    path: '/CabbageChat',
    name: 'CabbageChatPanel',
    component: getPluginComponent('CabbageChatPanel'),
  },
  {
    path: '/Object',
    name: 'Object',
    component: getPluginComponent('Object'),
  },
  {
    path: '/Pet',
    name: 'Pet',
    component: getPluginComponent('AITool'),
  },
  {
    path: '/LogView',
    name: 'LogView',
    component: getPluginComponent('LogTool'),
  },
  {
    path: '/FileManager',
    name: 'FileManager',
    component: getPluginComponent('FileManager'),
  },
  {
    path: '/SetUp',
    name: 'SetUp',
    component: getPluginComponent('EditorSettings'),
  },
  {
    path: '/Network',
    name: 'Network',
    component: getPluginComponent('Network'),
  },
  {
    path: '/ProjectSettings',
    name: 'ProjectSettings',
    component: getPluginComponent('ProjectSettings'),
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
    component: getPluginComponent('LightFieldCalibration'),
  },
];

const router = createRouter({
  history: createWebHashHistory(),
  routes,
});

router.beforeEach((to, from) => {});

window.__ROUTES__ = routes;

export default router;
