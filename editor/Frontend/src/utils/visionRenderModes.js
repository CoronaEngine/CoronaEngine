// Shared by the main viewport and detached camera windows.
export const visionRenderModes = [
  { value: 'path_tracing', backend: 'vision', label: 'Vision PT' },
  { value: 'restir', backend: 'vision', label: 'Vision ReSTIR' },
  { value: 'ssat', backend: 'vision', label: 'Vision SSAT' },
];

export const normalizeVisionRenderMode = (mode) =>
  mode === 'svgf' || mode === 'progressive_path_tracing' ? 'path_tracing' : mode || 'path_tracing';

export const visionAccumulationFromCamera = (camera) => {
  const enabled = camera?.vision_accumulation;
  if (enabled !== undefined) return enabled === true || enabled === 'true';
  return camera?.vision_render_mode === 'progressive_path_tracing';
};

export const visionDenoiseFromCamera = (camera) => {
  const enabled = camera?.vision_denoise;
  if (typeof enabled === 'boolean') return enabled;
  if (enabled === 'true') return true;
  if (enabled === 'false') return false;
  return camera?.vision_render_mode === 'svgf';
};
