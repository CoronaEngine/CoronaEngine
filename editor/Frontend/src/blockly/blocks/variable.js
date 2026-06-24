import * as Blockly from 'blockly/core';

export const defineVariableBlocks = () => {
  Blockly.Blocks['variable_add'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将')
        .appendField(new Blockly.FieldTextInput(), 'v')
        .appendField('增加')
        .appendField(new Blockly.FieldTextInput(), 'x');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('variable_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['variable_set'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将')
        .appendField(new Blockly.FieldTextInput(), 'v')
        .appendField('设为')
        .appendField(new Blockly.FieldTextInput(), 'x');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('variable_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['variable_show'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('显示变量')
        .appendField(new Blockly.FieldTextInput(), 'v');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('variable_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['variable_hide'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('隐藏变量')
        .appendField(new Blockly.FieldTextInput(), 'v');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('variable_blocks');
      this.setHelpUrl('');
    },
  };
};
