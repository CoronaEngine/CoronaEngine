<template>
  <div
    class="flex-1 min-h-0 w-full rounded-lg overflow-hidden relative bg-[#282828]/90 flex flex-col text-white font-sans"
  >
    <DockTitleBar
      v-if="!isDocked"
      :title="t('projectSettings.title')"
      extraClass="bg-[#D8B86C]"
      routePath="/ProjectSettings"
      @close="closeFloat"
    />

    <div v-if="loading" class="flex-1 flex items-center justify-center text-[#d8b86c] text-sm">
      {{ t('common.loading') }}
    </div>

    <div v-else-if="errorMsg" class="flex-1 flex items-center justify-center text-red-400 text-sm">
      {{ errorMsg }}
    </div>

    <div v-else class="flex-1 overflow-y-auto custom-scrollbar p-4 space-y-4 text-xs">
      <!-- 项目名称 -->
      <div class="flex flex-col gap-1">
        <label class="text-gray-400">{{ t('projectSettings.projectName') }}</label>
        <input
          v-model="form.name"
          type="text"
          class="bg-[#1a1a1a] border border-[#333] rounded px-2 py-1.5 text-white focus:border-[#d8b86c] outline-none"
          :placeholder="t('projectSettings.projectName')"
        />
      </div>

      <!-- 项目模式 -->
      <div class="flex flex-col gap-1">
        <label class="text-gray-400">{{ t('projectSettings.projectMode') }}</label>
        <select
          v-model="form.mode"
          class="bg-[#1a1a1a] border border-[#333] rounded px-2 py-1.5 text-white focus:border-[#d8b86c] outline-none"
        >
          <option value="3d">3D</option>
          <option value="2d">2D</option>
        </select>
      </div>

      <!-- 入口场景 -->
      <div class="flex flex-col gap-1">
        <label class="text-gray-400">{{ t('projectSettings.entryScene') }}</label>
        <div class="flex items-center gap-2">
          <input
            v-model="form.entrance_scene"
            type="text"
            class="flex-1 bg-[#1a1a1a] border border-[#333] rounded px-2 py-1.5 text-white focus:border-[#d8b86c] outline-none"
            placeholder="Scene/main.scene"
          />
          <button
            class="px-3 py-1.5 bg-[#d8b86c] hover:bg-[#6f8d4b] rounded text-white"
            @click="handleBrowseScene"
          >
            {{ t('common.browse') }}
          </button>
        </div>
      </div>

      <!-- 核心版本 -->
      <div class="flex flex-col gap-1">
        <label class="text-gray-400">{{ t('projectSettings.coreVersion') }}</label>
        <input
          v-model="form.core_version"
          type="text"
          class="bg-[#1a1a1a] border border-[#333] rounded px-2 py-1.5 text-white focus:border-[#d8b86c] outline-none"
          placeholder="1.0.0"
        />
      </div>

      <!-- 只读信息 -->
      <div class="grid grid-cols-2 gap-3 pt-2 border-t border-[#333]">
        <div class="flex flex-col gap-1">
          <label class="text-gray-400">{{ t('projectSettings.createdAt') }}</label>
          <div class="text-gray-300 truncate">{{ form.create_time || '-' }}</div>
        </div>
        <div class="flex flex-col gap-1">
          <label class="text-gray-400">{{ t('projectSettings.lastOpened') }}</label>
          <div class="text-gray-300 truncate">{{ form.last_opened || '-' }}</div>
        </div>
      </div>

      <!-- 提示信息 -->
      <div v-if="statusMsg" :class="['text-xs', statusOk ? 'text-[#d8b86c]' : 'text-red-400']">
        {{ statusMsg }}
      </div>
    </div>

    <!-- 底部操作栏 -->
    <div
      v-if="!loading && !errorMsg"
      class="flex justify-end gap-2 p-3 border-t border-[#333] bg-[#1a1a1a]/50"
    >
      <button
        class="px-4 py-1.5 bg-[#333] hover:bg-[#444] rounded text-xs text-white"
        :disabled="saving"
        @click="loadSettings"
      >
        {{ t('common.reset') }}
      </button>
      <button
        class="px-4 py-1.5 bg-[#d8b86c] hover:bg-[#6f8d4b] rounded text-xs text-white disabled:opacity-50"
        :disabled="saving"
        @click="handleSave"
      >
        {{ saving ? t('common.saving') : t('common.save') }}
      </button>
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue';
import { useI18n } from 'vue-i18n';
import { editorApi } from '@/api/editorApi.js';
import { useDockPanel } from '@/composables/useDockPanel.js';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';

const { closePanel: closeDockPanel, isDocked } = useDockPanel();
const { t } = useI18n();

const loading = ref(true);
const saving = ref(false);
const errorMsg = ref('');
const statusMsg = ref('');
const statusOk = ref(true);

const form = ref({
  name: '',
  mode: '3d',
  entrance_scene: '',
  core_version: '',
  create_time: '',
  last_opened: '',
});

const setStatus = (msg, ok = true) => {
  statusMsg.value = msg;
  statusOk.value = ok;
  if (msg) {
    setTimeout(() => {
      statusMsg.value = '';
    }, 3000);
  }
};

const loadSettings = async () => {
  loading.value = true;
  errorMsg.value = '';
  try {
    const res = await editorApi.projectSettings.getActiveProjectInfo();
    if (res && res.success && res.data) {
      form.value = { ...form.value, ...res.data };
    } else {
      errorMsg.value = (res && res.error) || t('projectSettings.loadFailed');
    }
  } catch (e) {
    errorMsg.value = String(e);
  } finally {
    loading.value = false;
  }
};

const handleSave = async () => {
  saving.value = true;
  try {
    const payload = {
      name: form.value.name,
      mode: form.value.mode,
      entrance_scene: form.value.entrance_scene,
      core_version: form.value.core_version,
    };
    const res = await editorApi.projectSettings.saveActiveProjectInfo(payload);
    if (res && res.success) {
      setStatus(t('projectSettings.saveSuccess'), true);
    } else {
      setStatus((res && res.error) || t('projectSettings.saveFailed'), false);
    }
  } catch (e) {
    setStatus(String(e), false);
  } finally {
    saving.value = false;
  }
};

const handleBrowseScene = async () => {
  try {
    const res = await editorApi.projectSettings.browseSceneFile();
    if (res && res.success && res.path) {
      form.value.entrance_scene = res.path;
    } else if (res && res.error) {
      setStatus(res.error, false);
    }
  } catch (e) {
    setStatus(String(e), false);
  }
};

const closeFloat = () => {
  if (closeDockPanel) {
    closeDockPanel();
    return;
  }
  window.__settingsOpen = false;
};

onMounted(loadSettings);
</script>

<style scoped>
.custom-scrollbar::-webkit-scrollbar {
  width: 6px;
}
.custom-scrollbar::-webkit-scrollbar-thumb {
  background: #444;
  border-radius: 3px;
}
.custom-scrollbar::-webkit-scrollbar-thumb:hover {
  background: #d8b86c;
}
</style>
