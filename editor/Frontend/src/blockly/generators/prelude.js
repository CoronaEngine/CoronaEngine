// 生成器前置代码（Prelude）注册表（支持多插入点）
// 用法：
// - 在某个积木的生成器中：import { need } from './prelude'; need('keyboard')
// - 生成流程中：resetPrelude()；然后在不同位置 renderPreludeAt('global'|'runPrologue'|'runEpilogue')

import { PYTHON_IMPORTS } from './constants';

// 已请求的前置段集合
const _required = new Set();

// 预设的前置片段清单（可按需扩展/修改）
// 每个键支持：
// - 字符串：仅用于 global 位置；
// - 或对象：{ global?: string, runPrologue?: string, runEpilogue?: string }
const PRELUDE_SNIPPETS = {
  // 键盘事件支持：当使用键盘事件积木时加入
  // 通过 CEF bridge (ScratchTool.key_event) → CE.handle_key_event → handle()
  keyboard: {
    global: [
      '# 键盘事件桥接：注册 handle 到 CE 模块',
      'from script_runtime.engine import corona_engine as _CE',
    ].join('\n'),
    runPrologue: [
      '# 将 handle 注册到 CE，CEF bridge 收到键盘事件后会调用它',
      '_CE.register_key_handler(handle)',
      'print("[Blockly] 键盘处理器已注册: " + handle.__name__)',
    ].join('\n'),
    runEpilogue: [],
  },

  // 鼠标事件支持：当使用鼠标事件积木时加入
  mouse: {
    global: [
      '# 鼠标事件桥接：注册 handle_mouse 到 CE 模块',
      'from script_runtime.engine import corona_engine as _CE',
    ].join('\n'),
    runPrologue: [
      '# 将 handle_mouse 注册到 CE，CEF bridge 收到鼠标事件后会调用它',
      '_CE.register_mouse_handler(handle_mouse)',
    ].join('\n'),
    runEpilogue: [],
  },
};

// 标记需要某个前置片段
export function need(name) {
  _required.add(name);
}

// 重置（在一次 workspaceToCode 开始前调用）
export function resetPrelude() {
  _required.clear();
}

// 渲染所有已请求的前置片段（旧接口：仅 global）
export function renderPrelude() {
  return renderPreludeAt('global');
}

// 渲染指定插入点的片段并返回文本（以单个换行结尾，或空串）
export function renderPreludeAt(where /* 'global' | 'runPrologue' | 'runEpilogue' */) {
  const parts = [];
  for (const name of _required) {
    const entry = PRELUDE_SNIPPETS[name];
    if (!entry) continue;
    let text = '';
    if (typeof entry === 'string') {
      if (where === 'global') text = entry;
    } else if (typeof entry === 'object') {
      text = entry[where] || '';
    }
    if (text) parts.push(String(text).replace(/[\r\n]+$/, '')); // 去除尾部多余换行
  }
  if (parts.length === 0) return '';
  return parts.join('\n') + '\n';
}
