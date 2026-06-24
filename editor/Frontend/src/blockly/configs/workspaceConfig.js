import { createToolboxConfig, TOOLBOX_CONFIG } from './toolboxConfig';
import { CoronaTheme } from './theme';

export const WORKSPACE_CONFIG = {
  toolbox: TOOLBOX_CONFIG,
  theme: CoronaTheme,
  toolboxPosition: 'start',
  horizontalLayout: false,
  contextMenu: true,

  // 内存优化：限制 undo 历史栈大小
  maxUndo: 50, // 默认无限制，这里限制为 50 步

  scrollbars: {
    horizontal: true,
    vertical: true,
    set: true,
  },
  grid: {
    spacing: 20,
    length: 3,
    colour: '#ccc',
    snap: true,
  },
  zoom: {
    controls: false,
    wheel: true,
    startScale: 1.0,
    maxScale: 3,
    minScale: 0.3,
  },
  trashcan: false,
  renderer: 'zelos',
};

export function createWorkspaceConfig(t) {
  return {
    ...WORKSPACE_CONFIG,
    toolbox: createToolboxConfig(t),
  };
}
