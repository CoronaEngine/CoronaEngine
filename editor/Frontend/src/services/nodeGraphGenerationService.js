import {
  applyGeneratedNodeGraph,
  getGeneratedNodeGraphSnapshot,
  PROJECT_NODE_GRAPH_TARGET_ID,
} from '@/blockly/node-editor/aiNodeGraphService.js';
import { aiService } from '@/services/aiService.js';
import { appService } from '@/services/appService.js';
import { coronaEventBus } from '@/utils/eventBus.js';
import { translateUiText } from '@/i18n/domTranslator.js';

const MUTATION_PATTERN =
  /(\u751f\u6210|\u5236\u4f5c|\u65b0\u5efa|\u521b\u5efa|\u642d\u5efa|\u5b9e\u73b0|\u5f04(?:\u4e00\u4e2a|\u4e2a)?|\u505a(?:\u4e00\u4e2a|\u4e2a)?|\u7f16\u8f91|\u4fee\u6539|\u6539(?:\u6210|\u4e3a)|\u66ff\u6362(?:\u6210|\u4e3a)|\u6362(?:\u6210|\u4e3a)|\u8c03\u6574(?:\u6210|\u4e3a)|\u8bbe\u7f6e(?:\u6210|\u4e3a)|\u8bbe(?:\u6210|\u4e3a)|\u53d8(?:\u6210|\u4e3a)|\u8865\u5145|\u589e\u52a0|\u6dfb\u52a0|\u6269\u5c55|\u52a0(?:\u4e0a|\u5165|\u4e00\u4e2a|\u4e2a)?|\u5220\u9664|\u79fb\u9664|\u91cd\u505a|\u6539\u9020|generate|create|build|make|edit|modify|add|extend|delete|remove)/i;
const TARGET_PATTERN =
  /(\u6e38\u620f|demo|deno|\u73a9\u6cd5|\u529f\u80fd|\u6548\u679c|\u884c\u4e3a|\u673a\u5236|\u4ea4\u4e92|\u64cd\u4f5c|\u89c4\u5219|\u6d41\u7a0b|\u72b6\u6001\u673a|\u53c2\u6570|\u6570\u503c|\u901f\u5ea6|\u89d2\u5ea6|\u5bf9\u8c61\u5f15\u7528|\u5f15\u7528|\u8282\u70b9|\u79ef\u6728|\u903b\u8f91|\u573a\u666f|\u7269\u4f53|\u5bf9\u8c61|\u6a21\u578b|\u7403|\u6444\u50cf\u673a|\u79fb\u52a8|\u8df3\u8dc3|\u78b0\u649e|\u63a7\u5236|\u6309\u952e|\u6309\u4e0b|\u8f93\u5165|\u4e8b\u4ef6|\u952e\u76d8|\u9f20\u6807|\u7a7a\u683c|wasd|game|gameplay|feature|behavior|mechanic|interaction|block|node|logic|scene|object|actor|model|camera|move|jump|collision|control|input|event)/i;
const IMPERATIVE_PREFIX_PATTERN =
  /(?:^|[\s\u3002\uff0c,!.\uff01?\uff1f])(?:(?:\u8ba9)?\u5305\u83dc[\s\u3001\u3002\uff0c,.:\uff1a]*)?(?:\u5e2e\u6211|\u5e2e\u5fd9|\u8bf7(?:\u4f60)?|\u7ed9\u6211|\u66ff\u6211|\u4e3a\u6211|\u9ebb\u70e6|\u6211\u8981|\u6211\u60f3(?:\u8ba9\u4f60)?|\u80fd\u5426|\u53ef\u4ee5(?:\u5e2e\u6211)?|please|could you|i want you to)/i;
const LEADING_MUTATION_PATTERN =
  /^(?:(?:\u8bf7(?:\u4f60)?|\u9ebb\u70e6|\u5e2e\u6211|\u5e2e\u5fd9|\u7ed9\u6211|\u66ff\u6211|\u4e3a\u6211)\s*)?(?:\u751f\u6210|\u5236\u4f5c|\u65b0\u5efa|\u521b\u5efa|\u642d\u5efa|\u5b9e\u73b0|\u505a(?:\u4e00\u4e2a|\u4e2a)?|\u7f16\u8f91|\u4fee\u6539|\u6539(?:\u6210|\u4e3a)|\u66ff\u6362(?:\u6210|\u4e3a)|\u6362(?:\u6210|\u4e3a)|\u8c03\u6574(?:\u6210|\u4e3a)|\u8bbe\u7f6e(?:\u6210|\u4e3a)|\u8bbe(?:\u6210|\u4e3a)|\u53d8(?:\u6210|\u4e3a)|\u8865\u5145|\u589e\u52a0|\u6dfb\u52a0|\u6269\u5c55|\u52a0(?:\u4e0a|\u5165|\u4e00\u4e2a|\u4e2a)?|\u5220\u9664|\u79fb\u9664|\u91cd\u505a|\u6539\u9020|generate|create|build|make|edit|modify|add|extend|delete|remove)/i;
const OBJECT_IMPERATIVE_PATTERN =
  /^(?:(?:\u8bf7(?:\u4f60)?|\u9ebb\u70e6)\s*)?(?:\u7ed9|\u4e3a|\u628a|\u5c06)(?:\u5f53\u524d|\u8fd9\u4e2a|\u8be5|\u6211\u7684)?[^\u3002\uff01\uff1f!?\n]{0,64}?(?:\u751f\u6210|\u5236\u4f5c|\u65b0\u5efa|\u521b\u5efa|\u642d\u5efa|\u5b9e\u73b0|\u505a(?:\u4e00\u4e2a|\u4e2a)?|\u7f16\u8f91|\u4fee\u6539|\u6539(?:\u6210|\u4e3a)|\u66ff\u6362(?:\u6210|\u4e3a)|\u6362(?:\u6210|\u4e3a)|\u8c03\u6574(?:\u6210|\u4e3a)|\u8bbe\u7f6e(?:\u6210|\u4e3a)|\u8bbe(?:\u6210|\u4e3a)|\u53d8(?:\u6210|\u4e3a)|\u8865\u5145|\u589e\u52a0|\u6dfb\u52a0|\u6269\u5c55|\u52a0(?:\u4e0a|\u5165|\u4e00\u4e2a|\u4e2a)?|\u5220\u9664|\u79fb\u9664|\u91cd\u505a|\u6539\u9020|generate|create|build|make|edit|modify|add|extend|delete|remove)/i;
const REPLACEMENT_COMMAND_PATTERN =
  /^(?:(?:\u8bf7(?:\u4f60)?|\u9ebb\u70e6|\u5e2e\u6211|\u5e2e\u5fd9|\u7ed9\u6211|\u66ff\u6211|\u4e3a\u6211)\s*)?(?:(?:\u5c06|\u628a)\s*)?[^\u3002\uff01\uff1f!?\n]{0,80}?(?:\u4fee\u6539(?:\u6210|\u4e3a)|\u6539(?:\u6210|\u4e3a)|\u66ff\u6362(?:\u6210|\u4e3a)|\u6362(?:\u6210|\u4e3a)|\u8c03\u6574(?:\u6210|\u4e3a)|\u8bbe\u7f6e(?:\u6210|\u4e3a)|\u8bbe(?:\u6210|\u4e3a)|\u53d8(?:\u6210|\u4e3a))/i;
const QUESTION_PATTERN =
  /(\u5982\u4f55|\u600e\u4e48|\u600e\u6837|\u4e3a\u4ec0\u4e48|\u662f\u4ec0\u4e48|\u6709\u4ec0\u4e48|\u80fd\u4e0d\u80fd|\u53ef\u4e0d\u53ef\u4ee5|\u5417\s*[?\uff1f]?$|how\s+(?:do|can|to)|what\b|why\b)/i;
const EXPLANATION_REQUEST_PATTERN =
  /(\u89e3\u91ca|\u8bf4\u660e|\u544a\u8bc9\u6211|\u56de\u7b54|\u5206\u6790|\u4ec0\u4e48\u65f6\u5019|\u662f\u4ec0\u4e48|\u4e3a\u4ec0\u4e48|explain|tell\s+me|answer|describe|when\b|what\b|why\b)/i;
const READ_ONLY_REQUEST_PATTERN =
  /((?:\u4e0d\u8981|\u65e0\u9700|\u4e0d\u7528|\u4e0d\u9700\u8981|\u8bf7\u52ff)[^\u3002\uff01\uff1f!?\n]{0,20}(?:\u4fee\u6539|\u7f16\u8f91|\u6539\u53d8|\u751f\u6210|\u589e\u52a0|\u6dfb\u52a0|\u5220\u9664)|(?:\u53ea|\u4ec5)(?:\u9700\u8981)?(?:\u89e3\u91ca|\u8bf4\u660e|\u56de\u7b54|\u5206\u6790)|(?:explain|answer)\s+only|(?:do\s+not|don't|without)\s+(?:modify|edit|change|generate|add|delete))/i;
const DELETE_PATTERN = /(\u5220\u9664|\u79fb\u9664|\u53bb\u6389|\u5220\u6389|delete|remove)/i;
const EDIT_PATTERN =
  /(\u4fee\u6539|\u6539(?:\u6210|\u4e3a)|\u66ff\u6362(?:\u6210|\u4e3a)|\u6362(?:\u6210|\u4e3a)|\u8c03\u6574(?:\u6210|\u4e3a)|\u8bbe\u7f6e(?:\u6210|\u4e3a)|\u8bbe(?:\u6210|\u4e3a)|\u53d8(?:\u6210|\u4e3a)|\u7f16\u8f91|\u6539\u9020|\u91cd\u505a|\u8c03\u6574|edit|modify|rebuild|change|replace)/i;
const EXTEND_PATTERN =
  /(\u8865\u5145|\u589e\u52a0|\u6dfb\u52a0|\u6269\u5c55|\u52a0(?:\u4e0a|\u5165|\u4e00\u4e2a|\u4e2a)?|extend|add)/i;
const FEATURE_TARGET_PATTERN = /(\u529f\u80fd|\u73a9\u6cd5|feature|function|gameplay)/i;
const POLL_INTERVAL_MS = 700;
const GENERATION_TIMEOUT_MS = 120000;

let activeGeneration = null;

const ACTIVE_PROJECT_PATH_KEY = 'corona.activeProjectPath';

function normalizeProjectPath(value) {
  return String(value || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/\/+$/, '')
    .toLocaleLowerCase('en-US');
}

function currentProjectPath() {
  return normalizeProjectPath(window.localStorage?.getItem(ACTIVE_PROJECT_PATH_KEY) || '');
}

function wait(delay) {
  return new Promise((resolve) => window.setTimeout(resolve, delay));
}

function messageOf(response, fallback) {
  return String(response?.message || response?.error || fallback);
}

function responseLanguageForInstruction(instruction) {
  if (/[\u3400-\u9fff]/.test(instruction)) return 'zh-CN';
  const locale = String(globalThis.document?.documentElement?.lang || '').trim();
  return locale.toLowerCase().startsWith('en') ? 'en-US' : 'zh-CN';
}

export function nodeGraphGenerationIntent(text) {
  const instruction = String(text || '').trim();
  const hasImperativePrefix = IMPERATIVE_PREFIX_PATTERN.test(instruction);
  const startsWithMutation = LEADING_MUTATION_PATTERN.test(instruction);
  const hasObjectImperative = OBJECT_IMPERATIVE_PATTERN.test(instruction);
  const hasReplacementCommand = REPLACEMENT_COMMAND_PATTERN.test(instruction);
  const isReadOnlyRequest =
    EXPLANATION_REQUEST_PATTERN.test(instruction) && READ_ONLY_REQUEST_PATTERN.test(instruction);
  const isUncommandedQuestion = QUESTION_PATTERN.test(instruction) && !hasImperativePrefix;
  if (
    !instruction ||
    !MUTATION_PATTERN.test(instruction) ||
    !TARGET_PATTERN.test(instruction) ||
    (!hasImperativePrefix &&
      !startsWithMutation &&
      !hasObjectImperative &&
      !hasReplacementCommand) ||
    isUncommandedQuestion ||
    isReadOnlyRequest
  ) {
    return { matched: false, operation: '', instruction };
  }
  let operation = 'create';
  if (DELETE_PATTERN.test(instruction)) operation = 'delete';
  else if (EDIT_PATTERN.test(instruction)) operation = 'edit';
  else if (EXTEND_PATTERN.test(instruction) || FEATURE_TARGET_PATTERN.test(instruction))
    operation = 'extend';
  return { matched: true, operation, instruction };
}

async function requestNodePanelOpen() {
  // MainPage owns presentation: AI generation requests only the centered in-editor
  // floating panel, avoiding a second NodeGraphPanel in the bottom Dock.
  coronaEventBus.emit('node-graph-panel-open-request');
  try {
    await appService.crossTabBroadcast('node-graph-panel-open-request', {});
  } catch (_) {}
}

async function acquireSnapshot() {
  let snapshot = await getGeneratedNodeGraphSnapshot({ timeoutMs: 900 });
  if (snapshot?.workspace) return snapshot;
  await requestNodePanelOpen();
  const deadline = Date.now() + 6000;
  while (Date.now() < deadline) {
    await wait(300);
    snapshot = await getGeneratedNodeGraphSnapshot({ timeoutMs: 700 });
    if (snapshot?.workspace) return snapshot;
  }
  throw new Error('请先打开“节点”窗口，包菜才能读取并修改当前节点逻辑。');
}

function assertCurrentRequest(state) {
  if (activeGeneration !== state || state.cancelled) {
    throw new Error('已停止本次节点生成。');
  }
  if (state.projectPath !== currentProjectPath()) {
    state.cancelled = true;
    throw new Error('生成期间已切换世界，旧结果不会修改当前节点图。');
  }
}

function workspaceEntityIds(workspace = {}, collectionName = '') {
  const collection = Array.isArray(workspace?.[collectionName]) ? workspace[collectionName] : [];
  return new Set(collection.map((item) => String(item?.id || '').trim()).filter(Boolean));
}

function createdWorkspaceIds(beforeWorkspace = {}, afterWorkspace = {}, collectionName = '') {
  const beforeIds = workspaceEntityIds(beforeWorkspace, collectionName);
  return [...workspaceEntityIds(afterWorkspace, collectionName)].filter((id) => !beforeIds.has(id));
}

export async function generateNodeGraphFromInstruction(instruction, operation = 'create') {
  if (activeGeneration) throw new Error('包菜正在生成上一份节点逻辑，请先等待或停止。');
  const state = {
    taskId: '',
    cancelled: false,
    projectPath: currentProjectPath(),
  };
  activeGeneration = state;
  try {
    const snapshot = await acquireSnapshot();
    assertCurrentRequest(state);
    const requestId = `node_generate_${Date.now()}_${Math.random().toString(36).slice(2, 9)}`;
    const payload = {
      schemaVersion: 1,
      requestId,
      targetId: PROJECT_NODE_GRAPH_TARGET_ID,
      projectScopeId: String(snapshot.projectScopeId || ''),
      baseGraphRevision: String(snapshot.graphRevision || ''),
      operation,
      instruction: String(instruction || '').trim(),
      responseLanguage: responseLanguageForInstruction(instruction),
      workspace: snapshot.workspace,
      projectContext: snapshot.projectContext || {},
    };
    if (!payload.projectScopeId || !payload.baseGraphRevision) {
      throw new Error('当前世界的节点上下文尚未准备好，请稍后再试。');
    }

    const started = await aiService.startNodeGraphGeneration(payload);
    state.taskId = String(started?.taskId || '');
    if (state.cancelled && state.taskId) {
      try {
        await aiService.cancelNodeGraphGeneration(state.taskId);
      } catch (_) {}
      throw new Error('已停止本次节点生成。');
    }
    if (started?.success !== true || !state.taskId) {
      throw new Error(messageOf(started, 'DeepSeek 节点生成服务暂时不可用。'));
    }

    const deadline = Date.now() + GENERATION_TIMEOUT_MS;
    let generated = null;
    while (Date.now() < deadline) {
      assertCurrentRequest(state);
      const status = await aiService.getNodeGraphGenerationStatus(state.taskId);
      assertCurrentRequest(state);
      if (status?.success !== true) {
        throw new Error(messageOf(status, '无法读取节点生成状态。'));
      }
      if (status.status === 'cancelled') throw new Error('已停止本次节点生成。');
      if (status.status === 'completed') {
        generated = status.result;
        break;
      }
      await wait(POLL_INTERVAL_MS);
    }
    if (!generated) throw new Error('DeepSeek 生成节点逻辑超时，当前节点图没有被修改。');
    if (generated.success !== true || generated.status !== 'ok') {
      throw new Error(messageOf(generated, 'DeepSeek 没有返回可应用的节点图。'));
    }
    for (const key of [
      'requestId',
      'targetId',
      'projectScopeId',
      'baseGraphRevision',
      'operation',
    ]) {
      if (String(generated[key] || '') !== String(payload[key] || '')) {
        throw new Error(`DeepSeek 返回的 ${key} 已过期，当前节点图没有被修改。`);
      }
    }

    const latest = await getGeneratedNodeGraphSnapshot({ timeoutMs: 1800 });
    if (
      !latest?.workspace ||
      String(latest.projectScopeId || '') !== payload.projectScopeId ||
      String(latest.graphRevision || '') !== payload.baseGraphRevision
    ) {
      throw new Error('生成期间当前世界或节点逻辑已经改变，旧结果没有覆盖你的编辑。');
    }

    assertCurrentRequest(state);
    const applied = await applyGeneratedNodeGraph(generated);
    if (applied?.success !== true) {
      const details = Array.isArray(applied?.errors) ? applied.errors.join('；') : '';
      throw new Error(details || '生成结果未通过节点编辑器校验，当前节点图没有被修改。');
    }
    const afterApplySnapshot = await getGeneratedNodeGraphSnapshot({ timeoutMs: 1800 });
    const afterWorkspace =
      afterApplySnapshot?.workspace &&
      String(afterApplySnapshot.projectScopeId || '') === payload.projectScopeId
        ? afterApplySnapshot.workspace
        : generated?.workspace || {};
    const createdNodeIds = createdWorkspaceIds(latest.workspace, afterWorkspace, 'nodes');
    const createdEdgeIds = createdWorkspaceIds(latest.workspace, afterWorkspace, 'edges');
    const appliedSummary =
      applied?.summary && typeof applied.summary === 'object' ? applied.summary : {};
    let visibleLocation = '';
    const focusedNodeName = translateUiText(String(appliedSummary.focusedNodeName || ''));
    if (appliedSummary.focusedKind === 'node' && focusedNodeName) {
      visibleLocation =
        Number(appliedSummary.visibleBlockCount || 0) > 0
          ? translateUiText(
              `\u5df2\u81ea\u52a8\u6253\u5f00\u201c${focusedNodeName}\u201d\u8282\u70b9\uff0c\u5e76\u663e\u793a\u5176\u4e2d ${Number(appliedSummary.visibleBlockCount)} \u4e2a\u53ef\u89c1\u79ef\u6728\u3002`
            )
          : translateUiText(
              `\u5df2\u81ea\u52a8\u5b9a\u4f4d\u5230\u201c${focusedNodeName}\u201d\u8282\u70b9\uff1b\u8be5\u8282\u70b9\u5f53\u524d\u6ca1\u6709\u5185\u90e8\u79ef\u6728\u3002`
            );
    } else if (appliedSummary.focusedKind === 'edge') {
      visibleLocation =
        Number(appliedSummary.visibleBlockCount || 0) > 0
          ? translateUiText(
              `\u5df2\u81ea\u52a8\u6253\u5f00\u8fde\u7ebf\u6761\u4ef6\uff0c\u5e76\u663e\u793a\u5176\u4e2d ${Number(appliedSummary.visibleBlockCount)} \u4e2a\u53ef\u89c1\u79ef\u6728\u3002`
            )
          : translateUiText(
              '\u5df2\u81ea\u52a8\u5b9a\u4f4d\u5230\u672c\u6b21\u4fee\u6539\u7684\u8282\u70b9\u8fde\u7ebf\u3002'
            );
    }
    return {
      success: true,
      summary: [
        String(
          generated.summary ||
            '\u8282\u70b9\u903b\u8f91\u5df2\u7ecf\u751f\u6210\u5e76\u4fdd\u5b58\u3002'
        ).trim(),
        visibleLocation,
      ]
        .filter(Boolean)
        .join('\n'),
      warnings: Array.isArray(applied.warnings) ? applied.warnings : [],
      operation: String(operation || ''),
      createdNodeIds,
      createdEdgeIds,
    };
  } finally {
    if (activeGeneration === state) activeGeneration = null;
  }
}

export async function cancelActiveNodeGraphGeneration() {
  const state = activeGeneration;
  if (!state) return false;
  state.cancelled = true;
  if (state.taskId) {
    try {
      await aiService.cancelNodeGraphGeneration(state.taskId);
    } catch (_) {}
  }
  return true;
}

function cancelGenerationForProjectChange() {
  void cancelActiveNodeGraphGeneration();
}

function cancelGenerationForProjectStorageChange(event) {
  if (event?.key === ACTIVE_PROJECT_PATH_KEY) void cancelActiveNodeGraphGeneration();
}

if (typeof window !== 'undefined') {
  window.addEventListener('corona-active-project-changed', cancelGenerationForProjectChange);
  window.addEventListener('storage', cancelGenerationForProjectStorageChange);
}
