/**
 * 剧情模式运行时：协调输入、玩家、摄像机、物理、交互和攻击系统。
 */
export function calculateViewRelativeMovement(axis, yaw) {
  const forward = {
    x: -Math.sin(yaw),
    z: -Math.cos(yaw),
  };
  const right = {
    x: Math.cos(yaw),
    z: -Math.sin(yaw),
  };
  const x = forward.x * -axis.z + right.x * axis.x;
  const z = forward.z * -axis.z + right.z * axis.x;
  const length = Math.hypot(x, z);

  if (!length) return { x: 0, z: 0 };
  return { x: x / length, z: z / length };
}

export function clampPitch(pitch, limit = 1.45) {
  return Math.max(-limit, Math.min(limit, pitch));
}

export function createStoryRuntime({
  input,
  camera,
  player,
  physics,
  cameraController,
  interactionSystem,
  combatSystem,
  onInteraction,
  onAttack,
}) {
  let paused = false;

  return {
    start() {
      paused = false;
    },

    pause() {
      paused = true;
    },

    resume() {
      paused = false;
    },

    update(delta) {
      if (paused) return;

      if (input.consumePressed('jump')) physics.jump();
      physics.update(delta);

      const axis = input.getMoveAxis();
      const movement = calculateViewRelativeMovement(axis, player.yaw);
      const distance = player.speed * delta;
      player.position.x += movement.x * distance;
      player.position.z += movement.z * distance;

      cameraController.updateLook(player, input.consumeLookDelta());
      cameraController.applyToCamera(camera, player);

      if (input.consumePressed('attack')) {
        onAttack?.(combatSystem?.attack());
      }

      if (input.consumePressed('interact')) {
        onInteraction?.(interactionSystem?.interact({ player, camera }));
      }
    },

    getMoveAxis() {
      return input.getMoveAxis();
    },

    getInteractionPrompt() {
      return interactionSystem?.getPrompt?.() || '';
    },

    stop() {
      paused = true;
    },

    isPaused() {
      return paused;
    },
  };
}

