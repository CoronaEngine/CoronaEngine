<script setup>
import { useRouter } from 'vue-router';
import { worldLoadingState } from '@/services/worldLoadingService.js';
const router = useRouter();
async function leave() {
  await router.replace('/StartScreen');
  if (!worldLoadingState.busy && !worldLoadingState.blocked) worldLoadingState.error = '';
}
</script>
<template>
  <div v-if="worldLoadingState.busy || worldLoadingState.blocked || worldLoadingState.error" class="world-loading-overlay"
    :role="worldLoadingState.error ? 'alert' : 'status'" aria-live="polite" :aria-busy="worldLoadingState.busy" @pointerdown.stop @wheel.stop.prevent>
    <div class="world-loading-card">
      <span v-if="worldLoadingState.busy" class="world-loading-spinner" aria-hidden="true" />
      <p>{{ worldLoadingState.error || `${worldLoadingState.phase}…` }}</p>
      <button v-if="worldLoadingState.error && !worldLoadingState.blocked" @click="leave">返回启动页</button>
    </div>
  </div>
</template>
<style scoped>
.world-loading-overlay { position: fixed; inset: 0; z-index: 100010; display: grid; place-items: center; background: #090d0bee; color: #e9dcbd; }
.world-loading-card { display: grid; justify-items: center; gap: 14px; width: min(420px, 85vw); padding: 28px; border: 1px solid #887346; background: #171c15; font-size: 14px; text-align: center; overflow-wrap: anywhere; }
p { margin: 0; }
button { padding: 8px 22px; border: 1px solid #b29454; background: #303523; color: #eddbb3; cursor: pointer; }
button:focus-visible { outline: 2px solid #f3cf79; outline-offset: 3px; }
.world-loading-spinner { width: 23px; height: 23px; border: 2px solid #61563e; border-top-color: #e1c383; border-radius: 50%; animation: world-loading-spin 1s linear infinite; }
@keyframes world-loading-spin { to { transform: rotate(360deg); } }
@media (prefers-reduced-motion: reduce) { .world-loading-spinner { animation: none; } }
</style>
