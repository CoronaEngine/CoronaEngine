import * as Blockly from 'blockly/core';

// ── 辅助：自动生成 secondary / tertiary（比主色暗 30% / 50%）──
function darker(hex, pct) {
  const num = parseInt(hex.replace('#', ''), 16);
  const r = Math.round(((num >> 16) & 0xff) * (1 - pct));
  const g = Math.round(((num >> 8) & 0xff) * (1 - pct));
  const b = Math.round((num & 0xff) * (1 - pct));
  return '#' + ((1 << 24) + (r << 16) + (g << 8) + b).toString(16).slice(1);
}

// ── 统一定义：每种分类色同时用于 blockStyle 和 categoryStyle ──
const COLOURS = {
  engine:     '#5631E4',
  camera:     '#4A90D9',
  appearance: '#C501F6',
  event:      '#FFDE59',
  control:    '#FFAB19',
  detect:     '#4CBFE6',
  math:       '#59C059',
  variable:   '#FF8C1A',
  list:       '#FF661A',
  text:       '#3EB07A',
  function:   '#C040C0',
  audio:      '#D94A8C',
};

// 生成标准格式的 blockStyle
function bs(hex) {
  return {
    colourPrimary: hex,
    colourSecondary: darker(hex, 0.3),
    colourTertiary: darker(hex, 0.5),
  };
}

export const CoronaTheme = Blockly.Theme.defineTheme('CoronaTheme', {
  base: Blockly.Themes.Classic,

  // Blockly v13 componentStyles：支持 CSS 变量驱动的组件样式（上游 #8274）
  // 深色主题配色，适配 CabbageEditor 整体暗色 UI
  componentStyles: {
    workspaceBackgroundColour: '#1e1e1e',
    toolboxBackgroundColour: '#252526',
    toolboxForegroundColour: '#ffffff',
    flyoutBackgroundColour: '#252526',
    flyoutForegroundColour: '#cccccc',
    flyoutOpacity: 1,
    scrollbarColour: '#797979',
    scrollbarOpacity: 0.4,
    insertionMarkerColour: '#ffffff',
    insertionMarkerOpacity: 0.15,
    cursorColour: '#d0d0d0',
    blackoutColour: 'rgba(0, 0, 0, .7)',
    // v13 CSS 变量兼容：为未来版本预留的自定义属性
    selectedGlowColour: '#84a65b',
    selectedGlowOpacity: 0.4,
    replacementGlowColour: '#ffffff',
    replacementGlowOpacity: 0.3,
  },

  // ── 积木块样式（blockStyles）──
  // 每个 blockStyle 的 colourPrimary 都与其对应 categoryStyle 的 colour 一致
  blockStyles: {
    // ── CoronaEngine 自定义分类 ──
    engine_blocks:     bs(COLOURS.engine),
    camera_blocks:     bs(COLOURS.camera),
    appearance_blocks: bs(COLOURS.appearance),
    event_blocks:      bs(COLOURS.event),
    control_blocks:    bs(COLOURS.control),
    detect_blocks:     bs(COLOURS.detect),
    math_blocks:       bs(COLOURS.math),
    variable_blocks:   bs(COLOURS.variable),
    list_blocks:       bs(COLOURS.list),
    text_blocks:       bs(COLOURS.text),
    procedure_blocks:  bs(COLOURS.function),
    audio_blocks:      bs(COLOURS.audio),

    // ── 覆盖标准样式 → 归入对应分类颜色 ──
    logic_blocks:             bs(COLOURS.control),
    loop_blocks:              bs(COLOURS.control),
    variable_dynamic_blocks:  bs(COLOURS.variable),
    colour_blocks:            bs('#a5745b'),
    hat_blocks:               bs('#4a4a5a'),
  },

  // ── 工具箱分类样式（categoryStyles）──
  categoryStyles: {
    engine_category:     { colour: COLOURS.engine },
    camera_category:     { colour: COLOURS.camera },
    appearance_category: { colour: COLOURS.appearance },
    event_category:      { colour: COLOURS.event },
    control_category:    { colour: COLOURS.control },
    detect_category:     { colour: COLOURS.detect },
    math_category:       { colour: COLOURS.math },
    variable_category:   { colour: COLOURS.variable },
    list_category:       { colour: COLOURS.list },
    text_category:       { colour: COLOURS.text },
    function_category:   { colour: COLOURS.function },
    audio_category:      { colour: COLOURS.audio },
  },

  fontStyle: {
    family: '"Microsoft YaHei", sans-serif',
    weight: 'normal',
    size: 12,
  },
});
