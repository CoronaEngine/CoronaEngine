import * as Blockly from 'blockly/core';

const SOUND_OPTIONS = [
  ['射击', 'shoot'],
  ['命中', 'hit'],
  ['命中靶心', 'hit_head'],
  ['换弹', 'reload'],
  ['空膛', 'empty_clip'],
  ['靶子升起', 'target_up'],
  ['靶子落下', 'target_down'],
  ['连击', 'combo'],
  ['倒计时', 'tick'],
  ['游戏开始', 'game_start'],
  ['游戏结束', 'game_end'],
  ['菜单BGM', 'bgm_menu'],
  ['游戏BGM', 'bgm_game'],
];

export const defineAudioBlocks = () => {
  Blockly.Blocks['audio_play'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('播放音效')
        .appendField(new Blockly.FieldDropdown(SOUND_OPTIONS), 'SOUND');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('audio_blocks');
      this.setTooltip('播放一次指定音效');
    },
  };

  Blockly.Blocks['audio_loop'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('循环播放')
        .appendField(new Blockly.FieldDropdown(SOUND_OPTIONS), 'SOUND');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('audio_blocks');
      this.setTooltip('循环播放指定音效（用于背景音乐）');
    },
  };

  Blockly.Blocks['audio_stop'] = {
    init: function () {
      this.appendDummyInput()
        .appendField('停止音效')
        .appendField(new Blockly.FieldDropdown(SOUND_OPTIONS), 'SOUND');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('audio_blocks');
      this.setTooltip('停止播放指定音效');
    },
  };

  Blockly.Blocks['audio_stop_all'] = {
    init: function () {
      this.appendDummyInput().appendField('停止全部音效');
      this.setInputsInline(true);
      this.setPreviousStatement(true, null);
      this.setNextStatement(true, null);
      this.setStyle('audio_blocks');
      this.setTooltip('停止所有正在播放的音效');
    },
  };
};
