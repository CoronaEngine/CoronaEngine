import { pythonGenerator } from 'blockly/python';

export const defineAudioGenerators = () => {
  pythonGenerator.forBlock['audio_play'] = function (block) {
    const sound = block.getFieldValue('SOUND');
    return `CoronaEngine.play_sound("${sound}")\n`;
  };

  pythonGenerator.forBlock['audio_loop'] = function (block) {
    const sound = block.getFieldValue('SOUND');
    return `CoronaEngine.loop_sound("${sound}")\n`;
  };

  pythonGenerator.forBlock['audio_stop'] = function (block) {
    const sound = block.getFieldValue('SOUND');
    return `CoronaEngine.stop_sound("${sound}")\n`;
  };

  pythonGenerator.forBlock['audio_stop_all'] = function () {
    return 'CoronaEngine.stop_all_sounds()\n';
  };
};
