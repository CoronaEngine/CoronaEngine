<template>
  <div
    v-if="enabled && cameraId" class="restir-settings no-drag" @mousedown.stop @pointerdown.stop @click.stop @wheel.stop
    @keydown.esc.stop="open = false; trigger?.focus()">
    <button ref="trigger" type="button" class="restir-trigger" :aria-expanded="open" @click="open = !open">ReSTIR 参数</button>
    <Teleport to="body">
    <form
      v-if="open" novalidate class="restir-panel no-drag" aria-label="ReSTIR 参数" @submit.prevent="apply" @input="saved = false"
      @mousedown.stop @pointerdown.stop @click.stop @wheel.stop @keydown.esc.stop="open = false; trigger?.focus()">
      <header><strong>ReSTIR 参数</strong><button type="button" aria-label="关闭 ReSTIR 参数" @click="open = false">×</button></header>
      <p class="hint">当前相机 · 应用后重置历史 · 随场景保存</p>
      <p v-if="busy && !loaded" role="status" class="hint">正在准备 ReSTIR 并读取当前相机参数…</p>
      <fieldset :disabled="busy || !loaded">
        <legend>路径追踪</legend>
        <label v-for="[key, label, min, max, step, hint] in restirPathFields" :key="key" :title="hint">
          <span>{{ label }}</span>
          <input v-model.number="draft[key]" type="number" :min="min" :max="max" :step="step" :aria-label="label" />
        </label>
      </fieldset>
      <fieldset v-for="group in restirGroups" :key="group.key" :disabled="busy || !loaded">
        <legend>{{ group.label }}</legend>
        <label title="关闭后保留本组参数，再次启用时继续使用。">
          <span>启用{{ group.label }}</span><input v-model="draft[group.key].open" type="checkbox" :aria-label="'启用' + group.label" />
        </label>
        <fieldset :disabled="!draft[group.key].open" :aria-label="group.prefix + ' 参数'">
          <label v-for="[key, label, min, max, step, hint] in group.fields" :key="key" :title="hint">
            <span>{{ label }}</span>
            <input v-model.number="draft[group.key][key]" type="number" :min="min" :max="max" :step="step" :aria-label="group.prefix + ' ' + label" />
          </label>
          <label v-if="group.key === 'direct'" title="启用成对多重重要性采样（Pairwise MIS）；修改后可能需要重新编译着色器。">
            <span>成对 MIS（Pairwise）</span><input v-model="draft.direct.pairwise" type="checkbox" aria-label="DI 成对 MIS" />
          </label>
          <fieldset v-for="reuse in restirReuseGroups" :key="reuse.key">
            <legend>{{ reuse.label }}</legend>
            <label title="关闭后保留本组参数，再次启用时继续使用。">
              <span>启用{{ reuse.label }}</span>
              <input v-model="draft[group.key][reuse.key].open" type="checkbox" :aria-label="'启用 ' + group.prefix + ' ' + reuse.label" />
            </label>
            <fieldset :disabled="!draft[group.key][reuse.key].open" :aria-label="group.prefix + ' ' + reuse.label + '参数'">
              <label v-for="[key, label, min, max, step, hint] in reuse.fields" :key="key" :title="hint">
                <span>{{ label }}</span>
                <input v-model.number="draft[group.key][reuse.key][key]" type="number" :min="min" :max="max" :step="step" :aria-label="group.prefix + ' ' + reuse.label + label" />
              </label>
            </fieldset>
          </fieldset>
          <fieldset v-if="group.key === 'indirect'" disabled aria-label="GI 分辨率比例（暂不可用）">
            <legend>分辨率比例（暂不可用）</legend>
            <label title="原生默认比例为 2；当前尚未接入渲染，不能调整。">
              <span>分辨率比例</span><input type="number" value="2" min="1" max="4" step="1" disabled readonly aria-label="GI 分辨率比例（暂不可用）" />
            </label>
            <p class="hint">当前尚未接入渲染，以上控件暂不可用。</p>
          </fieldset>
        </fieldset>
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
import { restirPathFields, restirGroups, restirReuseGroups, useRestirSettings } from '@/utils/restirSettings.js';
const props = defineProps({ sceneId: { type: String, default: '' }, cameraId: { type: String, default: '' }, enabled: Boolean });
const trigger = ref(null);
const { open, busy, error, saved, draft, loaded, apply, reset } = useRestirSettings(props, editorApi.sceneTools);
</script>

<style scoped>
.restir-settings { position: relative; -webkit-app-region: no-drag; }
button { border: 1px solid #645638; border-radius: 4px; background: #2e2a21; color: #eee3c8; padding: 4px 8px; cursor: pointer; font-size: 11px; }
button:disabled { opacity: .5; cursor: wait; }
button:focus-visible, input:focus-visible { outline: 2px solid #dfbd6c; outline-offset: 2px; }
.restir-trigger { white-space: nowrap; }
.restir-panel { position: fixed; top: 70px; right: 24px; z-index: 2147483500; width: 350px; max-width: calc(100vw - 48px); max-height: calc(100vh - 100px); overflow-y: auto; padding: 12px; border: 1px solid #74603b; border-radius: 7px; background: #24221e; color: #eee3c8; box-shadow: 0 6px 24px #0008; font-size: 12px; -webkit-app-region: no-drag; }
header, footer, label { display: flex; justify-content: space-between; align-items: center; gap: 10px; }
header { font-size: 13px; }
fieldset { min-width: 0; margin: 10px 0; padding: 8px; border: 1px solid #4d4434; border-radius: 4px; }
fieldset fieldset { margin-bottom: 0; }
fieldset:disabled { opacity: .55; }
legend { color: #dbc48d; padding: 0 4px; }
label { margin: 6px 0; }
input[type=number] { width: 78px; padding: 3px 5px; color: #f2eee5; background: #161614; border: 1px solid #62563e; border-radius: 3px; }
input[type=checkbox] { accent-color: #d8b86c; }
.hint { color: #bdb6a7; font-size: 11px; margin: 7px 0; }
.error { color: #f9a5a5; }
footer { position: sticky; bottom: -12px; background: #24221e; padding: 8px 0; }
</style>
