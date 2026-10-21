import { ref, watch } from 'vue';

export const reblurGroups = [
  { label: '时间累积', fields: [
    ['max_history', '最大历史帧数', 50, 1, 63, 1, '增大可减少静态噪声，但响应变化更慢。'],
    ['fast_history', '快速历史帧数', 6, 0, 63, 1, '用于跟踪亮度变化，不超过最大历史帧数。'],
    ['history_fix_frames', '历史修复帧数', 3, 0, 63, 1, '历史不足时使用空间邻域修复。'],
    ['history_fix_stride', '历史修复步长', 14, 0, 100, 1, '控制历史修复时邻域采样的间距。'],
  ] },
  { label: '空间滤波', fields: [
    ['diffuse_prepass', '漫反射预滤波半径', 15, 0, 100, 1, '屏幕像素半径，0 表示不扩散漫反射预滤波。'],
    ['specular_prepass', '镜面预滤波半径', 40, 0, 100, 1, '屏幕像素半径，增大可能模糊反射细节。'],
    ['min_radius', '最小模糊半径', 1, 0, 100, 0.5, '不超过最大模糊半径。'],
    ['max_radius', '最大模糊半径', 30, 0, 100, 1, '限制空间滤波的最大屏幕半径。'],
    ['lobe_fraction', '法线波瓣比例', 0.15, 0, 1, 0.01, '控制滤波对法线差异的容忍程度。'],
    ['roughness_fraction', '粗糙度容差', 0.15, 0, 1, 0.01, '控制镜面滤波对粗糙度差异的容忍程度。'],
    ['plane_sensitivity', '平面距离容差', 0.02, 0, 1, 0.001, '越小越严格地保留几何边界。'],
    ['min_hit_weight', '最小命中距离权重', 0.1, 0, 1, 0.01, '控制不同光线命中距离样本的混合。'],
  ] },
  { label: '稳定与抗拖影', fields: [
    ['stabilization', '时间稳定强度', 63 / 64, 0, 1, 0.05, '0 关闭额外时间稳定，1 使用完整强度。'],
    ['antilag_sigma', '抗拖影亮度容差', 4, 0, 10, 0.1, '以局部亮度标准差控制历史亮度截断。'],
    ['antilag_sensitivity', '抗拖影响应参数', 3, 0, 10, 0.1, '较小值会更快降低变化区域的历史权重。'],
  ] },
];
export const reblurDefaults = Object.fromEntries([
  ...reblurGroups.flatMap(group => group.fields.map(([key, , value]) => [key, value])),
  ['anti_firefly', true], ['reconstruction', 2],
]);

export function validateReblurSettings(settings) {
  for (const group of reblurGroups) {
    for (const [key, label, , min, max] of group.fields) {
      if (typeof settings[key] !== 'number' || !Number.isFinite(settings[key]) || settings[key] < min || settings[key] > max)
        return `${label}须在 ${min}–${max} 之间`;
    }
  }
  if (settings.fast_history > settings.max_history) return '快速历史帧数不能超过最大历史帧数';
  if (settings.min_radius > settings.max_radius) return '最小模糊半径不能超过最大模糊半径';
  if (![0, 1, 2].includes(settings.reconstruction)) return '请选择有效的命中距离重建模式';
  if (typeof settings.anti_firefly !== 'boolean') return '亮点抑制须为开关值';
  return '';
}

export function useReblurSettings(props, api) {
  const open = ref(false), busy = ref(false), error = ref(''), saved = ref(false);
  const draft = ref({ ...reblurDefaults });
  let generation = 0;
  watch(() => [props.sceneId, props.cameraId, props.enabled, open.value], async (_, __, onCleanup) => {
    const token = ++generation;
    onCleanup(() => { ++generation; });
    error.value = ''; saved.value = false; busy.value = false;
    draft.value = { ...reblurDefaults };
    if (!props.enabled || !props.cameraId) { open.value = false; return; }
    if (!open.value) return;
    busy.value = true;
    try {
      const result = await api.getReblurSettings(props.sceneId, props.cameraId);
      const response = result?.data ?? result;
      if (token !== generation) return;
      if (response?.status === 'error') throw new Error(response.message || '读取参数失败');
      draft.value = { ...reblurDefaults, ...response.settings };
      // Native settings use float32; hide binary serialization noise in the inputs.
      for (const group of reblurGroups) {
        for (const [key] of group.fields) {
          const value = draft.value[key];
          if (typeof value === 'number') draft.value[key] = Number(value.toPrecision(7));
        }
      }
    } catch (e) {
      if (token === generation) error.value = e?.message || '读取参数失败';
    } finally {
      if (token === generation) busy.value = false;
    }
  }, { immediate: true, flush: 'sync' });

  async function apply() {
    if (!props.enabled || !props.cameraId || busy.value) return false;
    error.value = validateReblurSettings(draft.value);
    saved.value = false;
    if (error.value) return false;
    const token = generation;
    const settings = { ...draft.value };
    busy.value = true;
    try {
      const result = await api.setReblurSettings(props.sceneId, props.cameraId, settings);
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
  function reset() { draft.value = { ...reblurDefaults }; error.value = ''; saved.value = false; }
  return { open, busy, error, saved, draft, apply, reset };
}
