<template>
  <div
    class="relative min-h-screen bg-[#0d0d0d] text-white overflow-hidden flex flex-col font-sans"
  >
    <!-- 背景装饰：径向辉光，延续 StartScreen 视觉 -->
    <div class="absolute inset-0 bg-gradient-to-b from-[#1a2a1a]/30 via-transparent to-transparent pointer-events-none"></div>
    <div class="absolute top-0 left-1/2 -translate-x-1/2 w-[700px] h-[700px] bg-[#84a65b]/[0.04] rounded-full blur-3xl pointer-events-none"></div>
    <div class="absolute bottom-0 left-1/3 w-[400px] h-[400px] bg-[#84a65b]/[0.025] rounded-full blur-3xl pointer-events-none"></div>

    <div class="relative z-10 flex-1 flex flex-col items-center px-8 py-6 overflow-hidden">

      <!-- 标题 -->
      <div class="text-center mb-6 shrink-0">
        <h1 class="text-3xl font-light tracking-wide">
          局域<span class="text-[#84a65b] font-medium">联机</span>
        </h1>
        <p class="text-sm text-gray-500 mt-1">与同一网络下的伙伴一起创造</p>
      </div>

      <!-- 顶部 segmented：加入 / 创建 -->
      <div class="inline-flex p-1 rounded-xl bg-[#1a1a1a] border border-[#333] mb-6 shrink-0">
        <button
          v-for="t in tabs"
          :key="t.id"
          class="px-10 py-2.5 rounded-lg text-base font-medium transition-all duration-300 inline-flex items-center gap-2"
          :class="activeTab === t.id ? 'bg-[#84a65b] text-white shadow-lg' : 'text-gray-400 hover:text-white'"
          @click="activeTab = t.id"
        >
          <span v-html="t.icon" class="w-5 h-5 inline-flex"></span>
          {{ t.label }}
        </button>
      </div>

      <!-- 本机网络信息条 -->
      <div class="w-full max-w-3xl flex items-center justify-between mb-4 px-1 shrink-0">
        <div class="flex items-center gap-2 text-sm text-gray-400">
          <span class="w-2 h-2 rounded-full bg-[#84a65b] animate-pulse"></span>
          本机局域网 IP
          <span class="text-[#b9d39a] font-mono">{{ localIp }}</span>
        </div>
        <div class="flex items-center gap-2 text-sm text-gray-500">
          <span>昵称</span>
          <input
            v-model="nickname"
            type="text"
            maxlength="16"
            class="w-36 bg-[#161616] border border-[#333] rounded px-3 py-1.5 text-sm text-white
                   focus:border-[#84a65b] outline-none transition-all"
            placeholder="玩家名"
          />
        </div>
      </div>

      <!-- ============ 加入房间 ============ -->
      <div v-show="activeTab === 'join'" class="w-full max-w-3xl flex-1 flex flex-col min-h-0">

        <!-- 房间列表头 -->
        <div class="flex items-center justify-between mb-3 shrink-0">
          <h3 class="text-sm font-semibold text-gray-400 uppercase tracking-wider">
            发现的房间 <span class="text-gray-600">({{ rooms.length }})</span>
          </h3>
          <button
            class="text-sm text-gray-400 hover:text-[#84a65b] transition-colors inline-flex items-center gap-1.5"
            :class="{ 'pointer-events-none opacity-60': scanning }"
            @click="handleRefresh"
          >
            <svg class="w-4 h-4" :class="{ 'animate-spin': scanning }" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/></svg>
            {{ scanning ? '扫描中…' : '刷新' }}
          </button>
        </div>

        <!-- 房间卡片列表 -->
        <div class="flex-1 overflow-y-auto pr-1 min-h-0">
          <div v-if="rooms.length > 0" class="space-y-2.5">
            <div
              v-for="room in rooms"
              :key="room.id"
              class="group p-4 rounded-xl bg-[#181818] border transition-all cursor-pointer
                     flex items-center gap-4"
              :class="selectedRoom === room.id
                ? 'border-[#84a65b] bg-[#84a65b]/[0.06]'
                : 'border-[#2a2a2a] hover:border-[#3d3d3d] hover:bg-[#1d1d1d]'"
              @click="selectedRoom = room.id"
              @dblclick="handleJoinRoom(room)"
            >
              <!-- 信号/锁 -->
              <div class="shrink-0 w-10 h-10 rounded-lg bg-[#222] flex items-center justify-center"
                   :class="selectedRoom === room.id ? 'text-[#84a65b]' : 'text-gray-500'">
                <svg v-if="room.locked" class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="11" width="18" height="11" rx="2"/><path d="M7 11V7a5 5 0 0 1 10 0v4"/></svg>
                <svg v-else class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M5 12.55a11 11 0 0 1 14.08 0"/><path d="M1.42 9a16 16 0 0 1 21.16 0"/><path d="M8.53 16.11a6 6 0 0 1 6.95 0"/><line x1="12" y1="20" x2="12.01" y2="20"/></svg>
              </div>

              <!-- 房间信息 -->
              <div class="flex-1 min-w-0">
                <div class="flex items-center gap-2">
                  <span class="text-base font-medium truncate">{{ room.name }}</span>
                  <span v-if="room.locked" class="text-[10px] px-1.5 py-0.5 rounded bg-[#333] text-gray-400">密码</span>
                </div>
                <div class="text-xs text-gray-500 font-mono mt-0.5 truncate">{{ room.host }}:{{ room.port }} · 主机 {{ room.hostName }}</div>
              </div>

              <!-- 人数 -->
              <div class="shrink-0 text-right">
                <div class="text-base font-medium" :class="room.players >= room.maxPlayers ? 'text-red-400' : 'text-[#b9d39a]'">
                  {{ room.players }}/{{ room.maxPlayers }}
                </div>
                <div class="text-[11px] text-gray-600">{{ room.ping }}ms</div>
              </div>

              <!-- 加入按钮（hover/选中显现） -->
              <button
                class="shrink-0 px-4 py-2 rounded-lg text-sm font-medium transition-all
                       bg-[#84a65b] text-white hover:bg-[#95b86c]
                       opacity-0 group-hover:opacity-100"
                :class="{ '!opacity-100': selectedRoom === room.id }"
                :disabled="room.players >= room.maxPlayers"
                @click.stop="handleJoinRoom(room)"
              >
                加入
              </button>
            </div>
          </div>

          <!-- 空状态 -->
          <div v-else class="h-full flex flex-col items-center justify-center text-center py-12">
            <svg class="w-12 h-12 text-gray-700 mb-3" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/></svg>
            <p class="text-sm text-gray-600">{{ scanning ? '正在扫描局域网…' : '未发现房间，可点击刷新或在下方手动加入' }}</p>
          </div>
        </div>

        <!-- 手动加入 -->
        <div class="shrink-0 mt-4 pt-4 border-t border-[#222]">
          <div class="text-sm text-gray-400 mb-2.5">手动加入</div>
          <div class="flex items-center gap-2">
            <input
              v-model="manual.ip"
              type="text"
              class="flex-1 bg-[#161616] border border-[#333] rounded-lg px-3 py-2.5 text-sm font-mono
                     focus:border-[#84a65b] outline-none transition-all placeholder:text-gray-600"
              placeholder="主机 IP，如 192.168.1.42"
            />
            <input
              v-model="manual.port"
              type="text"
              class="w-24 bg-[#161616] border border-[#333] rounded-lg px-3 py-2.5 text-sm font-mono
                     focus:border-[#84a65b] outline-none transition-all placeholder:text-gray-600"
              placeholder="端口"
            />
            <input
              v-model="manual.password"
              type="password"
              class="w-32 bg-[#161616] border border-[#333] rounded-lg px-3 py-2.5 text-sm
                     focus:border-[#84a65b] outline-none transition-all placeholder:text-gray-600"
              placeholder="密码(可选)"
            />
            <button
              class="px-6 py-2.5 rounded-lg text-sm font-bold bg-[#3d3d3d] hover:bg-[#4d4d4d] transition-colors disabled:opacity-50 disabled:cursor-not-allowed"
              :disabled="!manual.ip.trim()"
              @click="handleManualJoin"
            >
              连接
            </button>
          </div>
        </div>
      </div>

      <!-- ============ 创建房间 ============ -->
      <div v-show="activeTab === 'host'" class="w-full max-w-3xl flex-1 flex flex-col min-h-0 overflow-y-auto">
        <div class="space-y-5">
          <div class="space-y-2">
            <label class="text-sm text-gray-400">房间名称</label>
            <input
              v-model="host.roomName"
              type="text"
              class="w-full bg-[#161616] border border-[#333] rounded-lg px-4 py-3 text-base
                     focus:border-[#84a65b] outline-none transition-all placeholder:text-gray-600"
              placeholder="给你的房间起个名字…"
            />
          </div>

          <div class="grid grid-cols-2 gap-4">
            <div class="space-y-2">
              <label class="text-sm text-gray-400">访问密码（可选）</label>
              <input
                v-model="host.password"
                type="password"
                class="w-full bg-[#161616] border border-[#333] rounded-lg px-4 py-3 text-base
                       focus:border-[#84a65b] outline-none transition-all placeholder:text-gray-600"
                placeholder="留空则公开"
              />
            </div>
            <div class="space-y-2">
              <label class="text-sm text-gray-400">端口</label>
              <input
                v-model="host.port"
                type="text"
                class="w-full bg-[#161616] border border-[#333] rounded-lg px-4 py-3 text-base font-mono
                       focus:border-[#84a65b] outline-none transition-all placeholder:text-gray-600"
                placeholder="27960"
              />
            </div>
          </div>

          <div class="space-y-2">
            <label class="text-sm text-gray-400">最大人数</label>
            <div class="flex gap-2">
              <button
                v-for="n in maxPlayerOptions"
                :key="n"
                class="flex-1 py-2.5 rounded-lg text-sm font-medium border transition-all"
                :class="host.maxPlayers === n
                  ? 'border-[#84a65b] bg-[#84a65b]/15 text-[#84a65b]'
                  : 'border-[#2a2a2a] bg-[#161616] text-gray-400 hover:border-[#3d3d3d]'"
                @click="host.maxPlayers = n"
              >
                {{ n }} 人
              </button>
            </div>
          </div>

          <div class="rounded-lg bg-[#84a65b]/[0.05] border border-[#84a65b]/20 p-3 text-xs text-gray-400 flex items-start gap-2">
            <svg class="w-4 h-4 text-[#84a65b] shrink-0 mt-0.5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 16v-4"/><path d="M12 8h.01"/></svg>
            创建房间后将以当前项目作为联机世界，房间会在同一局域网内自动广播，伙伴无需手动输入 IP 即可发现。
          </div>
        </div>
      </div>

      <!-- 底部操作栏 -->
      <div class="w-full max-w-3xl flex items-center justify-between mt-6 shrink-0">
        <button
          class="px-5 py-3 text-base text-gray-400 hover:text-white hover:bg-[#222] rounded-lg transition-colors inline-flex items-center gap-1"
          @click="goHome"
        >
          <svg class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
          返回主页
        </button>

        <!-- 主 CTA：加入 tab 走选中房间；创建 tab 走开房 -->
        <button
          v-if="activeTab === 'join'"
          class="px-14 py-3 bg-[#84a65b] hover:bg-[#95b86c] disabled:bg-gray-700 disabled:cursor-not-allowed
                 rounded-lg font-bold text-base transition-all shadow-lg inline-flex items-center gap-2"
          :disabled="!selectedRoom"
          @click="handleJoinSelected"
        >
          <svg class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4"/><polyline points="10 17 15 12 10 7"/><line x1="15" y1="12" x2="3" y2="12"/></svg>
          加入房间
        </button>
        <button
          v-else
          class="px-14 py-3 bg-[#84a65b] hover:bg-[#95b86c] disabled:bg-gray-700 disabled:cursor-not-allowed
                 rounded-lg font-bold text-base transition-all shadow-lg inline-flex items-center gap-2"
          :disabled="!host.roomName.trim()"
          @click="handleCreateRoom"
        >
          <svg class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 5v14M5 12h14"/></svg>
          创建房间
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue';
import { useRouter } from 'vue-router';

const router = useRouter();

const tabs = [
  { id: 'join', label: '加入房间', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="w-5 h-5"><path d="M15 3h4a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2h-4"/><polyline points="10 17 15 12 10 7"/><line x1="15" y1="12" x2="3" y2="12"/></svg>' },
  { id: 'host', label: '创建房间', icon: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" class="w-5 h-5"><path d="M12 5v14M5 12h14"/></svg>' },
];

const activeTab = ref('join');
const nickname = ref('玩家1');
const localIp = ref('192.168.1.100'); // TODO(后端): lanChatService.getLocalIp()
const scanning = ref(false);
const selectedRoom = ref(null);

// TODO(后端): 房间列表应由局域网广播扫描得到，这里为静态 mock 仅用于 UI 设计
const rooms = ref([
  { id: 'r1', name: '一起搭城市', host: '192.168.1.42', port: 27960, hostName: '阿强', players: 2, maxPlayers: 4, locked: false, ping: 12 },
  { id: 'r2', name: '赛博朋克夜景测试', host: '192.168.1.57', port: 27960, hostName: 'Neo', players: 1, maxPlayers: 8, locked: true, ping: 28 },
  { id: 'r3', name: '剧情联机·第一章', host: '192.168.1.13', port: 27961, hostName: '小雨', players: 4, maxPlayers: 4, locked: false, ping: 9 },
]);

const manual = ref({ ip: '', port: '27960', password: '' });

const maxPlayerOptions = [2, 4, 8, 16];
const host = ref({ roomName: '', password: '', port: '27960', maxPlayers: 4 });

// ===== 以下 handler 仅占位，不接后端 =====

const handleRefresh = () => {
  // TODO(后端): 触发局域网房间扫描
  scanning.value = true;
  setTimeout(() => { scanning.value = false; }, 800);
};

const handleJoinRoom = (room) => {
  // TODO(后端): lanChatService.joinRoom({ ip, port, room, password, nickname })
  console.log('[UI] join room', room, 'as', nickname.value);
};

const handleJoinSelected = () => {
  const room = rooms.value.find((r) => r.id === selectedRoom.value);
  if (room) handleJoinRoom(room);
};

const handleManualJoin = () => {
  // TODO(后端): 用 manual.ip/port/password 加入
  console.log('[UI] manual join', manual.value, 'as', nickname.value);
};

const handleCreateRoom = () => {
  // TODO(后端): lanChatService.startRoom({ room, password, port }) + 开始联机
  console.log('[UI] create room', host.value, 'as', nickname.value);
};

const goHome = () => {
  router.push('/StartScreen');
};
</script>

<style scoped>
::-webkit-scrollbar {
  width: 4px;
}
::-webkit-scrollbar-track {
  background: transparent;
}
::-webkit-scrollbar-thumb {
  background: #444;
  border-radius: 10px;
}
::-webkit-scrollbar-thumb:hover {
  background: #84a65b;
}
</style>
