import { pythonGenerator } from 'blockly/python';

export const defineDetectGenerators = () => {
  pythonGenerator.forBlock['detect_touch'] = function (block) {
    const x = block.getFieldValue('x');
    return [`CoronaEngine.touch("${x}")`, pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_distance'] = function (block) {
    const x = block.getFieldValue('x');
    return [`CoronaEngine.distance("${x}")`, pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_ask'] = function (block) {
    const x = block.getFieldValue('x');
    return `CoronaEngine.ask("${x}")\n`;
  };

  pythonGenerator.forBlock['detect_keyboard1'] = function (block) {
    const x = block.getFieldValue('x');
    return [`CoronaEngine.keyboard("${x}")`, pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_keyboard0'] = function (block) {
    const x = block.getFieldValue('x');
    return [`CoronaEngine.keyboard0("${x}")`, pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_mouse1'] = function (block) {
    return [`CoronaEngine.mouse1()`, pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_mouse0'] = function (block) {
    return [`CoronaEngine.mouse0()`, pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_attribute'] = function (block) {
    const x = block.getFieldValue('x');
    return [`CoronaEngine.attribute('${x}')`, pythonGenerator.ORDER_ATOMIC];
  };

  // ── 射线检测生成器 ──

  pythonGenerator.forBlock['detect_raycast'] = function (block) {
    const ox = block.getFieldValue('OX');
    const oy = block.getFieldValue('OY');
    const oz = block.getFieldValue('OZ');
    const dx = block.getFieldValue('DX');
    const dy = block.getFieldValue('DY');
    const dz = block.getFieldValue('DZ');
    const maxDist = block.getFieldValue('MAX_DIST');
    const origin = `[${ox}, ${oy}, ${oz}]`;
    const direction = `[${dx}, ${dy}, ${dz}]`;
    return [`CoronaEngine.raycast_hit(${origin}, ${direction}, ${maxDist})`, pythonGenerator.ORDER_FUNCTION_CALL];
  };

  pythonGenerator.forBlock['detect_raycast_distance'] = function () {
    return ['CoronaEngine.raycast_distance()', pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_raycast_object'] = function () {
    return ['CoronaEngine.raycast_hit_object()', pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['detect_raycast_point'] = function (block) {
    const axis = block.getFieldValue('AXIS');
    const fn = axis === 'X' ? 'raycast_hit_point_x' : axis === 'Y' ? 'raycast_hit_point_y' : 'raycast_hit_point_z';
    return [`CoronaEngine.${fn}()`, pythonGenerator.ORDER_ATOMIC];
  };
};
