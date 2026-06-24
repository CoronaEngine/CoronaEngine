<template>
  <div
    class="relative h-screen border-2 border-[#84a65b] bg-[#282828]/95 text-white overflow-hidden flex flex-col font-sans"
  >
    <div class="flex-1 min-h-0 p-20 bg-[#1e1e1e] flex flex-col">
      <div class="mb-10 shrink-0">
        <h2 class="text-5xl font-bold text-[#84a65b] mb-2">Corona Editor</h2>
        <p class="text-base text-gray-500">版本: {{ appVersion }}</p>
      </div>

      <div class="shrink-0 mb-6">
        <h3 class="text-base font-semibold text-gray-400 uppercase tracking-wider">
          最近项目
        </h3>
      </div>

      <div class="flex-1 min-h-0 overflow-y-auto pr-1">
        <div v-if="recentProjects.length > 0" class="space-y-3">
          <div
            v-for="proj in recentProjects"
            :key="proj.path"
            class="p-5 rounded bg-[#2d2d2d] transition-colors group flex items-center gap-6"
            :class="[
              proj.if_exists
                ? 'cursor-pointer hover:bg-[#3d3d3d]'
                : 'cursor-not-allowed opacity-60',
              selectedProject === proj.path
                ? 'border border-[#84a65b]'
                : 'border border-transparent',
            ]"
            @click="proj.if_exists && (selectedProject = proj.path)"
            @dblclick="proj.if_exists && handleOpenProject(proj.path)"
          >
            <div class="min-w-0 flex-1">
              <div class="text-base font-medium truncate">
                <span v-if="proj.if_exists">{{ proj.name }}</span>
                <span v-else class="text-red-500">{{ proj.name }} (路径异常)</span>
              </div>
              <div class="text-xs text-gray-500 truncate mt-1">{{ proj.path }}</div>
            </div>
            <div class="shrink-0 min-w-40 text-right">
              <div class="text-[11px] text-gray-600 uppercase tracking-wider">上次编辑</div>
              <div class="text-sm text-gray-400 font-mono mt-1">{{ proj.last_edited || '-' }}</div>
            </div>
          </div>
        </div>
        <div
          v-else
          class="text-sm text-gray-600 italic p-6 text-center border border-dashed border-[#333] rounded"
        >
          暂无最近记录
        </div>
      </div>

      <div class="mt-6 pt-6 border-t border-[#333] flex items-center gap-3 shrink-0">
        <button
          class="flex-1 py-3 px-6 text-left text-base hover:bg-[#333] rounded flex items-center gap-3"
          @click="handleImport"
        >
          <span class="text-xl">📁</span>
          打开现有项目...
        </button>
        <button
          class="py-3 px-10 text-base rounded flex items-center justify-center gap-2 transition-colors shrink-0"
          :class="selectedProject ? 'bg-[#84a65b] text-white hover:bg-[#9bc46d]' : 'bg-[#333] text-gray-500 cursor-not-allowed'"
          :disabled="!selectedProject"
          @click="selectedProject && handleOpenProject(selectedProject)"
        >
          <svg class="w-5 h-5" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7z"/></svg>
          开始
        </button>
      </div>

      <div class="mt-6 shrink-0">
        <button
          class="px-5 py-3 text-base text-gray-400 hover:text-white hover:bg-[#333] rounded transition-colors inline-flex items-center gap-1 w-fit"
          @click="goBack"
        >
          <svg class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
          返回
        </button>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue';
import { useRouter } from 'vue-router';
import { projectLauncherService, appService } from '@/utils/bridge';

const router = useRouter();

const appVersion = ref('V1.0.0');
const recentProjects = ref([]);
const selectedProject = ref(null);

const goBack = () => {
  router.push('/StartScreen');
};

onMounted(async () => {
  try {
    const version = await projectLauncherService.getAppVersion();
    if (version) appVersion.value = version.data;

    const saved = await projectLauncherService.getRecentProjects();
    if (saved) recentProjects.value = saved.data;
  } catch (error) {
    console.error('RecentGames 初始化失败:', error);
  }
});

const handleOpenProject = async (path) => {
  try {
    const success = await projectLauncherService.openProject(path);
    if (success.data) {
      await appService.start_engine();
      router.push('/');
    }
  } catch (error) {
    console.error('打开项目失败:', error);
  }
};

const handleImport = async () => {
  const result = await projectLauncherService.openProjectFile();
  if (result && result.data.path) {
    handleOpenProject(result.data.path);
  }
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
