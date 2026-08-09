<template>
  <div class="flex h-[50px] w-[50px]">
    <div class="flex w-full h-full">
      <img
        ref="petImgRef"
        src="@/assets/cabbage.png"
        class="h-20 w-20 fixed left-10 bottom-10 cursor-move z-50"
        @contextmenu="openContextMenu($event)"
      />

      <AIHintBubble
        v-if="hintState.show"
        :show="hintState.show"
        :hint-text="hintState.text"
        :loading="hintState.loading"
        :anchor-el="petImgRef"
        :auto-hide-ms="PET_HINT_AUTO_HIDE_MS"
        @close="dismissHint"
      />
    </div>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted, onUnmounted, nextTick } from 'vue';
import { appService } from '@/services/appService.js';
import AIHintBubble from '@/components/ui/AIHintBubble.vue';

const petImgRef = ref(null);
const ROUTE_PATH = '/Pet';
const PET_HINT_INTERVAL_MS = 10000;
const PET_HINT_AUTO_HIDE_MS = 6000;
const PET_HINTS = [
  '记得保存当前项目',
  '可以拖拽模型到场景里',
  '右键对象查看更多操作',
  '调整相机看看构图效果',
  '打开日志面板检查运行状态',
];

const hintState = reactive({ show: false, text: '', loading: false });
let hintIndex = 0;
let hintTimer = null;

// ── Throttle ──
function throttle(fn, delay) {
  let last = 0;
  return function (...args) {
    const now = Date.now();
    if (now - last < delay) return;
    last = now;
    return fn(...args);
  };
}

// ── Drag region ──
const sendDragRegion = (el) => {
  if (!el) return;
  try {
    const r = el.getBoundingClientRect();
    appService.setDragRegions(ROUTE_PATH, Math.floor(r.x), Math.floor(r.y), Math.floor(r.width), Math.floor(r.height));
  } catch { /* ignore */ }
};
const throttledSend = throttle(sendDragRegion, 16);

const openContextMenu = (e) => e.preventDefault();


// ── Timed local hint logic ──
const dismissHint = () => {
  hintState.show = false;
  hintState.text = '';
  hintState.loading = false;
};

const showTimedHint = async () => {
  hintState.show = false;
  hintState.loading = false;
  hintState.text = PET_HINTS[hintIndex % PET_HINTS.length];
  hintIndex += 1;
  await nextTick();
  hintState.show = true;
};

const startTimedHints = () => {
  stopTimedHints();
  hintTimer = window.setInterval(showTimedHint, PET_HINT_INTERVAL_MS);
};

const stopTimedHints = () => {
  if (hintTimer) {
    clearInterval(hintTimer);
    hintTimer = null;
  }
};

// ── Lifecycle ──
let resizeObserver = null;

onMounted(() => {
  console.log('[Pet] mounted');

  startTimedHints();

  if (petImgRef.value) {
    sendDragRegion(petImgRef.value);
    try {
      resizeObserver = new ResizeObserver(() => throttledSend(petImgRef.value));
      resizeObserver.observe(petImgRef.value);
    } catch { /* non-critical */ }
    window.addEventListener('resize', onWindowResize);
  }
});

const onWindowResize = () => {
  if (petImgRef.value) throttledSend(petImgRef.value);
};

onUnmounted(() => {
  stopTimedHints();
  dismissHint();
  if (resizeObserver) { try { resizeObserver.disconnect(); } catch { /* ignore */ } resizeObserver = null; }
  window.removeEventListener('resize', onWindowResize);
});
</script>
