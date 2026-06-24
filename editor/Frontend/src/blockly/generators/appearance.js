import { pythonGenerator } from 'blockly/python';

export const defineAppearanceGenerators = () => {
  pythonGenerator.forBlock['appearance_cartoonSet'] = function (block) {
    const index = block.getFieldValue('INDEX');
    return `CoronaEngine.cartoonSet(${index})\n`;
  };

  pythonGenerator.forBlock['appearance_nextCartoon'] = function (block) {
    return `CoronaEngine.nextCartoon()\n`;
  };

  pythonGenerator.forBlock['appearance_playCartoon'] = function (block) {
    return `CoronaEngine.playCartoon()\n`;
  };

  pythonGenerator.forBlock['appearance_stopCartoon'] = function (block) {
    return `CoronaEngine.stopCartoon()\n`;
  };

  pythonGenerator.forBlock['appearance_resetCartoon'] = function (block) {
    return `CoronaEngine.resetCartoon()\n`;
  };

  pythonGenerator.forBlock['appearance_sizeAdd'] = function (block) {
    const ds = block.getFieldValue('DS');
    return `CoronaEngine.sizeAdd(${ds})\n`;
  };

  pythonGenerator.forBlock['appearance_sizeSet'] = function (block) {
    const size = block.getFieldValue('SIZE');
    return `CoronaEngine.sizeSet(${size})\n`;
  };

  pythonGenerator.forBlock['appearance_show'] = function (block) {
    return `CoronaEngine.show()\n`;
  };

  pythonGenerator.forBlock['appearance_hide'] = function (block) {
    return `CoronaEngine.hide()\n`;
  };

  pythonGenerator.forBlock['appearance_cartoon'] = function () {
    return ['CoronaEngine.cartoon()', pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['appearance_size'] = function () {
    return ['CoronaEngine.size()', pythonGenerator.ORDER_ATOMIC];
  };

  // ── 外观扩展生成器 ──

  pythonGenerator.forBlock['appearance_set_color'] = function (block) {
    const r = block.getFieldValue('R');
    const g = block.getFieldValue('G');
    const b = block.getFieldValue('B');
    return `CoronaEngine.set_color(${r}, ${g}, ${b})\n`;
  };

  pythonGenerator.forBlock['appearance_set_alpha'] = function (block) {
    const alpha = block.getFieldValue('ALPHA');
    return `CoronaEngine.set_alpha(${alpha})\n`;
  };
};
