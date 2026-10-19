import { ref, watch } from 'vue';

export const ptGroups = [{ label: '路径追踪', fields: [
  ['max_depth', '最大路径深度', 16, 0, 128, 1, '限制光线路径的散射深度；0 不追踪路径，增大通常增加耗时。'],
  ['min_depth', 'RR 起始深度', 5, 0, 20, 1, '达到此深度后，允许通过 Russian Roulette 随机终止低贡献路径。'],
  ['rr_threshold', 'RR 阈值', 1, 0, 1, 0.01, '路径贡献低于此阈值时允许随机终止；0 关闭随机终止。'],
] }];
export const ptDefaults = { max_depth: 16, min_depth: 5, rr_threshold: 1 };
export const ptAdaptiveDefaults = { enabled: false, threshold: 0.001, start_index: 128 };

export function validatePtSettings(settings) {
  for (const group of ptGroups) {
    for (const [key, label, , min, max] of group.fields) {
      const value = settings[key];
      if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max ||
        (key !== 'rr_threshold' && !Number.isInteger(value)))
        return `${label}须为 ${min}–${max} 之间的${key === 'rr_threshold' ? '数值' : '整数'}`;
    }
  }
  return '';
}

export function usePtSettings(props, api) {
  const open = ref(false), busy = ref(false), error = ref(''), saved = ref(false);
  const draft = ref({ ...ptDefaults });
  const adaptive = ref({ ...ptAdaptiveDefaults });
  const loaded = ref(false);
  let generation = 0;
  watch(() => [props.sceneId, props.cameraId, props.enabled, open.value], async (_, __, onCleanup) => {
    const token = ++generation;
    let cancelRetry = () => {};
    onCleanup(() => { ++generation; cancelRetry(); });
    error.value = ''; saved.value = false; busy.value = false;
    loaded.value = false;
    draft.value = { ...ptDefaults };
    adaptive.value = { ...ptAdaptiveDefaults };
    if (!props.enabled || !props.cameraId) { open.value = false; return; }
    if (!open.value) return;
    busy.value = true;
    try {
      const deadline = Date.now() + 30000;
      let response;
      while (true) {
        const result = await api.getPtSettings(props.sceneId, props.cameraId);
        response = result?.data ?? result;
        if (token !== generation) return;
        if (response?.status === 'error') throw new Error(response.message || '读取参数失败');
        if (response?.ready !== false) break;
        if (Date.now() >= deadline) throw new Error('PT 参数尚未就绪，请稍后重新打开面板');
        await new Promise(resolve => {
          const timer = setTimeout(resolve, 150);
          cancelRetry = () => { clearTimeout(timer); resolve(); };
        });
        if (token !== generation) return;
      }
      draft.value = { ...ptDefaults, ...response.settings };
      adaptive.value = { ...ptAdaptiveDefaults, ...response.adaptive };
      // Native settings use float32; hide binary serialization noise in the inputs.
      for (const group of ptGroups) {
        for (const [key] of group.fields) {
          const value = draft.value[key];
          if (typeof value === 'number') {
            const rounded = Number(value.toPrecision(7));
            if (Math.fround(rounded) === Math.fround(value)) draft.value[key] = rounded;
          }
        }
      }
      loaded.value = true;
    } catch (e) {
      if (token === generation) error.value = e?.message || '读取参数失败';
    } finally {
      if (token === generation) busy.value = false;
    }
  }, { immediate: true, flush: 'sync' });

  async function apply() {
    if (!props.enabled || !props.cameraId || busy.value || !loaded.value) return false;
    error.value = validatePtSettings(draft.value);
    saved.value = false;
    if (error.value) return false;
    const token = generation;
    const settings = { ...draft.value };
    busy.value = true;
    try {
      const result = await api.setPtSettings(props.sceneId, props.cameraId, settings);
      const response = result?.data ?? result;
      if (token !== generation) return false;
      if (response?.status === 'error') throw new Error(response.message || '应用参数失败');
      draft.value = { ...settings, ...response.settings };
      saved.value = true;
      return true;
    } catch (e) {
      if (token === generation) error.value = e?.message || '应用参数失败';
      return false;
    } finally {
      if (token === generation) busy.value = false;
    }
  }
  function reset() { draft.value = { ...ptDefaults }; error.value = ''; saved.value = false; }
  return { open, busy, error, saved, draft, adaptive, loaded, apply, reset };
}
