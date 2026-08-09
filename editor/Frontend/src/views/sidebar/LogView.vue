<template>
  <div
    class="flex-1 min-h-0 w-full rounded-lg overflow-hidden flex flex-col bg-[#1e1e1e] text-gray-300 font-mono text-xs"
  >
    <DockTitleBar v-if="!isDocked" title="日志" extraClass="bg-[#D8B86C]" routePath="/LogView" @close="closeFloat" />

    <div class="p-2 bg-[#2d2d2d] flex gap-4 items-center border-b border-black">
      <div class="flex gap-2">
        <label v-for="s in ['Engine', 'Python', 'Vue']" :key="s" class="flex items-center gap-1">
          <input v-model="filterSources" type="checkbox" :value="s" />
          {{ s }}
        </label>
      </div>

      <SimpleSelect
        v-model="filterLevel"
        :options="levelOptions"
        placeholder="所有级别"
        class="bg-[#3c3c3c] border-none text-white px-1 outline-none"
      />

      <input
        v-model="searchText"
        placeholder="搜索..."
        class="flex-1 bg-[#3c3c3c] border-none px-2 py-0.5 outline-none"
      />
      <button class="text-red-400 hover:text-red-300" @click="logs = []">清空</button>
    </div>

    <div ref="scrollBox" class="flex-1 overflow-y-auto p-2 space-y-0.5">
      <div
        v-for="(log, i) in filteredLogs"
        :key="i"
        class="flex gap-2 border-b border-white/5 whitespace-pre-wrap hover:bg-white/5"
      >
        <span :class="getSourceClass(log.source)" class="w-12 font-bold shrink-0">
          {{ log.source || 'N/A' }}
        </span>
        <span :class="getLevelClass(log.level)" class="w-16 uppercase shrink-0">
          [{{ log.level }}]
        </span>
        <span class="flex-1 break-all">{{ log.message }}</span>
      </div>
      <div
        v-if="logs.length > 0 && filteredLogs.length === 0"
        class="text-center text-gray-600 mt-4"
      >
        无匹配日志 (当前已缓存 {{ logs.length }} 条)
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, computed, nextTick, onMounted, onUnmounted } from 'vue';
import { editorApi } from '@/api/editorApi.js';
import SimpleSelect from '@/components/ui/SimpleSelect.vue';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import { useDockPanel } from '@/composables/useDockPanel.js';

const { closePanel: closeDockPanel, isDocked } = useDockPanel();

const logs = ref([]);
const filterSources = ref(['Engine', 'Python', 'Vue']);
const filterLevel = ref('all');
const searchText = ref('');
const scrollBox = ref(null);
let logBatchCallbackToken = null;

const levelOptions = [
  { value: 'all', label: '所有级别' },
  { value: 'trace', label: 'TRACE' },
  { value: 'debug', label: 'DEBUG' },
  { value: 'info', label: 'INFO' },
  { value: 'notice', label: 'NOTICE' },
  { value: 'warning', label: 'WARNING' },
  { value: 'error', label: 'ERROR' },
  { value: 'critical', label: 'CRITICAL' },
];

// 辅助函数：统一处理日志推入和滚动
const pushLog = (data) => {
  if (Array.isArray(data)) {
    logs.value.push(...data);
  } else {
    logs.value.push(data);
  }

  // 限制缓存数量
  if (logs.value.length > 2000) {
    logs.value = logs.value.slice(-2000);
  }

  nextTick(() => {
    if (scrollBox.value) {
      scrollBox.value.scrollTop = scrollBox.value.scrollHeight;
    }
  });
};

window.onReceiveLog = (logData) => pushLog(logData);
window.onReceiveLogBatch = (logBatch) => pushLog(logBatch);

const onLogBatch = (batch) => {
  if (window.onReceiveLogBatch) window.onReceiveLogBatch(batch);
};

const filteredLogs = computed(() => {
  return logs.value.filter((l) => {
    // 1. 来源过滤：增加容错，如果 log.source 不在列表里则默认归类为 Engine 或显示
    const sourceMatch = filterSources.value.includes(l.source);

    // 2. 级别过滤：关键修复！将数据转为小写后比对
    const rawLevel = String(l.level || '').toLowerCase();
    const levelMatch = filterLevel.value === 'all' || rawLevel === filterLevel.value;

    // 3. 文本搜索
    const messageMatch =
      !searchText.value ||
      String(l.message || '')
        .toLowerCase()
        .includes(searchText.value.toLowerCase());

    return sourceMatch && levelMatch && messageMatch;
  });
});

const getSourceClass = (s) => {
  const colors = { Engine: 'text-blue-400', Python: 'text-green-400', Vue: 'text-purple-400' };
  return colors[s] || 'text-gray-400';
};

const getLevelClass = (l) => {
  const level = String(l).toUpperCase();
  if (['ERROR', 'CRITICAL'].includes(level)) return 'text-red-500 font-bold';
  if (['WARNING', 'NOTICE'].includes(level)) return 'text-yellow-500';
  if (['DEBUG', 'TRACE'].includes(level)) return 'text-gray-500';
  return 'text-blue-300'; // INFO
};

const closeFloat = () => {
  if (closeDockPanel) { closeDockPanel(); return; }
};

onMounted(async () => {
  logBatchCallbackToken = await editorApi.events.onLogBatch(onLogBatch);
});

onUnmounted(() => {
  if (logBatchCallbackToken) {
    editorApi.off(logBatchCallbackToken).finally(() => {
      logBatchCallbackToken = null;
    });
  }
});
</script>
