import { ref, watch } from 'vue';

export const relaxGroups = [
  { label: "时间累积", fields: [
    ["diffuse_history", "漫反射历史帧数", 30, 1, 255, 1, "增大可减少静态噪声，但响应变化更慢。"],
    ["specular_history", "镜面历史帧数", 30, 1, 255, 1, "控制反射信号的历史长度。"],
    ["diffuse_fast_history", "漫反射快速历史", 6, 0, 255, 1, "不超过对应的历史帧数。"],
    ["specular_fast_history", "镜面快速历史", 6, 0, 255, 1, "不超过对应的历史帧数。"],
    ["history_fix_frames", "历史修复帧数", 3, 0, 255, 1, "历史不足时使用邻域修复。"],
    ["history_fix_stride", "历史修复步长", 14, 0, 100, 1, "控制修复的采样间距。"],
    ["history_fix_normal_power", "修复法线敏感度", 8, 0, 128, 1, "越大越严格地区分法线边界。"],
    ["variance_history_threshold", "方差历史阈值", 3, 0, 255, 1, "历史不足时从空间邻域估计方差。"],
  ] },
  { label: "预滤波与空间滤波", fields: [
    ["diffuse_prepass", "漫反射预滤波半径", 30, 0, 100, 1, "屏幕像素半径，0 关闭扩散。"],
    ["specular_prepass", "镜面预滤波半径", 50, 0, 100, 1, "增大可能模糊反射细节。"],
    ["min_hit_weight", "最小命中距离权重", 0.1, 0, 1, 0.01, "控制命中距离不同的样本混合。"],
    ["atrous_iterations", "À-trous 迭代次数", 5, 2, 8, 1, "更多迭代扩大滤波范围，也增加耗时。"],
    ["diffuse_phi", "漫反射亮度容差", 2, 0.01, 100, 0.1, "增大可混合亮度差异更大的样本。"],
    ["specular_phi", "镜面亮度容差", 1, 0.01, 100, 0.1, "越小越保留反射亮度细节。"],
  ] },
  { label: "边缘与反射", fields: [
    ["lobe_fraction", "法线波瓣比例", 0.5, 0, 1, 0.01, "控制法线差异的容忍程度。"],
    ["roughness_fraction", "粗糙度容差", 0.15, 0, 1, 0.01, "控制镜面粗糙度差异的容忍程度。"],
    ["specular_variance_boost", "镜面方差增强", 0, 0, 10, 0.1, "增大可增强低历史区域的镜面滤波。"],
    ["specular_lobe_slack", "镜面波瓣余量", 0.15, 0, 1, 0.01, "控制反射波瓣边界的放宽程度。"],
    ["color_box_sigma", "历史颜色裁剪范围", 2, 0, 10, 0.1, "较小值会更严格地限制历史颜色。"],
    ["depth_threshold", "深度边缘容差", 0.003, 0, 0.1, 0.001, "越小越严格地保留几何边界。"],
  ] },
  { label: "滤波放宽", fields: [
    ["normal_relaxation", "法线放宽强度", 0.3, 0, 1, 0.05, "放宽高方差区域的法线限制。"],
    ["roughness_relaxation", "亮度与粗糙度放宽", 1, 0, 1, 0.05, "按当前 NRD 映射同时放宽镜面亮度与粗糙度限制。"],
  ] },
  { label: "抗拖影", fields: [
    ["antilag_acceleration", "历史加速强度", 0.3, 0, 1, 0.05, "亮度变化时加快历史响应。"],
    ["antilag_spatial_sigma", "空间亮度容差", 4.5, 0, 10, 0.1, "用于判断变化是否超出当前邻域。"],
    ["antilag_temporal_sigma", "时间亮度容差", 0.5, 0, 10, 0.1, "用于比较快速历史和长期历史。"],
    ["antilag_reset", "历史重置强度", 0.5, 0, 1, 0.05, "越大越快丢弃发生变化的历史。"],
  ] },
];
export const relaxDefaults = Object.fromEntries([
  ...relaxGroups.flatMap(group => group.fields.map(([key, , value]) => [key, value])),
  ['anti_firefly', false], ['roughness_stopping', true], ['reconstruction', 0],
]);

export function validateRelaxSettings(settings) {
  for (const group of relaxGroups) {
    for (const [key, label, , min, max] of group.fields) {
      if (typeof settings[key] !== 'number' || !Number.isFinite(settings[key]) || settings[key] < min || settings[key] > max)
        return `${label}须在 ${min}–${max} 之间`;
    }
  }
  if (settings.diffuse_fast_history > settings.diffuse_history) return '漫反射快速历史不能超过历史帧数';
  if (settings.specular_fast_history > settings.specular_history) return '镜面快速历史不能超过历史帧数';
  if (!Number.isInteger(settings.atrous_iterations)) return '迭代次数须为整数';
  if (![0, 1, 2].includes(settings.reconstruction)) return '请选择有效的命中距离重建模式';
  if (typeof settings.anti_firefly !== 'boolean' || typeof settings.roughness_stopping !== 'boolean') return '滤波开关须为布尔值';
  return '';
}

export function useRelaxSettings(props, api) {
  const open = ref(false), busy = ref(false), error = ref(''), saved = ref(false);
  const draft = ref({ ...relaxDefaults });
  let generation = 0;
  watch(() => [props.sceneId, props.cameraId, props.enabled, open.value], async (_, __, onCleanup) => {
    const token = ++generation;
    onCleanup(() => { ++generation; });
    error.value = ''; saved.value = false; busy.value = false;
    draft.value = { ...relaxDefaults };
    if (!props.enabled || !props.cameraId) { open.value = false; return; }
    if (!open.value) return;
    busy.value = true;
    try {
      const result = await api.getRelaxSettings(props.sceneId, props.cameraId);
      const response = result?.data ?? result;
      if (token !== generation) return;
      if (response?.status === 'error') throw new Error(response.message || '读取参数失败');
      draft.value = { ...relaxDefaults, ...response.settings };
      // Native settings use float32; hide binary serialization noise in the inputs.
      for (const group of relaxGroups) {
        for (const [key] of group.fields) {
          const value = draft.value[key];
          if (typeof value === 'number') {
            const rounded = Number(value.toPrecision(7));
            if (Math.fround(rounded) === Math.fround(value)) draft.value[key] = rounded;
          }
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
    error.value = validateRelaxSettings(draft.value);
    saved.value = false;
    if (error.value) return false;
    const token = generation;
    const settings = { ...draft.value };
    busy.value = true;
    try {
      const result = await api.setRelaxSettings(props.sceneId, props.cameraId, settings);
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
  function reset() { draft.value = { ...relaxDefaults }; error.value = ''; saved.value = false; }
  return { open, busy, error, saved, draft, apply, reset };
}
