/** 小世界模式状态机：仅管理建造/试玩切换，不把临时试玩切换算作存档修改。 */
import { UGC_WORLD_MODES } from './ugcWorldState.js';

/** 创建模式控制器；保存中的锁由会话层统一管理。 */
export function createUgcWorldMode({ state, onModeChanged } = {}) {
  return {
    getMode: () => state.mode,
    isBuildMode: () => state.mode === 'build',
    isPlayMode: () => state.mode === 'play',
    canEdit: () => state.mode === 'build',
    setMode(mode) {
      if (!UGC_WORLD_MODES.includes(mode)) return false;
      if (state.mode === mode) return true;
      state.mode = mode;
      onModeChanged?.(mode);
      return true;
    },
    reset() {
      state.mode = 'build';
      onModeChanged?.('build');
      return true;
    },
  };
}
