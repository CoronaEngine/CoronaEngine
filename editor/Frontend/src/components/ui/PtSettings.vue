<template>
  <div v-if="enabled && cameraId" class="pt-settings no-drag" @mousedown.stop @pointerdown.stop @click.stop @wheel.stop
    @keydown.esc.stop="open = false; trigger?.focus()">
    <button ref="trigger" type="button" class="pt-trigger" :aria-expanded="open" @click="open = !open">PT 参数</button>
    <Teleport to="body">
    <form v-if="open" novalidate class="pt-panel no-drag" aria-label="PT 参数" @submit.prevent="apply" @input="saved = false"
      @mousedown.stop @pointerdown.stop @click.stop @wheel.stop @keydown.esc.stop="open = false; trigger?.focus()">
      <header><strong>PT 参数</strong><button type="button" aria-label="关闭 PT 参数" @click="open = false">×</button></header>
      <p class="hint">当前相机 · 应用后重置历史 · 随场景保存</p>
      <p v-if="busy && !loaded" role="status" class="hint">正在读取当前相机参数…</p>
      <fieldset v-for="group in ptGroups" :key="group.label" :disabled="busy || !loaded">
        <legend>{{ group.label }}</legend>
        <label v-for="[key, label, , min, max, step, hint] in group.fields" :key="key" :title="hint">
          <span>{{ label }}</span>
          <input v-model.number="draft[key]" type="number" :min="min" :max="max" :step="step" :aria-label="label" />
        </label>
      </fieldset>
      <fieldset disabled aria-label="自适应采样（暂不可用）">
        <legend>自适应采样（暂不可用）</legend>
        <label><span>启用自适应采样</span><input type="checkbox" :checked="adaptive.enabled" aria-label="启用自适应采样" /></label>
        <label><span>收敛阈值</span><input type="number" :value="adaptive.threshold" min="0" max="1" step="0.0001" aria-label="收敛阈值" /></label>
        <label><span>最少样本数</span><input type="number" :value="adaptive.start_index" min="32" max="1024" step="1" aria-label="最少样本数" /></label>
        <p class="hint">当前 PT 尚未接入自适应采样，以上控件暂不可用。</p>
      </fieldset>
      <p v-if="error" role="alert" class="error">{{ error }}</p>
      <p v-else-if="saved" role="status" class="hint">已应用到当前相机</p>
      <footer><button type="button" :disabled="busy || !loaded" @click="reset">恢复默认值</button><button type="submit" :disabled="busy || !loaded">{{ busy ? '处理中…' : '应用' }}</button></footer>
    </form>
    </Teleport>
  </div>
</template>

<script setup>
import { ref } from 'vue';
import { editorApi } from '@/api/editorApi.js';
import { ptGroups, usePtSettings } from '@/utils/ptSettings.js';
const props = defineProps({ sceneId: { type: String, default: '' }, cameraId: { type: String, default: '' }, enabled: Boolean });
const trigger = ref(null);
const { open, busy, error, saved, draft, adaptive, loaded, apply, reset } = usePtSettings(props, editorApi.sceneTools);
</script>

<style scoped>
.pt-settings { position: relative; -webkit-app-region: no-drag; }
button { border: 1px solid #645638; border-radius: 4px; background: #2e2a21; color: #eee3c8; padding: 4px 8px; cursor: pointer; font-size: 11px; }
button:disabled { opacity: .5; cursor: wait; }
button:focus-visible, input:focus-visible { outline: 2px solid #dfbd6c; outline-offset: 2px; }
.pt-trigger { white-space: nowrap; }
.pt-panel { position: fixed; top: 70px; right: 24px; z-index: 2147483500; width: 330px; max-width: calc(100vw - 48px); max-height: calc(100vh - 100px); overflow-y: auto; padding: 12px; border: 1px solid #74603b; border-radius: 7px; background: #24221e; color: #eee3c8; box-shadow: 0 6px 24px #0008; font-size: 12px; -webkit-app-region: no-drag; }
header, footer, label { display: flex; justify-content: space-between; align-items: center; gap: 10px; }
header { font-size: 13px; }
fieldset { margin: 10px 0; padding: 8px; border: 1px solid #4d4434; border-radius: 4px; }
fieldset:disabled { opacity: .55; }
legend { color: #dbc48d; padding: 0 4px; }
label { margin: 6px 0; }
input[type=number] { width: 78px; padding: 3px 5px; color: #f2eee5; background: #161614; border: 1px solid #62563e; border-radius: 3px; }
input[type=checkbox] { accent-color: #d8b86c; }
.hint { color: #bdb6a7; font-size: 11px; margin: 7px 0; }
.error { color: #f9a5a5; }
footer { position: sticky; bottom: -12px; background: #24221e; padding: 8px 0; }
</style>
