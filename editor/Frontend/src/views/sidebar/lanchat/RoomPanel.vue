<template>
  <div class="lanchat-panel relative flex flex-col h-full text-base text-gray-100">
    <!-- 未进房：大厅（开房 / 加入） -->
    <div v-if="!s.inRoom" class="flex-1 min-h-0 overflow-y-auto p-4 flex flex-col gap-4">
      <div class="space-y-4">
        <div class="grid grid-cols-2 gap-2">
          <button
            v-for="mode in workspaceModes"
            :key="mode.key"
            class="rounded border px-2 py-2 text-left transition-colors"
            :class="selectedWorkspaceMode === mode.key ? 'border-[#84A65B] bg-[#2f3b2b]' : 'border-gray-700 bg-[#2a2a2a] hover:border-gray-500'"
            @click="selectWorkspaceMode(mode.key)"
          >
            <div class="text-sm font-medium text-gray-100">{{ mode.label }}</div>
            <div class="mt-1 text-[11px] leading-snug text-gray-400">{{ mode.hint }}</div>
          </button>
        </div>

        <!-- tab 切换 -->
        <div v-if="selectedWorkspaceMode === 'multiplayer_multi_agent'" class="flex gap-2">
            <button
              class="flex-1 py-2 rounded text-sm"
            :class="lobbyTab === 'create' ? 'bg-[#84A65B] text-white' : 'bg-[#3a3a3a]/60'"
            @click="lobbyTab = 'create'"
          >
            创建房间
          </button>
          <button
            class="flex-1 py-2 rounded text-sm"
            :class="lobbyTab === 'join' ? 'bg-[#84A65B] text-white' : 'bg-[#3a3a3a]/60'"
            @click="lobbyTab = 'join'"
          >
            加入房间
          </button>
        </div>

        <!-- 创建房间 -->
        <div v-if="lobbyTab === 'create'" class="space-y-3">
          <template v-if="roomMode === 'multi'">
            <input v-model="form.room" placeholder="房间号" :class="inputCls" />
            <input v-model="form.password" placeholder="密码（可选）" :class="inputCls" />
          </template>
          <button
            class="w-full py-2 rounded bg-[#84A65B] text-white text-sm disabled:opacity-50"
            @click="onCreate"
          >
            {{ createButtonText }}
          </button>
        </div>

        <!-- 加入房间 -->
        <div v-else class="space-y-3">
          <input v-model="form.ip" placeholder="房主 IP（如 192.168.1.5）" :class="inputCls" :disabled="isJoining" />
          <input v-model.number="form.port" placeholder="房主端口" :class="inputCls" :disabled="isJoining" />
          <input v-model="form.room" placeholder="房间号" :class="inputCls" :disabled="isJoining" />
          <input v-model="form.password" placeholder="密码（可选）" :class="inputCls" :disabled="isJoining" />
          <input v-model="form.nickname" placeholder="你的昵称" :class="inputCls" :disabled="isJoining" />
          <button
            class="w-full py-2 rounded bg-[#84A65B] text-white text-sm disabled:opacity-50"
            :disabled="isJoining"
            @click="onJoin"
          >
            {{ isJoining ? joinStatusText : '加入' }}
          </button>
          <div v-if="isJoining" class="text-[#B8D58D] text-xs">{{ joinStatusText }}</div>
        </div>

        <div v-if="s.error" class="text-red-400 text-xs">{{ errorText }}</div>
      </div>

      <div class="mt-auto space-y-2 border-t border-gray-700 pt-4">
        <div class="flex items-center justify-between">
          <div class="text-sm font-medium text-gray-200">历史记录</div>
          <div class="flex items-center gap-1.5">
            <button
              class="px-2 py-1 rounded bg-[#3a3a3a] text-xs text-gray-200 disabled:opacity-50"
              :disabled="s.historyLoading"
              @click="refreshHistoryRooms"
            >
              刷新
            </button>
            <button
              v-if="hasMoreHistoryRooms"
              class="px-2 py-1 rounded bg-[#3a3a3a] text-xs text-[#B8D58D] hover:bg-[#46553d]"
              @click="showAllHistory = true"
            >
              更多
            </button>
          </div>
        </div>
        <div v-if="s.historyError" class="text-red-400 text-xs">{{ s.historyError }}</div>
        <div v-if="s.historyLoading && !s.historyRooms.length" class="text-gray-400 text-sm">
          正在加载历史记录…
        </div>
        <div v-else-if="!s.historyRooms.length" class="text-gray-500 text-sm">
          暂无历史记录
        </div>
        <div v-else class="space-y-1">
          <button
            v-for="room in visibleHistoryRooms"
            :key="room.room_id"
            class="w-full text-left px-3 py-2 rounded bg-[#2a2a2a] border border-gray-700 hover:border-[#84A65B] transition-colors"
            :class="s.selectedHistoryRoom?.room_id === room.room_id ? 'border-[#84A65B]' : ''"
            @click="loadHistoryRoom(room)"
            @dblclick="continueHistoryFromList(room)"
          >
            <div class="flex items-center justify-between gap-2">
              <span class="font-medium text-gray-100 truncate">{{ historyRoomTitle(room) }}</span>
              <span class="text-xs text-gray-500 shrink-0">{{ formatHistoryTime(room.last_ts) }}</span>
            </div>
            <div class="mt-1 text-xs text-gray-400 truncate">
              {{ room.message_count || 0 }} 条 · {{ room.last_sender_name || '未知' }}：{{ room.last_text || '' }}
            </div>
          </button>
        </div>
        <button
          v-if="s.selectedHistoryRoom"
          class="w-full py-1.5 rounded bg-[#3a3a3a] text-xs text-[#B8D58D] hover:bg-[#46553d]"
          @click="continueHistoryAsSingle(s.selectedHistoryRoom)"
        >
          作为单人聊天室继续
        </button>
      </div>

      <div
        v-if="showAllHistory"
        class="absolute inset-0 z-20 overflow-y-auto bg-[#282828] p-4 space-y-3"
      >
        <div class="flex items-center justify-between gap-2">
          <button
            class="px-2 py-1 rounded bg-[#3a3a3a] text-xs text-gray-200 hover:bg-[#4a4a4a]"
            @click="showAllHistory = false"
          >
            返回
          </button>
          <div class="text-sm font-medium text-gray-200">全部历史记录</div>
          <button
            class="px-2 py-1 rounded bg-[#3a3a3a] text-xs text-gray-200 disabled:opacity-50"
            :disabled="s.historyLoading"
            @click="refreshHistoryRooms"
          >
            刷新
          </button>
        </div>
        <div v-if="s.historyError" class="text-red-400 text-xs">{{ s.historyError }}</div>
        <div v-if="s.historyLoading && !s.historyRooms.length" class="text-gray-400 text-sm">
          正在加载历史记录…
        </div>
        <div v-else-if="!s.historyRooms.length" class="text-gray-500 text-sm">
          暂无历史记录
        </div>
        <div v-else class="space-y-1">
          <button
            v-for="room in s.historyRooms"
            :key="room.room_id"
            class="w-full text-left px-3 py-2 rounded bg-[#2a2a2a] border border-gray-700 hover:border-[#84A65B] transition-colors"
            :class="s.selectedHistoryRoom?.room_id === room.room_id ? 'border-[#84A65B]' : ''"
            @click="loadHistoryRoom(room)"
            @dblclick="continueHistoryFromList(room)"
          >
            <div class="flex items-center justify-between gap-2">
              <span class="font-medium text-gray-100 truncate">{{ historyRoomTitle(room) }}</span>
              <span class="text-xs text-gray-500 shrink-0">{{ formatHistoryTime(room.last_ts) }}</span>
            </div>
            <div class="mt-1 text-xs text-gray-400 truncate">
              {{ room.message_count || 0 }} 条 · {{ room.last_sender_name || '未知' }}：{{ room.last_text || '' }}
            </div>
          </button>
        </div>
      </div>
    </div>

    <!-- 已进房：聊天界面 -->
    <div v-else class="flex flex-col h-full">
      <!-- 房间信息条 -->
      <div class="flex items-center justify-between gap-3 px-3 py-2.5 bg-[#343434] text-sm">
        <div class="min-w-0">
          <div class="text-[13px] text-gray-400">{{ s.mode === 'single' ? '本地单人协作' : '局域网协作' }}</div>
          <div class="truncate text-base font-semibold text-gray-100">
            {{ s.mode === 'single' ? '单人聊天室' : s.room }}
            <template v-if="s.role === 'host' && s.mode === 'multi'"> · {{ s.ip }}:{{ s.port }}</template>
          </div>
        </div>
        <div class="flex shrink-0 items-center gap-2">
          <span
            v-if="s.mode === 'multi'"
            class="rounded-full px-2 py-1 text-[12px]"
            :class="s.connection === 'connected' ? 'bg-[#84A65B]/20 text-[#B8D58D]' : 'bg-yellow-500/20 text-yellow-300'"
          >
            {{ roomStatusLabel }}
          </span>
          <button class="px-2.5 py-1.5 rounded bg-red-500/80 text-white text-sm" @click="onLeave">
            {{ s.mode === 'multi' && s.role === 'host' ? '关闭房间' : '退出聊天' }}
          </button>
        </div>
      </div>

      <!-- 重连提示条 -->
      <div
        v-if="s.connection === 'reconnecting'"
        class="px-3 py-1.5 bg-yellow-500/20 text-yellow-300 text-sm flex items-center gap-2"
      >
        <span class="inline-block w-2 h-2 rounded-full bg-yellow-400 animate-pulse"></span>
        连接已断开
      </div>

      <div
        v-if="currentDisclosure"
        class="px-3 py-3 border-b border-gray-700 bg-[#222722] text-sm"
      >
        <div class="flex items-center justify-between gap-2">
          <div class="min-w-0">
            <div class="flex items-center gap-2">
              <span
                v-if="isWaitingDisclosure"
                class="inline-block h-2.5 w-2.5 rounded-full bg-[#B8D58D] animate-pulse"
              ></span>
              <span class="text-[#B8D58D] font-semibold truncate">{{ currentDisclosure.stage || '协作状态' }}</span>
              <span v-if="disclosureAgeText" class="shrink-0 text-[12px] text-gray-500">{{ disclosureAgeText }}</span>
            </div>
            <div class="text-[15px] text-gray-200 leading-relaxed mt-1">{{ currentDisclosure.public_message }}</div>
            <div v-if="resourceDiagnosisText" class="text-gray-400 leading-relaxed mt-1">
              {{ resourceDiagnosisText }}
            </div>
            <div v-if="waitHintText" class="mt-1.5 text-[13px] text-gray-400">
              {{ waitHintText }}
            </div>
          </div>
          <div class="shrink-0 text-right">
            <div class="text-lg font-semibold text-gray-100 tabular-nums">{{ currentDisclosure.progress }}%</div>
            <div class="text-[11px] text-gray-500">{{ isWaitingDisclosure ? '处理中' : '状态' }}</div>
          </div>
        </div>
        <div class="mt-2.5 h-2 rounded bg-[#3a3a3a] overflow-hidden">
          <div
            class="h-full bg-[#84A65B] transition-all duration-300"
            :style="{ width: `${currentDisclosure.progress}%` }"
          ></div>
        </div>
        <div v-if="waitSteps.length" class="mt-2.5 grid grid-cols-5 gap-1">
          <div
            v-for="step in waitSteps"
            :key="step.key"
            class="h-1.5 rounded-full"
            :class="step.active ? 'bg-[#84A65B]' : (step.done ? 'bg-[#84A65B]/45' : 'bg-[#3a3a3a]')"
            :title="step.label"
          ></div>
        </div>
        <div v-if="currentDisclosure.available_actions.length" class="mt-2.5 flex flex-wrap gap-1.5">
          <template
            v-for="action in currentDisclosure.available_actions"
            :key="action"
          >
            <button
              v-if="isDisclosureActionSendable(action)"
              class="px-2.5 py-1 rounded bg-[#3a3a3a] text-gray-200 hover:bg-[#84A65B]/70"
              @click="sendDisclosureAction(action)"
            >
              {{ disclosureActionLabel(action) }}
            </button>
            <span
              v-else
              class="px-2.5 py-1 rounded bg-[#3a3a3a] text-gray-200"
            >
              {{ disclosureActionLabel(action) }}
            </span>
          </template>
        </div>
        <div
          v-if="currentDisclosure.requires_confirmation && currentDisclosure.proposal_id && !lanchat.isProposalHandled(currentDisclosure.proposal_id) && s.role === 'host'"
          class="mt-2.5 flex gap-2"
        >
          <button
            class="px-3 py-1.5 rounded bg-[#84A65B] text-white text-sm"
            @click="sendGmDecision(currentDisclosure.proposal_id, 'confirm')"
          >
            确认
          </button>
          <button
            class="px-3 py-1.5 rounded bg-[#3a3a3a] text-gray-100 text-sm"
            @click="sendGmDecision(currentDisclosure.proposal_id, 'reject')"
          >
            拒绝
          </button>
        </div>
      </div>

      <div class="flex flex-1 min-h-0">
        <!-- 消息区 -->
        <div class="flex-1 flex flex-col min-h-0">
          <div ref="msgRef" class="flex-1 min-w-0 overflow-y-auto overflow-x-hidden p-4 space-y-3.5">
            <div
              v-for="m in displayMessages"
              :key="m.renderKey"
              class="flex min-w-0 max-w-full flex-col"
              :class="m.self ? 'items-end' : 'items-start'"
            >
              <template v-if="m.kind === 'pending_reply'">
                <span class="max-w-[88%] truncate text-base leading-relaxed text-gray-400 mb-1">{{ m.targetLabel }}</span>
                <div class="lanchat-message-bubble rounded-lg bg-[#E8E8E8]/90 px-3.5 py-2.5 text-gray-800 shadow-sm">
                  <div class="flex items-center gap-2 text-[15px] leading-relaxed">
                    <span class="inline-block h-4 w-4 rounded-full border-2 border-gray-400 border-t-[#84A65B] animate-spin"></span>
                    <span>{{ pendingReplyText }}</span>
                    <span class="typing-dots text-gray-500"><span>.</span><span>.</span><span>.</span></span>
                  </div>
                  <div v-if="pendingReplyHint" class="mt-1 text-[12px] text-gray-500">
                    {{ pendingReplyHint }}
                  </div>
                </div>
              </template>
              <template v-else>
                <span class="max-w-[88%] truncate text-base leading-relaxed text-gray-400 mb-1">{{ m.displayFrom || m.from }}</span>
                <div
                  class="lanchat-message-bubble px-3.5 py-2.5 rounded-lg text-base leading-relaxed"
                  :class="m.self ? 'bg-[#84A65B] text-white' : 'bg-[#E8E8E8]/90 text-gray-800'"
                >
                  {{ m.displayText || m.text }}
                </div>
                <div
                  v-if="isGmProposalActionable(m) && s.role === 'host'"
                  class="mt-1 flex gap-1"
                >
                  <button
                    class="px-2 py-0.5 rounded bg-[#84A65B] text-white text-[11px]"
                    @click="sendGmDecision(gmProposalId(m), 'confirm')"
                  >
                    确认
                  </button>
                  <button
                    class="px-2 py-0.5 rounded bg-[#3a3a3a] text-gray-100 text-[11px]"
                    @click="sendGmDecision(gmProposalId(m), 'reject')"
                  >
                    拒绝
                  </button>
                </div>
              </template>
            </div>
          </div>

          <!-- 输入区 -->
          <div
            v-if="isWaitingDisclosure"
            class="px-3 py-2 border-t border-gray-700 bg-[#242424] text-[13px] text-gray-400"
          >
            {{ inputAssistText }}
          </div>
          <div class="p-3 border-t border-gray-600 flex gap-2">
            <div class="relative flex-1 space-y-2">
              <div ref="mentionRoot" class="relative">
                <textarea
                  ref="draftInput"
                  v-model="draft"
                  rows="2"
                  :class="draftInputCls"
                  :disabled="s.connection === 'reconnecting'"
                  :placeholder="draftPlaceholder"
                  @input="onDraftInput"
                  @keydown="onDraftKeydown"
                ></textarea>
                <button
                  type="button"
                  class="absolute bottom-2 left-2 inline-flex h-6 w-6 items-center justify-center rounded bg-[#3a3a3a] text-base leading-none text-[#B8D58D] hover:bg-[#46553d] hover:text-white"
                  title="指定 AI 助手"
                  @click="toggleMentionPicker"
                >
                  +
                </button>
                <div
                  v-if="mentionCandidates.length"
                  class="lanchat-scrollbar absolute bottom-full left-0 z-10 mb-2 max-h-64 w-full overflow-y-auto rounded border border-gray-600 bg-[#2a2a2a] shadow-xl"
                >
                  <div
                    v-for="(c, i) in mentionCandidates"
                    :key="`${c.name}-${i}`"
                    class="px-2 py-1.5 text-sm text-gray-200 cursor-pointer"
                    :class="i === mentionActiveIndex ? 'bg-[#84A65B]/60 text-white' : 'hover:bg-[#84A65B]/40'"
                    @mousedown.prevent
                    @click="pickMention(c)"
                  >
                    {{ mentionIcon(c) }}{{ c.name }}<span v-if="c.hint" class="text-[10px] text-gray-400 ml-1">{{ c.hint }}</span>
                  </div>
                </div>
              </div>
              <div
                class="flex items-center justify-between gap-2 text-[11px]"
                :class="routeGuardText ? 'text-yellow-300' : 'text-gray-500'"
              >
                <span>{{ routeGuardText || inputRouteHint }}</span>
                <label
                  v-if="s.role === 'host'"
                  class="flex shrink-0 items-center gap-1.5 text-gray-400 cursor-pointer select-none"
                >
                  <input
                    type="checkbox"
                    class="accent-[#84A65B]"
                    :checked="s.generationOptions.vlmEnabled"
                    @change="onVlmToggle"
                  />
                  <span>VLM 外观检查</span>
                </label>
              </div>
            </div>
            <button
              class="self-stretch px-4 rounded bg-[#84A65B] text-white text-base disabled:opacity-50"
              :disabled="sendDisabled"
              @click="onSend"
            >
              发送
            </button>
          </div>
        </div>

        <!-- 成员区 -->
        <div class="w-36 border-l border-gray-600 py-2 overflow-y-auto">
        <MemberList
          :members="s.members"
          :member-details="s.memberDetails"
          :peer-id="s.peerId"
          :show-self-marker="s.mode === 'multi'"
          :agents="s.agents"
          @remove-agent="onRemoveAgent"
          @add-agent="showAddAgent = true"
        />
        </div>
      </div>

      <div v-if="s.error" class="text-red-400 text-xs px-3 py-1">{{ errorText }}</div>

      <!-- 添加 AI 助手弹窗 -->
      <div
        v-if="showAddAgent"
        class="absolute inset-0 bg-black/50 flex items-center justify-center z-10"
        @pointerdown="onAddAgentBackdropPointerDown"
        @pointerup="onAddAgentBackdropPointerUp"
      >
        <div class="bg-[#2a2a2a] p-4 rounded w-[26rem] max-w-[calc(100%-2rem)] space-y-3">
          <div class="text-sm text-gray-200">添加 AI 助手</div>
          <div class="flex gap-1.5">
            <button
              v-for="bundle in roleTemplateBundles"
              :key="bundle.key"
              class="flex-1 px-2 py-1 rounded bg-[#42543b] text-xs text-gray-100 hover:bg-[#84A65B]/80"
              :title="bundle.hint"
              @click="addRoleTemplateBundle(bundle)"
            >
              {{ bundle.name }}
            </button>
          </div>
          <input v-model="agentForm.name" placeholder="助手名字（如 小策）" :class="inputCls" />
          <textarea
            v-model="agentForm.persona"
            placeholder="人设提示词（可选，也可直接写自定义角色）"
            rows="3"
            :class="inputCls"
          ></textarea>
          <div class="flex gap-2">
            <button class="flex-1 py-1.5 rounded bg-[#3a3a3a] text-gray-200 text-sm" @click="showAddAgent = false">取消</button>
            <button class="flex-1 py-1.5 rounded bg-[#84A65B] text-white text-sm" @click="onAddAgent">添加</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { reactive, ref, computed, nextTick, watch, onMounted, onBeforeUnmount } from 'vue';
import lanchat from '../../../stores/lanchat.js';
import { Bridge, networkService } from '../../../utils/bridge.js';
import { coronaEventBus } from '../../../utils/eventBus.js';
import { getActorContext } from '../../../blockly/composables/useActorContext.js';
import {
  buildGmDecisionMessage,
  buildGmDisclosureActionMessage,
  buildManualGmMessageOptions,
  buildParticipantDisclosureDraft,
} from '../../../stores/lanchatDisclosure.js';
import MemberList from './MemberList.vue';
import {
  aiReplyDisplayText,
  displaySenderName,
  effectiveDraftAction,
  pendingReplyMatchesMessage,
  resolveSelectedTargetKey,
  routeGuardMessage,
  targetKeyFromMentionText,
  targetPayloadForKey,
} from './routeSelection.js';
import {
  createExpertGroupConfig,
  selectedExpertPayloads as buildSelectedExpertPayloads,
} from './expertGroupConfig.js';

const s = lanchat.state;
const draft = ref('');
const normalizeVisibleWorkspaceMode = (mode) => (
  mode === 'multiplayer_multi_agent' ? 'multiplayer_multi_agent' : 'solo_single_agent'
);
const initialWorkspaceMode = normalizeVisibleWorkspaceMode(s.workspaceMode);
const lobbyTab = ref('create');
const roomMode = ref(initialWorkspaceMode === 'multiplayer_multi_agent' ? 'multi' : 'single');
const selectedWorkspaceMode = ref(initialWorkspaceMode);
const showAllHistory = ref(false);
const selectedTargetKey = ref('scene');
const showAddAgent = ref(false);
const agentForm = reactive({ name: '', persona: '' });
const addAgentBackdropPointerStarted = ref(false);
const mentionRoot = ref(null);
const mentionCandidates = ref([]);
const mentionActiveIndex = ref(0);
const manualMentionOpen = ref(false);
const msgRef = ref(null);
const draftInput = ref(null);
const nowMs = ref(Date.now());
const pendingReply = ref(null);
let waitClock = null;
let modelTransferPollTimer = null;
const DRAFT_MIN_ROWS = 2;
const DRAFT_MAX_ROWS = 4;
const PENDING_MODEL_TRANSFER_POLL_LIMIT = 16;
const modelTransferSnapshotRequests = new Set();
const remoteRegisteredActorIdentities = new Set();
const snapshotActorCreateKeys = new Set();

const roleTemplates = [
  {
    key: 'elder',
    name: '长者',
    persona: '长者',
    hint: '沉稳、传统、实用、安全、秩序感',
  },
  {
    key: 'little_girl',
    name: '小女孩',
    persona: '小女孩',
    hint: '明亮、可爱、装饰性强、童趣、柔和颜色',
  },
  {
    key: 'bandit',
    name: '山贼',
    persona: '山贼',
    hint: '粗犷、木质、营地感、防御性、战利品',
  },
  {
    key: 'scholar',
    name: '学者',
    persona: '学者',
    hint: '书籍、秩序、研究工具、安静区域',
  },
  {
    key: 'merchant',
    name: '商人',
    persona: '商人',
    hint: '摊位、货物、展示、交易动线',
  },
];

const roleTemplateBundles = [
  {
    key: 'night_market_validation',
    name: '夜市验证组',
    hint: '一键添加长者、商人、小女孩、山贼，适合今晚多人/多 Agent 验证',
    roles: ['elder', 'merchant', 'little_girl', 'bandit'],
  },
];

const defaultExpertRoleKeys = roleTemplateBundles[0]?.roles || [];
const expertGroupConfig = reactive(createExpertGroupConfig(roleTemplates, defaultExpertRoleKeys));

const workspaceModes = [
  {
    key: 'solo_single_agent',
    label: '自己设计',
    hint: '本地单人，内置专家默认启用',
  },
  {
    key: 'multiplayer_multi_agent',
    label: '多人共创',
    hint: '房主开房，内置专家默认启用',
  },
];

const form = reactive({
  room: '',
  password: '',
  ip: '',
  port: 27960,
  nickname: '',
});

const inputCls =
  'w-full px-3 py-2 rounded bg-[#2a2a2a] border border-gray-600 text-[15px] text-gray-100 outline-none focus:border-[#84A65B]';
const draftInputCls =
  'lanchat-scrollbar w-full resize-none rounded border border-gray-600 bg-[#2a2a2a] px-3 py-2 text-[15px] leading-relaxed text-gray-100 outline-none focus:border-[#84A65B] disabled:opacity-60';

const ERROR_TEXT = {
  WRONG_PASSWORD: '密码错误',
  ROOM_NOT_FOUND: '房间不存在',
  ROOM_FULL: '房间已满',
  NAME_TAKEN: '昵称已被占用',
  ROOM_CLOSED: '房间已关闭',
  START_FAILED: '开房失败',
  JOIN_FAILED: '加入失败',
  HOST_UNREACHABLE: '无法连接到房主',
  ROOM_MISMATCH: '房间号不匹配',
  JOIN_TIMEOUT: '加入超时',
  CONNECTING: '连接尚未完成',
  SYNCING: '正在同步房间',
};
const errorText = computed(() => ERROR_TEXT[s.error] || s.error || '');
const isJoining = computed(() => lanchat.isJoining());
const joinStatusText = computed(() => (s.connection === 'syncing' ? '正在同步房间…' : '正在连接房主…'));
const createButtonText = computed(() => {
  if (roomMode.value === 'multi') return '创建多人房间';
  if (s.selectedHistoryRoom) return '继续所选历史';
  return '进入自己设计';
});
const selectedExpertPayloads = computed(() => buildSelectedExpertPayloads(expertGroupConfig, roleTemplates));
const visibleHistoryRooms = computed(() => (s.historyRooms || []).slice(0, 2));
const hasMoreHistoryRooms = computed(() => (s.historyRooms || []).length > 2);
const roomStatusLabel = computed(() => {
  if (s.connection === 'connected') return s.role === 'host' ? '房主在线' : '已连接';
  if (s.connection === 'reconnecting') return '重连中';
  if (s.connection === 'syncing') return '同步中';
  if (s.connection === 'connecting') return '连接中';
  return '未连接';
});
const currentDisclosure = computed(() => {
  const items = s.disclosures || [];
  if (!items.length) return null;
  if (s.role === 'host') {
    const pending = [...items].reverse().find((item) => (
      item.requires_confirmation &&
      item.proposal_id &&
      !lanchat.isProposalHandled(item.proposal_id)
    ));
    if (pending) return pending;
  }
  return items[items.length - 1];
});
const isWaitingDisclosure = computed(() => {
  const disclosure = currentDisclosure.value;
  if (!disclosure) return false;
  if (disclosure.requires_confirmation) return false;
  const stage = String(disclosure.stage || '');
  const message = String(disclosure.public_message || '');
  const progress = Number(disclosure.progress || 0);
  if (progress > 0 && progress < 100) return true;
  return /排队|等待|生成|资源|图片|模型|检索|下载|导入|组装|审查|检查|可介入|执行中/.test(`${stage} ${message}`);
});
const disclosureAgeText = computed(() => {
  const disclosure = currentDisclosure.value;
  if (!disclosure?.created_at) return '';
  const elapsed = Math.max(0, Math.floor((nowMs.value - Number(disclosure.created_at) * 1000) / 1000));
  if (elapsed < 15) return '刚刚更新';
  if (elapsed < 60) return `${elapsed} 秒前`;
  const minutes = Math.floor(elapsed / 60);
  if (minutes < 60) return `${minutes} 分钟前`;
  return `${Math.floor(minutes / 60)} 小时前`;
});
const waitHintText = computed(() => {
  if (!isWaitingDisclosure.value) return '';
  const disclosure = currentDisclosure.value || {};
  const stage = `${disclosure.stage || ''} ${disclosure.public_message || ''}`;
  if (/排队|等待资源/.test(stage)) return '任务已进入队列，生成资源空出来后会自动继续。';
  if (/图片/.test(stage)) return '图片阶段耗时可能较长，期间可以继续补充风格或新增物体。';
  if (/模型|检索|下载/.test(stage)) return '模型资源正在准备，慢任务会分批完成并继续更新。';
  if (/导入|组装/.test(stage)) return '正在把本批模型放入场景，建议等当前批次落地后再评价布局。';
  if (/审查|检查|VLM/.test(stage)) return '正在做外观或摆放检查，结果只会形成建议，不会直接覆盖你的场景。';
  if (/可介入/.test(stage)) return '现在可以补充“新增、调整、说明、问题”，系统会优先带入下一批。';
  return '系统仍在处理；长耗时阶段会持续更新，未完成前不要反复确认同一任务。';
});
const waitSteps = computed(() => {
  const disclosure = currentDisclosure.value;
  if (!disclosure || !isWaitingDisclosure.value) return [];
  const labels = [
    { key: 'plan', label: '理解方案' },
    { key: 'image', label: '图片/素材' },
    { key: 'model', label: '模型生成' },
    { key: 'import', label: '导入组装' },
    { key: 'review', label: '检查收尾' },
  ];
  const progress = Number(disclosure.progress || 0);
  const text = `${disclosure.stage || ''} ${disclosure.public_message || ''}`;
  let activeIndex = Math.min(labels.length - 1, Math.max(0, Math.floor(progress / 25)));
  if (/图片|素材|检索|下载/.test(text)) activeIndex = 1;
  if (/模型/.test(text)) activeIndex = 2;
  if (/导入|组装|摆放/.test(text)) activeIndex = 3;
  if (/审查|检查|最终|完成/.test(text)) activeIndex = 4;
  return labels.map((step, index) => ({
    ...step,
    active: index === activeIndex,
    done: index < activeIndex,
  }));
});
const inputAssistText = computed(() => {
  if (!isWaitingDisclosure.value) return '';
  if (s.role === 'host') {
    return '等待期间可以继续输入：说明、调整、新增或 @GM 查询状态；系统会按批次吸收。';
  }
  return '等待期间可以继续补充想法；涉及执行和确认的操作会交给房主处理。';
});
const draftPlaceholder = computed(() => {
  if (s.connection === 'reconnecting') return '连接已断开';
  if (isWaitingDisclosure.value) return '生成中也可输入：新增一个… / 调整… / 问题…';
  return '输入@来指定AI助手~';
});
const currentPendingReply = computed(() => (
  pendingReplyBelongsToCurrentRoom(pendingReply.value) ? pendingReply.value : null
));
const pendingReplyText = computed(() => {
  const pending = currentPendingReply.value;
  const target = pending?.targetLabel || 'AI 助手';
  const elapsed = Math.max(0, Math.floor((nowMs.value - Number(pending?.createdAtMs || 0)) / 1000));
  if (elapsed >= 20) return `${target} 仍在处理`;
  if (elapsed >= 8) return `${target} 正在整理`;
  return `${target} 正在思考`;
});
const pendingReplyHint = computed(() => {
  const pending = currentPendingReply.value;
  if (!pending) return '';
  const elapsed = Math.max(0, Math.floor((nowMs.value - Number(pending.createdAtMs || 0)) / 1000));
  if (elapsed < 12) return '';
  return '复杂方案或工具调用可能需要更久，你可以继续补充要求。';
});
const displayMessages = computed(() => {
  const messages = (s.messages || []).map((message, index) => ({
    ...message,
    displayFrom: displaySenderName(message),
    displayText: aiReplyDisplayText(message, s.messages || [], s.memberDetails || []),
    renderKey: message.message_id || `message-${index}`,
    kind: 'message',
  }));
  const pending = currentPendingReply.value;
  if (!pending) return messages;
  const pendingMessage = {
    ...pending,
    renderKey: `pending-${pending.correlationId}`,
    kind: 'pending_reply',
    self: false,
  };
  const anchorIndex = messages.findIndex((message) => (
    message.self &&
    pending.correlationId &&
    message.correlation_id === pending.correlationId
  ));
  if (anchorIndex >= 0) {
    return [
      ...messages.slice(0, anchorIndex + 1),
      pendingMessage,
      ...messages.slice(anchorIndex + 1),
    ];
  }
  return [...messages, pendingMessage];
});
const resourceDiagnosisText = computed(() => {
  if (!currentDisclosure.value || currentDisclosure.value.stage !== '资源调度') return '';
  return resourceDiagnosisLabel(currentDisclosure.value.metadata?.diagnosis);
});
const targetOptions = computed(() => {
  const options = [];
  const agents = Array.isArray(s.agents) ? s.agents : [];
  options.push({ key: 'gm', label: '主持人', scope: 'gm', agentId: 'gm', agentName: 'GM' });
  options.push({ key: 'group', label: '全部AI助手', scope: 'group' });
  if (agents.length) {
    for (const agent of agents) {
      options.push({
        key: `agent:${agent.agent_id || agent.name}`,
        label: agent.name,
        scope: 'agent',
        agentId: agent.agent_id || agent.name || '',
        agentName: agent.name || agent.agent_name || '',
      });
    }
  } else {
    options.push({
      key: 'agent:design-assistant',
      label: '设计助手',
      scope: 'agent',
      agentId: 'design-assistant',
      agentName: '设计助手',
    });
  }
  options.push({ key: 'scene', label: '当前场景', scope: 'scene' });
  return options;
});
const selectedTarget = computed(() => (
  targetOptions.value.find((item) => item.key === selectedTargetKey.value) || null
));
const inputRouteHint = computed(() => {
  if (!selectedTarget.value) {
    return '直接发送；输入 @ 指定AI助手';
  }
  const target = selectedTarget.value?.label || '当前目标';
  return `发送给 ${target}`;
});
const routeGuardText = computed(() => routeGuardMessage(
  'chat',
  selectedTarget.value,
  draft.value
));
const sendDisabled = computed(() => s.connection === 'reconnecting' || Boolean(routeGuardText.value));

function refreshHistoryRooms() {
  return lanchat.refreshHistoryRooms();
}

function loadHistoryRoom(room) {
  clearPendingReply();
  return lanchat.loadHistoryRoom(room);
}

function historyRoomTitle(room) {
  if (!room) return '';
  const displayRoomId = room.display_room_id || room.room_id;
  const roomId = String(displayRoomId || '');
  if (!roomId) return '';
  return isLocalSingleRoomId(roomId) ? '单人聊天室' : `多人聊天室-${roomId}`;
}

function formatHistoryTime(ts) {
  const seconds = Number(ts || 0);
  if (!seconds) return '';
  const date = new Date(seconds * 1000);
  const pad = (value) => String(value).padStart(2, '0');
  return `${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}`;
}

onMounted(refreshHistoryRooms);

onMounted(() => {
  resizeDraftInput();
  waitClock = window.setInterval(() => {
    nowMs.value = Date.now();
  }, 1000);
  ensureModelTransferPolling();
  coronaEventBus.on('actor-sync-broadcast', handleActorSyncBroadcast);
  document.addEventListener('pointerdown', onDocumentPointerDown);
});

onBeforeUnmount(() => {
  if (waitClock) window.clearInterval(waitClock);
  waitClock = null;
  stopModelTransferPolling();
  coronaEventBus.off('actor-sync-broadcast', handleActorSyncBroadcast);
  document.removeEventListener('pointerdown', onDocumentPointerDown);
});

watch(
  () => [s.inRoom, s.connection, s.role, s.mode, s.room, s.ip],
  () => {
    ensureModelTransferPolling();
    requestModelTransferSnapshotForJoin();
  },
  { flush: 'post' },
);

watch(
  () => [s.inRoom, s.mode, s.room],
  () => {
    if (pendingReply.value && !pendingReplyBelongsToCurrentRoom()) {
      clearPendingReply();
    }
  }
);

function selectWorkspaceMode(mode) {
  const visibleMode = normalizeVisibleWorkspaceMode(mode);
  selectedWorkspaceMode.value = visibleMode;
  lanchat.setWorkspaceMode(visibleMode);
  selectedTargetKey.value = 'scene';
  applyInputRouteState();
  if (visibleMode === 'multiplayer_multi_agent') {
    roomMode.value = 'multi';
  } else {
    roomMode.value = 'single';
    lobbyTab.value = 'create';
  }
}

function selectTarget(key) {
  selectedTargetKey.value = resolveSelectedTargetKey(key, targetOptions.value);
  applyInputRouteState();
}

function applyInputRouteState() {
  lanchat.setDraftAction('chat');
  lanchat.setActiveTarget(targetPayloadForKey(selectedTargetKey.value, targetOptions.value));
}

async function onCreate() {
  selectWorkspaceMode(selectedWorkspaceMode.value);
  if (roomMode.value === 'single') {
    if (s.selectedHistoryRoom) {
      await continueHistoryAsSingle(s.selectedHistoryRoom);
      return;
    }
    const res = await lanchat.openRoom({
      room: makeLocalRoomId(),
      password: '',
      port: form.port || 27960,
      mode: 'single',
    });
    if (!(res && res.ok)) return;
    await addDefaultExpertGroup();
    return;
  }
  if (!form.room.trim()) return;
  const res = await lanchat.openRoom({
    room: form.room.trim(),
    password: form.password,
    port: form.port || 27960,
    mode: roomMode.value,
  });
  if (res && res.ok) {
    await addDefaultExpertGroup();
  }
}

async function continueHistoryFromList(room) {
  const roomId = typeof room === 'string' ? room : room?.room_id;
  if (!roomId) return;
  await loadHistoryRoom(room);
  if (isLocalSingleRoomId(roomId)) {
    await continueHistoryAsSingle(roomId);
  } else {
    await continueHistoryAsMulti(roomId);
  }
}

async function continueHistoryAsSingle(room = s.selectedHistoryRoom) {
  const roomId = typeof room === 'string' ? room : room?.room_id;
  if (!roomId) return;
  roomMode.value = 'single';
  lobbyTab.value = 'create';
  lanchat.setWorkspaceMode('solo_single_agent');
  const res = await lanchat.continueHistoryAsLocalRoom({
    room: roomId,
  });
  if (res && res.ok) {
    await addDefaultExpertGroup();
  }
}

async function continueHistoryAsMulti(room = s.selectedHistoryRoom) {
  const roomId = typeof room === 'string' ? room : room?.room_id;
  if (!roomId) return;
  roomMode.value = 'multi';
  lobbyTab.value = 'create';
  lanchat.setWorkspaceMode('multiplayer_multi_agent');
  const res = await lanchat.continueHistoryAsMultiRoom({
    room: roomId,
    port: form.port || 27960,
  });
  if (res && res.ok) {
    await addDefaultExpertGroup();
  }
}

function makeLocalRoomId() {
  return 'single-default';
}

function isLocalSingleRoomId(roomId) {
  const value = String(roomId || '');
  return value === 'single-default' || /^single-\d{8}-\d{6}-[a-z0-9]+$/.test(value);
}

function currentModelTransferSceneName() {
  const context = getActorContext();
  return String(context?.scene || '').trim() || 'Scene/default.scene';
}

function unwrapCefResult(res) {
  return res && res.data !== undefined ? res.data : res;
}

function hashString(str) {
  let hash = 0;
  for (let i = 0; i < str.length; i += 1) {
    const ch = str.charCodeAt(i);
    hash = (hash << 5) - hash + ch;
    hash |= 0;
  }
  return hash >>> 0;
}

function lastPathPart(value) {
  return (
    String(value || '')
      .replace(/\\/g, '/')
      .split('/')
      .filter(Boolean)
      .pop() || ''
  );
}

function isInternalSyncName(value) {
  const text = String(value || '').trim();
  return text.startsWith('__') || lastPathPart(text).startsWith('__');
}

function isActorSyncable(actorData) {
  if (!actorData) return false;
  if (actorData._suppress_network_broadcast) return false;
  if (actorData.actor_type === 'actor') return false;
  if (!actorData.geometry || typeof actorData.geometry !== 'object') return false;
  if (isInternalSyncName(actorData.name)) return false;
  if (isInternalSyncName(actorData.scene)) return false;
  return Boolean(actorData.path || actorData.model);
}

function actorCreateBroadcastKey(sceneName, actorGuid, modelPath) {
  return `${sceneName}:${actorGuid}:${modelPath}`;
}

function rememberActorCreateBroadcast(sceneName, actorGuid, modelPath) {
  if (!sceneName || !actorGuid || !modelPath) return;
  snapshotActorCreateKeys.add(actorCreateBroadcastKey(sceneName, actorGuid, modelPath));
}

function modelTransferActive() {
  return s.inRoom && s.connection === 'connected' && s.mode === 'multi';
}

function ensureModelTransferPolling() {
  if (!modelTransferActive()) {
    stopModelTransferPolling();
    return;
  }
  if (modelTransferPollTimer) return;
  modelTransferPollTimer = window.setInterval(pollModelTransfer, 2000);
  pollModelTransfer();
}

function stopModelTransferPolling() {
  if (modelTransferPollTimer) {
    window.clearInterval(modelTransferPollTimer);
    modelTransferPollTimer = null;
  }
  modelTransferSnapshotRequests.clear();
  remoteRegisteredActorIdentities.clear();
  snapshotActorCreateKeys.clear();
}

async function getActorSnapshot(sceneName) {
  const raw = await Bridge.callCEF('SceneTools', 'get_actor_sync_snapshot', [sceneName]);
  return unwrapCefResult(raw);
}

async function registerActorIdentityFromData(actorData, locallyOwned = true) {
  if (!modelTransferActive() || !actorData) return false;
  const actorGuid = actorData.actor_guid || '';
  const actorHandle = actorData.handle || '';
  if (!actorGuid || !actorHandle) return false;
  const identityKey = `${actorGuid}:${actorHandle}:${locallyOwned ? 'local' : 'remote'}`;
  if (!locallyOwned && remoteRegisteredActorIdentities.has(identityKey)) return true;
  const registered = await networkService.registerActorIdentity(actorGuid, actorHandle, locallyOwned);
  if (registered?.ok !== true) return false;
  if (!locallyOwned) {
    remoteRegisteredActorIdentities.add(identityKey);
  }
  return true;
}

async function broadcastCurrentSceneSnapshot(sceneName, includeActorCreates = true) {
  if (!modelTransferActive()) return;
  const targetScene = String(sceneName || '').trim() || currentModelTransferSceneName();
  const snapshot = await getActorSnapshot(targetScene);
  if (!snapshot || snapshot.status === 'error') return;
  const actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
  if (includeActorCreates) {
    for (const actor of actors) {
      if (!isActorSyncable(actor)) continue;
      const actorGuid = actor.actor_guid || '';
      const modelPath = actor.path || actor.model || '';
      if (!actorGuid || !modelPath) continue;
      const key = actorCreateBroadcastKey(targetScene, actorGuid, modelPath);
      if (snapshotActorCreateKeys.has(key)) continue;
      const sent = await networkService
        .broadcastActorCreate(actorGuid, targetScene, modelPath, { ...actor, scene: targetScene })
        .then(() => true)
        .catch(() => false);
      if (sent) rememberActorCreateBroadcast(targetScene, actorGuid, modelPath);
    }
  }
  await networkService.broadcastSceneSnapshot(targetScene, snapshot).catch(() => {});
}

async function applyRemoteSceneSnapshot(sceneName, snapshotPayload) {
  const targetScene = String(sceneName || '').trim() || currentModelTransferSceneName();
  let snapshot = snapshotPayload || {};
  if (typeof snapshotPayload === 'string') {
    try {
      snapshot = JSON.parse(snapshotPayload);
    } catch (_) {
      snapshot = {};
    }
  }
  if (!snapshot || !Array.isArray(snapshot.actors)) return;
  snapshot.actors = snapshot.actors.map((actor) => ({
    ...(actor || {}),
    _suppress_network_broadcast: true,
  }));
  await networkService.setSyncPaused(true);
  try {
    const applied = await Bridge.callCEF('SceneTools', 'apply_actor_sync_snapshot_internal', [
      targetScene,
      snapshot,
    ]);
    const appliedData = unwrapCefResult(applied);
    const changedActors = [...(appliedData?.created || []), ...(appliedData?.updated || [])];
    for (const actorData of changedActors) {
      await registerActorIdentityFromData(actorData, false);
    }
  } finally {
    await networkService.setSyncPaused(false);
  }
}

async function pollPendingActorCreates() {
  for (let i = 0; i < PENDING_MODEL_TRANSFER_POLL_LIMIT; i += 1) {
    const pending = await networkService.pollPendingActorCreate();
    if (!pending || !pending.has_pending) break;
    await networkService.setSyncPaused(true);
    try {
      pending.actor_data = pending.actor_data || {};
      pending.actor_data.actor_guid = pending.actor_guid || '';
      pending.actor_data._suppress_network_broadcast = true;
      const created = await Bridge.callCEF('SceneTools', 'create_actor_internal', [
        pending.scene_name,
        pending.model_path,
        'model',
        pending.actor_data,
      ]);
      const createdData = unwrapCefResult(created);
      await registerActorIdentityFromData(createdData?.actor || createdData, false);
    } finally {
      await networkService.setSyncPaused(false);
    }
  }
}

async function pollModelTransfer() {
  if (!modelTransferActive()) return;
  try {
    await pollPendingActorCreates();
    if (s.role === 'host') {
      for (let i = 0; i < PENDING_MODEL_TRANSFER_POLL_LIMIT; i += 1) {
        const pendingRequest = await networkService.pollPendingSceneSnapshotRequest();
        if (!pendingRequest || !pendingRequest.has_pending) break;
        await broadcastCurrentSceneSnapshot(pendingRequest.scene_name || currentModelTransferSceneName(), true);
      }
    } else if (s.role === 'guest') {
      for (let i = 0; i < PENDING_MODEL_TRANSFER_POLL_LIMIT; i += 1) {
        const pendingSnapshot = await networkService.pollPendingSceneSnapshot();
        if (!pendingSnapshot || !pendingSnapshot.has_pending) break;
        await applyRemoteSceneSnapshot(
          pendingSnapshot.scene_name || currentModelTransferSceneName(),
          pendingSnapshot.snapshot_json,
        );
      }
    }
  } catch (error) {
    console.warn('[LANChat] model transfer polling failed', error);
  }
}

function handleActorSyncBroadcast(actorData) {
  if (!modelTransferActive()) return;
  if (!isActorSyncable(actorData)) return;
  const modelPath = actorData.path || actorData.model || '';
  if (!modelPath) return;
  const sceneName = String(actorData.scene || '').trim() || currentModelTransferSceneName();
  const actorGuid =
    actorData.actor_guid ||
    `actor-${hashString(`${sceneName}|${modelPath}|${actorData.name || ''}`)}`;
  actorData.actor_guid = actorGuid;
  registerActorIdentityFromData(actorData).catch(() => {});
  networkService
    .broadcastActorCreate(actorGuid, sceneName, modelPath, actorData)
    .then(() => {
      rememberActorCreateBroadcast(sceneName, actorGuid, modelPath);
    })
    .catch(() => {});
}

async function requestModelTransferSnapshotForJoin() {
  if (!s.inRoom || s.connection !== 'connected') return;
  if (s.role !== 'guest' || s.mode !== 'multi') return;
  const sceneName = currentModelTransferSceneName();
  const requestKey = `${s.room || ''}|${s.ip || ''}|${sceneName}`;
  if (modelTransferSnapshotRequests.has(requestKey)) return;
  modelTransferSnapshotRequests.add(requestKey);
  try {
    await networkService.requestSceneSnapshot(sceneName);
  } catch (error) {
    modelTransferSnapshotRequests.delete(requestKey);
    console.warn('[LANChat] request model transfer snapshot failed', error);
  }
}

async function onJoin() {
  if (!form.ip.trim() || !form.room.trim()) return;
  await lanchat.joinRoom({
    ip: form.ip.trim(),
    port: form.port || 27960,
    room: form.room.trim(),
    password: form.password,
    nickname: form.nickname.trim() || '用户',
  });
}

async function onLeave() {
  clearPendingReply();
  if (s.role === 'host') {
    await lanchat.closeRoom();
  } else {
    await lanchat.leaveRoom();
  }
}

function onSend() {
  const text = draft.value;
  syncSelectedTargetFromDraft(text);
  if (!text.trim() || sendDisabled.value) return;
  const pending = pendingReplyForText(text);
  if (pending) startPendingReply(pending);
  lanchat.sendMessage(text, messageOptionsForText(text, pending)).then((res) => {
    if (res && res.ok === false) {
      if (pending) clearPendingReply(pending.correlationId);
      return;
    }
    draft.value = '';
    syncSelectedTargetFromDraft('');
    closeMentionPicker();
    resizeDraftInput();
  }).catch((error) => {
    if (pending) clearPendingReply(pending.correlationId);
    console.warn('[LANChat] send message failed', error);
  });
}

function startPendingReply(pending) {
  if (!pending) return;
  pendingReply.value = pending;
}

function clearPendingReply(correlationId = '') {
  if (correlationId && pendingReply.value?.correlationId !== correlationId) return;
  pendingReply.value = null;
}

function pendingReplyBelongsToCurrentRoom(pending = pendingReply.value) {
  if (!pending || !s.inRoom) return false;
  return (
    String(pending.roomId || '') === String(s.room || '') &&
    String(pending.roomMode || '') === String(s.mode || '')
  );
}

function makePendingCorrelationId() {
  const randomPart = Math.random().toString(36).slice(2, 8);
  return `ui-${Date.now().toString(36)}-${randomPart}`;
}

function pendingReplyForText() {
  const target = selectedTarget.value;
  if (!target || !['agent', 'gm'].includes(target.scope)) return null;
  const plainTargetLabel = target.label || target.agentName || 'AI 助手';
  const targetLabel = target.scope === 'agent' && !plainTargetLabel.startsWith('🤖')
    ? `🤖${plainTargetLabel}`
    : plainTargetLabel;
  return {
    correlationId: makePendingCorrelationId(),
    roomId: s.room || '',
    roomMode: s.mode || '',
    targetAgentId: target.agentId || '',
    targetLabel,
    createdAtMs: Date.now(),
  };
}

function isAiReplyMessage(message) {
  if (!message || message.self) return false;
  const from = String(message.from || '');
  if (!from || from === '系统') return false;
  if (from === s.nickname) return false;
  return true;
}

function isPendingReplyMessage(message, pending = currentPendingReply.value) {
  if (!pending || !isAiReplyMessage(message)) return false;
  return pendingReplyMatchesMessage(message, pending);
}

function gmProposalId(message) {
  if (message?.correlation_id && message?.message_kind === 'gm_proposal') {
    return String(message.correlation_id);
  }
  const text = String(message?.text || '');
  if (!text.includes('GM 提案')) return '';
  const match = text.match(/\bgm-\d+\b/i);
  return match ? match[0] : '';
}

function isGmProposalActionable(message) {
  const proposalId = gmProposalId(message);
  return Boolean(proposalId && !lanchat.isProposalHandled(proposalId));
}

async function sendGmDecision(proposalId, decision) {
  const message = buildGmDecisionMessage(proposalId, decision);
  if (!message) return;
  await lanchat.sendMessage(message.text, { ...(message.options || {}), skipStructuredRoute: true });
  lanchat.markProposalHandled(proposalId);
  lanchat.dismissDisclosureByProposal(proposalId);
}

function isDisclosureActionSendable(action) {
  return Boolean(
    buildGmDisclosureActionMessage(action) ||
    buildParticipantDisclosureDraft(action, currentDisclosure.value)
  );
}

async function sendDisclosureAction(action) {
  const message = buildGmDisclosureActionMessage(action);
  if (message) {
    await lanchat.sendMessage(message.text, { ...(message.options || {}), skipStructuredRoute: true });
    return;
  }
  const draftText = buildParticipantDisclosureDraft(action, currentDisclosure.value);
  if (!draftText) return;
  draft.value = draftText;
  mentionCandidates.value = [];
  mentionActiveIndex.value = 0;
  await nextTick();
  draftInput.value?.focus?.();
}

async function onAddAgent() {
  if (!agentForm.name.trim()) return;
  await lanchat.addAgent({ name: agentForm.name.trim(), persona: agentForm.persona });
  agentForm.name = '';
  agentForm.persona = '';
  showAddAgent.value = false;
  addAgentBackdropPointerStarted.value = false;
}

function onAddAgentBackdropPointerDown(event) {
  addAgentBackdropPointerStarted.value = event.target === event.currentTarget;
}

function onAddAgentBackdropPointerUp(event) {
  const shouldClose =
    addAgentBackdropPointerStarted.value &&
    event.target === event.currentTarget;
  addAgentBackdropPointerStarted.value = false;
  if (shouldClose) {
    showAddAgent.value = false;
  }
}

async function addRoleTemplateBundle(bundle) {
  const keys = Array.isArray(bundle?.roles) ? bundle.roles : [];
  for (const key of keys) {
    const role = roleTemplates.find((item) => item.key === key);
    if (!role) continue;
    await lanchat.addAgent({ name: role.name, persona: role.persona });
  }
  agentForm.name = '';
  agentForm.persona = '';
  showAddAgent.value = false;
}

async function addDefaultExpertGroup() {
  const existingNames = new Set(
    (s.agents || [])
      .map((agent) => String(agent.name || '').trim())
      .filter(Boolean)
  );
  for (const expert of selectedExpertPayloads.value) {
    const name = String(expert?.name || '').trim();
    if (!name || existingNames.has(name)) continue;
    const res = await lanchat.addAgent(expert);
    if (res && res.ok) existingNames.add(name);
  }
}

async function onRemoveAgent(agentId) {
  await lanchat.removeAgent(agentId);
}

function onDraftInput() {
  const text = draft.value;
  syncSelectedTargetFromDraft(text);
  resizeDraftInput();
  const at = text.lastIndexOf('@');
  if (at === -1) {
    closeMentionPicker();
    return;
  }
  const prefix = text.slice(at + 1);
  if (prefix.includes(' ')) {
    closeMentionPicker();
    return;
  }
  manualMentionOpen.value = false;
  mentionCandidates.value = buildMentionCandidates(prefix);
  if (mentionCandidates.value.length) {
    mentionActiveIndex.value = Math.min(mentionActiveIndex.value, mentionCandidates.value.length - 1);
  } else {
    mentionActiveIndex.value = 0;
  }
}

function resizeDraftInput() {
  nextTick(() => {
    const el = draftInput.value;
    if (!el) return;
    const styles = window.getComputedStyle(el);
    const lineHeight = Number.parseFloat(styles.lineHeight) || 22;
    const paddingY =
      (Number.parseFloat(styles.paddingTop) || 0) +
      (Number.parseFloat(styles.paddingBottom) || 0);
    const borderY =
      (Number.parseFloat(styles.borderTopWidth) || 0) +
      (Number.parseFloat(styles.borderBottomWidth) || 0);
    const minHeight = Math.ceil(lineHeight * DRAFT_MIN_ROWS + paddingY + borderY);
    const maxHeight = Math.ceil(lineHeight * DRAFT_MAX_ROWS + paddingY + borderY);
    el.style.height = `${minHeight}px`;
    const nextHeight = Math.min(maxHeight, Math.max(minHeight, el.scrollHeight));
    el.style.height = `${nextHeight}px`;
    el.style.overflowY = el.scrollHeight > maxHeight ? 'auto' : 'hidden';
  });
}

function syncSelectedTargetFromDraft(text = draft.value) {
  const key = targetKeyFromMentionText(text, targetOptions.value);
  selectedTargetKey.value = key ? resolveSelectedTargetKey(key, targetOptions.value) : '';
  applyInputRouteState();
}

function buildMentionCandidates(prefix = '') {
  const query = String(prefix || '').toLowerCase();
  const members = (s.memberDetails.length
    ? s.memberDetails
        .filter((member) => member.member_id !== s.peerId)
        .map((member) => ({ name: member.nickname, isAgent: false }))
    : s.members
        .filter((name) => name !== s.nickname)
        .map((name) => ({ name, isAgent: false })));
  const gm = [{ name: 'GM', isAgent: true, isGM: true, hint: '主持 / 仲裁', targetKey: 'gm' }];
  const group = s.agents.length
    ? [{ name: '全部AI助手', isAgent: true, isGroup: true, hint: '所有AI助手', targetKey: 'group' }]
    : [];
  const agents = s.agents
    .filter((a) => String(a.name || '').toLowerCase() !== 'gm')
    .map((a) => ({ name: a.name, isAgent: true, targetKey: `agent:${a.agent_id || a.name}` }));
  return [...gm, ...group, ...agents, ...members].filter((c) => (
    !query || String(c.name || '').toLowerCase().startsWith(query)
  ));
}

function openMentionPicker() {
  manualMentionOpen.value = true;
  mentionCandidates.value = buildMentionCandidates('');
  mentionActiveIndex.value = 0;
  nextTick(() => {
    draftInput.value?.focus?.();
  });
}

function closeMentionPicker() {
  manualMentionOpen.value = false;
  mentionCandidates.value = [];
  mentionActiveIndex.value = 0;
}

function toggleMentionPicker() {
  if (mentionCandidates.value.length && manualMentionOpen.value) {
    closeMentionPicker();
    return;
  }
  openMentionPicker();
}

function onDocumentPointerDown(event) {
  if (!mentionCandidates.value.length) return;
  const target = event?.target;
  const root = mentionRoot.value;
  if (root && target && root.contains(target)) return;
  closeMentionPicker();
}

function mentionIcon(candidate) {
  if (candidate?.isGM) return '🎲 ';
  if (candidate?.isGroup) return '◎ ';
  if (candidate?.isAgent) return '🤖 ';
  return '';
}

function pickMention(c) {
  const text = draft.value;
  const at = text.lastIndexOf('@');
  const insert = `@${c.name} `;
  if (at >= 0) {
    const prefix = text.slice(at + 1);
    if (!prefix.includes(' ')) {
      draft.value = text.slice(0, at) + insert + text.slice(at + 1 + prefix.length);
    } else {
      draft.value = `${text}${text && !/\s$/.test(text) ? ' ' : ''}${insert}`;
    }
  } else {
    draft.value = `${text}${text && !/\s$/.test(text) ? ' ' : ''}${insert}`;
  }
  syncSelectedTargetFromDraft();
  closeMentionPicker();
  nextTick(() => {
    draftInput.value?.focus?.();
  });
}

function messageOptionsForText(text, pending = null) {
  const trimmed = String(text || '').trim();
  const generationMetadata = s.role === 'host' ? lanchat.generationOptionsMetadata() : {};
  syncSelectedTargetFromDraft(text);
  const routedDraftAction = effectiveDraftAction('chat', trimmed);
  const targetPayload = targetPayloadForKey(selectedTargetKey.value, targetOptions.value);
  if (/^@GM(?:\s|$)/i.test(trimmed)) {
    if (!pending?.correlationId && !Object.keys(generationMetadata).length) {
      return buildManualGmMessageOptions(s.role);
    }
    const options = buildManualGmMessageOptions(s.role);
    if (pending?.correlationId) options.correlation_id = pending.correlationId;
    options.metadata = {
      ...(options.metadata || {}),
      ...generationMetadata,
    };
    return options;
  }
  const metadata = {
    ...generationMetadata,
    draft_action: routedDraftAction,
    target_scope: targetPayload.scope || 'scene',
  };
  if (targetPayload.agentId) metadata.target_agent_id = targetPayload.agentId;
  if (targetPayload.agentName) metadata.target_agent_name = targetPayload.agentName;
  if (targetPayload.planId) metadata.target_plan_id = targetPayload.planId;
  const options = { metadata };
  if (pending?.correlationId) options.correlation_id = pending.correlationId;
  if (pending?.targetAgentId) options.target_agent_id = pending.targetAgentId;
  return options;
}

function onVlmToggle(event) {
  const enabled = Boolean(event?.target?.checked);
  lanchat.setGenerationOptions({
    vlmEnabled: enabled,
    vlmMaxTargets: enabled ? 1 : 0,
  });
}

function disclosureActionLabel(action) {
  return {
    confirm_plan: '确认方案',
    request_clarification: '继续澄清',
    pause_discussion: '暂停讨论',
    pause_after_batch: '批次后暂停',
    add_intervention: '补充介入',
    request_review: '请求审查',
    approve_final: '确认结果',
    request_repair: '要求修复',
    continue_generation: '继续生成',
    add_note: '补充想法',
    request_add: '请求新增',
    request_modify: '请求调整',
    report_issue: '报告问题',
    propose_seed_plan: '整理方案',
    resolve_conflict: '仲裁冲突',
    control_pace: '控制节奏',
    execute_constraints: '执行约束',
    report_blocker: '报告阻塞',
    confirm_conflict_resolution: '确认仲裁',
    reject_conflict_resolution: '拒绝仲裁',
  }[action] || '查看状态';
}

function resourceDiagnosisLabel(diagnosis) {
  if (!diagnosis || typeof diagnosis !== 'object') return '';
  const state = String(diagnosis.state || '');
  const reasons = Array.isArray(diagnosis.reasons)
    ? diagnosis.reasons.map((item) => String(item))
    : [];
  if (state === 'stopped' || reasons.includes('scheduler_stopped')) {
    return '资源状态：生成已停止，需要重新开始后续生成。';
  }
  if (state === 'paused' || reasons.includes('paused_sessions')) {
    return '资源状态：已暂停，等待房主继续或取消。';
  }
  if (state === 'saturated' || reasons.includes('queue_at_capacity') || reasons.includes('recent_queue_full')) {
    return '资源状态：队列拥堵，系统会先处理已排队任务。';
  }
  if (state === 'strained' || reasons.includes('queue_near_capacity') || reasons.includes('import_stage_busy')) {
    return '资源状态：负载较高，建议先等待当前批次完成。';
  }
  if (state === 'active') {
    return '资源状态：正在处理生成任务。';
  }
  return '';
}

function onDraftKeydown(e) {
  if (mentionCandidates.value.length) {
    if (e.key === 'ArrowDown') {
      e.preventDefault();
      mentionActiveIndex.value = (mentionActiveIndex.value + 1) % mentionCandidates.value.length;
      return;
    }
    if (e.key === 'ArrowUp') {
      e.preventDefault();
      mentionActiveIndex.value =
        (mentionActiveIndex.value - 1 + mentionCandidates.value.length) % mentionCandidates.value.length;
      return;
    }
    if (e.key === 'Enter' || e.key === 'Tab') {
      e.preventDefault();
      pickMention(mentionCandidates.value[mentionActiveIndex.value] || mentionCandidates.value[0]);
      return;
    }
    if (e.key === 'Escape') {
      e.preventDefault();
      closeMentionPicker();
      return;
    }
  }
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
    e.preventDefault();
    onSend();
  }
}

// 新消息自动滚到底
watch(
  () => s.messages.length,
  async () => {
    const latest = s.messages[s.messages.length - 1];
    if (isPendingReplyMessage(latest)) clearPendingReply(latest?.correlation_id);
    await nextTick();
    if (msgRef.value) msgRef.value.scrollTop = msgRef.value.scrollHeight;
  }
);

watch(
  () => currentDisclosure.value?.event_id,
  async () => {
    await nextTick();
    if (msgRef.value) msgRef.value.scrollTop = msgRef.value.scrollHeight;
  }
);

watch(
  pendingReply,
  async () => {
    await nextTick();
    if (msgRef.value) msgRef.value.scrollTop = msgRef.value.scrollHeight;
  }
);

watch(
  targetOptions,
  () => {
    syncSelectedTargetFromDraft();
  },
  { immediate: true }
);

watch(
  draft,
  () => {
    resizeDraftInput();
  },
  { flush: 'post' }
);

watch(
  () => s.workspaceMode,
  (mode) => {
    if (mode && mode !== selectedWorkspaceMode.value) {
      selectedWorkspaceMode.value = mode;
    }
  }
);
</script>

<style scoped>
.lanchat-scrollbar {
  scrollbar-width: thin;
  scrollbar-color: #444 transparent;
}

.lanchat-scrollbar::-webkit-scrollbar {
  width: 4px;
}

.lanchat-scrollbar::-webkit-scrollbar-track {
  background: transparent;
}

.lanchat-scrollbar::-webkit-scrollbar-thumb {
  background: #444;
  border-radius: 10px;
}

.lanchat-scrollbar::-webkit-scrollbar-thumb:hover {
  background: #84a65b;
}

.lanchat-message-bubble {
  max-width: 88%;
  min-width: 0;
  overflow: hidden;
  overflow-wrap: anywhere;
  white-space: pre-wrap;
  word-break: break-word;
}
</style>
