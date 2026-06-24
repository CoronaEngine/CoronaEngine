import * as Blockly from 'blockly/core';

// 辅助函数，用于设置通用属性
const setCommonProperties = (block, styleName, tooltip = '') => {
  block.setInputsInline(true);
  block.setPreviousStatement(true, null);
  block.setNextStatement(true, null);
  block.setStyle(styleName);
  block.setTooltip(tooltip);
};

export const defineAppearanceBlocks = () => {
  Blockly.Blocks['appearance_cartoonSet'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('换成')
        .appendField(new Blockly.FieldNumber(0, 0), 'INDEX')
        .appendField('动画');
      setCommonProperties(this, 'appearance_blocks', '输入有效的动画序号（整数且大于等于 0）');
    },
  };

  Blockly.Blocks['appearance_nextCartoon'] = {
    init: function () {
      this.appendDummyInput().appendField('下一个动画');
      setCommonProperties(this, 'appearance_blocks', '切换到下一个动画');
    },
  };

  Blockly.Blocks['appearance_playCartoon'] = {
    init: function () {
      this.appendDummyInput().appendField('播放动画');
      setCommonProperties(this, 'appearance_blocks', '播放当前动画');
    },
  };

  Blockly.Blocks['appearance_stopCartoon'] = {
    init: function () {
      this.appendDummyInput().appendField('停止动画');
      setCommonProperties(this, 'appearance_blocks', '停止当前动画');
    },
  };

  Blockly.Blocks['appearance_resetCartoon'] = {
    init: function () {
      this.appendDummyInput().appendField('重置动画');
      setCommonProperties(this, 'appearance_blocks', '将当前动画重置到第一帧');
    },
  };

  Blockly.Blocks['appearance_sizeAdd'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('大小增加')
        .appendField(new Blockly.FieldNumber(10), 'DS');
      setCommonProperties(this, 'appearance_blocks', '增加或减少角色的大小，正数增加，负数减少');
    },
  };

  Blockly.Blocks['appearance_sizeSet'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('大小设为')
        .appendField(new Blockly.FieldNumber(100), 'SIZE');
      setCommonProperties(this, 'appearance_blocks', '设置角色的大小，输入有效的大小数值');
    },
  };

  Blockly.Blocks['appearance_show'] = {
    init: function () {
      this.appendDummyInput().appendField('显示');
      setCommonProperties(this, 'appearance_blocks', '显示角色');
    },
  };

  Blockly.Blocks['appearance_hide'] = {
    init: function () {
      this.appendDummyInput().appendField('隐藏');
      setCommonProperties(this, 'appearance_blocks', '隐藏角色');
    },
  };

  Blockly.Blocks['appearance_cartoon'] = {
    init: function () {
      this.setStyle('appearance_blocks');
      this.appendDummyInput().appendField('动画');
      this.setOutput(true, 'Number'); // 关键：输出数值类型
      this.setTooltip('该角色的动画序号');
    },
  };

  Blockly.Blocks['appearance_size'] = {
    init: function () {
      this.setStyle('appearance_blocks');
      this.appendDummyInput().appendField('大小');
      this.setOutput(true, 'Number');
      this.setTooltip('该角色的大小');
    },
  };

  // ── 外观扩展：颜色与透明度 ──

  Blockly.Blocks['appearance_set_color'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('设置颜色 R')
        .appendField(new Blockly.FieldNumber(1, 0, 1), 'R')
        .appendField('G')
        .appendField(new Blockly.FieldNumber(0, 0, 1), 'G')
        .appendField('B')
        .appendField(new Blockly.FieldNumber(0, 0, 1), 'B');
      setCommonProperties(this, 'appearance_blocks', '设置物体漫反射颜色（R/G/B 范围 0~1）');
    },
  };

  Blockly.Blocks['appearance_set_alpha'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('透明度设为')
        .appendField(new Blockly.FieldNumber(1, 0, 1), 'ALPHA');
      setCommonProperties(this, 'appearance_blocks', '设置物体透明度（1=不透明，0=全透明）');
    },
  };
};
