<template>
  <div ref="root" class="ik-bone-select" @keydown="handleKeydown" @focusout="handleFocusOut">
    <button
      ref="trigger"
      type="button"
      class="bone-trigger"
      :class="{ invalid }"
      aria-haspopup="listbox"
      :aria-expanded="open"
      :aria-controls="open ? listId : undefined"
      :aria-invalid="invalid || undefined"
      :title="modelValue || '选择末端骨骼'"
      @click="toggle"
    >
      <span>{{ modelValue || '— 选择末端骨骼 —' }}</span><span aria-hidden="true">{{ open ? '▴' : '▾' }}</span>
    </button>
    <div v-if="open" class="bone-picker">
      <input
        ref="search"
        v-model="query"
        type="search"
        class="bone-search"
        role="combobox"
        aria-label="搜索末端骨骼"
        aria-autocomplete="list"
        :aria-expanded="open"
        :aria-controls="listId"
        :aria-activedescendant="activeDescendant"
        placeholder="搜索骨骼名称…"
      />
      <p class="bone-count">{{ filtered.length }} 个节点 · ↑↓ 移动，Enter 选择</p>
      <div
        :id="listId"
        ref="viewport"
        role="listbox"
        aria-label="可选末端骨骼"
        class="bone-list"
        :style="{ height: `${viewportHeight}px` }"
        @scroll="scrollTop = $event.target.scrollTop"
        @wheel.stop
      >
        <div class="bone-spacer" :style="{ height: `${filtered.length * ROW_HEIGHT}px` }">
          <div
            v-for="item in visibleOptions"
            :id="`${listId}-${item.index}`"
            :key="item.name"
            role="option"
            class="bone-option"
            :class="{ active: item.index === activeIndex, selected: item.name === modelValue }"
            :aria-selected="item.name === modelValue"
            :aria-posinset="item.index + 1"
            :aria-setsize="filtered.length"
            :title="item.name"
            :style="{ top: `${item.index * ROW_HEIGHT}px`, height: `${ROW_HEIGHT}px` }"
            @mousedown.prevent
            @click="choose(item.name)"
          >{{ item.name }}</div>
        </div>
        <p v-if="!filtered.length" class="bone-empty">没有匹配的骨骼</p>
      </div>
    </div>
  </div>
</template>

<script setup>
import { computed, nextTick, onBeforeUnmount, ref, useId, watch } from 'vue';

const props = defineProps({
  modelValue: { type: String, default: '' },
  options: { type: Array, default: () => [] },
  invalid: { type: Boolean, default: false },
});
const emit = defineEmits(['update:modelValue', 'change']);
const ROW_HEIGHT = 30;
const VISIBLE_ROWS = 7;
const OVERSCAN = 2;
const listId = `ik-bones-${useId()}`;
const root = ref(null);
const trigger = ref(null);
const search = ref(null);
const viewport = ref(null);
const open = ref(false);
const query = ref('');
const activeIndex = ref(-1);
const scrollTop = ref(0);
const filtered = computed(() => {
  const needle = query.value.trim().toLocaleLowerCase();
  return needle ? props.options.filter((name) => name.toLocaleLowerCase().includes(needle)) : props.options;
});
const viewportHeight = computed(() => Math.max(1, Math.min(VISIBLE_ROWS, filtered.value.length)) * ROW_HEIGHT);
const visibleOptions = computed(() => {
  const first = Math.max(0, Math.floor(scrollTop.value / ROW_HEIGHT) - OVERSCAN);
  const end = Math.min(filtered.value.length, first + VISIBLE_ROWS + OVERSCAN * 2);
  return filtered.value.slice(first, end).map((name, offset) => ({ name, index: first + offset }));
});
const activeDescendant = computed(() => visibleOptions.value.some((item) => item.index === activeIndex.value)
  ? `${listId}-${activeIndex.value}` : undefined);

function outsidePointer(event) {
  if (!root.value?.contains(event.target)) close();
}

function close(restoreFocus = false) {
  open.value = false;
  document.removeEventListener('pointerdown', outsidePointer, true);
  if (restoreFocus) trigger.value?.focus();
}

async function show() {
  query.value = '';
  scrollTop.value = 0;
  activeIndex.value = Math.max(0, props.options.indexOf(props.modelValue));
  if (!props.options.length) activeIndex.value = -1;
  open.value = true;
  document.addEventListener('pointerdown', outsidePointer, true);
  await nextTick();
  if (!open.value) return;
  revealActive();
  search.value?.focus();
}

function toggle() {
  if (open.value) close();
  else void show();
}

function revealActive() {
  const view = viewport.value;
  if (!view || activeIndex.value < 0) return;
  const top = activeIndex.value * ROW_HEIGHT;
  if (top < view.scrollTop) view.scrollTop = top;
  else if (top + ROW_HEIGHT > view.scrollTop + viewportHeight.value) {
    view.scrollTop = top + ROW_HEIGHT - viewportHeight.value;
  }
  scrollTop.value = view.scrollTop;
}

function choose(name) {
  if (!props.options.includes(name)) return;
  if (name !== props.modelValue) {
    emit('update:modelValue', name);
    emit('change', name);
  }
  close(true);
}

function handleKeydown(event) {
  if (event.isComposing) return;
  if (!open.value) {
    if (['ArrowDown', 'ArrowUp', 'Enter', ' '].includes(event.key)) {
      event.preventDefault();
      void show();
    }
    return;
  }
  if (event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    close(true);
  } else if (event.key === 'Tab') {
    close();
  } else if (event.key === 'Enter') {
    event.preventDefault();
    if (activeIndex.value >= 0) choose(filtered.value[activeIndex.value]);
  } else if (['ArrowDown', 'ArrowUp', 'PageDown', 'PageUp'].includes(event.key)) {
    event.preventDefault();
    const delta = (event.key.endsWith('Down') ? 1 : -1) * (event.key.startsWith('Page') ? VISIBLE_ROWS : 1);
    activeIndex.value = Math.max(0, Math.min(filtered.value.length - 1, activeIndex.value + delta));
    if (!filtered.value.length) activeIndex.value = -1;
    revealActive();
  }
}

function handleFocusOut(event) {
  if (!root.value?.contains(event.relatedTarget)) close();
}

watch(query, () => {
  activeIndex.value = filtered.value.length ? 0 : -1;
  scrollTop.value = 0;
  if (viewport.value) viewport.value.scrollTop = 0;
}, { flush: 'sync' });
// A mode/actor switch must discard the old menu and its search rather than select
// a similarly positioned node in a different list.
watch(() => props.options, () => close());
onBeforeUnmount(() => close());
</script>

<style scoped>
.ik-bone-select { min-width: 0; width: 100%; }
.bone-trigger { display: flex; align-items: center; justify-content: space-between; gap: 6px; width: 100%; min-width: 0; padding: 5px 6px; border: 1px solid rgba(216,184,108,.22); border-radius: 4px; background: #0f0e0a; color: #f2ead5; font-size: 11px; text-align: left; cursor: pointer; }
.bone-trigger span:first-child { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.bone-trigger.invalid { border-color: #e2a45e; }
.bone-trigger:focus-visible, .bone-search:focus { outline: 1px solid #d8b86c; }
.bone-picker { margin-top: 4px; padding: 5px; border: 1px solid #6c5933; border-radius: 4px; background: #17140e; }
.bone-search { box-sizing: border-box; width: 100%; min-width: 0; padding: 5px 6px; border: 1px solid #6c5933; border-radius: 3px; background: #0f0e0a; color: #f2ead5; font-size: 11px; }
.bone-count { margin: 5px 0; color: #b9ad8f; font-size: 10px; }
.bone-list { overflow-y: auto; overflow-x: hidden; overscroll-behavior: contain; scrollbar-gutter: stable; }
.bone-spacer { position: relative; }
.bone-option { position: absolute; left: 0; right: 0; box-sizing: border-box; display: flex; align-items: center; padding: 0 6px; color: #e9dfc5; font-size: 11px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; cursor: pointer; }
.bone-option.active, .bone-option:hover { background: #3c321e; }
.bone-option.selected { color: #ffdc83; }
.bone-empty { margin: 5px; color: #b9ad8f; font-size: 11px; }
</style>
