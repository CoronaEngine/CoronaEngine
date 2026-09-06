/**
 * 剧情模式玩家模型：集中保存玩家的位置、移动速度、出生点和视角状态。
 */
import * as THREE from 'three';

export function createStoryPlayer(options = {}) {
  const spawn = new THREE.Vector3(...(options.spawn ?? [0, 1.7, 4]));
  const player = {
    position: spawn.clone(),
    spawn: spawn.clone(),
    velocityY: 0,
    grounded: true,
    height: options.height ?? 1.7,
    speed: options.speed ?? 5,
    yaw: options.yaw ?? 0,
    pitch: options.pitch ?? 0,
  };

  return {
    player,
    resetToSpawn() {
      player.position.copy(player.spawn);
      player.velocityY = 0;
      player.grounded = true;
      player.yaw = options.yaw ?? 0;
      player.pitch = options.pitch ?? 0;
    },
  };
}
