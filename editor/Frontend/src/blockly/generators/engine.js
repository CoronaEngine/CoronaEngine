import { pythonGenerator } from 'blockly/python';

export const defineEngineGenerators = () => {
  pythonGenerator.forBlock['engine_move'] = function (block) {
    const steps = block.getFieldValue('STEPS');
    return `CoronaEngine.move(${steps})\n`;
  };

  pythonGenerator.forBlock['engine_rotateX'] = function (block) {
    const angle = block.getFieldValue('ANGLE');
    return `CoronaEngine.rotateX(${angle})\n`;
  };

  pythonGenerator.forBlock['engine_rotateY'] = function (block) {
    const angle = block.getFieldValue('ANGLE');
    return `CoronaEngine.rotateY(${angle})\n`;
  };

  pythonGenerator.forBlock['engine_rotateZ'] = function (block) {
    const angle = block.getFieldValue('ANGLE');
    return `CoronaEngine.rotateZ(${angle})\n`;
  };

  pythonGenerator.forBlock['engine_face'] = function (block) {
    const direction = block.getFieldValue('DIRECTION');
    return `CoronaEngine.face(${direction})\n`;
  };

  pythonGenerator.forBlock['engine_moveto'] = function (block) {
    const position = block.getFieldValue('POSITION');
    return `CoronaEngine.moveto("${position}")\n`;
  };

  pythonGenerator.forBlock['engine_movetoXYZ'] = function (block) {
    const x = block.getFieldValue('X');
    const y = block.getFieldValue('Y');
    const z = block.getFieldValue('Z');
    return `CoronaEngine.movetoXYZtime(0, ${x}, ${y}, ${z})\n`;
  };

  pythonGenerator.forBlock['engine_movetoXYZtime'] = function (block) {
    const t = block.getFieldValue('TIME');
    const x = block.getFieldValue('X');
    const y = block.getFieldValue('Y');
    const z = block.getFieldValue('Z');
    return `CoronaEngine.movetoXYZtime(${t}, ${x}, ${y}, ${z})\n`;
  };

  pythonGenerator.forBlock['engine_Xset'] = function (block) {
    const x = block.getFieldValue('X');
    return `CoronaEngine.Xset(${x})\n`;
  };

  pythonGenerator.forBlock['engine_Yset'] = function (block) {
    const y = block.getFieldValue('Y');
    return `CoronaEngine.Yset(${y})\n`;
  };

  pythonGenerator.forBlock['engine_Zset'] = function (block) {
    const z = block.getFieldValue('Z');
    return `CoronaEngine.Zset(${z})\n`;
  };

  pythonGenerator.forBlock['engine_Xadd'] = function (block) {
    const dx = block.getFieldValue('DX');
    return `CoronaEngine.Xadd(${dx})\n`;
  };

  pythonGenerator.forBlock['engine_Yadd'] = function (block) {
    const dy = block.getFieldValue('DY');
    return `CoronaEngine.Yadd(${dy})\n`;
  };

  pythonGenerator.forBlock['engine_Zadd'] = function (block) {
    const dz = block.getFieldValue('DZ');
    return `CoronaEngine.Zadd(${dz})\n`;
  };

  pythonGenerator.forBlock['engine_X'] = function () {
    return ['CoronaEngine.X()', pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['engine_Y'] = function () {
    return ['CoronaEngine.Y()', pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['engine_Z'] = function () {
    return ['CoronaEngine.Z()', pythonGenerator.ORDER_ATOMIC];
  };

  // ── 物理扩展生成器 ──

  pythonGenerator.forBlock['engine_set_velocity'] = function (block) {
    const vx = block.getFieldValue('VX');
    const vy = block.getFieldValue('VY');
    const vz = block.getFieldValue('VZ');
    return `CoronaEngine.set_velocity(${vx}, ${vy}, ${vz})\n`;
  };

  pythonGenerator.forBlock['engine_apply_impulse'] = function (block) {
    const ix = block.getFieldValue('IX');
    const iy = block.getFieldValue('IY');
    const iz = block.getFieldValue('IZ');
    return `CoronaEngine.apply_impulse(${ix}, ${iy}, ${iz})\n`;
  };

  pythonGenerator.forBlock['engine_get_velocity'] = function (block) {
    const axis = block.getFieldValue('AXIS');
    return [`CoronaEngine.get_velocity('${axis}')`, pythonGenerator.ORDER_ATOMIC];
  };
};
