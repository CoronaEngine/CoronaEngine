<template>
  <div
    class="flex-1 min-h-0 h-full w-full rounded-lg overflow-hidden relative bg-[#282828]/90 flex flex-col text-white font-sans"
  >
    <DockTitleBar
      v-if="!isDocked"
      title="局域网聊天"
      extraClass="bg-[#84A65B]"
      routePath="/Network"
      @close="closeFloat"
    />

    <div class="flex-1 overflow-y-auto custom-scrollbar p-4 space-y-4 text-sm">
      <!-- ═══ 会话控制 ═══ -->
      <div class="space-y-3">
        <div class="flex flex-col gap-1">
          <label class="text-gray-400">实例名称</label>
          <input
            v-model="instanceName"
            type="text"
            maxlength="31"
            placeholder="输入名称..."
            class="bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1.5 text-white focus:border-[#4a9eff] focus:outline-none"
          />
        </div>

        <div class="flex flex-col gap-1">
          <label class="text-gray-400">端口 (UDP)</label>
          <input
            v-model.number="port"
            type="number"
            min="1024"
            max="65535"
            class="bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1.5 text-white w-24 focus:border-[#4a9eff] focus:outline-none"
          />
        </div>

        <div class="flex gap-2">
          <button
            v-if="!sessionActive"
            @click="startHostSession"
            class="px-4 py-1.5 bg-[#4a9eff] hover:bg-[#3a8eef] rounded text-white font-medium transition-colors"
          >
            创建房间
          </button>
          <button
            v-else
            @click="stopSession"
            class="px-4 py-1.5 bg-red-600 hover:bg-red-500 rounded text-white font-medium transition-colors"
          >
            停止会话
          </button>
        </div>

        <div v-if="sessionActive" class="flex items-center gap-2 text-green-400">
          <span class="w-2 h-2 rounded-full bg-green-400 animate-pulse"></span>
          会话运行中 — {{ roleLabel }} — 端口 {{ port }}
        </div>
        <div v-if="sessionActive && localIp" class="text-gray-400">本机 IP：{{ localIp }}</div>
        <div v-if="sessionActive && sessionRole === 'client' && hostAddress" class="text-gray-400">
          房主：{{ hostAddress }}:{{ hostPort }}
        </div>
        <div v-if="errorMsg" class="text-red-400">{{ errorMsg }}</div>
      </div>

      <!-- ═══ 手动连接 ═══ -->
      <div class="border-t border-gray-700 pt-4 space-y-3">
        <span class="text-gray-400">手动连接</span>
        <div class="flex flex-col gap-1">
          <label class="text-gray-500">IP 地址</label>
          <input
            v-model="remoteIp"
            type="text"
            placeholder="192.168.1.100"
            class="bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1.5 text-white focus:border-[#4a9eff] focus:outline-none"
          />
        </div>
        <div class="flex gap-2">
          <div class="flex flex-col gap-1 flex-1">
            <label class="text-gray-500">端口</label>
            <input
              v-model.number="remotePort"
              type="number"
              min="1024"
              max="65535"
              class="bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1.5 text-white w-24 focus:border-[#4a9eff] focus:outline-none"
            />
          </div>
          <div class="flex flex-col gap-1 flex-1">
            <label class="text-gray-500">对方名称</label>
            <input
              v-model="remotePeerName"
              type="text"
              placeholder="可选"
              class="bg-[#1e1e1e] border border-gray-600 rounded px-2 py-1.5 text-white focus:border-[#4a9eff] focus:outline-none"
            />
          </div>
        </div>
        <button
          @click="doConnectToPeer"
          :disabled="!remoteIp.trim() || connectStatus === 'connecting'"
          class="px-4 py-1.5 bg-[#84A65B] hover:bg-[#6f8d4a] rounded text-white font-medium transition-colors disabled:opacity-50 disabled:cursor-not-allowed"
        >
          加入房间
        </button>
        <div v-if="connectStatus === 'connecting'" class="text-yellow-400 text-xs">
          连接请求已发送，等待握手...
        </div>
        <div v-else-if="connectStatus === 'connected'" class="text-green-400 text-xs">已连接</div>
        <div v-else-if="connectStatus" class="text-red-400 text-xs">{{ connectStatus }}</div>
      </div>

      <!-- ═══ Peer 列表 ═══ -->
      <div class="border-t border-gray-700 pt-4">
        <div class="flex items-center justify-between mb-2">
          <span class="text-gray-400">已连接用户数</span>
          <span class="text-gray-500 tabular-nums">{{ peers.length }}</span>
        </div>

        <div v-if="peers.length === 0" class="text-gray-500 italic">
          {{ sessionActive ? '等待其他用户加入...' : '创建房间或输入房主 IP 加入' }}
        </div>

        <div v-else class="space-y-1">
          <div
            v-for="peer in peers"
            :key="peer.name"
            class="flex items-center gap-2 px-2 py-1 bg-[#1e1e1e] rounded"
          >
            <span class="w-2 h-2 rounded-full bg-green-400"></span>
            <span class="text-gray-300 truncate">{{ peer.name }}</span>
            <span class="text-gray-600 text-[10px] ml-auto">{{ peer.id }}</span>
          </div>
        </div>
      </div>

      <!-- ═══ 文件同步状态 ═══ -->
      <div v-if="fileStatus" class="border-t border-gray-700 pt-3">
        <div v-if="fileStatus.type === 'transferring'" class="text-yellow-400 text-xs">
          正在接收文件: {{ fileStatus.path }} ({{ Math.round(fileStatus.progress * 100) }}%)
        </div>
        <div v-else-if="fileStatus.type === 'success'" class="text-green-400 text-xs">
          文件同步完成: {{ fileStatus.path }}
        </div>
        <div v-else-if="fileStatus.type === 'error'" class="text-red-400 text-xs">
          文件同步失败: {{ fileStatus.path }}
        </div>
      </div>

      <!-- ═══ 远程 Actor 日志 ═══ -->
      <div v-if="remoteActorLog" class="border-t border-gray-700 pt-3 text-green-400 text-xs">
        {{ remoteActorLog }}
      </div>

      <!-- ═══ 说明 ═══ -->
      <div class="border-t border-gray-700 pt-3 text-gray-500 leading-relaxed">
        <p class="mb-1 font-medium text-gray-400">使用说明</p>
        <ul class="list-disc list-inside space-y-1">
          <li>房主点击"创建房间"，客户端输入房主 IP 后点击"加入房间"</li>
          <li>两端端口需要一致，默认使用 27960/UDP</li>
          <li>同时编辑同一物体时，最后写入者胜出 (LWW)</li>
        </ul>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, ref, onMounted, onUnmounted } from 'vue';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import { Bridge, networkService } from '@/utils/bridge';
import { useDockPanel } from '@/composables/useDockPanel.js';
import { coronaEventBus } from '@/utils/eventBus';

const { closePanel: closeDockPanel, isDocked } = useDockPanel();
const instanceName = ref('');
const port = ref(27960);
const sessionActive = ref(false);
const sessionRole = ref('none');
const localIp = ref('');
const hostAddress = ref('');
const hostPort = ref(0);
const errorMsg = ref('');
const peers = ref([]);

const remoteIp = ref('');
const remotePort = ref(27960);
const remotePeerName = ref('');
const connectStatus = ref(''); // '' | 'connecting' | 'connected' | error
const fileStatus = ref(null); // null | { type: 'transferring'|'success'|'error', path, progress? }
const remoteActorLog = ref(''); // latest remote actor creation log

let pollTimer = null;
const CONNECT_TIMEOUT_MS = 5000;
const PENDING_POLL_BATCH_LIMIT = 16;
const connectionAttemptStartedAt = ref(0);
const ownershipClaimTimes = new Map();
const currentSceneName = ref('Scene/default.scene');
const snapshotRequestedScenes = new Set();
const remoteRegisteredActorIdentities = new Set();
const lastBroadcastSnapshotHashes = new Map();
const snapshotActorCreateKeys = new Set();

const roleLabel = computed(() => {
  if (sessionRole.value === 'host') return '房主';
  if (sessionRole.value === 'client') return '客户端';
  return '未加入';
});

function applySessionInfo(info) {
  if (!info) return;
  const active = Boolean(info.active ?? sessionActive.value);
  if (!active) {
    resetSessionInfo();
    return;
  }
  sessionActive.value = true;
  sessionRole.value = info.role || sessionRole.value || 'none';
  localIp.value = info.local_ip || localIp.value || '';
  hostAddress.value = info.host_address || '';
  hostPort.value = info.host_port || 0;
  const listenPort = Number(info.listen_port || 0);
  if (listenPort > 0) {
    port.value = listenPort;
  }
}

function resetSessionInfo() {
  sessionActive.value = false;
  sessionRole.value = 'none';
  localIp.value = '';
  hostAddress.value = '';
  hostPort.value = 0;
  peers.value = [];
  connectStatus.value = '';
  connectionAttemptStartedAt.value = 0;
  snapshotRequestedScenes.clear();
  remoteRegisteredActorIdentities.clear();
  lastBroadcastSnapshotHashes.clear();
  snapshotActorCreateKeys.clear();
}

function nativeRoleLabel(role) {
  if (role === 'host') return '房主';
  if (role === 'client') return '客户端';
  return '未知角色';
}

async function attachExistingSession({ showErrors = false } = {}) {
  try {
    const info = await networkService.getSessionInfo();
    if (!info || !info.active) {
      return null;
    }
    applySessionInfo(info);
    startPolling();
    return info;
  } catch (e) {
    if (showErrors) {
      errorMsg.value = e?.message || '读取网络会话失败';
    }
    return null;
  }
}

async function ensureProjectRoot() {
  try {
    const mod = await import('@/utils/bridge');
    const raw = await mod.projectSettingsService.getActiveProjectInfo();
    const info = raw?.data || raw || {};
    const projPath = info?.project_path || '';
    if (projPath) {
      await networkService.setProjectRoot(projPath);
    }
  } catch (_) {
    /* best effort */
  }
}

async function startSessionAsRole(role) {
  errorMsg.value = '';
  try {
    const existingSession = await attachExistingSession({ showErrors: true });
    if (existingSession?.active) {
      const existingRole = existingSession.role || sessionRole.value || 'none';
      if (existingRole !== 'none' && existingRole !== role) {
        errorMsg.value = `已有网络会话正在运行（当前角色：${nativeRoleLabel(existingRole)}），请先停止当前会话后再切换角色。`;
        return false;
      }
      return true;
    }

    await ensureProjectRoot();
    const res = await networkService.startSession(instanceName.value, 0, port.value, role);
    if (res && res.ok) {
      applySessionInfo(res);
      startPolling();
      return true;
    } else {
      errorMsg.value = (res && res.error) || '启动失败';
      return false;
    }
  } catch (e) {
    errorMsg.value = e.message;
    return false;
  }
}

async function startHostSession() {
  return startSessionAsRole('host');
}

async function stopSession() {
  errorMsg.value = '';
  try {
    await networkService.stopSession();
    resetSessionInfo();
    stopPolling();
  } catch (e) {
    errorMsg.value = e.message;
  }
}

async function pollPeers() {
  try {
    const res = await networkService.getPeerCount();
    applySessionInfo(res);
    if (res && res.active === false) {
      stopPolling();
      return;
    }
    if (res && res.peer_count !== undefined) {
      const count = Number(res.peer_count || 0);
      if (peers.value.length < count) {
        while (peers.value.length < count) {
          peers.value.push({
            name: `已连接用户 ${peers.value.length + 1}`,
            id: 'handshake confirmed',
          });
        }
      } else
        while (peers.value.length > count) {
          peers.value.pop();
        }
      if (connectStatus.value === 'connecting') {
        if (count > 0) {
          connectStatus.value = 'connected';
          connectionAttemptStartedAt.value = 0;
        } else if (
          connectionAttemptStartedAt.value > 0 &&
          Date.now() - connectionAttemptStartedAt.value >= CONNECT_TIMEOUT_MS
        ) {
          connectStatus.value = '无法连接到房主';
          connectionAttemptStartedAt.value = 0;
        }
      }
      if (count > 0 && sessionRole.value === 'client') {
        await requestSceneSnapshotOnce(currentSceneName.value);
      }
      if (count > 0 && sessionRole.value === 'host') {
        await broadcastCurrentSceneSnapshot(currentSceneName.value, false, false);
      }
    }

    // Poll for pending remote actor creation (file transfer completed) before
    // applying snapshots/state updates that may target those actors.
    try {
      for (let i = 0; i < PENDING_POLL_BATCH_LIMIT; i += 1) {
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
    } catch (_) {
      /* best effort — actor creation polling is secondary */
    }

    try {
      for (let i = 0; i < PENDING_POLL_BATCH_LIMIT; i += 1) {
        const pendingRequest = await networkService.pollPendingSceneSnapshotRequest();
        if (!pendingRequest || !pendingRequest.has_pending) break;
        if (sessionRole.value === 'host') {
          const sceneName = pendingRequest.scene_name || currentSceneName.value;
          await broadcastCurrentSceneSnapshot(sceneName, true, true);
        }
      }
    } catch (_) {
      /* best effort — snapshot request polling is secondary */
    }

    try {
      for (let i = 0; i < PENDING_POLL_BATCH_LIMIT; i += 1) {
        const pendingSnapshot = await networkService.pollPendingSceneSnapshot();
        if (!pendingSnapshot || !pendingSnapshot.has_pending) break;
        await applyRemoteSceneSnapshot(
          pendingSnapshot.scene_name || currentSceneName.value,
          pendingSnapshot.snapshot_json
        );
      }
    } catch (_) {
      /* best effort — snapshot polling is secondary */
    }

    try {
      for (let i = 0; i < PENDING_POLL_BATCH_LIMIT; i += 1) {
        const pendingState = await networkService.pollPendingActorStateUpdate();
        if (!pendingState || !pendingState.has_pending) break;
        let actorData = {};
        try {
          actorData = JSON.parse(pendingState.actor_json || '{}');
        } catch (_) {
          actorData = {};
        }
        actorData.actor_guid = actorData.actor_guid || pendingState.actor_guid || '';
        actorData._suppress_network_broadcast = true;
        const updated = await Bridge.callCEF('SceneTools', 'apply_actor_state_internal', [
          pendingState.scene_name || currentSceneName.value,
          pendingState.actor_guid || actorData.actor_guid || '',
          actorData,
        ]);
        const updatedData = unwrapCefResult(updated);
        if (updatedData?.status !== 'error') {
          remoteActorLog.value = `远程 Actor 状态已更新: ${actorData.name || actorData.actor_guid || 'unknown'}`;
          setTimeout(() => {
            remoteActorLog.value = '';
          }, 3000);
        }
      }
    } catch (_) {
      /* best effort — state sync is secondary */
    }

    try {
      for (let i = 0; i < PENDING_POLL_BATCH_LIMIT; i += 1) {
        const pendingTransform = await networkService.pollPendingActorTransform();
        if (!pendingTransform || !pendingTransform.has_pending) break;
        const actorData = {
          actor_guid: pendingTransform.actor_guid || '',
          geometry: pendingTransform.geometry || {},
          source_user_id: pendingTransform.source_user_id || '',
          correlation_id: pendingTransform.correlation_id || '',
        };
        const updated = await Bridge.callCEF('SceneTools', 'apply_actor_transform_internal', [
          pendingTransform.scene_name || 'Scene/default.scene',
          pendingTransform.actor_guid || '',
          actorData,
        ]);
        const updatedData = unwrapCefResult(updated);
        if (updatedData?.status !== 'error') {
          remoteActorLog.value = `远程 Actor 已更新: ${pendingTransform.actor_guid || 'unknown'}`;
          setTimeout(() => {
            remoteActorLog.value = '';
          }, 3000);
        }
      }
    } catch (_) {
      /* best effort — transform sync is demo-grade */
    }

    try {
      for (let i = 0; i < PENDING_POLL_BATCH_LIMIT; i += 1) {
        const pendingDelete = await networkService.pollPendingActorDelete();
        if (!pendingDelete || !pendingDelete.has_pending) break;
        const deleted = await Bridge.callCEF('SceneTools', 'remove_actor_internal', [
          pendingDelete.scene_name || 'Scene/default.scene',
          pendingDelete.actor_guid || '',
          pendingDelete.actor_name || '',
        ]);
        const deletedData = unwrapCefResult(deleted);
        if (deletedData?.status !== 'error') {
          remoteActorLog.value = `远程 Actor 已删除: ${pendingDelete.actor_name || pendingDelete.actor_guid || 'unknown'}`;
          setTimeout(() => {
            remoteActorLog.value = '';
          }, 3000);
        }
      }
    } catch (_) {
      /* best effort — actor delete polling is secondary */
    }
  } catch (e) {
    // ignore polling errors
  }
}

function startPolling() {
  stopPolling();
  pollTimer = setInterval(pollPeers, 2000);
}

function stopPolling() {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
}

async function doConnectToPeer() {
  connectStatus.value = 'connecting';
  connectionAttemptStartedAt.value = Date.now();
  try {
    if (!sessionActive.value) {
      const started = await startSessionAsRole('client');
      if (!started) {
        connectStatus.value = errorMsg.value || '本地会话启动失败';
        connectionAttemptStartedAt.value = 0;
        return;
      }
    }
    const peerName = remotePeerName.value || remoteIp.value;
    const res = await networkService.connectToPeer(remoteIp.value, remotePort.value, peerName);
    if (res && res.ok) {
      applySessionInfo(res);
      startPolling();
      await pollPeers();
      await requestSceneSnapshotOnce(currentSceneName.value);
    } else {
      connectStatus.value = (res && res.error) || '连接失败';
      connectionAttemptStartedAt.value = 0;
    }
  } catch (e) {
    connectStatus.value = e.message;
    connectionAttemptStartedAt.value = 0;
  }
}

function hashString(str) {
  let hash = 0;
  for (let i = 0; i < str.length; i++) {
    const ch = str.charCodeAt(i);
    hash = (hash << 5) - hash + ch;
    hash |= 0;
  }
  return hash >>> 0;
}

function stableStringify(value) {
  if (Array.isArray(value)) {
    return `[${value.map((item) => stableStringify(item)).join(',')}]`;
  }
  if (value && typeof value === 'object') {
    const keys = Object.keys(value).sort();
    return `{${keys.map((key) => `${JSON.stringify(key)}:${stableStringify(value[key])}`).join(',')}}`;
  }
  return JSON.stringify(value);
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

const AI_SCENE_FRAMEWORK_SYNC_NAMES = new Set([
  '__room_box',
  '__room_terrain',
  '__terrain_grass',
  '__terrain_boundary',
  '__interior_floor',
  '__foundation_surface',
]);
const AI_SCENE_FRAMEWORK_SYNC_PREFIXES = ['__shell_'];

function isAiSceneFrameworkSyncName(value) {
  const text = String(value || '').trim();
  const leaf = lastPathPart(text);
  return (
    AI_SCENE_FRAMEWORK_SYNC_NAMES.has(text) ||
    AI_SCENE_FRAMEWORK_SYNC_NAMES.has(leaf) ||
    AI_SCENE_FRAMEWORK_SYNC_PREFIXES.some(
      (prefix) => text.startsWith(prefix) || leaf.startsWith(prefix)
    )
  );
}

function isInternalSyncName(value) {
  const text = String(value || '').trim();
  return text.startsWith('__') || lastPathPart(text).startsWith('__');
}

function isInternalActorSyncName(value) {
  return isInternalSyncName(value) && !isAiSceneFrameworkSyncName(value);
}

function isActorSyncable(actorData) {
  if (!actorData) return false;
  if (actorData._suppress_network_broadcast) return false;
  if (actorData.actor_type === 'actor') return false;
  if (!actorData.geometry || typeof actorData.geometry !== 'object') return false;
  if (isInternalActorSyncName(actorData.name)) return false;
  if (isInternalSyncName(actorData.scene)) return false;
  return Boolean(actorData.path || actorData.model);
}

function rememberSceneName(sceneName) {
  const value = String(sceneName || '').trim();
  if (value) currentSceneName.value = value;
  return currentSceneName.value || 'Scene/default.scene';
}

function actorCreateBroadcastKey(sceneName, actorGuid, modelPath) {
  return `${sceneName}:${actorGuid}:${modelPath}`;
}

function rememberActorCreateBroadcast(sceneName, actorGuid, modelPath) {
  if (!sceneName || !actorGuid || !modelPath) return;
  snapshotActorCreateKeys.add(actorCreateBroadcastKey(sceneName, actorGuid, modelPath));
}

function forgetActorCreateBroadcast(sceneName, actorGuid) {
  if (!sceneName || !actorGuid) return;
  const prefix = `${sceneName}:${actorGuid}:`;
  for (const key of [...snapshotActorCreateKeys]) {
    if (key.startsWith(prefix)) {
      snapshotActorCreateKeys.delete(key);
    }
  }
}

function closeFloat() {
  closeDockPanel();
}

function unwrapCefResult(res) {
  return res && res.data !== undefined ? res.data : res;
}

async function getActorSnapshot(sceneName) {
  const raw = await Bridge.callCEF('SceneTools', 'get_actor_sync_snapshot', [sceneName]);
  return unwrapCefResult(raw);
}

async function broadcastCurrentSceneSnapshot(sceneName, includeActorCreates, force = false) {
  const targetScene = rememberSceneName(sceneName);
  const snapshot = await getActorSnapshot(targetScene);
  if (!snapshot || snapshot.status === 'error') return;
  const actors = Array.isArray(snapshot.actors) ? snapshot.actors : [];
  if (includeActorCreates) {
    for (const actor of actors) {
      if (!isActorSyncable(actor)) continue;
      const actorGuid = actor.actor_guid || '';
      const modelPath = actor.path || actor.model || '';
      if (!actorGuid || !modelPath) continue;
      const snapshotCreateKey = actorCreateBroadcastKey(targetScene, actorGuid, modelPath);
      if (snapshotActorCreateKeys.has(snapshotCreateKey)) continue;
      const sent = await networkService
        .broadcastActorCreate(actorGuid, targetScene, modelPath, { ...actor, scene: targetScene })
        .then(() => true)
        .catch(() => false);
      if (sent) {
        rememberActorCreateBroadcast(targetScene, actorGuid, modelPath);
      }
    }
  }
  const snapshotHash = hashString(stableStringify(snapshot));
  if (!force && lastBroadcastSnapshotHashes.get(targetScene) === snapshotHash) return;
  lastBroadcastSnapshotHashes.set(targetScene, snapshotHash);
  await networkService.broadcastSceneSnapshot(targetScene, snapshot).catch(() => {});
}

async function requestSceneSnapshotOnce(sceneName) {
  if (!sessionActive.value || sessionRole.value !== 'client') return;
  const targetScene = rememberSceneName(sceneName);
  if (snapshotRequestedScenes.has(targetScene)) return;
  snapshotRequestedScenes.add(targetScene);
  await networkService.requestSceneSnapshot(targetScene).catch(() => {
    snapshotRequestedScenes.delete(targetScene);
  });
}

async function applyRemoteSceneSnapshot(sceneName, snapshotPayload) {
  const targetScene = rememberSceneName(sceneName);
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
    if (changedActors.length > 0) {
      remoteActorLog.value = `远程场景快照已同步: ${changedActors.length} 个 Actor`;
      setTimeout(() => {
        remoteActorLog.value = '';
      }, 3000);
    }
  } finally {
    await networkService.setSyncPaused(false);
  }
}

async function registerActorIdentityFromData(actorData, locallyOwned = true) {
  if (!sessionActive.value || !actorData) return false;
  const actorGuid = actorData.actor_guid || '';
  const actorHandle = actorData.handle || '';
  if (!actorGuid || !actorHandle) return false;
  const identityKey = `${actorGuid}:${actorHandle}:${locallyOwned ? 'local' : 'remote'}`;
  if (!locallyOwned && remoteRegisteredActorIdentities.has(identityKey)) return true;
  try {
    const registered = await networkService.registerActorIdentity(
      actorGuid,
      actorHandle,
      locallyOwned
    );
    if (registered?.ok !== true) {
      remoteActorLog.value = `Actor 身份注册失败: ${actorData.name || actorGuid}`;
      setTimeout(() => {
        remoteActorLog.value = '';
      }, 3000);
      return false;
    }
    if (!locallyOwned) {
      remoteRegisteredActorIdentities.add(identityKey);
    }
    return true;
  } catch (_) {
    remoteActorLog.value = `Actor 身份注册失败: ${actorData.name || actorGuid}`;
    setTimeout(() => {
      remoteActorLog.value = '';
    }, 3000);
    return false;
  }
}

onMounted(() => {
  // Try to auto-fill a default name
  if (!instanceName.value) {
    instanceName.value = 'Editor-' + Math.random().toString(36).slice(2, 8);
  }

  attachExistingSession();

  // Listen for actor-sync-broadcast from Python (Actor creation triggered locally,
  // needs to be forwarded to remote peers)
  coronaEventBus.on('actor-sync-broadcast', (actorData) => {
    if (!sessionActive.value) return;
    if (!isActorSyncable(actorData)) return;
    const modelPath = actorData.path || actorData.model || '';
    if (!modelPath) return;
    // Get scene name from the actor's parent scene if available
    const sceneName = rememberSceneName(actorData.scene || 'Scene/default.scene');
    const actorGuid =
      actorData.actor_guid ||
      `actor-${hashString(`${sceneName}|${modelPath}|${actorData.name || ''}`)}`;
    actorData.actor_guid = actorGuid;
    registerActorIdentityFromData(actorData);
    networkService
      .broadcastActorCreate(actorGuid, sceneName, modelPath, actorData)
      .then(() => {
        rememberActorCreateBroadcast(sceneName, actorGuid, modelPath);
      })
      .catch(() => {});
  });

  coronaEventBus.on('actor-transform-sync-broadcast', (actorData) => {
    if (!sessionActive.value || !actorData) return;
    const actorGuid = actorData.actor_guid || '';
    if (!actorGuid) return;
    const sceneName = rememberSceneName(actorData.scene || 'Scene/default.scene');
    networkService.broadcastActorTransform(actorGuid, sceneName, actorData).catch(() => {});
  });

  coronaEventBus.on('actor-state-sync-broadcast', (actorData) => {
    if (!sessionActive.value || !actorData) return;
    if (actorData._suppress_network_broadcast) return;
    const actorGuid = actorData.actor_guid || '';
    if (!actorGuid) return;
    const sceneName = rememberSceneName(actorData.scene || 'Scene/default.scene');
    networkService.broadcastActorStateUpdate(actorGuid, sceneName, actorData).catch(() => {});
  });

  coronaEventBus.on('actor-delete-sync-broadcast', (actorData) => {
    if (!sessionActive.value || !actorData) return;
    const actorGuid = actorData.actor_guid || '';
    const actorName = actorData.actor_name || actorData.name || '';
    if (!actorGuid && !actorName) return;
    const sceneName = rememberSceneName(actorData.scene || 'Scene/default.scene');
    forgetActorCreateBroadcast(sceneName, actorGuid);
    networkService.broadcastActorDelete(actorGuid, sceneName, actorName).catch(() => {});
  });

  coronaEventBus.on('scene-tree-changed', (sceneName) => {
    rememberSceneName(sceneName);
  });

  coronaEventBus.on('network-sync-pause-request', ({ paused } = {}) => {
    networkService.setSyncPaused(Boolean(paused)).catch(() => {});
  });

  coronaEventBus.on('actor-ownership-claim', ({ actor_guid }) => {
    if (!sessionActive.value || !actor_guid) return;
    const now = Date.now();
    const lastClaim = ownershipClaimTimes.get(actor_guid) || 0;
    if (now - lastClaim < 1000) return;
    ownershipClaimTimes.set(actor_guid, now);
    networkService.claimActorOwnership(actor_guid).catch(() => {});
  });

  // Listen for file-sync-status from Python (C++ reports transfer progress)
  coronaEventBus.on('file-sync-status', ({ status, model_path, progress }) => {
    if (status === 'transferring') {
      fileStatus.value = { type: 'transferring', path: model_path, progress };
    } else if (status === 'complete') {
      fileStatus.value = { type: 'success', path: model_path };
      setTimeout(() => {
        fileStatus.value = null;
      }, 5000);
    } else if (status === 'error') {
      fileStatus.value = { type: 'error', path: model_path };
      setTimeout(() => {
        fileStatus.value = null;
      }, 5000);
    }
  });

  // Listen for import-asset-complete from Python (remote actor created)
  coronaEventBus.on('import-asset-complete', (actorData) => {
    // A remote actor was created (either via file transfer or direct creation).
    // The actor data is available for UI update.
    registerActorIdentityFromData(actorData);
    remoteActorLog.value = `远程 Actor 已创建: ${actorData.name || 'unknown'}`;
    setTimeout(() => {
      remoteActorLog.value = '';
    }, 5000);
  });
});

onUnmounted(() => {
  stopPolling();
  ownershipClaimTimes.clear();
  // Clean up event listeners
  coronaEventBus.off('actor-sync-broadcast');
  coronaEventBus.off('actor-transform-sync-broadcast');
  coronaEventBus.off('actor-state-sync-broadcast');
  coronaEventBus.off('actor-delete-sync-broadcast');
  coronaEventBus.off('scene-tree-changed');
  coronaEventBus.off('network-sync-pause-request');
  coronaEventBus.off('actor-ownership-claim');
  coronaEventBus.off('file-sync-status');
  coronaEventBus.off('import-asset-complete');
});
</script>
