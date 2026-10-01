import { shallowReactive } from 'vue';
import { createLoadOperation, hasPendingLoadTimeout, subscribePendingLoads } from '../../../../game/frontend/worldLoading.mjs';
import { trackWorldSessionWork } from './worldSessionLifecycle.js';

export const worldLoadingState = shallowReactive({ operationId: '', busy: false, blocked: false, phase: '', error: '' });
// Launcher and page initialization share trace IDs, not UI ownership.
let activeOperation = null;
subscribePendingLoads(blocked => { worldLoadingState.blocked = blocked; });
export function beginWorldLoad(options = {}) {
  const operation = createLoadOperation({ ...options, trackWork: trackWorldSessionWork, onUpdate(record) {
    if (activeOperation !== operation) return;
    if (record.status === 'loading') Object.assign(worldLoadingState, { busy: true, phase: record.phase, error: '' });
    if (record.status === 'error') Object.assign(worldLoadingState, { busy: false, error: `${record.phase}：${record.message}`, blocked: record.pending });
    if (record.status === 'settled') worldLoadingState.blocked = hasPendingLoadTimeout();
    if (record.status === 'canceled') Object.assign(worldLoadingState, { busy: false, blocked: hasPendingLoadTimeout() });
    if (record.status === 'complete') Object.assign(worldLoadingState, { busy: false, blocked: false, phase: '', error: '' });
  } });
  activeOperation = operation;
  Object.assign(worldLoadingState, { operationId: operation.id, busy: true, blocked: false, phase: '准备加载', error: '' });
  return operation;
}
