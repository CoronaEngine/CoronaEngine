/**
 * 剧情模式摄像机控制器：负责鼠标视角、灵敏度和摄像机同步。
 */
export const STORY_CAMERA_CONFIG = Object.freeze({
  sensitivity: 0.0025,
  pitchLimit: 1.45,
});

export function createStoryCameraController(options = {}) {
  const sensitivity = options.sensitivity ?? STORY_CAMERA_CONFIG.sensitivity;
  const pitchLimit = options.pitchLimit ?? STORY_CAMERA_CONFIG.pitchLimit;

  return {
    updateLook(player, delta) {
      player.yaw -= delta.x * sensitivity;
      player.pitch = Math.max(
        -pitchLimit,
        Math.min(pitchLimit, player.pitch - delta.y * sensitivity),
      );
    },

    applyToCamera(camera, player) {
      camera.position.copy(player.position);
      camera.rotation.set(player.pitch, player.yaw, 0, 'YXZ');
    },

    reset(player) {
      player.yaw = 0;
      player.pitch = 0;
    },
  };
}
