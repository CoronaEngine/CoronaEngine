/**
 * 剧情模式资源配置：集中定义可选的外部场景资源。
 *
 * 当前已移除之前导入的地形模型，因此 terrain.enabled 默认关闭。
 * 后续重新导入 GLB 时，只需将文件放到 public/assets/story/terrain/，
 * 再将 enabled 改为 true 即可恢复加载。
 */
const baseUrl = typeof import.meta.env?.BASE_URL === 'string' ? import.meta.env.BASE_URL : '/';

export const STORY_ASSETS = Object.freeze({
  mossRuins: Object.freeze({
    enabled: true,
    baseUrl: `${baseUrl}assets/story/moss-ruins/`,
  }),
  terrain: Object.freeze({
    enabled: false,
    url: `${baseUrl}assets/story/terrain/terrain.glb`,
    // 后续重新导入模型时，在这里调整模型缩放。
    scale: Object.freeze([0.08, 0.08, 0.08]),
    // 后续重新导入模型时，在这里调整模型位置。
    position: Object.freeze([81.92, -12, -140]),
    rotation: Object.freeze([0, 0, 0]),
  }),
});
