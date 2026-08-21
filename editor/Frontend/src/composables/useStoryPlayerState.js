import { computed } from 'vue';

export function useStoryPlayerState(cameraBinding, ready) {
  return computed(() => {
    const binding = cameraBinding.value;
    return {
      sceneId: String(binding?.sceneId || ''),
      position: Array.isArray(binding?.position) ? [...binding.position] : [0, 0, 0],
      forward: Array.isArray(binding?.forward) ? [...binding.forward] : [0, 0, 1],
      worldUp: Array.isArray(binding?.worldUp) ? [...binding.worldUp] : [0, 1, 0],
      isReady: Boolean(ready.value && binding),
    };
  });
}
