<template>
  <div
    class="relative min-h-screen bg-[#0d0d0d] text-white overflow-hidden flex flex-col font-sans"
  >
    <!-- 背景装饰：径向辉光，延续 StartScreen 视觉 -->
    <div class="absolute inset-0 bg-gradient-to-b from-[#1a2a1a]/30 via-transparent to-transparent pointer-events-none"></div>
    <div class="absolute top-0 left-1/2 -translate-x-1/2 w-[700px] h-[700px] bg-[#84a65b]/[0.04] rounded-full blur-3xl pointer-events-none"></div>
    <div class="absolute bottom-0 left-1/4 w-[400px] h-[400px] bg-[#84a65b]/[0.025] rounded-full blur-3xl pointer-events-none"></div>

    <!-- 主体三段式 -->
    <div class="relative z-10 flex-1 flex flex-col px-8 py-6 overflow-hidden">

      <!-- ① 上方：悬浮提示词层（漂浮 / 发光 / 可点击注入） -->
      <div class="relative h-44 shrink-0">
        <button
          v-for="(hint, i) in floatingHints"
          :key="hint.text"
          class="hint-chip absolute px-4 py-2 rounded-full text-sm whitespace-nowrap
                 bg-[#84a65b]/[0.06] border border-[#84a65b]/25 text-[#b9d39a]
                 backdrop-blur-sm hover:bg-[#84a65b]/20 hover:border-[#84a65b]/60 hover:text-white
                 transition-colors duration-300 cursor-pointer"
          :style="{
            top: hint.top,
            left: hint.left,
            '--dur': hint.dur,
            '--delay': hint.delay,
          }"
          @click="applyHint(hint.text)"
        >
          {{ hint.text }}
        </button>
      </div>

      <!-- ② 中间：标题 + 世界描述输入框（页面焦点） -->
      <div class="flex-1 flex flex-col items-center justify-center min-h-0">
        <h1 class="text-3xl font-light tracking-wide mb-2 text-center">
          你想创造一个怎样的
          <span class="text-[#84a65b] font-medium">世界</span>？
        </h1>
        <p class="text-sm text-gray-500 mb-7 text-center">用一句话描述它，AI 会替你把它构建出来</p>

        <div class="w-full max-w-3xl">
          <textarea
            ref="promptRef"
            v-model="worldPrompt"
            rows="5"
            class="w-full bg-[#161616] border border-[#333] rounded-xl p-5 text-lg leading-relaxed
                   focus:border-[#84a65b] focus:shadow-[0_0_40px_rgba(132,166,91,0.12)]
                   outline-none transition-all resize-none placeholder:text-gray-600"
            placeholder="例如：一座漂浮在云海之上的赛博朋克城市，永远是雨夜，霓虹倒映在湿漉漉的街道……"
            @keydown.ctrl.enter="handleCreate"
          ></textarea>
        </div>
      </div>

      <!-- ③ 底部：模式二选一 + 操作按钮 -->
      <div class="shrink-0 flex flex-col items-center gap-6 pb-2">
        <!-- 模式二选一 -->
        <div class="inline-flex p-1 rounded-xl bg-[#1a1a1a] border border-[#333]">
          <button
            v-for="m in modes"
            :key="m.id"
            class="px-8 py-2.5 rounded-lg text-base font-medium transition-all duration-300"
            :class="mode === m.id
              ? 'bg-[#84a65b] text-white shadow-lg'
              : 'text-gray-400 hover:text-white'"
            @click="mode = m.id"
          >
            {{ m.label }}
          </button>
        </div>

        <!-- 操作按钮 -->
        <div class="w-full max-w-3xl flex items-center justify-between">
          <button
            class="px-5 py-3 text-base text-gray-400 hover:text-white hover:bg-[#222] rounded-lg transition-colors inline-flex items-center gap-1"
            @click="goHome"
          >
            <svg class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 18 9 12 15 6"/></svg>
            返回主页
          </button>

          <button
            :disabled="creating"
            class="px-14 py-3 bg-[#84a65b] hover:bg-[#95b86c] disabled:bg-gray-700 disabled:cursor-not-allowed
                   rounded-lg font-bold text-base transition-all shadow-lg
                   inline-flex items-center gap-2"
            @click="handleCreate"
          >
            <svg v-if="!creating" class="w-5 h-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 3v18M3 12h18"/><path d="M5.6 5.6l1.4 1.4M17 17l1.4 1.4M18.4 5.6L17 7M7 17l-1.4 1.4"/></svg>
            <svg v-else class="w-5 h-5 animate-spin" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M21 12a9 9 0 1 1-6.219-8.56"/></svg>
            {{ creating ? '创造中…' : '创造世界' }}
          </button>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import { ref } from 'vue';
import { useRouter } from 'vue-router';
import { projectLauncherService, appService } from '@/utils/bridge';

const router = useRouter();

const worldPrompt = ref('');
const mode = ref('story'); // 'story' 剧情模式 | 'creative' 创造模式
const creating = ref(false);
const promptRef = ref(null);

const modes = [
  { id: 'story', label: '剧情模式' },
  { id: 'creative', label: '创造模式' },
];

// 静态预设的悬浮提示词（点击注入输入框）；top/left 散布，dur/delay 错峰漂浮
const floatingHints = [
  { text: '赛博朋克雨夜都市',   top: '8%',  left: '6%',  dur: '7s',  delay: '0s' },
  { text: '漂浮的群岛与天空之城', top: '52%', left: '14%', dur: '8.5s', delay: '0.6s' },
  { text: '废土上的最后绿洲',   top: '24%', left: '34%', dur: '6.5s', delay: '1.2s' },
  { text: '水墨风的仙侠秘境',   top: '64%', left: '46%', dur: '9s',  delay: '0.3s' },
  { text: '深海下的远古遗迹',   top: '10%', left: '60%', dur: '7.5s', delay: '1.6s' },
  { text: '霓虹蒸汽朋克工坊',   top: '46%', left: '72%', dur: '8s',  delay: '0.9s' },
  { text: '永夜极光下的雪原',   top: '18%', left: '84%', dur: '6.8s', delay: '0.2s' },
];

const applyHint = (text) => {
  worldPrompt.value = worldPrompt.value.trim()
    ? `${worldPrompt.value.trim()}，${text}`
    : text;
  promptRef.value?.focus();
};

const goHome = () => {
  router.push('/StartScreen');
};

const handleCreate = async () => {
  if (creating.value) return;
  const prompt = worldPrompt.value.trim(); // 允许为空：无提示词也可创建

  creating.value = true;
  try {
    // 后端自动命名 + 存到引擎 data 目录，返回 { name, path }
    const result = await projectLauncherService.createWorldProject({
      mode: mode.value,
      prompt,
    });
    const info = result?.data;

    if (info && info.path) {
      await projectLauncherService.setProjectMode(mode.value, { prompt });
      const opened = await projectLauncherService.openProject(info.path);
      if (opened?.data) {
        await appService.start_engine();
        router.push('/');
        return;
      }
    }
    alert('创建失败');
  } catch (error) {
    console.error('创造世界失败:', error);
    alert('创建失败: ' + (error?.message || error));
  } finally {
    creating.value = false;
  }
};
</script>

<style scoped>
/* 悬浮提示词缓慢漂浮 + 发光呼吸 */
@keyframes floatDrift {
  0%, 100% { transform: translateY(0); }
  50% { transform: translateY(-12px); }
}
.hint-chip {
  animation: floatDrift var(--dur, 8s) ease-in-out infinite;
  animation-delay: var(--delay, 0s);
  box-shadow: 0 0 18px rgba(132, 166, 91, 0.08);
}

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
