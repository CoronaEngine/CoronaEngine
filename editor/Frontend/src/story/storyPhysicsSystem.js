/**
 * 剧情模式物理：提供可替换的重力、地面检测、跳跃和越界复位逻辑。
 */
export function createStoryPhysicsSystem({
  player,
  gravity = -18,
  jumpVelocity = 7,
  groundY = 0,
  boundary = 90,
  onRespawn,
}) {
  return {
    jump() {
      if (!player.grounded) return false;
      player.velocityY = jumpVelocity;
      player.grounded = false;
      return true;
    },

    update(delta) {
      player.velocityY += gravity * delta;
      player.position.y += player.velocityY * delta;

      if (player.position.y <= groundY + player.height) {
        player.position.y = groundY + player.height;
        player.velocityY = 0;
        player.grounded = true;
      }

      if (
        Math.abs(player.position.x) > boundary
        || Math.abs(player.position.z) > boundary
        || player.position.y < -20
      ) {
        player.position.copy(player.spawn);
        player.velocityY = 0;
        player.grounded = true;
        onRespawn?.();
      }
    },
  };
}
