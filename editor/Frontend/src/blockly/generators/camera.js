import { pythonGenerator } from 'blockly/python';

export const defineCameraGenerators = () => {
  pythonGenerator.forBlock['camera_lock_mouse'] = function () {
    return 'CoronaEngine.lock_mouse()\n';
  };

  pythonGenerator.forBlock['camera_unlock_mouse'] = function () {
    return 'CoronaEngine.unlock_mouse()\n';
  };

  pythonGenerator.forBlock['camera_mouse_dx'] = function () {
    return ['CoronaEngine.mouse_dx()', pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['camera_mouse_dy'] = function () {
    return ['CoronaEngine.mouse_dy()', pythonGenerator.ORDER_ATOMIC];
  };

  pythonGenerator.forBlock['camera_set_fov'] = function (block) {
    const fov = block.getFieldValue('FOV');
    return `CoronaEngine.set_fov(${fov})\n`;
  };
};
