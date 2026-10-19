import { ref, watch } from 'vue';

export const restirPathFields = [
  ['max_depth', '最大间接路径深度', 0, 128, 1, '限制间接路径的续接深度；0 仍计算首个表面的直接光照，增大通常增加耗时。'],
  ['min_depth', 'RR 起始深度', 0, 20, 1, '达到此深度后，允许通过 Russian Roulette 随机终止低贡献路径。'],
  ['rr_threshold', 'RR 阈值', 0, 1, 0.01, '路径贡献低于此阈值时允许随机终止；0 关闭随机终止。'],
];
export const restirGroups = [
  { key: 'direct', label: '直接光照（DI）', prefix: 'DI', fields: [
    ['M_light', '光源候选数', 0, 100, 1, '每个像素从光源采样的候选数量。'],
    ['M_bsdf', 'BSDF 候选数', 0, 100, 1, '每个像素通过材质散射分布采样的候选数量。'],
    ['max_age', '最大样本年龄', 0, 100, 1, '限制复用样本保留的帧数。'],
    ['max_recursion', '最大递归次数', 1, 100, 1, '限制直接光照递归次数；修改后可能需要重新编译着色器。'],
    ['diffuse_factor', '漫反射差异阈值', 0, 1, 0.01, '复用邻居样本时，漫反射因子的相对差异超出此阈值则拒绝复用。'],
  ] },
  { key: 'indirect', label: '间接光照（GI）', prefix: 'GI', fields: [
    ['max_age', '最大样本年龄', 0, 100, 1, '限制间接光照复用样本保留的帧数。'],
    ['diffuse_factor', '漫反射差异阈值', 0, 1, 0.01, '复用邻居样本时，漫反射因子的相对差异超出此阈值则拒绝复用。'],
  ] },
];
export const restirReuseGroups = [
  { key: 'temporal', label: '时间复用', fields: [
    ['history_limit', '历史样本上限', 0, 50, 1, '限制上一帧参与时间复用的历史样本数量。'],
    ['theta', '法线角度阈值', 0, 90, 0.1, '允许复用的表面法线最大夹角，单位为度。'],
    ['depth', '深度阈值', 0, 1, 0.01, '允许复用的表面相对深度差阈值。'],
    ['radius', '半径', 0, 50, 0.1, '时间复用时搜索历史样本的半径。'],
  ] },
  { key: 'spatial', label: '空间复用', fields: [
    ['theta', '法线角度阈值', 0, 90, 0.1, '允许复用的邻域表面法线最大夹角，单位为度。'],
    ['depth', '深度阈值', 0, 1, 0.01, '允许复用的邻域表面相对深度差阈值。'],
    ['radius', '半径', 0, 50, 0.1, '空间复用时搜索邻域样本的半径。'],
  ] },
];
export const restirDefaults = {
  max_depth: 15, min_depth: 5, rr_threshold: 1,
  direct: { open: true, M_light: 10, M_bsdf: 1, max_age: 30, max_recursion: 5,
    diffuse_factor: 0.3, pairwise: true,
    temporal: { open: true, history_limit: 5, theta: 35, depth: 0.3, radius: 8 },
    spatial: { open: true, theta: 35, depth: 0.3, radius: 30 } },
  indirect: { open: true, max_age: 30, diffuse_factor: 0.3,
    temporal: { open: true, history_limit: 5, theta: 35, depth: 0.3, radius: 8 },
    spatial: { open: true, theta: 35, depth: 0.3, radius: 12 } },
};

function validateFields(settings, fields, prefix = '') {
  for (const [key, label, min, max, step] of fields) {
    const value = settings?.[key];
    if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max ||
      (step === 1 && !Number.isInteger(value)))
      return `${prefix}${label}须为 ${min}–${max} 之间的${step === 1 ? '整数' : '数值'}`;
  }
  return '';
}

export function validateRestirSettings(settings) {
  let error = validateFields(settings, restirPathFields);
  if (error) return error;
  for (const group of restirGroups) {
    const values = settings[group.key];
    if (typeof values?.open !== 'boolean') return `启用${group.label}须为布尔值`;
    error = validateFields(values, group.fields, `${group.prefix} `);
    if (error) return error;
    if (group.key === 'direct' && typeof values.pairwise !== 'boolean') return 'DI 成对 MIS 须为布尔值';
    for (const reuse of restirReuseGroups) {
      if (typeof values[reuse.key]?.open !== 'boolean') return `启用 ${group.prefix} ${reuse.label}须为布尔值`;
      error = validateFields(values[reuse.key], reuse.fields, `${group.prefix} ${reuse.label}`);
      if (error) return error;
    }
  }
  return '';
}

// The defaults also define the writable shape: unsupported native options never enter a request.
function copySettings(source = {}, fallback = restirDefaults) {
  return Object.fromEntries(Object.entries(fallback).map(([key, value]) => {
    const incoming = source?.[key];
    if (value !== null && typeof value === 'object') return [key, copySettings(incoming, value)];
    return [key, incoming === undefined ? value : incoming];
  }));
}

function tidyFloat32(settings) {
  for (const [key, value] of Object.entries(settings)) {
    if (value !== null && typeof value === 'object') tidyFloat32(value);
    else if (typeof value === 'number') {
      const rounded = Number(value.toPrecision(7));
      if (Math.fround(rounded) === Math.fround(value)) settings[key] = rounded;
    }
  }
}

export function useRestirSettings(props, api) {
  const open = ref(false), busy = ref(false), error = ref(''), saved = ref(false);
  const draft = ref(copySettings()), loaded = ref(false);
  let generation = 0;
  watch(() => [props.sceneId, props.cameraId, props.enabled, open.value], async (_, __, onCleanup) => {
    const token = ++generation;
    let cancelRetry = () => {};
    onCleanup(() => { ++generation; cancelRetry(); });
    error.value = ''; saved.value = false; busy.value = false;
    loaded.value = false; draft.value = copySettings();
    if (!props.enabled || !props.cameraId) { open.value = false; return; }
    if (!open.value) return;
    busy.value = true;
    try {
      const deadline = Date.now() + 120000;
      let response;
      while (true) {
        const result = await api.getRestirSettings(props.sceneId, props.cameraId);
        response = result?.data ?? result;
        if (token !== generation) return;
        if (response?.status === 'error') throw new Error(response.message || '读取参数失败');
        if (response?.ready !== false) break;
        if (Date.now() >= deadline) throw new Error('ReSTIR 参数尚未就绪，请稍后重新打开面板');
        await new Promise(resolve => {
          const timer = setTimeout(resolve, 150);
          cancelRetry = () => { clearTimeout(timer); resolve(); };
        });
        if (token !== generation) return;
      }
      draft.value = copySettings(response.settings);
      tidyFloat32(draft.value);
      loaded.value = true;
    } catch (e) {
      if (token === generation) error.value = e?.message || '读取参数失败';
    } finally {
      if (token === generation) busy.value = false;
    }
  }, { immediate: true, flush: 'sync' });

  async function apply() {
    if (!props.enabled || !props.cameraId || busy.value || !loaded.value) return false;
    error.value = validateRestirSettings(draft.value);
    saved.value = false;
    if (error.value) return false;
    const token = generation, settings = copySettings(draft.value);
    busy.value = true;
    try {
      const result = await api.setRestirSettings(props.sceneId, props.cameraId, settings);
      const response = result?.data ?? result;
      if (token !== generation) return false;
      if (response?.status === 'error') throw new Error(response.message || '应用参数失败');
      draft.value = copySettings(response.settings, settings);
      saved.value = true;
      return true;
    } catch (e) {
      if (token === generation) error.value = e?.message || '应用参数失败';
      return false;
    } finally {
      if (token === generation) busy.value = false;
    }
  }
  function reset() { draft.value = copySettings(); error.value = ''; saved.value = false; }
  return { open, busy, error, saved, draft, loaded, apply, reset };
}
