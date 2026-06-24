import * as Blockly from 'blockly/core';

export const defineControlBlocks = () => {
  Blockly.Blocks['control_wait'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('等待')
        .appendField(new Blockly.FieldNumber(1, 0), 'SECONDS')
        .appendField('秒');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('暂停执行指定秒数');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_for'] = {
    init: function () {
      this.appendStatementInput('DO').setCheck(null).appendField('重复执行（永久）');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('无限循环执行内部代码块');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_forX'] = {
    init: function () {
      this.appendValueInput('TIMES')
        .setCheck('Number')
        .appendField('重复执行')
        .appendField(new Blockly.FieldNumber(1, 1), 'DEFAULT_TIMES')
        .appendField('次');
      this.appendStatementInput('DO').setCheck(null).appendField('执行');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks'); // 控制类积木常用颜色
      this.setTooltip('重复执行指定次数的代码块');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_if'] = {
    init: function () {
      this.appendValueInput('CONDITION').setCheck('Boolean').appendField('如果');
      this.appendStatementInput('DO').setCheck(null).appendField('那么');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('如果条件满足，则执行对应的代码块');
      this.setHelpUrl('');
    },
  };

  // 定义如果那么否则积木块
  Blockly.Blocks['control_else'] = {
    init: function () {
      this.appendValueInput('CONDITION').setCheck('Boolean').appendField('如果');
      this.appendStatementInput('DO').setCheck(null).appendField('那么');
      this.appendStatementInput('ELSE').setCheck(null).appendField('否则');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks'); // 控制类积木常用颜色
      this.setTooltip('如果条件满足，执行对应的代码块；否则，执行另一个代码块');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_wait2'] = {
    init: function () {
      this.appendValueInput('CONDITION')
        .setCheck('Boolean') // 确保输入为布尔类型
        .appendField('等待直到');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('等待直到条件满足后继续执行');
      this.setHelpUrl('');
    },
  };

  // 定义重复执行直到的积木块
  Blockly.Blocks['control_until'] = {
    init: function () {
      this.appendValueInput('CONDITION')
        .setCheck('Boolean') // 确保输入为布尔类型
        .appendField('重复执行直到');
      this.appendStatementInput('DO').setCheck(null).appendField('执行');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('重复执行代码块，直到条件满足');
      this.setHelpUrl('');
    },
  };

  const stopOptions = [
    ['当前脚本', 'CURRENT_SCRIPT'],
    ['全部脚本', 'ALL_SCRIPTS'],
    ['该角色的其他脚本', 'OTHER_SCRIPTS_OF_ACTOR'],
  ];
  Blockly.Blocks['control_stop'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('停止')
        .appendField(new Blockly.FieldDropdown(stopOptions), 'STOP_OPTION');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('停止指定的脚本执行');
    },
  };

  Blockly.Blocks['control_cloneStart'] = {
    init: function () {
      this.appendDummyInput().appendField('当作为克隆体启动时');
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_clone'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('克隆')
        .appendField(new Blockly.FieldTextInput(''), 'x');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('克隆指定角色');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_cloneDEL'] = {
    init: function () {
      this.appendDummyInput().appendField('删除此克隆体');
      this.setInputsInline(true);
      // 终止块：有上接点，无下接点
      this.setPreviousStatement(true, null);
      this.setNextStatement(false, null);
      this.setStyle('control_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_senceSet'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('换成')
        .appendField(new Blockly.FieldTextInput(''), 'x')
        .appendField('场景');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setTooltip('切换到指定场景');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['control_nextSence'] = {
    init: function () {
      this.appendDummyInput().appendField('下一个场景');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('control_blocks');
      this.setHelpUrl('');
    },
  };
};
