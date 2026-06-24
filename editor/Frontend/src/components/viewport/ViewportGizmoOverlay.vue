<template>
  <div
    v-if="visible"
    class="absolute inset-0 z-20 pointer-events-none select-none"
    data-viewport-gizmo-overlay
  >
    <div
      class="absolute left-3 top-3 pointer-events-auto flex items-center gap-1 rounded border border-white/10 bg-[#15181d]/80 p-1 shadow-lg backdrop-blur-sm"
      @pointerdown.stop
      @mousedown.stop
    >
      <button
        v-for="item in modeItems"
        :key="item.mode"
        class="h-7 min-w-9 rounded px-2 text-[11px] font-medium"
        :class="renderModel.mode === item.mode ? 'bg-[#4b5563] text-white' : 'text-[#cbd5e1] hover:bg-white/10'"
        type="button"
        :title="item.title"
        @click="$emit('mode-change', item.mode)"
      >
        {{ item.label }}
      </button>
    </div>

  </div>
</template>

<script setup>
import { computed } from 'vue';

const props = defineProps({
  state: { type: Object, default: null },
  mode: { type: String, default: 'move' },
});

defineEmits(['mode-change']);

const modeItems = [
  { mode: 'move', label: 'Move', title: '移动' },
  { mode: 'scale', label: 'Scale', title: '缩放' },
  { mode: 'rotate', label: 'Rotate', title: '旋转' },
];

const renderModel = computed(() => ({
  mode: props.mode === 'scale' || props.mode === 'rotate' ? props.mode : 'move',
  visible: Boolean(props.state),
}));
const visible = computed(() => renderModel.value.visible);
</script>
