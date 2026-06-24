// JSON 格式工具箱配置 —— 自定义 CoronaEngine 积木 + 标准 Blockly 积木
export const TOOLBOX_CONFIG = {
  kind: 'categoryToolbox',
  scrollbars: true,
  contents: [
    // ===================================================================
    // 1. 引擎 —— CoronaEngine 自定义运动/旋转/坐标积木
    // ===================================================================
    {
      kind: 'category',
      name: '引擎',
      categorystyle: 'engine_category',
      contents: [
        { kind: 'block', type: 'engine_move' },
        { kind: 'block', type: 'engine_rotateX' },
        { kind: 'block', type: 'engine_rotateY' },
        { kind: 'block', type: 'engine_rotateZ' },
        { kind: 'block', type: 'engine_face' },
        { kind: 'block', type: 'engine_moveto' },
        { kind: 'block', type: 'engine_movetoXYZ' },
        { kind: 'block', type: 'engine_movetoXYZtime' },
        { kind: 'block', type: 'engine_Xset' },
        { kind: 'block', type: 'engine_Yset' },
        { kind: 'block', type: 'engine_Zset' },
        { kind: 'block', type: 'engine_Xadd' },
        { kind: 'block', type: 'engine_Yadd' },
        { kind: 'block', type: 'engine_Zadd' },
        { kind: 'block', type: 'engine_X' },
        { kind: 'block', type: 'engine_Y' },
        { kind: 'block', type: 'engine_Z' },
        { kind: 'block', type: 'engine_set_velocity' },
        { kind: 'block', type: 'engine_apply_impulse' },
        { kind: 'block', type: 'engine_get_velocity' },
      ],
    },

    // ===================================================================
    // 2. 摄像机 —— FPS 视角控制积木（新增）
    // ===================================================================
    {
      kind: 'category',
      name: '摄像机',
      categorystyle: 'camera_category',
      contents: [
        { kind: 'block', type: 'camera_lock_mouse' },
        { kind: 'block', type: 'camera_unlock_mouse' },
        { kind: 'block', type: 'camera_mouse_dx' },
        { kind: 'block', type: 'camera_mouse_dy' },
        { kind: 'block', type: 'camera_set_fov' },
      ],
    },

    // ===================================================================
    // 3. 外观 —— CoronaEngine 自定义动画/尺寸/显隐积木
    // ===================================================================
    {
      kind: 'category',
      name: '外观',
      categorystyle: 'appearance_category',
      contents: [
        { kind: 'block', type: 'appearance_cartoonSet' },
        { kind: 'block', type: 'appearance_nextCartoon' },
        { kind: 'block', type: 'appearance_playCartoon' },
        { kind: 'block', type: 'appearance_stopCartoon' },
        { kind: 'block', type: 'appearance_resetCartoon' },
        { kind: 'block', type: 'appearance_sizeAdd' },
        { kind: 'block', type: 'appearance_sizeSet' },
        { kind: 'block', type: 'appearance_show' },
        { kind: 'block', type: 'appearance_hide' },
        { kind: 'block', type: 'appearance_cartoon' },
        { kind: 'block', type: 'appearance_size' },
        { kind: 'block', type: 'appearance_set_color' },
        { kind: 'block', type: 'appearance_set_alpha' },
      ],
    },

    // ===================================================================
    // 3. 事件 —— CoronaEngine 自定义事件积木
    // ===================================================================
    {
      kind: 'category',
      name: '事件',
      categorystyle: 'event_category',
      contents: [
        { kind: 'block', type: 'event_gameStart' },
        { kind: 'block', type: 'event_keyboard' },
        { kind: 'block', type: 'event_RB' },
        { kind: 'block', type: 'event_broadcast' },
        { kind: 'block', type: 'event_broadcastWait' },
        { kind: 'block', type: 'event_keyboard_combo' },
        { kind: 'block', type: 'event_mouse_click' },
        { kind: 'block', type: 'event_mouse_move' },
        { kind: 'block', type: 'event_mouse_wheel' },
        { kind: 'block', type: 'event_mouse_contextmenu' },
      ],
    },

    // ===================================================================
    // 4. 控制 —— 自定义控制 + 标准逻辑 + 标准循环
    // ===================================================================
    {
      kind: 'category',
      name: '控制',
      categorystyle: 'control_category',
      contents: [
        // ── 自定义控制积木 ──
        { kind: 'block', type: 'control_wait' },
        { kind: 'block', type: 'control_for' },
        { kind: 'block', type: 'control_forX' },
        { kind: 'block', type: 'control_if' },
        { kind: 'block', type: 'control_else' },
        { kind: 'block', type: 'control_wait2' },
        { kind: 'block', type: 'control_until' },
        { kind: 'block', type: 'control_stop' },
        { kind: 'block', type: 'control_cloneStart' },
        { kind: 'block', type: 'control_clone' },
        { kind: 'block', type: 'control_cloneDEL' },
        { kind: 'block', type: 'control_senceSet' },
        { kind: 'block', type: 'control_nextSence' },
        // ── 标准逻辑积木 ──
        { kind: 'block', type: 'logic_boolean' },
        { kind: 'block', type: 'logic_compare' },
        { kind: 'block', type: 'logic_operation' },
        { kind: 'block', type: 'logic_negate' },
        { kind: 'block', type: 'logic_null' },
        { kind: 'block', type: 'logic_ternary' },
        { kind: 'block', type: 'controls_if' },
        { kind: 'block', type: 'controls_ifelse' },
        // ── 标准循环积木 ──
        { kind: 'block', type: 'controls_repeat_ext' },
        { kind: 'block', type: 'controls_repeat' },
        { kind: 'block', type: 'controls_whileUntil' },
        { kind: 'block', type: 'controls_for' },
        { kind: 'block', type: 'controls_forEach' },
        { kind: 'block', type: 'controls_flow_statements' },
      ],
    },

    // ===================================================================
    // 5. 侦测 —— CoronaEngine 自定义感知积木
    // ===================================================================
    {
      kind: 'category',
      name: '侦测',
      categorystyle: 'detect_category',
      contents: [
        { kind: 'block', type: 'detect_touch' },
        { kind: 'block', type: 'detect_distance' },
        { kind: 'block', type: 'detect_ask' },
        { kind: 'block', type: 'detect_keyboard1' },
        { kind: 'block', type: 'detect_keyboard0' },
        { kind: 'block', type: 'detect_mouse1' },
        { kind: 'block', type: 'detect_mouse0' },
        { kind: 'block', type: 'detect_attribute' },
        { kind: 'block', type: 'detect_raycast' },
        { kind: 'block', type: 'detect_raycast_distance' },
        { kind: 'block', type: 'detect_raycast_object' },
        { kind: 'block', type: 'detect_raycast_point' },
      ],
    },

    // ===================================================================
    // 7. 运算 —— 自定义运算 + 标准数学
    // ===================================================================
    {
      kind: 'category',
      name: '运算',
      categorystyle: 'math_category',
      contents: [
        // ── 自定义运算积木 ──
        { kind: 'block', type: 'math_add' },
        { kind: 'block', type: 'math_mul' },
        { kind: 'block', type: 'math_div' },
        { kind: 'block', type: 'math_sub' },
        { kind: 'block', type: 'math_random' },
        { kind: 'block', type: 'math_G' },
        { kind: 'block', type: 'math_L' },
        { kind: 'block', type: 'math_E' },
        { kind: 'block', type: 'math_AND' },
        { kind: 'block', type: 'math_OR' },
        { kind: 'block', type: 'math_NOT' },
        { kind: 'block', type: 'math_connect' },
        // ── 标准数学积木 ──
        { kind: 'block', type: 'math_number' },
        { kind: 'block', type: 'math_arithmetic' },
        { kind: 'block', type: 'math_single' },
        { kind: 'block', type: 'math_trig' },
        { kind: 'block', type: 'math_constant' },
        { kind: 'block', type: 'math_number_property' },
        { kind: 'block', type: 'math_change' },
        { kind: 'block', type: 'math_round' },
        { kind: 'block', type: 'math_on_list' },
        { kind: 'block', type: 'math_modulo' },
        { kind: 'block', type: 'math_constrain' },
        { kind: 'block', type: 'math_random_int' },
        { kind: 'block', type: 'math_random_float' },
        { kind: 'block', type: 'math_atan2' },
      ],
    },

    // ===================================================================
    // 7. 变量 —— 自定义变量 + 标准变量
    // ===================================================================
    {
      kind: 'category',
      name: '变量',
      categorystyle: 'variable_category',
      contents: [
        // ── 自定义变量积木 ──
        { kind: 'block', type: 'variable_add' },
        { kind: 'block', type: 'variable_set' },
        { kind: 'block', type: 'variable_show' },
        { kind: 'block', type: 'variable_hide' },
        // ── 标准变量积木 ──
        { kind: 'block', type: 'variables_get' },
        { kind: 'block', type: 'variables_set' },
        { kind: 'block', type: 'variables_get_dynamic' },
        { kind: 'block', type: 'variables_set_dynamic' },
      ],
    },

    // ===================================================================
    // 8. 列表 —— 自定义列表 + 标准列表
    // ===================================================================
    {
      kind: 'category',
      name: '列表',
      categorystyle: 'list_category',
      contents: [
        // ── 自定义列表积木 ──
        { kind: 'block', type: 'list_show' },
        { kind: 'block', type: 'list_hide' },
        // ── 标准列表积木 ──
        { kind: 'block', type: 'lists_create_empty' },
        { kind: 'block', type: 'lists_create_with' },
        { kind: 'block', type: 'lists_repeat' },
        { kind: 'block', type: 'lists_length' },
        { kind: 'block', type: 'lists_isEmpty' },
        { kind: 'block', type: 'lists_indexOf' },
        { kind: 'block', type: 'lists_getIndex' },
        { kind: 'block', type: 'lists_setIndex' },
        { kind: 'block', type: 'lists_getSublist' },
        { kind: 'block', type: 'lists_reverse' },
        { kind: 'block', type: 'lists_sort' },
        { kind: 'block', type: 'lists_split' },
      ],
    },

    // ===================================================================
    // 9. 文本 —— 标准 Blockly 文本处理积木（新增分类）
    // ===================================================================
    {
      kind: 'category',
      name: '文本',
      categorystyle: 'text_category',
      contents: [
        { kind: 'block', type: 'text' },
        { kind: 'block', type: 'text_join' },
        { kind: 'block', type: 'text_append' },
        { kind: 'block', type: 'text_length' },
        { kind: 'block', type: 'text_isEmpty' },
        { kind: 'block', type: 'text_indexOf' },
        { kind: 'block', type: 'text_charAt' },
        { kind: 'block', type: 'text_getSubstring' },
        { kind: 'block', type: 'text_changeCase' },
        { kind: 'block', type: 'text_trim' },
        { kind: 'block', type: 'text_count' },
        { kind: 'block', type: 'text_replace' },
        { kind: 'block', type: 'text_reverse' },
        { kind: 'block', type: 'text_print' },
        { kind: 'block', type: 'text_prompt_ext' },
      ],
    },

    // ===================================================================
    // 11. 音效 —— CoronaEngine 音效控制积木（新增分类）
    // ===================================================================
    {
      kind: 'category',
      name: '音效',
      categorystyle: 'audio_category',
      contents: [
        { kind: 'block', type: 'audio_play' },
        { kind: 'block', type: 'audio_loop' },
        { kind: 'block', type: 'audio_stop' },
        { kind: 'block', type: 'audio_stop_all' },
      ],
    },

    // ===================================================================
    // 12. 函数 —— 标准 Blockly 函数/过程积木
    // ===================================================================
    {
      kind: 'category',
      name: '函数',
      categorystyle: 'function_category',
      contents: [
        { kind: 'block', type: 'procedures_defnoreturn' },
        { kind: 'block', type: 'procedures_defreturn' },
        { kind: 'block', type: 'procedures_callnoreturn' },
        { kind: 'block', type: 'procedures_callreturn' },
        { kind: 'block', type: 'procedures_ifreturn' },
      ],
    },
  ],
};

const CATEGORY_KEYS = {
  引擎: 'blocklyToolbox.engine',
  摄像机: 'blocklyToolbox.camera',
  外观: 'blocklyToolbox.appearance',
  事件: 'blocklyToolbox.event',
  控制: 'blocklyToolbox.control',
  侦测: 'blocklyToolbox.detect',
  运算: 'blocklyToolbox.math',
  变量: 'blocklyToolbox.variable',
  列表: 'blocklyToolbox.list',
  文本: 'blocklyToolbox.text',
  音效: 'blocklyToolbox.audio',
  函数: 'blocklyToolbox.functions',
};

export function createToolboxConfig(t = (key) => key) {
  const clone = structuredClone(TOOLBOX_CONFIG);
  for (const category of clone.contents || []) {
    if (category.kind === 'category' && CATEGORY_KEYS[category.name]) {
      category.name = t(CATEGORY_KEYS[category.name]);
    }
  }
  return clone;
}
