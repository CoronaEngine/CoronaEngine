import * as Blockly from 'blockly/core';

export const defineMathBlocks = () => {
  // ── 修复 math_change（"给…加…"）积木样式 ──
  // 标准 Blockly 中 math_change 使用 variable_blocks 样式（橙色），
  // 但它在运算分类中，应与其他数学积木一致使用 math_blocks 样式（绿色）。
  if (Blockly.Blocks['math_change']) {
    const _init = Blockly.Blocks['math_change'].init;
    Blockly.Blocks['math_change'].init = function () {
      _init.call(this);
      this.setStyle('math_blocks');
    };
  }

  Blockly.Blocks['math_add'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('+')
        .appendField(new Blockly.FieldTextInput(0), 'x2');
      this.setOutput(true, 'Number');
    },
  };

  Blockly.Blocks['math_mul'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('\u00D7')
        .appendField(new Blockly.FieldTextInput(0), 'x2');
      this.setOutput(true, 'Number');
    },
  };

  Blockly.Blocks['math_sub'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('-')
        .appendField(new Blockly.FieldTextInput(0), 'x2');
      this.setOutput(true, 'Number');
    },
  };

  Blockly.Blocks['math_div'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('\u00F7')
        .appendField(new Blockly.FieldTextInput(0), 'x2');
      this.setOutput(true, 'Number');
    },
  };

  Blockly.Blocks['math_random'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField('在')
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('到')
        .appendField(new Blockly.FieldTextInput(0), 'x2')
        .appendField('之间的一个随机数');
      this.setInputsInline(true);
      this.setOutput(true, 'Number');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['math_G'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('>')
        .appendField(new Blockly.FieldTextInput(0), 'x2');
      this.setOutput(true, 'Boolean');
    },
  };

  Blockly.Blocks['math_L'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('<')
        .appendField(new Blockly.FieldTextInput(0), 'x2');
      this.setOutput(true, 'Boolean');
    },
  };

  Blockly.Blocks['math_E'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendDummyInput()
        .appendField(new Blockly.FieldTextInput(0), 'x1')
        .appendField('=')
        .appendField(new Blockly.FieldTextInput(0), 'x2');
      this.setOutput(true, 'Boolean');
    },
  };

  Blockly.Blocks['math_AND'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendValueInput('A').setCheck('Boolean').appendField('');
      this.appendValueInput('B').setCheck('Boolean').appendField('与');
      this.setInputsInline(true);
      this.setOutput(true, 'Boolean');
      this.setTooltip('逻辑与运算，两个条件都满足时返回 true，否则返回 false');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['math_OR'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendValueInput('A').setCheck('Boolean').appendField('');
      this.appendValueInput('B').setCheck('Boolean').appendField('或');
      this.setInputsInline(true);
      this.setOutput(true, 'Boolean');
      this.setTooltip('逻辑或运算，两个条件中至少一个满足时返回 true，否则返回 false');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['math_NOT'] = {
    init: function () {
      this.setStyle('math_blocks');
      this.appendValueInput('A').setCheck('Boolean').appendField('非');
      this.setInputsInline(true);
      this.setOutput(true, 'Boolean');
      this.setTooltip('逻辑非运算，条件不满足时返回 true，满足时返回 false');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['math_connect'] = {
    init: function () {
      // 使用数学类样式，通常为圆形
      this.setStyle('math_blocks');
      this.appendValueInput('LEFT').appendField('连接');
      this.appendValueInput('RIGHT').appendField('和');
      this.setInputsInline(true);
      this.setOutput(true, 'String');
      this.setTooltip('将左右两边的内容连接成一个字符串');
      this.setHelpUrl('');
    },
  };
};
