import * as Blockly from 'blockly/core';

// 定义事件相关的积木块，适配 Vue 鼠标键盘事件
export const defineEventBlocks = (broadcastList, createNewBroadcast) => {
  // 中文注释：安全获取广播下拉选项，避免 broadcastList 为 null/undefined 时出错
  const getBroadcastOptions = () => {
    const list = Array.isArray(broadcastList?.value) ? broadcastList.value : [];
    const options = list.length ? list.map((item) => [item, item]) : [['<无广播>', 'NO_BROADCAST']];
    options.push(['新建广播...', 'CREATE_NEW']);
    return options;
  };

  Blockly.Blocks['event_gameStart'] = {
    init: function () {
      this.appendDummyInput().appendField('当游戏开始时');
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  // 常用键选项（字母、数字、方向键与若干特殊键）
  const KEY_OPTIONS = (function () {
    const letters = Array.from({ length: 26 }, (v, i) => String.fromCharCode(65 + i));
    const nums = Array.from({ length: 10 }, (v, i) => String(i));
    const keys = [];
    letters.forEach((k) => keys.push([k, `Key${k}`]));
    nums.forEach((n) => keys.push([n, `Digit${n}`]));
    // 方向键与特殊键
    const special = [
      ['ArrowUp', 'ArrowUp'],
      ['ArrowDown', 'ArrowDown'],
      ['ArrowLeft', 'ArrowLeft'],
      ['ArrowRight', 'ArrowRight'],
      ['Space', 'Space'],
      ['Enter', 'Enter'],
      ['Escape', 'Escape'],
      ['Tab', 'Tab'],
      ['Backspace', 'Backspace'],
    ];
    special.forEach((s) => keys.push(s));
    return keys;
  })();

  Blockly.Blocks['event_keyboard'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('当按下')
        .appendField(new Blockly.FieldDropdown(KEY_OPTIONS), 'x')
        .appendField('时');
      this.appendStatementInput('DO') // 新增：允许后续语句块串联
        .setCheck(null);
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['event_RB'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('当接收到广播')
        .appendField(
          new Blockly.FieldDropdown(
            // 生成菜单
            () => getBroadcastOptions(),
            // 校验器：处理“新建广播...”的特殊选项
            function (value) {
              if (value === 'CREATE_NEW') {
                try {
                  createNewBroadcast && createNewBroadcast();
                } catch (e) {}
                // 返回上一次有效值，阻止将字段设置为 CREATE_NEW
                return this.getValue();
              }
              if (value === 'NO_BROADCAST') {
                // 占位不可选，恢复原值
                return this.getValue();
              }
              return value;
            }
          ),
          'x'
        );
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['event_broadcast'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('发送广播')
        .appendField(
          new Blockly.FieldDropdown(
            () => getBroadcastOptions(),
            function (value) {
              if (value === 'CREATE_NEW') {
                try {
                  createNewBroadcast && createNewBroadcast();
                } catch (e) {}
                return this.getValue();
              }
              if (value === 'NO_BROADCAST') {
                return this.getValue();
              }
              return value;
            }
          ),
          'x'
        );
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  Blockly.Blocks['event_broadcastWait'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('发送广播')
        .appendField(
          new Blockly.FieldDropdown(
            () => getBroadcastOptions(),
            function (value) {
              if (value === 'CREATE_NEW') {
                try {
                  createNewBroadcast && createNewBroadcast();
                } catch (e) {}
                return this.getValue();
              }
              if (value === 'NO_BROADCAST') {
                return this.getValue();
              }
              return value;
            }
          ),
          'x'
        )
        .appendField('并等待');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  // 键盘组合键事件积木块
  Blockly.Blocks['event_keyboard_combo'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('当按下组合键')
        .appendField(new Blockly.FieldTextInput('Ctrl+Alt+K'), 'combo')
        .appendField('时');
      this.appendStatementInput('DO').setCheck(null);
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  // 鼠标点击事件积木块
  Blockly.Blocks['event_mouse_click'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('当鼠标点击')
        .appendField(
          new Blockly.FieldDropdown([
            ['左键', 'left'],
            ['右键', 'right'],
            ['中键', 'middle'],
          ]),
          'button'
        )
        .appendField('时');
      this.appendStatementInput('DO').setCheck(null);
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  // 鼠标移动事件积木块
  Blockly.Blocks['event_mouse_move'] = {
    init: function () {
      this.appendDummyInput().appendField('当鼠标移动时');
      this.appendStatementInput('DO').setCheck(null);
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  // 鼠标滚轮事件积木块
  Blockly.Blocks['event_mouse_wheel'] = {
    init: function () {
      this.appendDummyInput().appendField('当鼠标滚轮滚动时');
      this.appendStatementInput('DO').setCheck(null);
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };

  // 鼠标右键菜单事件积木块
  Blockly.Blocks['event_mouse_contextmenu'] = {
    init: function () {
      this.appendDummyInput().appendField('当鼠标右键菜单时');
      this.appendStatementInput('DO').setCheck(null);
      this.setInputsInline(true);
      this.setPreviousStatement(false, null);
      this.setNextStatement(true, null);
      this.setStyle('event_blocks');
      this.setHelpUrl('');
    },
  };
};
