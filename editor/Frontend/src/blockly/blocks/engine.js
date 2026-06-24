import * as Blockly from 'blockly/core';

export const defineEngineBlocks = () => {
  Blockly.Blocks['engine_move'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('移动')
        .appendField(new Blockly.FieldNumber(10, 0), 'STEPS')
        .appendField('步');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('让角色向前移动指定的步数');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_rotateX'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('水平旋转')
        .appendField(new Blockly.FieldNumber(15, -Infinity), 'ANGLE')
        .appendField('度');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('让角色绕 X 轴旋转指定角度');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_rotateY'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('竖直旋转')
        .appendField(new Blockly.FieldNumber(15, -Infinity), 'ANGLE')
        .appendField('度');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('让角色绕 Y 轴旋转指定角度');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_rotateZ'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('旋转')
        .appendField(new Blockly.FieldNumber(15, -Infinity), 'ANGLE')
        .appendField('度');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('让角色绕 Z 轴旋转指定角度（2D平面旋转）');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_face'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('面向')
        .appendField(new Blockly.FieldNumber(0, 0, 360), 'DIRECTION')
        .appendField('度方向');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('让角色面向指定角度（0=右, 90=上, 180=左, 270=下）');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_moveto'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('移动到')
        .appendField(
          new Blockly.FieldDropdown([
            ['随机位置', 'random_position'],
            ['准星位置', 'sight_position'],
          ]),
          'POSITION'
        );
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('将角色移动到预设位置');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_movetoXYZ'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('移到 X:')
        .appendField(new Blockly.FieldNumber(0), 'X')
        .appendField('Y:')
        .appendField(new Blockly.FieldNumber(0), 'Y')
        .appendField('Z:')
        .appendField(new Blockly.FieldNumber(0), 'Z');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('将角色移动到指定的 XYZ 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_movetoXYZtime'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('在')
        .appendField(new Blockly.FieldNumber(1, 0), 'TIME')
        .appendField('秒内移到')
        .appendField(new Blockly.FieldNumber(0), 'X')
        .appendField(new Blockly.FieldNumber(0), 'Y')
        .appendField(new Blockly.FieldNumber(0), 'Z');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('在指定时间内平滑移动到目标 XYZ 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_Xset'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将 X 坐标设为')
        .appendField(new Blockly.FieldNumber(0), 'X');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('直接设置角色的 X 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_Yset'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将 Y 坐标设为')
        .appendField(new Blockly.FieldNumber(0), 'Y');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('直接设置角色的 Y 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_Zset'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将 Z 坐标设为')
        .appendField(new Blockly.FieldNumber(0), 'Z');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('直接设置角色的 Z 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_Xadd'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将 X 坐标增加')
        .appendField(new Blockly.FieldNumber(10), 'DX');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('在当前位置基础上增加 X 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_Yadd'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将 Y 坐标增加')
        .appendField(new Blockly.FieldNumber(10), 'DY');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('在当前位置基础上增加 Y 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_Zadd'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('将 Z 坐标增加')
        .appendField(new Blockly.FieldNumber(10), 'DZ');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('在当前位置基础上增加 Z 坐标');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['engine_X'] = {
    init: function () {
      this.appendDummyInput().appendField('X');
      this.setOutput(true, 'Number');
      this.setStyle('engine_blocks');
      this.setTooltip('该角色的 X 坐标');
    },
  };

  Blockly.Blocks['engine_Y'] = {
    init: function () {
      this.appendDummyInput().appendField('Y');
      this.setOutput(true, 'Number');
      this.setStyle('engine_blocks');
      this.setTooltip('该角色的 Y 坐标');
    },
  };

  Blockly.Blocks['engine_Z'] = {
    init: function () {
      this.appendDummyInput().appendField('Z');
      this.setOutput(true, 'Number');
      this.setStyle('engine_blocks');
      this.setTooltip('该角色的 Z 坐标');
    },
  };

  // ── 物理扩展：速度与冲量 ──

  Blockly.Blocks['engine_set_velocity'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('设置速度 X')
        .appendField(new Blockly.FieldNumber(0), 'VX')
        .appendField('Y')
        .appendField(new Blockly.FieldNumber(0), 'VY')
        .appendField('Z')
        .appendField(new Blockly.FieldNumber(0), 'VZ');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('设置物体当前线速度（单位/秒），用于移动靶运动控制');
    },
  };

  Blockly.Blocks['engine_apply_impulse'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('施加冲量 X')
        .appendField(new Blockly.FieldNumber(0), 'IX')
        .appendField('Y')
        .appendField(new Blockly.FieldNumber(0), 'IY')
        .appendField('Z')
        .appendField(new Blockly.FieldNumber(0), 'IZ');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('engine_blocks');
      this.setTooltip('施加瞬时冲量（N·s），用于子弹命中反馈/爆炸推开效果');
    },
  };

  Blockly.Blocks['engine_get_velocity'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('当前速度')
        .appendField(
          new Blockly.FieldDropdown([['X', 'X'], ['Y', 'Y'], ['Z', 'Z']]),
          'AXIS'
        );
      this.setOutput(true, 'Number');
      this.setStyle('engine_blocks');
      this.setTooltip('获取物体当前速度分量');
    },
  };
};