<template>
  <div v-if="enabled && cameraId" class="relax-settings no-drag" @mousedown.stop @pointerdown.stop @click.stop @wheel.stop
    @keydown.esc.stop="open = false; trigger?.focus()">
    <button ref="trigger" type="button" class="relax-trigger" :aria-expanded="open" @click="open = !open">RELAX 参数</button>
    <Teleport to="body">
    <form v-if="open" novalidate class="relax-panel no-drag" aria-label="RELAX 参数" @submit.prevent="apply" @input="saved = false"
      @mousedown.stop @pointerdown.stop @click.stop @wheel.stop @keydown.esc.stop="open = false; trigger?.focus()">
      <header><strong>RELAX 参数</strong><button type="button" aria-label="关闭 RELAX 参数" @click="open = false">×</button></header>
      <p class="hint">当前相机 · 更改数值后重置历史 · 随场景保存</p>
      <fieldset v-for="group in relaxGroups" :key="group.label" :disabled="busy">
        <legend>{{ group.label }}</legend>
        <label v-for="[key, label, , min, max, step, hint] in group.fields" :key="key" :title="hint">
          <span>{{ label }}</span>
          <input v-model.number="draft[key]" type="number" :min="min" :max="max" :step="step" :aria-label="label" />
        </label>
      </fieldset>
      <fieldset :disabled="busy">
        <legend>重建与滤波开关</legend>
        <label title="抑制孤立的高亮噪声"><span>亮点抑制</span><input v-model="draft.anti_firefly" type="checkbox" aria-label="亮点抑制" /></label>
        <label title="按粗糙度差异阻止跨边缘混合"><span>粗糙度边缘停止</span><input v-model="draft.roughness_stopping" type="checkbox" aria-label="粗糙度边缘停止" /></label>
        <div class="reconstruction"><span>命中距离重建</span>
          <div role="group" aria-label="命中距离重建">
            <button v-for="(label, index) in ['关闭', '3×3', '5×5']" :key="index" type="button"
              :aria-pressed="draft.reconstruction === index" @click="draft.reconstruction = index; saved = false">{{ label }}</button>
          </div>
        </div>
        <p class="hint">重建可填补缺失样本；开启会增加计算量。</p>
      </fieldset>
      <p v-if="error" role="alert" class="error">{{ error }}</p>
      <p v-else-if="saved" role="status" class="hint">已应用到当前相机</p>
      <footer><button type="button" :disabled="busy" @click="reset">恢复默认值</button><button type="submit" :disabled="busy">{{ busy ? '处理中…' : '应用' }}</button></footer>
    </form>
    </Teleport>
  </div>
</template>

<script setup>
import { ref } from 'vue';
import { editorApi } from '@/api/editorApi.js';
import { relaxGroups, useRelaxSettings } from '@/utils/relaxSettings.js';
const props = defineProps({ sceneId: { type: String, default: '' }, cameraId: { type: String, default: '' }, enabled: Boolean });
const trigger = ref(null);
const { open, busy, error, saved, draft, apply, reset } = useRelaxSettings(props, editorApi.sceneTools);
</script>

<style scoped>
.relax-settings { position: relative; -webkit-app-region: no-drag; }
button { border: 1px solid #645638; border-radius: 4px; background: #2e2a21; color: #eee3c8; padding: 4px 8px; cursor: pointer; font-size: 11px; }
button:disabled { opacity: .5; cursor: wait; }
button:focus-visible, input:focus-visible { outline: 2px solid #dfbd6c; outline-offset: 2px; }
.relax-trigger { white-space: nowrap; }
.relax-panel { position: fixed; top: 70px; right: 24px; z-index: 2147483500; width: 330px; max-width: calc(100vw - 48px); max-height: calc(100vh - 100px); overflow-y: auto; padding: 12px; border: 1px solid #74603b; border-radius: 7px; background: #24221e; color: #eee3c8; box-shadow: 0 6px 24px #0008; font-size: 12px; -webkit-app-region: no-drag; }
header, footer, label, .reconstruction { display: flex; justify-content: space-between; align-items: center; gap: 10px; }
header { font-size: 13px; }
fieldset { margin: 10px 0; padding: 8px; border: 1px solid #4d4434; border-radius: 4px; }
legend { color: #dbc48d; padding: 0 4px; }
label { margin: 6px 0; }
input[type=number] { width: 78px; padding: 3px 5px; color: #f2eee5; background: #161614; border: 1px solid #62563e; border-radius: 3px; }
input[type=checkbox] { accent-color: #d8b86c; }
.hint { color: #bdb6a7; font-size: 11px; margin: 7px 0; }
.error { color: #f9a5a5; }
.reconstruction button { padding: 3px 6px; }
.reconstruction button[aria-pressed=true] { background: #6a552b; border-color: #d8b86c; }
footer { position: sticky; bottom: -12px; background: #24221e; padding: 8px 0; }
</style>
