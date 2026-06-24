// 统一注册各分类的 Python 代码生成器，并自定义 workspaceToCode
import { pythonGenerator } from 'blockly/python';
import { resetPrelude, renderPreludeAt } from './prelude';
import { PYTHON_IMPORTS } from './constants';

import { defineAppearanceGenerators } from './appearance';
import { defineAudioGenerators } from './audio';
import { defineCameraGenerators } from './camera';
import { defineControlGenerators } from './control';
import { defineDetectGenerators } from './detect';
import { defineEngineGenerators } from './engine';
import { defineEventGenerators } from './event';
import { defineListGenerators } from './list';
import { defineMathGenerators } from './math';
import { defineVariableGenerators } from './variable';

// 注册所有分类的生成器（幂等）
try { defineAppearanceGenerators?.(); } catch {}
try { defineAudioGenerators?.(); } catch {}
try { defineCameraGenerators?.(); } catch {}
try { defineControlGenerators?.(); } catch {}
try { defineDetectGenerators?.(); } catch {}
try { defineEngineGenerators?.(); } catch {}
try { defineEventGenerators?.(); } catch {}
try { defineListGenerators?.(); } catch {}
try { defineMathGenerators?.(); } catch {}
try { defineVariableGenerators?.(); } catch {}

// 辅助：规范化 blockToCode 的返回（string | [string, order] | null）
function normalizeCode(out) {
  if (!out) return '';
  if (Array.isArray(out)) return String(out[0] ?? '');
  return String(out);
}

// 缩进工具
function indentBlock(s) {
  if (!s) return '';
  // 去除首尾空行，避免产生多余的空白行
  s = s.replace(/^\s*\n+|\n+\s*$/g, '');
  return s
    .split('\n')
    .map((line) => (line ? '    ' + line : ''))
    .join('\n');
}

// ── 自定义工作区 → Python 代码 ──
pythonGenerator.workspaceToCode = function customWorkspaceToCode(workspace) {
  // 在一次生成开始前，重置前置代码请求集合
  resetPrelude();
  // 初始化生成器（包括 procedure / variable 数据库）
  pythonGenerator.init(workspace);

  // 拿到顶层积木并按坐标排序
  const topBlocks = workspace.getTopBlocks(true);
  topBlocks.sort((a, b) => {
    const aXY = a.getRelativeToSurfaceXY();
    const bXY = b.getRelativeToSurfaceXY();
    return aXY.y - bXY.y || aXY.x - bXY.x;
  });

  // 区分帽子积木（无 previousConnection）和孤立积木
  const hatBlocks = topBlocks.filter((b) => !b.previousConnection);
  const orphanCount = topBlocks.length - hatBlocks.length;

  // ── 积木类型分类 ──
  const KEYBOARD_BLOCK_TYPES = new Set(['event_keyboard', 'event_keyboard_combo']);
  const MOUSE_BLOCK_TYPES = new Set([
    'event_mouse_click', 'event_mouse_move',
    'event_mouse_wheel', 'event_mouse_contextmenu',
  ]);
  // 标准函数定义块 —— 生成的 def 语句放在顶层，不嵌套在 run() 内
  const PROCEDURE_BLOCK_TYPES = new Set([
    'procedures_defnoreturn',
    'procedures_defreturn',
  ]);

  let mainCode = '';
  let handlerCode = '';
  let mouseHandlerCode = '';
  let procedureCode = '';

  for (const block of hatBlocks) {
    // Blockly v12+: block.disabled 仅反映自身禁用状态，不包含父级继承的禁用。
    // 使用 isEnabled() + getInheritedDisabled() 确保完整检查（上游 issue #9372）。
    if (!block.isEnabled() || block.getInheritedDisabled()) continue;
    let blockCode = pythonGenerator.blockToCode(block);
    let chunk = normalizeCode(blockCode);
    if (chunk && !chunk.endsWith('\n')) chunk += '\n';

    if (KEYBOARD_BLOCK_TYPES.has(block.type)) {
      handlerCode += chunk;
    } else if (MOUSE_BLOCK_TYPES.has(block.type)) {
      mouseHandlerCode += chunk;
    } else if (PROCEDURE_BLOCK_TYPES.has(block.type)) {
      // 函数定义放在顶层
      procedureCode += chunk;
    } else {
      mainCode += chunk;
    }
  }

  // ── 孤立积木警告 ──
  let orphanWarning = '';
  if (orphanCount > 0) {
    orphanWarning =
      `# =========================================\n` +
      `# WARNING: ${orphanCount} 个孤立积木未连接任何事件积木，不会执行\n` +
      `# 请将它们连接到事件积木（如"当游戏开始时"）下方\n` +
      `# =========================================\n`;
  }

  // ── 结束生成 ──
  mainCode = pythonGenerator.finish(mainCode);
  if (mainCode && !mainCode.endsWith('\n')) mainCode += '\n';

  // ── 头注释 ──
  const timestamp = new Date().toISOString();
  const header = [
    '# -*- coding: utf-8 -*-',
    `# Generated from Blockly by CabbageEditor @ ${timestamp}`,
    PYTHON_IMPORTS.ENGINE_IMPORT,
  ].join('\n');

  // ── 前置片段 ──
  const preludeGlobal = renderPreludeAt('global');
  const preludeRunPrologue = renderPreludeAt('runPrologue');
  const preludeRunEpilogue = renderPreludeAt('runEpilogue');

  // ── 组装输出 ──
  const parts = [];
  parts.push(header);
  if (orphanWarning) parts.push(orphanWarning.trimEnd());
  if (preludeGlobal) parts.push(preludeGlobal.trimEnd());

  // 函数定义（顶层，不缩进 — 可被 run() 内代码调用）
  if (procedureCode.trim()) {
    parts.push('');
    parts.push(procedureCode.trimEnd());
  }

  // 键盘事件 handler
  if (handlerCode.trim()) {
    parts.push('');
    parts.push('def handle(key, _mods=None):');
    const indentedHandlers = indentBlock(handlerCode);
    if (indentedHandlers) parts.push(indentedHandlers);
    else parts.push('    pass');
  }

  // 鼠标事件 handler
  if (mouseHandlerCode.trim()) {
    parts.push('');
    parts.push('def handle_mouse(_event_type, _button, _x, _y):');
    const indentedMouseHandlers = indentBlock(mouseHandlerCode);
    if (indentedMouseHandlers) parts.push(indentedMouseHandlers);
    else parts.push('    pass');
  }

  // 主函数 def run()
  parts.push('');
  parts.push('def run():');
  const runBody = [];
  const indentedPrologue = indentBlock(preludeRunPrologue);
  if (indentedPrologue) runBody.push(indentedPrologue);
  const indentedMain = indentBlock(mainCode);
  if (indentedMain) runBody.push(indentedMain);
  const indentedEpilogue = indentBlock(preludeRunEpilogue);
  if (indentedEpilogue) runBody.push(indentedEpilogue);
  if (runBody.length) {
    parts.push(runBody.join('\n'));
  } else {
    parts.push('    pass');
  }

  // 末尾统一加一个换行
  return parts.join('\n') + '\n';
};

export { pythonGenerator };
