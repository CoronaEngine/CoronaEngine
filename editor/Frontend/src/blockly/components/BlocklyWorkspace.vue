<template>
  <div
    :class="embedded ? 'flex-1 min-h-0 w-full' : 'h-screen'"
    class="rounded-lg overflow-hidden relative bg-[#282828]/90 flex flex-col text-white font-sans"
  >
    <DockTitleBar
      v-if="!embedded && !isDocked"
      :title="editingTarget ? `${t('blockly.title')} - ${editingTarget}` : t('blockly.title')"
      extraClass="bg-[#84A65B]"
      routePath="/ScratchTool"
      @close="handleClose"
    />

    <!-- 导航栏已迁移至物体属性面板的积木标签页 -->

    <!-- 工作区工具栏（始终可见，不遮挡任何内容） -->
    <div
      class="flex items-center gap-1 px-2 py-1 select-none shrink-0"
      style="background: #252525; border-bottom: 1px solid rgba(255,255,255,0.06);"
    >
      <button class="toolbar-btn group" :title="t('blockly.toolbarImport')" @click="triggerFileImport">
        <svg class="toolbar-icon" viewBox="0 0 16 16" fill="none"><path d="M2 10v3h12v-3M8 3v8M5 6l3-3 3 3" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>
        <span class="toolbar-label">{{ t('blockly.importBlocks') }}</span>
      </button>
      <button class="toolbar-btn group" :title="t('blockly.toolbarExport')" @click="handleToolbarExport">
        <svg class="toolbar-icon" viewBox="0 0 16 16" fill="none"><path d="M2 10v3h12v-3M8 3v8M5 10l3 3 3-3" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>
        <span class="toolbar-label">{{ t('blockly.exportBlocks') }}</span>
      </button>
      <button class="toolbar-btn group" :title="t('blockly.toolbarClear')" @click="handleClear">
        <svg class="toolbar-icon" viewBox="0 0 16 16" fill="none"><path d="M2 4h12M5.33 4V2.67a1 1 0 011-1h3.34a1 1 0 011 1V4m1.33 0v9.33a1 1 0 01-1 1H4.33a1 1 0 01-1-1V4h9.34z" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>
        <span class="toolbar-label">{{ t('blockly.clear') }}</span>
      </button>
      <div class="flex-1"></div>
      <div class="flex items-center gap-0.5">
        <button class="toolbar-btn group" :title="t('blockly.toolbarZoomOut')" @click="handleZoomOut">
          <svg class="toolbar-icon" viewBox="0 0 16 16" fill="none"><path d="M4 8h8" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>
        </button>
        <button class="toolbar-btn group" :title="t('blockly.toolbarZoomReset')" @click="handleZoomReset">
          <span class="toolbar-label" style="min-width:36px;text-align:center;">{{ scaleText }}</span>
        </button>
        <button class="toolbar-btn group" :title="t('blockly.toolbarZoomIn')" @click="handleZoomIn">
          <svg class="toolbar-icon" viewBox="0 0 16 16" fill="none"><path d="M4 8h8M8 4v8" stroke="currentColor" stroke-width="1.5" stroke-linecap="round"/></svg>
        </button>
      </div>
      <button
        class="toolbar-btn group"
        :class="{ 'toolbar-btn-active': store.hasLayoutSider.value }"
        :title="t('blockly.codeArea')"
        @click="store.hasLayoutSider.value = !store.hasLayoutSider.value"
      >
        <svg class="toolbar-icon" viewBox="0 0 16 16" fill="none"><path d="M5 2H3a1 1 0 00-1 1v10a1 1 0 001 1h2M11 2h2a1 1 0 011 1v10a1 1 0 01-1 1h-2M6.5 6l2 2-2 2" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round"/></svg>
        <span class="toolbar-label">{{ t('blockly.codeArea') }}</span>
      </button>
    </div>

    <div class="flex-1 flex overflow-hidden">
      <div ref="blockdiv" class="flex-1 relative" style="overflow: hidden;">
        <Search />
        <Trashcan />
        <!-- 拖拽导入高亮遮罩 -->
        <div
          id="drag-overlay"
          class="absolute inset-0 z-20 flex items-center justify-center pointer-events-none"
          style="background: rgba(132,166,91,0.15); border: 3px dashed #84a65b; display: none;"
        >
          <div class="text-center text-[#84a65b]">
            <div class="text-4xl mb-2">📂</div>
            <div class="text-lg font-bold">{{ t('blockly.dragImport') }}</div>
          </div>
        </div>
        <!-- 隐藏的文件导入输入框 -->
        <input
          ref="fileImportInput"
          type="file"
          accept=".blockly,.json"
          class="hidden"
          @change="handleFileImportChange"
        />
        <!-- 导入反馈提示 -->
        <div
          id="import-toast"
          class="absolute top-4 left-1/2 -translate-x-1/2 z-30 px-4 py-2 rounded shadow-lg text-sm font-medium transition-opacity duration-300 pointer-events-none"
          style="opacity: 0;"
        ></div>
        <div id="status-overlay" class="absolute inset-0 flex items-center justify-center z-10" style="background: #1e1e1e;">
          <div class="text-center">
            <div class="text-xl font-bold text-green-400 mb-2">{{ t('blockly.title') }}</div>
            <div class="text-sm text-gray-400">{{ t('blockly.loadingWorkspace') }}</div>
          </div>
        </div>
        <!-- 状态栏 -->
        <div
          id="blockly-status-bar"
          class="absolute bottom-0 left-0 right-0 z-10 flex items-center justify-between px-3 py-1 text-xs select-none"
          style="background: rgba(30,30,30,0.85); border-top: 1px solid rgba(255,255,255,0.08);"
        >
          <span class="text-gray-500">{{ autoSaveLabel }}</span>
          <span
            v-if="orphanCount > 0"
            class="text-yellow-500/80"
          >{{ t('blockly.orphanBlocks', { count: orphanCount }) }}</span>
        </div>
      </div>

      <div
        v-if="store.hasLayoutSider.value"
        class="w-80 flex flex-col border-l border-gray-700 bg-[#1e1e1e]"
      >
        <div class="flex items-center justify-between px-3 py-2 border-b border-gray-700">
          <span class="text-sm text-gray-300 font-medium">{{ t('blockly.pythonCode') }}</span>
          <div class="flex items-center gap-1">
            <button
              class="text-xs px-2 py-1 rounded transition-colors font-medium select-none"
              :class="codeRunning
                ? 'text-red-300 bg-red-700/40 hover:bg-red-600/50 hover:text-red-200'
                : 'text-green-400 bg-green-700/30 hover:bg-green-600/40 hover:text-green-200'"
              @click="handleToggleRun"
            >{{ codeRunning ? `⏹ ${t('blockly.stop')}` : `▶ ${t('blockly.run')}` }}</button>
            <button
              class="text-gray-400 hover:text-white transition-colors text-lg leading-none px-1"
              @click="store.hasLayoutSider.value = false"
            >✕</button>
          </div>
        </div>
        <pre class="flex-1 overflow-auto p-3 text-sm text-green-400 font-mono whitespace-pre-wrap m-0">{{ generatedCode }}</pre>
      </div>
    </div>
  </div>
</template>

<script>
// ============================================================
// 模块级只保留跨组件注册表。每个 BlocklyWorkspace 的目标、计时器、
// workspace 状态都必须留在组件实例内，避免多个详情窗口串台。
// ============================================================

/** 自动保存间隔（毫秒） */
const AUTO_SAVE_DELAY = 3000;

const mountedBlocklyWorkspaces = new Set();

function refreshBlocklyGlobalHooks() {
  if (typeof window === 'undefined') return;

  if (mountedBlocklyWorkspaces.size === 0) {
    delete window.__coronaBlocklyFlushSave;
    delete window.__coronaBlocklyReloadAll;
    delete window.__coronaBlocklyClearCaches;
    return;
  }

  window.__coronaBlocklyFlushSave = () => Promise.all(
    Array.from(mountedBlocklyWorkspaces, (api) => api.flushSave())
  );
  window.__coronaBlocklyReloadAll = () => Promise.all(
    Array.from(mountedBlocklyWorkspaces, (api) => api.reloadFromProject())
  );
  window.__coronaBlocklyClearCaches = () => {
    mountedBlocklyWorkspaces.forEach((api) => api.clearCache());
  };
}

function registerBlocklyWorkspace(api) {
  mountedBlocklyWorkspaces.add(api);
  refreshBlocklyGlobalHooks();
}

function unregisterBlocklyWorkspace(api) {
  mountedBlocklyWorkspaces.delete(api);
  refreshBlocklyGlobalHooks();
}
</script>

<script setup>
import { ref, watch, onMounted, onUnmounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { useErrorHandler } from '@/composables/useErrorHandler.js';
import { useDockPanel } from '@/composables/useDockPanel.js';
import { scriptingService } from '@/utils/bridge.js';

const { closePanel: closeDockPanel, isDocked } = useDockPanel();
const { t, locale } = useI18n();
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import Search from './Search.vue';

import Trashcan from './Trashcan.vue';
import { useStore } from '../store/store.js';
import {
  currentSceneName,
  currentActorName,
  currentTargetType,
  getBlockTargetContext,
  syncActorContextFromStorage,
} from '../composables/useActorContext.js';

const props = defineProps({
  actorName: { type: String, default: '' },
  /** 嵌入式模式：作为子组件内嵌到物体属性面板 */
  embedded: { type: Boolean, default: false },
  /** 场景名称（嵌入式时由父组件传入） */
  sceneName: { type: String, default: '' },
});

const emit = defineEmits(['resize']);
const { error: logError } = useErrorHandler('BlocklyWorkspace');

const blockdiv = ref(null);
const fileImportInput = ref(null);
const store = useStore();
const generatedCode = ref('');
const codeRunning = ref(false);
const autoSaveLabel = ref('');
const orphanCount = ref(0);

// 标题栏显示的当前编辑目标
const editingTarget = ref('');

// 关键修复：不使用 Vue ref()/shallowRef() 存储 Blockly workspace
// 原因：Vue ref() 内部会对对象调用 reactive() 生成 Proxy
// Blockly workspace 被 Proxy 包裹后，序列化/反序列化时访问
// caller/callee/arguments 等内部属性会触发严格模式限制而报错
// 参考 Blockly 官方 issue #8441
let workspace = null;

let BlocklyLib = null;
let blocklyCN = null;
let blocklyEN = null;
let blocksRegistered = false;

// 保存 store 引用，供 onUnmounted 清理共享状态
let sharedStore = null;

let loadedActorKey = '';
let loadedTargetInfo = null;
let currentActorNameVar = '';
let pollTimer = null;
let autoSaveTimer = null;
let isLoadingWorkspace = false;
let targetSwitchSeq = 0;
let pythonGenerator = null;
let latestBlocklySavePromise = Promise.resolve(true);

// CEF 键盘事件转发：在 OSR 模式下 Blockly FieldTextInput 的 HTML widget
// 无法接收原生键盘事件，需要在 document 层面拦截并手动转发
let cefKeyHandler = null;

function setupCefFieldInputFix() {
  cefKeyHandler = (e) => {
    // 已有原生 input 获得焦点时，不干预
    const activeEl = document.activeElement;
    if (activeEl && (activeEl.tagName === 'INPUT' || activeEl.tagName === 'TEXTAREA')) {
      // 如果焦点就在 blocklyHtmlInput 上，说明原生键盘已生效，不转发
      if (activeEl.classList.contains('blocklyHtmlInput')) return;
      return;
    }

    // 查找 Blockly 字段编辑器输入框
    const htmlInput = document.querySelector('.blocklyHtmlInput');
    if (!htmlInput) return;

    // 转发可打印字符到输入框
    if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
      e.preventDefault();
      e.stopPropagation();
      const start = htmlInput.selectionStart ?? htmlInput.value.length;
      const end = htmlInput.selectionEnd ?? start;
      htmlInput.value =
        htmlInput.value.slice(0, start) + e.key + htmlInput.value.slice(end);
      htmlInput.selectionStart = htmlInput.selectionEnd = start + 1;
      htmlInput.dispatchEvent(new InputEvent('input', { bubbles: true, inputType: 'insertText', data: e.key }));
      return;
    }

    // 特殊功能键：先聚焦到输入框，让 Blockly 的内部处理器接管
    if (['Backspace', 'Delete', 'ArrowLeft', 'ArrowRight', 'Home', 'End', 'Enter', 'Tab'].includes(e.key)) {
      htmlInput.focus();
    }
  };
  document.addEventListener('keydown', cefKeyHandler, true);
}

function teardownCefFieldInputFix() {
  if (cefKeyHandler) {
    document.removeEventListener('keydown', cefKeyHandler, true);
    cefKeyHandler = null;
  }
}

// ============================================================
// 积木键盘事件转发：将按键发送到 Python handle()
// ============================================================
let scriptKeyHandler = null;
let scriptKeyUpHandler = null;

function setupScriptKeyForwarding() {
  scriptKeyHandler = (e) => {
    // 焦点在输入框时跳过（不干扰文字输入）
    const activeEl = document.activeElement;
    if (activeEl && (activeEl.tagName === 'INPUT' || activeEl.tagName === 'TEXTAREA')) {
      return;
    }

    const code = e.code || e.key;
    const displayKey = e.key || code;
    const mods = [];
    if (e.ctrlKey || e.metaKey) mods.push('Ctrl');
    if (e.shiftKey) mods.push('Shift');
    if (e.altKey) mods.push('Alt');

    // ── 快速通道：coronaBridge.injectInput → CEF ProcessMessage → 队列 → Python 批量消费 ──
    const bridge = window.coronaBridge;
    if (bridge && typeof bridge.injectInput === 'function') {
      try { bridge.injectInput(0, code, mods.join(','), displayKey); return; } catch (e) {}
    }
    // ── 慢通道：cefQuery 回退 ──
    scriptingService.sendKeyEvent(code, mods.join(','), displayKey).catch(() => {});
  };

  scriptKeyUpHandler = (e) => {
    const activeEl = document.activeElement;
    if (activeEl && (activeEl.tagName === 'INPUT' || activeEl.tagName === 'TEXTAREA')) {
      return;
    }
    // ── 快速通道：coronaBridge.injectInput → CEF ProcessMessage → 队列 → Python 批量消费 ──
    const bridge = window.coronaBridge;
    if (bridge && typeof bridge.injectInput === 'function') {
      try { bridge.injectInput(1, e.code || e.key, e.key || e.code); return; } catch (e) {}
    }
    // ── 慢通道：cefQuery 回退 ──
    scriptingService.sendKeyUpEvent(e.code || e.key, e.key || e.code).catch(() => {});
  };

  document.addEventListener('keydown', scriptKeyHandler, true);
  document.addEventListener('keyup', scriptKeyUpHandler, true);
}

function teardownScriptKeyForwarding() {
  if (scriptKeyHandler) {
    document.removeEventListener('keydown', scriptKeyHandler, true);
    scriptKeyHandler = null;
  }
  if (scriptKeyUpHandler) {
    document.removeEventListener('keyup', scriptKeyUpHandler, true);
    scriptKeyUpHandler = null;
  }
}

async function updateGeneratedCode() {
  if (!workspace || !pythonGenerator) {
    generatedCode.value = '';
    return;
  }
  try {
    const code = pythonGenerator.workspaceToCode(workspace);
    generatedCode.value = code || t('blockly.codeEmpty');
  } catch {
    generatedCode.value = t('blockly.codeGenerationFailed');
  }
}

function getCurrentTarget() {
  if (props.embedded) {
    return {
      targetType: 'actor',
      scene: props.sceneName || '',
      actor: props.actorName || '',
    };
  }
  return getBlockTargetContext();
}

function getTargetKey(target) {
  if (target?.targetType === 'project') return 'project/global';
  return target?.scene && target?.actor ? `${target.scene}/${target.actor}` : '';
}

function getTargetDisplayName(target) {
  if (target?.targetType === 'project') return t('blockly.projectGlobalBlocks');
  return target?.actor ? `${target.actor} [${target.scene || ''}]` : '';
}

async function handleToggleRun() {
  if (codeRunning.value) {
    // 当前正在执行 → 停止
    clearPollTimer();
    try {
      await scriptingService.stopScriptExecution();
    } catch (e) {
      console.warn('[Blockly] 停止脚本失败:', e);
    }
    codeRunning.value = false;
    return;
  }

  // 当前未执行 → 运行
  const code = generatedCode.value;
  if (!code || code === t('blockly.codeEmpty') || code === t('blockly.codeGenerationFailed')) {
    alert(t('blockly.noExecutableCode'));
    return;
  }

  flushAutoSave();

  const target = getCurrentTarget();
  if (target.targetType === 'actor' && (!target.scene || !target.actor)) {
    alert(t('blockly.noSelectedActor'));
    return;
  }

  codeRunning.value = true;
  try {
    const result = await scriptingService.executePythonCode(
      code,
      0,
      target.scene,
      target.actor,
      target.targetType,
    );
    const execResult = result?.data ?? result;
    if (execResult?.status === 'error') {
      alert(t('blockly.codeRunningFailed', { message: execResult.message || t('blockly.codeRunUnknownError') }));
      codeRunning.value = false;
    } else {
      // 脚本已在后台线程启动，开始轮询状态
      startPollTimer();
    }
  } catch (e) {
    alert(t('blockly.codeRunFailed', { message: e.message || t('blockly.codeRunUnknownError') }));
    codeRunning.value = false;
  }
}

function clearPollTimer() {
  if (pollTimer) {
    clearTimeout(pollTimer);
    pollTimer = null;
  }
}

function startPollTimer() {
  clearPollTimer();
  const startTime = Date.now();
  const MAX_RUNTIME = 30000; // 30秒强制超时

  const poll = async () => {
    if (!codeRunning.value) return;

    // 安全超时：超过30秒强制重置
    if (Date.now() - startTime > MAX_RUNTIME) {
      console.warn('[Blockly] 脚本执行超时，强制停止');
      try { await scriptingService.stopScriptExecution(); } catch (_) {}
      codeRunning.value = false;
      return;
    }

    try {
      const status = await scriptingService.getScriptStatus();
      const pollResult = status?.data ?? status;
      if (pollResult?.status === 'idle') {
        codeRunning.value = false;
        return;
      }
    } catch (e) {
      console.warn('[Blockly] 脚本状态查询失败:', e);
      codeRunning.value = false;
      return;
    }
    pollTimer = setTimeout(poll, 500);
  };
  pollTimer = setTimeout(poll, 500);
}

const loadBlocklyModules = async () => {
  try {
    BlocklyLib = await import('blockly/core');
    blocklyCN = await import('blockly/msg/zh-hans');
    blocklyEN = await import('blockly/msg/en');
    return true;
  } catch (e) {
    logError('Blockly 模块加载失败', e);
    return false;
  }
};

const broadcastList = ref([]);
const createNewBroadcast = () => {
  const name = prompt(t('blockly.selectBroadcastName'));
  if (name && name.trim() !== '') broadcastList.value.push(name.trim());
};

// ============================================================
// 按 Actor 管理工作区状态（Blockly 官方推荐模式）
function saveCurrentWorkspace() {
  if (isLoadingWorkspace || !workspace || !BlocklyLib || !loadedActorKey) {
    latestBlocklySavePromise = Promise.resolve(false);
    return latestBlocklySavePromise;
  }
  try {
    updateGeneratedCode();
    const state = BlocklyLib.serialization.workspaces.save(workspace);
    const target = loadedTargetInfo || getCurrentTarget();
    latestBlocklySavePromise = scriptingService.saveBlocklyTarget({
      target_type: target.targetType,
      scene_name: target.scene,
      actor_name: target.actor,
      workspace: state,
      code: generatedCode.value || '',
      enabled: true,
    }).catch((e) => {
      console.warn('[Blockly] 保存项目积木镜像失败:', e);
      return false;
    });
    const now = new Date();
    const ts = now.toLocaleTimeString(locale.value, { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    autoSaveLabel.value = `${t('common.save')} ${ts}`;
    return latestBlocklySavePromise;
  } catch (e) {
    logError('保存工作区状态失败', e);
    autoSaveLabel.value = t('blockly.saveFailed');
    latestBlocklySavePromise = Promise.resolve(false);
    return latestBlocklySavePromise;
  }
}

/** 防抖自动保存 */
function scheduleAutoSave() {
  if (autoSaveTimer) clearTimeout(autoSaveTimer);
  autoSaveTimer = setTimeout(() => {
    saveCurrentWorkspace();
    autoSaveTimer = null;
  }, AUTO_SAVE_DELAY);
}

/** 立即保存 */
function flushAutoSave() {
  if (autoSaveTimer) {
    clearTimeout(autoSaveTimer);
    autoSaveTimer = null;
  }
  return saveCurrentWorkspace();
}

/** 标记孤立积木：未连接事件积木(Hat)的块变灰 */
function markOrphanBlocks() {
  if (!workspace || !BlocklyLib) return;
  try {
    const allBlocks = workspace.getAllBlocks(false);
    const topBlocks = workspace.getTopBlocks(true);

    // 收集 hat block 及其所有后代
    const reachable = new Set();
    for (const top of topBlocks) {
      if (!top.previousConnection) {
        reachable.add(top.id);
        const descendants = top.getDescendants(false);
        for (const desc of descendants) reachable.add(desc.id);
      }
    }

    // 应用可见性
    let orphanCnt = 0;
    for (const block of allBlocks) {
      const svgRoot = block.getSvgRoot();
      if (!svgRoot) continue;
      const isHat = !block.previousConnection && topBlocks.includes(block);
      const isReachable = reachable.has(block.id);
      if (!isHat && !isReachable) {
        svgRoot.style.opacity = '0.35';
        svgRoot.style.filter = 'grayscale(0.8)';
        svgRoot.setAttribute('data-orphan', 'true');
        orphanCnt++;
      } else {
        svgRoot.style.opacity = '';
        svgRoot.style.filter = '';
        svgRoot.removeAttribute('data-orphan');
      }
    }
    orphanCount.value = orphanCnt;
  } catch (e) { /* 静默 */ }
}

/** 工作区变更统一回调 */
function onWorkspaceChange() {
  if (isLoadingWorkspace) return;
  updateGeneratedCode();
  scheduleAutoSave();
  markOrphanBlocks();
  if (loadedActorKey) autoSaveLabel.value = t('blockly.unsavedChanges');
}

async function loadWorkspaceStateFromProject(target) {
  const result = await scriptingService.loadBlocklyTarget({
    target_type: target.targetType,
    scene_name: target.scene,
    actor_name: target.actor,
  });
  const payload = result?.data ?? result;
  if (payload?.status === 'error') {
    throw new Error(payload.message || t('blockly.loadFailed'));
  }
  return payload?.workspace && typeof payload.workspace === 'object'
    ? payload.workspace
    : {};
}

function hasSerializedWorkspaceContent(state) {
  if (!state || typeof state !== 'object') return false;
  if (Array.isArray(state.blocks?.blocks) && state.blocks.blocks.length > 0) return true;
  if (Array.isArray(state.variables) && state.variables.length > 0) return true;
  return Object.keys(state).length > 0 && JSON.stringify(state) !== '{}';
}

async function loadTargetWorkspace(target, { saveCurrent = true, force = false } = {}) {
  if (!workspace || !BlocklyLib) return;

  const normalized = target || { targetType: 'actor', scene: '', actor: '' };
  const newKey = getTargetKey(normalized);
  if (!force && newKey === loadedActorKey) return;
  const seq = ++targetSwitchSeq;

  if (saveCurrent && loadedActorKey) {
    await flushAutoSave();
  }

  let nextState = {};
  if (newKey) {
    try {
      nextState = await loadWorkspaceStateFromProject(normalized);
    } catch (e) {
      logError('加载目标积木失败', e);
      autoSaveLabel.value = t('blockly.loadFailed');
    }
  }
  if (seq !== targetSwitchSeq) return;

  isLoadingWorkspace = true;
  try {
    workspace.clear();
    if (hasSerializedWorkspaceContent(nextState)) {
      BlocklyLib.serialization.workspaces.load(nextState, workspace);
    }
  } catch (e) {
    logError('切换工作区失败', e);
  } finally {
    isLoadingWorkspace = false;
  }

  loadedActorKey = newKey;
  loadedTargetInfo = normalized.targetType === 'project'
    ? { targetType: 'project', scene: '', actor: '' }
    : { targetType: 'actor', scene: normalized.scene || '', actor: normalized.actor || '' };
  currentActorNameVar = loadedTargetInfo.actor || '';
  editingTarget.value = getTargetDisplayName(loadedTargetInfo);
  autoSaveLabel.value = newKey ? t('blockly.loaded') : '';
  updateGeneratedCode();
  markOrphanBlocks();
}

/**
 * 切换到指定积木目标的工作区状态
 * 参考 Blockly 官方序列化 API：
 *   - 保存当前状态 → 清空工作区 → 加载目标状态
 */
function switchToTarget(target) {
  return loadTargetWorkspace(target, { saveCurrent: true, force: false });
}

function reloadCurrentTargetFromProject() {
  const target = loadedTargetInfo || getCurrentTarget();
  return loadTargetWorkspace(target, { saveCurrent: false, force: true });
}

function clearWorkspaceCache() {
  if (autoSaveTimer) {
    clearTimeout(autoSaveTimer);
    autoSaveTimer = null;
  }
}

function switchToActor(sceneName, actorName) {
  return switchToTarget({ targetType: 'actor', scene: sceneName, actor: actorName });
}

// 嵌入模式监听 props，独立模式监听全局选中
if (props.embedded) {
  watch(
    () => [props.sceneName, props.actorName],
    ([s, a]) => { switchToActor(s, a); },
  );
} else {
  watch(
    [currentTargetType, currentSceneName, currentActorName],
    () => { switchToTarget(getBlockTargetContext()); },
    { immediate: false },
  );
}

const initBlocklyAndGenerators = async () => {
  currentActorNameVar = props.actorName || '';

  if (!blocksRegistered) {
    const [
      { defineAudioBlocks },
      { defineCameraBlocks },
      { defineEngineBlocks },
      { defineAppearanceBlocks },
      { defineEventBlocks },
      { defineControlBlocks },
      { defineDetectBlocks },
      { defineMathBlocks },
      { defineVariableBlocks },
      { defineListBlocks },
    ] = await Promise.all([
      import('@/blockly/blocks/audio.js'),
      import('@/blockly/blocks/camera.js'),
      import('@/blockly/blocks/engine.js'),
      import('@/blockly/blocks/appearance.js'),
      import('@/blockly/blocks/event.js'),
      import('@/blockly/blocks/control.js'),
      import('@/blockly/blocks/detect.js'),
      import('@/blockly/blocks/math.js'),
      import('@/blockly/blocks/variable.js'),
      import('@/blockly/blocks/list.js'),
    ]);

    try {
      defineAudioBlocks();
      defineCameraBlocks();
      defineEngineBlocks();
      defineAppearanceBlocks();
      defineEventBlocks(broadcastList, createNewBroadcast);
      defineControlBlocks();
      defineDetectBlocks();
      defineMathBlocks();
      defineVariableBlocks();
      defineListBlocks();

      // 加载自定义 workspaceToCode（hat过滤、handler路由、prelude等）
      // 该模块内部已包含所有分类生成器的注册，无需在此重复调用 define*Generators()
      await import('@/blockly/generators/index.js');

      blocksRegistered = true;
    } catch (e) {
      logError('注册积木/生成器失败', e);
      throw e;
    }
  }
};

function blocklyMessageBundle() {
  const module = locale.value === 'en-US' ? blocklyEN : blocklyCN;
  return module?.default || module || {};
}

function applyBlocklyLocale() {
  if (!BlocklyLib) return;
  try {
    BlocklyLib.setLocale(blocklyMessageBundle());
  } catch (e) {
    logError('setLocale failed', e);
  }
}

async function refreshBlocklyLanguage() {
  if (!workspace || !BlocklyLib) return;
  applyBlocklyLocale();
  try {
    const { createToolboxConfig } = await import('@/blockly/configs/toolboxConfig.js');
    workspace.updateToolbox(createToolboxConfig(t));
    const state = BlocklyLib.serialization.workspaces.save(workspace);
    isLoadingWorkspace = true;
    workspace.clear();
    BlocklyLib.serialization.workspaces.load(state, workspace);
  } catch (e) {
    logError('刷新 Blockly 语言失败', e);
  } finally {
    isLoadingWorkspace = false;
    updateGeneratedCode();
    markOrphanBlocks();
    resizeBlockly();
  }
}

const initBlockly = async () => {
  const container = blockdiv.value;
  if (!container) {
    logError('Blockly 容器未找到');
    return false;
  }

  const modulesOk = await loadBlocklyModules();
  if (!modulesOk) return false;

  const Blockly = BlocklyLib;
  applyBlocklyLocale();

  await initBlocklyAndGenerators();

  const { createWorkspaceConfig } = await import('@/blockly/configs/workspaceConfig.js');
  const store = useStore();
  sharedStore = store;

  try {
    workspace = Blockly.inject(container, createWorkspaceConfig(t));
  } catch (e) {
    logError('Blockly.inject 失败', e);
    return false;
  }

  store.workspace.value = workspace;
  store.workspaceSvg.value = workspace.workspaceSvg || workspace;

  // 缩放比例监听
  setupScaleListener(workspace);

  // 积木键盘事件转发到 Python（必须在 setupCefFieldInputFix 之前注册）
  setupScriptKeyForwarding();
  // CEF OSR 键盘转发修复
  setupCefFieldInputFix();

  try { Blockly.ContextMenuRegistry.registry.unregister('saveWorkspace'); } catch {}
  try { Blockly.ContextMenuRegistry.registry.unregister('loadWorkspace'); } catch {}
  try { Blockly.ContextMenuRegistry.registry.unregister('copyWorkspace'); } catch {}
  try { Blockly.ContextMenuRegistry.registry.unregister('pasteWorkspace'); } catch {}

  Blockly.ContextMenuRegistry.registry.register({
    displayText: () => t('blockly.saveWorkspace'),
    preconditionFn: () => workspace && workspace.getAllBlocks(false).length > 0 ? 'enabled' : 'disabled',
    callback: async () => {
      const data = Blockly.serialization.workspaces.save(workspace);
      const jsonStr = JSON.stringify(data, null, 2);
      const blob = new Blob([jsonStr], { type: 'application/json' });
      const suggestedName = getExportFilename();
      try {
        const opts = {
          types: [{ description: t('blockly.blocklyProjectFile'), accept: { 'application/json': ['.blockly'] } }],
          suggestedName,
        };
        const handle = await window.showSaveFilePicker(opts);
        const writable = await handle.createWritable();
        await writable.write(blob);
        await writable.close();
        showToast(`✅ ${t('blockly.exportSuccess')}`, 'success');
      } catch {
        // 用户取消或 API 不可用 → 回退到下载链接
        fallbackDownload(jsonStr, suggestedName);
      }
    },
    scopeType: Blockly.ContextMenuRegistry.ScopeType.WORKSPACE,
    id: 'saveWorkspace',
    weight: 1,
  });

  Blockly.ContextMenuRegistry.registry.register({
    displayText: () => t('blockly.importBlocks'),
    preconditionFn: () => workspace ? 'enabled' : 'disabled',
    callback: () => {
      triggerFileImport();
    },
    scopeType: Blockly.ContextMenuRegistry.ScopeType.WORKSPACE,
    id: 'loadWorkspace',
    weight: 2,
  });

  Blockly.ContextMenuRegistry.registry.register({
    displayText: () => t('blockly.copyBlocks'),
    preconditionFn: () => workspace && workspace.getAllBlocks(false).length > 0 ? 'enabled' : 'disabled',
    callback: async () => {
      try {
        const data = Blockly.serialization.workspaces.save(workspace);
        const jsonStr = JSON.stringify(data);
        await navigator.clipboard.writeText(jsonStr);
        showToast(`📋 ${t('blockly.copiedToClipboard')}`, 'success');
      } catch (e) {
        logError('复制积木失败', e);
        showToast(`❌ ${t('blockly.copyFailed')}`, 'error');
      }
    },
    scopeType: Blockly.ContextMenuRegistry.ScopeType.WORKSPACE,
    id: 'copyWorkspace',
    weight: 3,
  });

  Blockly.ContextMenuRegistry.registry.register({
    displayText: () => t('blockly.pasteBlocks'),
    preconditionFn: () => 'enabled',
    callback: async () => {
      try {
        const text = await navigator.clipboard.readText();
        if (!text || !text.trim()) {
          showToast(`⚠️ ${t('blockly.clipboardEmpty')}`, 'warn');
          return;
        }
        let json;
        try {
          json = JSON.parse(text);
        } catch {
          showToast(`❌ ${t('blockly.invalidClipboard')}`, 'error');
          return;
        }
        if (!json || typeof json !== 'object') {
          showToast(`❌ ${t('blockly.invalidWorkspaceData')}`, 'error');
          return;
        }
        const hasBlocks = workspace.getAllBlocks(false).length > 0;
        if (hasBlocks && !confirm(t('blockly.pasteConfirm'))) {
          return;
        }
        Blockly.serialization.workspaces.load(json, workspace);
        showToast(`✅ ${t('blockly.pasteSuccess')}`, 'success');
      } catch (e) {
        logError('粘贴积木失败', e);
        showToast(`❌ ${t('blockly.pasteFailed')}`, 'error');
      }
    },
    scopeType: Blockly.ContextMenuRegistry.ScopeType.WORKSPACE,
    id: 'pasteWorkspace',
    weight: 4,
  });

  setupKeyboardShortcuts();

  const { pythonGenerator: pyGen } = await import('blockly/python');
  pythonGenerator = pyGen;
  updateGeneratedCode();

  workspace.addChangeListener(onWorkspaceChange);

  hideOverlay();
  resizeBlockly();
  return true;
};

// ============================================================
// 导入/导出工具函数
// ============================================================

/** 显示短暂的提示消息 */
let toastTimer = null;
function showToast(message, type = 'info') {
  const toast = document.getElementById('import-toast');
  if (!toast) return;
  if (toastTimer) clearTimeout(toastTimer);
  const colors = {
    success: 'bg-green-700 text-green-100',
    error: 'bg-red-700 text-red-100',
    warn: 'bg-yellow-700 text-yellow-100',
    info: 'bg-blue-700 text-blue-100',
  };
  toast.className = colors[type] || colors.info;
  toast.textContent = message;
  toast.style.opacity = '1';
  toastTimer = setTimeout(() => {
    toast.style.opacity = '0';
    toastTimer = null;
  }, 2500);
}

/** 生成导出文件名（包含 Actor 和场景信息） */
function getExportFilename() {
  const target = loadedTargetInfo || getCurrentTarget();
  const ts = new Date().toISOString().slice(0, 10);
  if (target.targetType === 'project') return `project_global_${ts}.blockly`;
  const scenePart = target.scene || 'unknown';
  const actorPart = target.actor || 'unknown';
  return `${actorPart}_${scenePart}_${ts}.blockly`;
}

/** 回退下载方案（showSaveFilePicker 不可用时） */
function fallbackDownload(jsonStr, filename) {
  const blob = new Blob([jsonStr], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  setTimeout(() => URL.revokeObjectURL(url), 150);
  showToast(`✅ ${t('blockly.exportSuccess')}`, 'success');
}

/** 工具栏导出按钮处理 */
async function handleToolbarExport() {
  if (!workspace || !BlocklyLib) return;
  if (workspace.getAllBlocks(false).length === 0) {
    showToast(`⚠️ ${t('blockly.emptyWorkspaceExport')}`, 'warn');
    return;
  }
  const data = BlocklyLib.serialization.workspaces.save(workspace);
  const jsonStr = JSON.stringify(data, null, 2);
  const blob = new Blob([jsonStr], { type: 'application/json' });
  const suggestedName = getExportFilename();
  if (window.showSaveFilePicker) {
    try {
      const opts = {
        types: [{ description: t('blockly.blocklyProjectFile'), accept: { 'application/json': ['.blockly'] } }],
        suggestedName,
      };
      const handle = await window.showSaveFilePicker(opts);
      const writable = await handle.createWritable();
      await writable.write(blob);
      await writable.close();
      showToast(`✅ ${t('blockly.exportSuccess')}`, 'success');
    } catch {
      fallbackDownload(jsonStr, suggestedName);
    }
  } else {
    fallbackDownload(jsonStr, suggestedName);
  }
}

/** 触发文件选择器导入 */
function triggerFileImport() {
  if (fileImportInput.value) {
    fileImportInput.value.value = ''; // 清除旧值，确保重复选择同一文件能触发 change
    fileImportInput.value.click();
  } else {
    // 回退：动态创建 input
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = '.blockly,.json';
    input.addEventListener('change', (e) => handleFileImportChange(e));
    input.click();
  }
}

/** 文件选择器 change 事件处理 */
function handleFileImportChange(event) {
  const file = event.target?.files?.[0];
  if (!file) return;
  importWorkspaceFromFileObject(file);
}

/** 从 File 对象导入积木 */
function importWorkspaceFromFileObject(file) {
  if (!workspace) {
    showToast(`❌ ${t('blockly.workspaceNotReady')}`, 'error');
    return;
  }
  const ext = (file.name || '').split('.').pop()?.toLowerCase();
  if (ext !== 'blockly' && ext !== 'json') {
    showToast(`⚠️ ${t('blockly.unsupportedFile')}`, 'warn');
    return;
  }
  const reader = new FileReader();
  reader.addEventListener('load', function () {
    let text = this.result;
    // 移除 BOM
    if (text.charCodeAt(0) === 0xFEFF) text = text.slice(1);

    let json;
    try {
      json = JSON.parse(text);
    } catch (e) {
      showToast(`❌ ${t('blockly.invalidJson', { message: e.message || t('blockly.codeRunUnknownError') })}`, 'error');
      return;
    }

    if (!json || typeof json !== 'object') {
      showToast(`❌ ${t('blockly.invalidFileFormat')}`, 'error');
      return;
    }

    // 确认覆盖
    const hasBlocks = workspace.getAllBlocks(false).length > 0;
    if (hasBlocks && !confirm(t('blockly.replaceConfirm'))) {
      return;
    }

    try {
      BlocklyLib.serialization.workspaces.load(json, workspace);
      showToast(`✅ ${t('blockly.importSuccess', { name: file.name })}`, 'success');
    } catch (e) {
      showToast(`❌ ${t('blockly.workspaceLoadFailed', { message: e.message || t('blockly.codeRunUnknownError') })}`, 'error');
    }
  });
  reader.addEventListener('error', () => {
    showToast(`❌ ${t('blockly.fileReadFailed')}`, 'error');
  });
  reader.readAsText(file, 'UTF-8');
}

// ============================================================
// 键盘快捷键：Ctrl+S 导出、Ctrl+O 导入
// ============================================================
let kbShortcutHandler = null;

function setupKeyboardShortcuts() {
  const injectionDiv = workspace?.workspaceSvg?.injectionDiv
    || (workspace?.injectionDiv)
    || document.querySelector('.injectionDiv');
  if (!injectionDiv) return;

  teardownKeyboardShortcuts();

  kbShortcutHandler = (e) => {
    // 跳过文本输入框
    const tag = document.activeElement?.tagName?.toLowerCase();
    if (tag === 'input' || tag === 'textarea' || tag === 'select') return;

    const mod = e.ctrlKey || e.metaKey;

    if (mod && e.key === 's') {
      e.preventDefault();
      e.stopPropagation();
      // 导出当前工作区
      if (!workspace || workspace.getAllBlocks(false).length === 0) {
        showToast(`⚠️ ${t('blockly.emptyWorkspaceExport')}`, 'warn');
        return;
      }
      const data = BlocklyLib.serialization.workspaces.save(workspace);
      const jsonStr = JSON.stringify(data, null, 2);
      const suggestedName = getExportFilename();
      if (window.showSaveFilePicker) {
        const blob = new Blob([jsonStr], { type: 'application/json' });
        const opts = {
          types: [{ description: t('blockly.blocklyProjectFile'), accept: { 'application/json': ['.blockly'] } }],
          suggestedName,
        };
        window.showSaveFilePicker(opts).then(async (handle) => {
          const writable = await handle.createWritable();
          await writable.write(blob);
          await writable.close();
          showToast(`✅ ${t('blockly.exportSuccess')}`, 'success');
        }).catch(() => {
          fallbackDownload(jsonStr, suggestedName);
        });
      } else {
        fallbackDownload(jsonStr, suggestedName);
      }
    }

    if (mod && e.key === 'o') {
      e.preventDefault();
      e.stopPropagation();
      triggerFileImport();
    }
  };

  injectionDiv.addEventListener('keydown', kbShortcutHandler, true);
}

function teardownKeyboardShortcuts() {
  if (kbShortcutHandler) {
    const injectionDiv = document.querySelector('.injectionDiv');
    if (injectionDiv) {
      injectionDiv.removeEventListener('keydown', kbShortcutHandler, true);
    }
    kbShortcutHandler = null;
  }
}

// ============================================================
// 拖拽导入 .blockly 文件
// ============================================================
let dragEnterCount = 0;

function setupDragDrop() {
  const container = blockdiv.value;
  if (!container) return;
  container.addEventListener('dragover', handleDragOver);
  container.addEventListener('dragenter', handleDragEnter);
  container.addEventListener('dragleave', handleDragLeave);
  container.addEventListener('drop', handleDrop);
}

function teardownDragDrop() {
  const container = blockdiv.value;
  if (!container) return;
  container.removeEventListener('dragover', handleDragOver);
  container.removeEventListener('dragenter', handleDragEnter);
  container.removeEventListener('dragleave', handleDragLeave);
  container.removeEventListener('drop', handleDrop);
}

function handleDragOver(e) {
  e.preventDefault();
  if (e.dataTransfer) e.dataTransfer.dropEffect = 'copy';
}

function handleDragEnter(e) {
  e.preventDefault();
  dragEnterCount++;
  const overlay = document.getElementById('drag-overlay');
  if (overlay) overlay.style.display = 'flex';
}

function handleDragLeave(e) {
  dragEnterCount--;
  if (dragEnterCount <= 0) {
    dragEnterCount = 0;
    const overlay = document.getElementById('drag-overlay');
    if (overlay) overlay.style.display = 'none';
  }
}

function handleDrop(e) {
  e.preventDefault();
  dragEnterCount = 0;
  const overlay = document.getElementById('drag-overlay');
  if (overlay) overlay.style.display = 'none';

  const file = e.dataTransfer?.files?.[0];
  if (!file) return;
  importWorkspaceFromFileObject(file);
}

const hideOverlay = () => {
  const overlay = document.getElementById('status-overlay');
  if (overlay) overlay.style.display = 'none';
};

const resizeBlockly = () => {
  try {
    if (!workspace) return;
    BlocklyLib && BlocklyLib.svgResize(workspace);
    emit('resize');
  } catch {}
};

const handleClose = async () => {
  if (closeDockPanel) { closeDockPanel(); return; }
};

/** 清空工作区 */
function handleClear() {
  if (!workspace || !BlocklyLib) return;
  if (workspace.getAllBlocks(false).length === 0) {
    showToast(`⚠️ ${t('blockly.workspaceEmpty')}`, 'warn');
    return;
  }
  if (!confirm(t('blockly.clearConfirm'))) return;

  flushAutoSave();
  isLoadingWorkspace = true;
  try {
    workspace.clear();
    showToast(`✅ ${t('blockly.workspaceCleared')}`, 'success');
  } catch (e) {
    logError('清空工作区失败', e);
    showToast(`❌ ${t('blockly.clear')} ${t('common.error')}`, 'error');
  } finally {
    isLoadingWorkspace = false;
  }
  updateGeneratedCode();
  saveCurrentWorkspace();
}

// ============================================================
// 缩放功能
// ============================================================
const scaleText = ref('100%');

function updateScale() {
  const ws = store.workspace.value;
  if (ws) {
    scaleText.value = Math.floor((ws.scale || 1.0) * 100) + '%';
  }
}

function handleZoomIn() {
  const ws = store.workspace.value;
  if (ws) { ws.zoom(0, 0, 0.15); updateScale(); }
}

function handleZoomOut() {
  const ws = store.workspace.value;
  if (ws) { ws.zoom(0, 0, -0.15); updateScale(); }
}

function handleZoomReset() {
  const ws = store.workspace.value;
  if (ws) { ws.zoomTo(0, 0, 1.0); updateScale(); }
}

// 缩放变化监听器引用
let scaleChangeListener = null;

function setupScaleListener(ws) {
  teardownScaleListener();
  if (!ws) return;
  updateScale();
  scaleChangeListener = () => updateScale();
  ws.addChangeListener(scaleChangeListener);
}

function teardownScaleListener() {
  if (scaleChangeListener) {
    const ws = store.workspace.value;
    if (ws) {
      try { ws.removeChangeListener(scaleChangeListener); } catch {}
    }
    scaleChangeListener = null;
  }
}

const handleWindowResize = () => resizeBlockly();

let resizeObserver = null;

const blocklyWorkspaceApi = {
  flushSave: () => flushAutoSave(),
  reloadFromProject: () => reloadCurrentTargetFromProject(),
  clearCache: () => clearWorkspaceCache(),
};

watch(locale, () => {
  refreshBlocklyLanguage();
});

onMounted(async () => {
  window.addEventListener('resize', handleWindowResize);

  if (blockdiv.value) {
    resizeObserver = new ResizeObserver(() => resizeBlockly());
    resizeObserver.observe(blockdiv.value);
    setupDragDrop();
  }

  try {
    await initBlockly();
    if (props.embedded) {
      await switchToActor(props.sceneName, props.actorName);
    } else {
      syncActorContextFromStorage();
      await switchToTarget(getBlockTargetContext());
    }
    registerBlocklyWorkspace(blocklyWorkspaceApi);
  } catch (err) {
    logError('初始化失败', err);
    const overlay = document.getElementById('status-overlay');
    if (overlay) {
      overlay.innerHTML = `<div class="text-center"><div class="text-xl font-bold text-red-400 mb-2">${t('common.error')}</div><div class="text-sm text-gray-400">${t('blockly.loadFailed')}</div></div>`;
    }
  }
});

onUnmounted(async () => {
  // 1) 标记组件已卸载（阻止后续 resize/keyboard 回调操作已销毁对象）
  let disposed = false;
  const isAlive = () => !disposed && workspace !== null;
  const safeResize = () => { if (isAlive() && BlocklyLib) { try { BlocklyLib.svgResize(workspace); } catch {} } };

  window.removeEventListener('resize', handleWindowResize);
  if (resizeObserver) { resizeObserver.disconnect(); resizeObserver = null; }
  // 清理拖拽导入
  teardownDragDrop();
  // 清理缩放监听
  teardownScaleListener();
  // 清理键盘快捷键
  teardownKeyboardShortcuts();
  // 清理 CEF 键盘转发
  teardownCefFieldInputFix();
  // 清理积木键盘转发
  teardownScriptKeyForwarding();
  // 清理状态轮询
  clearPollTimer();
  unregisterBlocklyWorkspace(blocklyWorkspaceApi);
  // 清理自动保存定时器
  if (autoSaveTimer) { clearTimeout(autoSaveTimer); autoSaveTimer = null; }
  // 清理提示定时器
  if (toastTimer) { clearTimeout(toastTimer); toastTimer = null; }

  // ★ 关键修复：销毁前等待最后的保存完成，防止数据丢失
  if (workspace) {
    try { await flushAutoSave(); } catch {}
    try { await latestBlocklySavePromise; } catch {}
  }

  disposed = true;

  if (workspace) {
    // 移除 change listener 防止 dispose 过程中的回调
    try { workspace.removeChangeListener(onWorkspaceChange); } catch {}
    try { workspace.dispose(); } catch {}
    workspace = null;
  }
  // 清理共享 store 中的 workspace 引用，防止其他组件持有已销毁的对象
  if (sharedStore) {
    sharedStore.workspace.value = null;
    sharedStore.workspaceSvg.value = null;
  }
  // 所有工作区实例都已卸载时，重置共享的模块级状态
  // 使用 mountedBlocklyWorkspaces 的大小作为引用计数（此时当前实例已通过 unregister 移除）
  if (mountedBlocklyWorkspaces.size === 0) {
    blocksRegistered = false;
    pythonGenerator = null;
  }
  generatedCode.value = '';
});

defineExpose({
  resize: resizeBlockly,
});
</script>

<style scoped>
/* ── 工作区工具栏 ── */
.toolbar-btn {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 3px 8px;
  font-size: 11px;
  color: #999;
  background: transparent;
  border: 1px solid transparent;
  border-radius: 4px;
  cursor: pointer;
  transition: all 0.15s ease;
  line-height: 1.4;
  white-space: nowrap;
  user-select: none;
}

.toolbar-btn:hover {
  color: #e0e0e0;
  background: rgba(255,255,255,0.06);
  border-color: rgba(255,255,255,0.12);
}

.toolbar-btn:active {
  background: rgba(255,255,255,0.1);
  transform: scale(0.97);
}

.toolbar-btn-active {
  color: #f0c060 !important;
  background: rgba(240,192,96,0.1) !important;
  border-color: rgba(240,192,96,0.3) !important;
}

.toolbar-btn-active:hover {
  background: rgba(240,192,96,0.18) !important;
  border-color: rgba(240,192,96,0.5) !important;
}

.toolbar-icon {
  width: 14px;
  height: 14px;
  flex-shrink: 0;
  opacity: 0.75;
}

.toolbar-btn:hover .toolbar-icon {
  opacity: 1;
}

.toolbar-label {
  font-weight: 500;
  letter-spacing: 0.02em;
}
</style>
