<template>
  <div
    v-if="visible"
    class="modal-overlay fixed inset-0 bg-black bg-opacity-50 flex items-center justify-center z-50"
    @click.self="close"
  >
    <div class="modal-container bg-white rounded-lg shadow-xl p-6 max-w-md text-gray-900" @click.stop>
      <div class="modal-header flex justify-between items-center mb-6">
        <h3 class="text-xl font-bold text-gray-900">设置</h3>
        <button
          class="text-gray-400 hover:text-gray-600 text-xl leading-none px-2 py-1"
          @click="close"
        >
          ×
        </button>
      </div>
      <div id="modal-content">
        <div class="setting-item flex justify-between items-center w-full select-none mb-6">
          <p class="text-gray-800">积木盒宽度</p>
          <div class="radio-group text-gray-800">
            <label class="radio-label inline-block mr-2">
              <input
                type="radio"
                name="flyoutMode"
                value="full"
                :checked="flyoutMode === 'full'"
                class="mr-1"
                @change="handleFlyoutChange('full')"
              />
              适应
            </label>
            <label class="radio-label inline-block">
              <input
                type="radio"
                name="flyoutMode"
                value="fixed"
                :checked="flyoutMode === 'fixed'"
                class="mr-1"
                @change="handleFlyoutChange('fixed')"
              />
              固定
            </label>
          </div>
        </div>
        <div class="setting-item flex justify-between items-center w-full select-none">
          <p class="text-gray-800">主题</p>
          <select
            :value="themeMode"
            class="border rounded p-2 text-gray-800 bg-white"
            @change="handleThemeChange($event.target.value)"
          >
            <option value="light">白天模式</option>
            <option value="dark">黑夜模式</option>
            <option value="auto">跟随系统</option>
          </select>
        </div>
      </div>
    </div>
  </div>
</template>

<script setup>
import * as Blockly from 'blockly/core';
import { onMounted, onUnmounted, ref } from 'vue';
import { useStore } from '../store/store';
import { CoronaTheme } from '../configs/theme';

const store = useStore();
const visible = ref(false);
const flyoutMode = ref(localStorage.getItem('flyout') || 'full');
const themeMode = ref(localStorage.getItem('theme') || 'auto');

function close() {
  visible.value = false;
}

function handleClick() {
  visible.value = true;
}

function setFlyout() {
  const flyout = document.querySelector('div.injectionDiv > svg.blocklyFlyout');
  if (flyout) {
    if (flyoutMode.value === 'fixed') {
      flyout.style.width = '320px';
      flyout.style.minWidth = '320px';
    } else {
      flyout.style.width = '';
      flyout.style.minWidth = '';
    }
  }
}

function handleFlyoutChange(value) {
  flyoutMode.value = value;
  localStorage.setItem('flyout', value);
  setFlyout();
}

function getEffectiveTheme(value) {
  if (value === 'auto') {
    return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  }
  return value;
}

function setTheme(value) {
  const theme = getEffectiveTheme(value);
  document.body.setAttribute('arco-theme', theme);

  const ws = store.workspace.value;
  if (ws) {
    try {
      const blocklyTheme = theme === 'dark'
        ? CoronaTheme
        : Blockly.Themes.Classic;
      ws.setTheme(blocklyTheme);
    } catch {}
  }
}

function handleThemeChange(value) {
  setTheme(value);
  themeMode.value = value;
  localStorage.setItem('theme', value);
}

function onEscape(e) {
  if (e.key === 'Escape' && visible.value) {
    close();
  }
}

let mediaListener = null;

onMounted(() => {
  setTheme(localStorage.getItem('theme') || 'auto');

  mediaListener = function (event) {
    if (themeMode.value === 'auto') {
      setTheme(event.matches ? 'dark' : 'light');
    }
  };
  window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', mediaListener);

  document.addEventListener('keydown', onEscape);
});

onUnmounted(() => {
  if (mediaListener) {
    window.matchMedia('(prefers-color-scheme: dark)').removeEventListener('change', mediaListener);
    mediaListener = null;
  }
  document.removeEventListener('keydown', onEscape);
});

defineExpose({ handleClick });
</script>