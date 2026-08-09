<template>
  <div
    class="cabbage-chat-shell flex h-full min-h-0 w-full flex-1 flex-col overflow-hidden rounded-lg relative"
    :class="{ resident: props.resident }"
  >
    <DockTitleBar
      v-if="!props.resident && !isDocked"
      title="AI 创作助手"
      routePath="/CabbageChat"
      @close="closeFloat"
    />

    <div v-if="props.resident" class="resident-chat-title">
      <span>AI 创作助手</span>
      <button
        type="button"
        class="resident-float-button"
        :title="'弹出为可拖动窗口'"
        :disabled="assistant.chatBusy"
        @click.stop="emit('detach')"
      >
        &#x29C9;
      </button>
    </div>

    <div class="context-strip">
      <div class="context-title">当前任务</div>
      <select v-model="selectedKey" :disabled="!contextTasks.length" class="context-select">
        <option value="">
          {{ assistant.tasks.length ? '全部待处理任务' : '当前没有待处理任务' }}
        </option>
        <option
          v-for="task in contextTasks"
          :key="task.taskKey || task.issueKey"
          :value="task.taskKey || task.issueKey"
        >
          {{ localizedTaskField(task, 'title') }}
        </option>
      </select>
      <span class="context-count">{{ contextTasks.length }}</span>
    </div>

    <div ref="historyRef" class="chat-history">
      <div v-if="!assistant.messages.length" class="chat-empty">
        <strong>开始使用 AI 创作助手</strong>
        <p>可以询问当前任务、让 AI 修改节点图，或根据你的描述生成节点逻辑。</p>
      </div>
      <article
        v-for="message in assistant.messages"
        :key="message.id"
        class="chat-message"
        :class="message.role"
      >
        <div class="chat-role">{{ message.role === 'assistant' ? 'AI' : '你' }}</div>
        <div class="chat-content">
          <div>
            {{
              message.role === 'assistant'
                ? localizedAssistantText(message.content)
                : message.content
            }}
          </div>
          <ol v-if="message.role === 'assistant' && message.steps?.length" class="guidance-steps">
            <li v-for="(item, index) in message.steps" :key="`${message.id}_step_${index}`">
              {{ item }}
            </li>
          </ol>
          <button
            v-if="message.role === 'assistant' && message.needsShowcase && message.guidanceIntent"
            type="button"
            class="message-showcase"
            @click="showcaseMessage(message)"
          >
            展示
          </button>
        </div>
      </article>
      <article v-if="streamingContent" class="chat-message assistant streaming">
        <div class="chat-role">AI</div>
        <div class="chat-content">{{ cleanedStreamingContent }}</div>
      </article>
      <div v-if="assistant.chatBusy && !streamingContent" class="chat-pending">
        AI 正在查看当前世界与任务…
      </div>
    </div>

    <div v-if="assistant.chatError" class="chat-error">{{ assistant.chatError }}</div>

    <form class="chat-composer" @submit.prevent="sendMessage">
      <textarea
        ref="composerInputRef"
        v-model="input"
        data-guidance="cabbage-chat-input"
        rows="3"
        maxlength="2000"
        @pointerdown="recordComposerPointerDown"
        @keydown.enter.exact.prevent="sendMessage"
      />
      <div class="composer-actions">
        <button
          type="button"
          class="secondary"
          :disabled="assistant.chatBusy || !assistant.messages.length"
          @click="assistant.clearChat()"
        >
          清空会话
        </button>
        <button v-if="assistant.chatBusy" type="button" class="danger" @click="stopWaiting">
          停止等待
        </button>
        <button v-else type="submit" class="primary" data-guidance="cabbage-chat-send" :disabled="!input.trim()">发送</button>
      </div>
    </form>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { useI18n } from 'vue-i18n';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import { useDockPanel } from '@/composables/useDockPanel.js';
import { useCabbageAssistantStore } from '@/stores/cabbageAssistantStore.js';
import { aiService } from '@/services/aiService.js';
import { reviewScopeId } from '@/services/nodeGraphReviewService.js';
import { guidanceService } from '@/services/cabbageGuidanceService.js';
import { translateUiText } from '@/i18n/domTranslator.js';
import {
  cancelActiveNodeGraphGeneration,
  generateNodeGraphFromInstruction,
  nodeGraphGenerationIntent,
} from '@/services/nodeGraphGenerationService.js';
import {
  cabbageContextService,
  publishCabbageAssistantContext,
  subscribeCabbageAssistantContext,
} from '@/services/cabbageAssistantContextService.js';

const props = defineProps({
  resident: { type: Boolean, default: false },
});

const emit = defineEmits(['detach']);

const assistant = useCabbageAssistantStore();
const { locale } = useI18n();
const { closePanel, isDocked } = useDockPanel();
const input = ref('');
const historyRef = ref(null);
const composerInputRef = ref(null);
const streamingContent = ref('');
const activeTaskId = ref('');
const activeRequestKind = ref('');
let requestSequence = 0;
let pollTimer = null;
let unsubscribeAssistantContext = null;
let activeMessageContext = null;
let activePrompt = '';

const selectedKey = computed({
  get: () => assistant.selectedTaskKey,
  set: (value) => {
    assistant.selectTask(value);
    if (assistant.selectedTask?.transient !== true) publishCabbageAssistantContext(assistant);
  },
});

function localizedTaskField(task, field) {
  const source = String(task?.[field] || '');
  if (locale.value !== 'en-US') return source;
  const english = String(task?.[`${field}En`] || '').trim();
  return english || translateUiText(source);
}

const contextTasks = computed(() => {
  const tasks = assistant.tasks.map((task) => ({ ...task, __history: false }));
  const selected = assistant.completedTasks.find(
    (task) => task.taskKey === assistant.selectedTaskKey
  );
  if (selected && !tasks.some((task) => task.taskKey === selected.taskKey)) {
    tasks.push({ ...selected, __history: true });
  }
  const selectedWarning =
    assistant.preWarning?.taskKey === assistant.selectedTaskKey ? assistant.preWarning : null;
  if (selectedWarning && !tasks.some((task) => task.taskKey === selectedWarning.taskKey)) {
    tasks.unshift({ ...selectedWarning, __transient: true });
  }
  return tasks;
});

function cleanAssistantText(value = '') {
  return String(value || '')
    .replace(/^\s*```[^\n]*$/gm, '')
    .replace(/^\s*#{1,6}\s+/gm, '')
    .replace(/^\s*>\s?/gm, '')
    .replace(/^\s*[-*_]{3,}\s*$/gm, '')
    .replace(/\[([^\]]+)\]\([^\s)]+\)/g, '$1')
    .replace(/\*\*|__/g, '')
    .replace(/`([^`\n]+)`/g, '$1')
    .replace(/^\s*[-*+]\s+/gm, '')
    .replace(/[ \t]+$/gm, '')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function localizedAssistantText(value = '') {
  return cleanAssistantText(value)
    .split('\n')
    .map((line) => translateUiText(line))
    .join('\n');
}

const cleanedStreamingContent = computed(() => localizedAssistantText(streamingContent.value));

const NODE_GRAPH_OPERATION_COPY = Object.freeze({
  create: {
    progress:
      'AI \u6b63\u5728\u8bfb\u53d6\u79ef\u6728\u6587\u6863\u5e76\u751f\u6210\u5f53\u524d\u8282\u70b9\u903b\u8f91\u2026',
    success: '\u8282\u70b9\u903b\u8f91\u5df2\u7ecf\u751f\u6210\u5e76\u4fdd\u5b58\u3002',
    failure: '\u8282\u70b9\u903b\u8f91\u751f\u6210\u5931\u8d25\u3002',
  },
  extend: {
    progress:
      'AI \u6b63\u5728\u8bfb\u53d6\u73b0\u6709\u8282\u70b9\u5e76\u8865\u5145\u903b\u8f91\u2026',
    success:
      '\u5df2\u5728\u73b0\u6709\u8282\u70b9\u56fe\u4e2d\u8865\u5145\u5e76\u4fdd\u5b58\u6240\u9700\u903b\u8f91\u3002',
    failure: '\u8282\u70b9\u903b\u8f91\u8865\u5145\u5931\u8d25\u3002',
  },
  edit: {
    progress:
      'AI \u6b63\u5728\u8bfb\u53d6\u73b0\u6709\u8282\u70b9\u5e76\u8fdb\u884c\u5c40\u90e8\u4fee\u6539\u2026',
    success:
      '\u5df2\u4fdd\u7559\u65e0\u5173\u903b\u8f91\u5e76\u5b8c\u6210\u5c40\u90e8\u4fee\u6539\u3002',
    failure: '\u8282\u70b9\u903b\u8f91\u4fee\u6539\u5931\u8d25\u3002',
  },
  delete: {
    progress:
      'AI \u6b63\u5728\u5b9a\u4f4d\u5e76\u5220\u9664\u6307\u5b9a\u8282\u70b9\u903b\u8f91\u2026',
    success: '\u5df2\u5220\u9664\u6307\u5b9a\u903b\u8f91\u5e76\u4fdd\u5b58\u8282\u70b9\u56fe\u3002',
    failure: '\u8282\u70b9\u903b\u8f91\u5220\u9664\u5931\u8d25\u3002',
  },
});

function nodeGraphOperationCopy(operation) {
  return NODE_GRAPH_OPERATION_COPY[operation] || NODE_GRAPH_OPERATION_COPY.create;
}

function scrollToBottom() {
  nextTick(() => {
    if (historyRef.value) historyRef.value.scrollTop = historyRef.value.scrollHeight;
  });
}

function recordComposerPointerDown(event) {
  if (Number(event?.button ?? 0) !== 0) return;
  void cabbageContextService.recordEvent({
    type: 'ai_composer_focused',
    category: 'tutorial',
    success: true,
    details: { source: 'user' },
  });
}

function clearPollTimer() {
  if (pollTimer) window.clearTimeout(pollTimer);
  pollTimer = null;
}

function finishRequest(
  requestId,
  { error = '', keepPartial = false, needsShowcase = false, guidanceIntent = '', steps = [] } = {}
) {
  if (assistant.activeRequestId !== requestId) return;
  clearPollTimer();
  const completedContent = cleanAssistantText(streamingContent.value);
  if (keepPartial && completedContent) {
    const message = assistant.appendMessage({
      role: 'assistant',
      content: completedContent,
      ...(activeMessageContext || {}),
      needsShowcase: needsShowcase === true,
      guidanceIntent: String(guidanceIntent || ''),
      steps: Array.isArray(steps) ? steps : [],
    });
    if (message) void cabbageContextService.appendMessage(message);
  }
  streamingContent.value = '';
  activeTaskId.value = '';
  activeRequestKind.value = '';
  assistant.activeRequestId = '';
  assistant.chatBusy = false;
  assistant.chatError = error;
  activeMessageContext = null;
  activePrompt = '';
  scrollToBottom();
}

function scheduleStatusPoll(requestId, taskId, delay = 320) {
  clearPollTimer();
  pollTimer = window.setTimeout(() => pollStatus(requestId, taskId), delay);
}

async function pollStatus(requestId, taskId) {
  if (assistant.activeRequestId !== requestId || activeTaskId.value !== taskId) return;
  try {
    const response = await aiService.getNodeGraphReviewChatStatus(taskId);
    if (assistant.activeRequestId !== requestId || activeTaskId.value !== taskId) return;
    if (response?.success !== true) {
      finishRequest(requestId, {
        error: String(response?.message || 'AI 创作助手暂时不可用，请稍后再试。'),
      });
      return;
    }

    streamingContent.value = String(response?.content || '');
    scrollToBottom();
    if (response.status === 'completed') {
      if (!streamingContent.value.trim()) {
        finishRequest(requestId, { error: 'DeepSeek 没有返回可显示的内容。' });
        return;
      }
      void cabbageContextService.recordEvent({
        type: 'ai_question_answered',
        category: 'tutorial',
        success: true,
        details: {
          prompt: activePrompt,
          mode: 'ask',
          responseReceived: true,
        },
      });
      finishRequest(requestId, {
        keepPartial: true,
        needsShowcase: response?.needsShowcase === true,
        guidanceIntent: String(response?.guidanceIntent || ''),
        steps: Array.isArray(response?.steps) ? response.steps : [],
      });
      return;
    }
    if (response.status === 'cancelled') {
      finishRequest(requestId, { error: '已停止等待本次回答。', keepPartial: true });
      return;
    }
    if (response.status === 'error') {
      finishRequest(requestId, {
        error: String(response?.message || 'AI 创作助手暂时不可用，请稍后再试。'),
      });
      return;
    }
    scheduleStatusPoll(requestId, taskId);
  } catch (error) {
    if (assistant.activeRequestId === requestId) {
      finishRequest(requestId, {
        error: String(error?.message || 'AI 创作助手暂时不可用，请稍后再试。'),
      });
    }
  }
}

const DETAIL_GUIDANCE_PATTERN =
  /(不理解|看不懂|不会|怎么做|怎么连接|放在哪里|为什么还是不行|一步一步|具体步骤|展示|演示)/i;

function requestsDetailedGuidance(content = '') {
  return DETAIL_GUIDANCE_PATTERN.test(String(content || ''));
}

function showcaseMessage(message) {
  if (!message?.needsShowcase || !message?.guidanceIntent) return;
  void guidanceService.start({
    sourceType: 'chat',
    title: '操作展示',
    guidanceIntent: message.guidanceIntent,
  });
}

async function sendMessage() {
  const content = input.value.trim();
  if (!content || assistant.chatBusy) return;
  input.value = '';
  activePrompt = content;
  const selectedTask = assistant.selectedTask;
  const messageContext = {
    taskKey: String(selectedTask?.taskKey || ''),
    issueCode: selectedTask?.type === 'node-issue' ? String(selectedTask?.code || '') : '',
    nodeId: String(selectedTask?.nodeId || ''),
    blockId: String(selectedTask?.blockId || ''),
  };
  activeMessageContext = messageContext;
  const userMessage = assistant.appendMessage({ role: 'user', content, ...messageContext });
  if (userMessage) void cabbageContextService.appendMessage(userMessage);
  assistant.chatError = '';
  assistant.chatBusy = true;
  streamingContent.value = '';
  const requestId = `cabbage_chat_${Date.now()}_${++requestSequence}`;
  assistant.activeRequestId = requestId;
  scrollToBottom();

  const generationIntent = nodeGraphGenerationIntent(content);
  if (generationIntent.matched) {
    activeRequestKind.value = 'generation';
    const operationCopy = nodeGraphOperationCopy(generationIntent.operation);
    // The progress sentence is UI-only and is never persisted as chat history.
    streamingContent.value = operationCopy.progress;
    try {
      const generated = await generateNodeGraphFromInstruction(content, generationIntent.operation);
      if (assistant.activeRequestId !== requestId || activeRequestKind.value !== 'generation')
        return;
      streamingContent.value = String(generated.summary || operationCopy.success);
      void cabbageContextService.recordEvent({
        type: 'ai_node_graph_changed',
        category: 'tutorial',
        success: true,
        details: {
          prompt: activePrompt,
          mode: generationIntent.operation === 'edit' ? 'modify' : 'generate',
          operation: generationIntent.operation,
          applied: true,
          createdNodeIds: Array.isArray(generated.createdNodeIds) ? generated.createdNodeIds : [],
          createdEdgeIds: Array.isArray(generated.createdEdgeIds) ? generated.createdEdgeIds : [],
        },
      });
      finishRequest(requestId, { keepPartial: true });
    } catch (error) {
      if (assistant.activeRequestId === requestId) {
        const reason = String(error?.message || operationCopy.failure);
        const unchanged = reason.includes('\u6ca1\u6709\u88ab\u4fee\u6539')
          ? reason
          : `${reason} \u5f53\u524d\u8282\u70b9\u56fe\u6ca1\u6709\u88ab\u4fee\u6539\u3002`;
        finishRequest(requestId, { error: unchanged });
      }
    }
    return;
  }

  activeRequestKind.value = 'chat';
  try {
    const response = await aiService.startNodeGraphReviewChat({
      requestId,
      locale: locale.value,
      worldId: assistant.worldId,
      projectScopeId: assistant.projectScopeId,
      graphRevision: assistant.graphRevision,
      assistanceProfile: assistant.assistanceProfile,
      selectedTaskKey: messageContext.taskKey,
      tasks: contextTasks.value,
      graphExcerpt: assistant.graphExcerpt,
      projectContext: assistant.projectContext,
      messages: assistant.messages.map(({ role, content: text }) => ({ role, content: text })),
      detailGuidanceRequested: requestsDetailedGuidance(content),
    });
    if (assistant.activeRequestId !== requestId) return;
    if (response?.success !== true || !String(response?.taskId || '').trim()) {
      finishRequest(requestId, {
        error: String(
          response?.message ||
            'AI \u521b\u4f5c\u52a9\u624b\u6682\u65f6\u4e0d\u53ef\u7528\uff0c\u8bf7\u7a0d\u540e\u518d\u8bd5\u3002'
        ),
      });
      return;
    }
    activeTaskId.value = String(response.taskId);
    scheduleStatusPoll(requestId, activeTaskId.value, 0);
  } catch (error) {
    if (assistant.activeRequestId === requestId) {
      finishRequest(requestId, {
        error: String(
          error?.message ||
            'AI \u521b\u4f5c\u52a9\u624b\u6682\u65f6\u4e0d\u53ef\u7528\uff0c\u8bf7\u7a0d\u540e\u518d\u8bd5\u3002'
        ),
      });
    }
  }
}

async function stopWaiting() {
  const requestId = assistant.activeRequestId;
  const taskId = activeTaskId.value;
  const requestKind = activeRequestKind.value;
  if (!requestId) return;
  const partial = requestKind === 'chat' ? cleanAssistantText(streamingContent.value) : '';
  clearPollTimer();
  assistant.activeRequestId = '';
  activeTaskId.value = '';
  activeRequestKind.value = '';
  streamingContent.value = '';
  assistant.chatBusy = false;
  assistant.chatError = '\u5df2\u505c\u6b62\u7b49\u5f85\u672c\u6b21\u56de\u7b54\u3002';
  if (partial) {
    const message = assistant.appendMessage({
      role: 'assistant',
      content: partial,
      ...(activeMessageContext || {}),
    });
    if (message) void cabbageContextService.appendMessage(message);
  }
  activeMessageContext = null;
  activePrompt = '';
  if (requestKind === 'generation') {
    await cancelActiveNodeGraphGeneration();
  } else if (taskId) {
    try {
      await aiService.cancelNodeGraphReviewChat(taskId);
    } catch (_) {}
  }
  scrollToBottom();
}

function closeFloat() {
  closePanel();
}

function resetChatForProjectChange() {
  const taskId = activeTaskId.value;
  const requestKind = activeRequestKind.value;
  clearPollTimer();
  assistant.activeRequestId = '';
  activeTaskId.value = '';
  activeRequestKind.value = '';
  streamingContent.value = '';
  assistant.chatBusy = false;
  assistant.chatError = '';
  activeMessageContext = null;
  activePrompt = '';
  if (requestKind === 'generation') cancelActiveNodeGraphGeneration().catch(() => {});
  else if (taskId) aiService.cancelNodeGraphReviewChat(taskId).catch(() => {});
}

function resetChatForProjectStorageChange(event) {
  if (event?.key === 'corona.activeProjectPath') resetChatForProjectChange();
}

function focusResidentComposer() {
  if (!props.resident) return;
  nextTick(() => {
    const composer = composerInputRef.value;
    if (!composer) return;
    try {
      composer.focus({ preventScroll: true });
    } catch (_) {
      composer.focus();
    }
  });
}

watch(() => assistant.messages.length, scrollToBottom);
watch(streamingContent, scrollToBottom);

onMounted(() => {
  if (props.resident) {
    window.addEventListener('cabbage-chat-focus-request', focusResidentComposer);
  }
  window.addEventListener('corona-active-project-changed', resetChatForProjectChange);
  window.addEventListener('storage', resetChatForProjectStorageChange);
  const currentProjectScopeId = () =>
    reviewScopeId(String(window.localStorage?.getItem('corona.activeProjectPath') || ''));
  unsubscribeAssistantContext = subscribeCabbageAssistantContext(
    (snapshot) => assistant.hydrateContext(snapshot),
    { projectScopeId: currentProjectScopeId, emitCurrent: true }
  );
});

onBeforeUnmount(() => {
  window.removeEventListener('cabbage-chat-focus-request', focusResidentComposer);
  window.removeEventListener('corona-active-project-changed', resetChatForProjectChange);
  window.removeEventListener('storage', resetChatForProjectStorageChange);
  unsubscribeAssistantContext?.();
  unsubscribeAssistantContext = null;
  clearPollTimer();
  const taskId = activeTaskId.value;
  const requestKind = activeRequestKind.value;
  assistant.activeRequestId = '';
  activeTaskId.value = '';
  activeRequestKind.value = '';
  streamingContent.value = '';
  assistant.chatBusy = false;
  activePrompt = '';
  if (requestKind === 'generation') cancelActiveNodeGraphGeneration().catch(() => {});
  else if (taskId) aiService.cancelNodeGraphReviewChat(taskId).catch(() => {});
});
</script>

<style scoped>
.cabbage-chat-shell {
  z-index: 2147483200;
  color: #f2ead5;
  background: linear-gradient(180deg, rgba(21, 19, 13, 0.98), rgba(17, 16, 13, 0.96));
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 8px;
  box-shadow: 0 18px 42px rgba(0, 0, 0, 0.34);
}

.cabbage-chat-shell.resident {
  z-index: auto;
}

.resident-chat-title {
  min-height: 38px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
  padding: 0 7px 0 12px;
  border-bottom: 1px solid rgba(216, 184, 108, 0.22);
  background: linear-gradient(180deg, rgba(39, 32, 18, 0.96), rgba(25, 23, 17, 0.92));
  color: #f0d58c;
  font-size: 13px;
  font-weight: 700;
}

.resident-float-button {
  width: 24px;
  height: 24px;
  display: inline-flex;
  align-items: center;
  justify-content: center;
  flex: 0 0 auto;
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 4px;
  background: rgba(255, 255, 255, 0.06);
  color: #d7cba9;
  font-size: 14px;
  line-height: 1;
  transition:
    background-color 140ms ease,
    border-color 140ms ease,
    color 140ms ease;
}

.resident-float-button:hover:not(:disabled) {
  border-color: rgba(216, 184, 108, 0.36);
  background: rgba(216, 184, 108, 0.18);
  color: #ffffff;
}

.resident-float-button:disabled {
  opacity: 0.38;
  cursor: not-allowed;
}

.context-strip {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 44px;
  padding: 7px 10px;
  border-bottom: 1px solid rgba(255, 255, 255, 0.07);
  background: linear-gradient(180deg, rgba(25, 23, 17, 0.9), rgba(25, 23, 17, 0.84));
}

.context-title {
  color: #c9bea0;
  font-size: 11px;
  font-weight: 600;
  white-space: nowrap;
}

.context-select {
  min-width: 0;
  flex: 1;
  height: 28px;
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 5px;
  background: rgba(17, 16, 13, 0.78);
  color: #f2ead5;
  padding: 0 8px;
  font-size: 11px;
  outline: none;
}

.context-select:hover:not(:disabled) {
  border-color: rgba(216, 184, 108, 0.42);
}

.context-select:focus {
  border-color: #d8b86c;
  box-shadow: 0 0 0 2px rgba(216, 184, 108, 0.13);
}

.context-select:disabled {
  color: #a99c7d;
  cursor: default;
}

.context-count {
  min-width: 23px;
  height: 23px;
  display: grid;
  place-items: center;
  border: 1px solid rgba(216, 184, 108, 0.28);
  border-radius: 999px;
  background: rgba(216, 184, 108, 0.13);
  color: #e9dfc5;
  font-size: 10px;
  font-weight: 700;
}

.chat-history {
  flex: 1;
  min-height: 0;
  overflow: auto;
  padding: 13px 12px 16px;
  display: flex;
  flex-direction: column;
  gap: 11px;
  background:
    radial-gradient(circle at 50% 0%, rgba(216, 184, 108, 0.06), transparent 38%),
    rgba(25, 23, 17, 0.52);
  scrollbar-color: rgba(216, 184, 108, 0.48) transparent;
  scrollbar-width: thin;
}

.chat-history::-webkit-scrollbar {
  width: 6px;
}

.chat-history::-webkit-scrollbar-track {
  background: transparent;
}

.chat-history::-webkit-scrollbar-thumb {
  border-radius: 999px;
  background: rgba(216, 184, 108, 0.42);
}

.chat-empty {
  margin: auto;
  max-width: 300px;
  padding: 14px 18px;
  text-align: center;
  color: #b9ad8f;
  font-size: 12px;
  line-height: 1.7;
}


.chat-empty strong {
  display: block;
  color: #fff7dc;
  font-size: 14px;
  font-weight: 600;
}

.chat-empty p {
  margin: 8px 0 0;
}

.chat-message {
  max-width: 88%;
}

.chat-message.user {
  align-self: flex-end;
}

.chat-role {
  margin: 0 5px 4px;
  color: #9d9278;
  font-size: 10px;
  font-weight: 600;
}

.chat-message.user .chat-role {
  text-align: right;
}

.chat-content {
  white-space: pre-wrap;
  overflow-wrap: anywhere;
  border: 1px solid rgba(255, 255, 255, 0.08);
  border-radius: 8px 8px 8px 3px;
  background: linear-gradient(180deg, rgba(63, 48, 24, 0.94), rgba(36, 32, 22, 0.94));
  box-shadow: 0 5px 16px rgba(0, 0, 0, 0.14);
  padding: 9px 11px;
  color: #f2ead5;
  font-size: 12px;
  line-height: 1.65;
}

.chat-message.user .chat-content {
  border-color: rgba(216, 184, 108, 0.32);
  border-radius: 8px 8px 3px 8px;
  background: linear-gradient(180deg, rgba(112, 84, 35, 0.94), rgba(75, 57, 28, 0.96));
  color: #ffffff;
}

.chat-message.streaming .chat-content {
  border-color: rgba(216, 184, 108, 0.38);
}

.guidance-steps {
  margin: 9px 0 0;
  padding-left: 21px;
  color: #e9dfc5;
}

.guidance-steps li + li {
  margin-top: 5px;
}

.message-showcase {
  display: block;
  margin: 10px 0 0 auto;
  border: 1px solid rgba(216, 184, 108, 0.48);
  border-radius: 5px;
  background: #6d5226;
  color: #fff7dc;
  padding: 5px 11px;
  font-size: 11px;
  font-weight: 600;
}

.message-showcase:hover {
  border-color: #d8b86c;
  background: #8c6f36;
}

.chat-pending {
  display: inline-flex;
  align-items: center;
  align-self: flex-start;
  gap: 7px;
  color: #c9bea0;
  font-size: 11px;
}

.chat-pending::before {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: #d8b86c;
  box-shadow: 0 0 0 4px rgba(216, 184, 108, 0.1);
  content: '';
  animation: cabbage-pulse 1.1s ease-in-out infinite;
}

.chat-error {
  margin: 0 10px 8px;
  border: 1px solid rgba(197, 112, 94, 0.42);
  border-radius: 6px;
  background: rgba(86, 43, 37, 0.72);
  color: #f2c1b7;
  padding: 8px 10px;
  font-size: 11px;
  line-height: 1.5;
}

.chat-composer {
  border-top: 1px solid rgba(255, 255, 255, 0.07);
  background: linear-gradient(180deg, rgba(25, 23, 17, 0.92), rgba(17, 16, 13, 0.96));
  padding: 10px;
}

.chat-composer textarea {
  width: 100%;
  resize: none;
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 6px;
  background: rgba(15, 14, 10, 0.82);
  color: #fff3c8;
  padding: 9px 10px;
  font-size: 12px;
  line-height: 1.55;
  outline: none;
  transition:
    border-color 140ms ease,
    box-shadow 140ms ease,
    background-color 140ms ease;
}

.chat-composer textarea:hover {
  background: rgba(25, 23, 17, 0.9);
}

.chat-composer textarea:focus {
  border-color: #d8b86c;
  box-shadow: 0 0 0 2px rgba(216, 184, 108, 0.13);
}

.composer-actions {
  margin-top: 8px;
  display: flex;
  justify-content: flex-end;
  gap: 7px;
}

.composer-actions button {
  min-height: 27px;
  border: 1px solid rgba(255, 255, 255, 0.1);
  border-radius: 5px;
  padding: 5px 11px;
  color: #e9dfc5;
  font-size: 11px;
  font-weight: 600;
  transition:
    background-color 140ms ease,
    border-color 140ms ease,
    color 140ms ease,
    transform 140ms ease;
}

.composer-actions button:disabled {
  opacity: 0.42;
  cursor: not-allowed;
}

.composer-actions button:active:not(:disabled) {
  transform: translateY(1px);
}

.primary {
  border-color: rgba(216, 184, 108, 0.52) !important;
  background: #8c6f36;
  color: #ffffff !important;
}

.primary:hover:not(:disabled) {
  background: #b8924a;
}

.secondary {
  background: rgba(255, 255, 255, 0.055);
}

.secondary:hover:not(:disabled) {
  border-color: rgba(216, 184, 108, 0.38);
  background: rgba(216, 184, 108, 0.12);
  color: #ffffff;
}

.danger {
  border-color: rgba(197, 112, 94, 0.48) !important;
  background: rgba(121, 67, 59, 0.92);
  color: #ffffff !important;
}

.danger:hover:not(:disabled) {
  background: rgba(145, 76, 66, 0.96);
}

@keyframes cabbage-pulse {
  0%,
  100% {
    opacity: 0.45;
    transform: scale(0.86);
  }
  50% {
    opacity: 1;
    transform: scale(1);
  }
}
</style>
