import { pythonGenerator } from 'blockly/python';

// 将数值字段格式化为 Python 字面量：整数去小数点，浮点数保留合理精度
function formatNumber(raw) {
  const num = parseFloat(raw || '0');
  if (Number.isNaN(num)) return '0';
  return Number.isInteger(num) ? num.toString() : parseFloat(num.toFixed(6)).toString();
}

export const defineVariableGenerators = () => {
  pythonGenerator.forBlock['variable_add'] = function (block) {
    const v = block.getFieldValue('v');
    const x = formatNumber(block.getFieldValue('x'));
    return `CoronaEngine.var_add("${v}",${x})\n`;
  };

  pythonGenerator.forBlock['variable_set'] = function (block) {
    const v = block.getFieldValue('v');
    const x = formatNumber(block.getFieldValue('x'));
    return `CoronaEngine.var_set("${v}",${x})\n`;
  };

  pythonGenerator.forBlock['variable_show'] = function (block) {
    const v = block.getFieldValue('v');
    return `CoronaEngine.var_show("${v}")\n`;
  };

  pythonGenerator.forBlock['variable_hide'] = function (block) {
    const v = block.getFieldValue('v');
    return `CoronaEngine.var_hide("${v}")\n`;
  };
};
