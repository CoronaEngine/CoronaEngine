<!-- 小世界资源面板：选择对象、材料和世界碎片，不处理键盘输入或 Three.js。 -->
<template>
  <aside class="resource-panel" @pointerdown.stop @click.stop>
    <div class="panel-title">
      <h2>小世界编辑</h2>
      <span>{{ objectCount }}/128</span>
    </div>

    <template v-if="mode === 'build'">
      <label class="field-label" for="ugc-object-type">对象</label>
      <select id="ugc-object-type" v-model="objectType">
        <option value="block">基础方块</option>
        <option value="spawn">出生点</option>
        <option value="target">目标</option>
      </select>

      <label v-if="objectType === 'block'" class="field-label" for="ugc-material">材料</label>
      <select v-if="objectType === 'block'" id="ugc-material" v-model="materialId">
        <option v-for="item in materials" :key="item.id" :value="item.id">
          {{ item.name }} ×{{ item.quantity }}
        </option>
      </select>

      <div class="position-fields">
        <span class="field-label">落点坐标</span>
        <div class="coordinate-grid">
          <label v-for="(axis, index) in ['X', 'Y', 'Z']" :key="axis">
            <span>{{ axis }}</span>
            <input
              v-model.number="placementPosition[index]"
              type="number"
              step="1"
              min="-30"
              max="30"
              :aria-label="`${axis} 坐标`"
            />
          </label>
        </div>
      </div>

      <button
        class="action-button"
        type="button"
        :disabled="objectType === 'block' && !materialId"
        @click="placeObject"
      >
        放置选中对象
      </button>

      <div v-if="objects.length" class="object-list">
        <div class="subheading">场景对象</div>
        <button
          v-for="object in objects"
          :key="object.id"
          type="button"
          :class="['object-row', { selected: selectedObjectId === object.id }]"
          @click="selectObject(object.id)"
        >
          <span>{{ objectLabel(object) }}</span>
          <span>{{ object.id.slice(-6) }}</span>
        </button>
        <button v-if="selectedObjectId" class="danger-button" type="button" @click="deleteSelected">
          删除选中对象
        </button>
      </div>

      <div v-if="selectedObject?.type === 'target' && fragments.length" class="binding-box">
        <div class="subheading">目标逻辑</div>
        <label class="field-label" for="ugc-fragment">世界碎片</label>
        <select id="ugc-fragment" v-model="fragmentId">
          <option v-for="fragment in fragments" :key="fragment.id" :value="fragment.id">
            {{ fragment.name }}
          </option>
        </select>
        <button class="action-button secondary" type="button" @click="bindFragment">
          绑定到目标
        </button>
      </div>
    </template>

    <div class="resource-list">
      <div class="subheading">材料</div>
      <div v-for="item in materials" :key="item.id" class="resource-row">
        <span>{{ item.name }}</span>
        <strong>{{ item.quantity }}</strong>
      </div>
      <div class="subheading">世界碎片</div>
      <template v-if="fragments.length">
        <div v-for="item in fragments" :key="item.id" class="resource-row">
          <span>{{ item.name }}</span>
          <strong>可用</strong>
        </div>
      </template>
      <span v-else class="empty-text">暂无世界碎片</span>
    </div>

    <p v-if="selectedObject && mode === 'build'" class="selection-info">
      已选：{{ objectLabel(selectedObject) }}
    </p>
    <p v-if="error" class="error-message">{{ error }}</p>
  </aside>
</template>

<script setup>
import { computed, ref, watch } from 'vue';

const props = defineProps({
  world: { type: Object, default: null },
  resources: { type: Object, default: () => ({ materials: [], fragments: [] }) },
  mode: { type: String, default: 'build' },
  error: { type: String, default: '' },
});

const emit = defineEmits(['place', 'delete', 'bind']);
const objectType = ref('block');
const materialId = ref('');
const fragmentId = ref('');
const selectedObjectId = ref('');
const placementPosition = ref([0, 0, 0]);

const materials = computed(() => props.resources.materials || []);
const fragments = computed(() => props.resources.fragments || []);
const objects = computed(() => props.world?.objects || []);
const selectedObject = computed(
  () => objects.value.find((object) => object.id === selectedObjectId.value) || null
);
const objectCount = computed(() => objects.value.length);

watch(
  materials,
  (items) => {
    if (!items.some((item) => item.id === materialId.value)) materialId.value = items[0]?.id || '';
  },
  { immediate: true }
);
watch(
  fragments,
  (items) => {
    if (!items.some((item) => item.id === fragmentId.value)) fragmentId.value = items[0]?.id || '';
  },
  { immediate: true }
);
watch(
  objects,
  (items) => {
    if (!items.some((item) => item.id === selectedObjectId.value)) {
      selectedObjectId.value =
        items.find((item) => item.type === 'target')?.id || items[0]?.id || '';
    }
  },
  { immediate: true }
);
watch(
  selectedObject,
  (object) => {
    if (!object) return;
    const offset = object.type === 'spawn' ? 1.7 : object.type === 'target' ? 1 : 0.5;
    placementPosition.value = [object.position[0], object.position[1] - offset, object.position[2]];
  },
  { immediate: true }
);

function selectObject(id) {
  selectedObjectId.value = id;
}

function placeObject() {
  emit('place', {
    type: objectType.value,
    materialId: objectType.value === 'block' ? materialId.value : null,
    position: placementPosition.value.map(Number),
    fragmentIds: [],
  });
}

function deleteSelected() {
  if (selectedObjectId.value) emit('delete', selectedObjectId.value);
}

function bindFragment() {
  if (selectedObject.value?.type === 'target' && fragmentId.value) {
    emit('bind', { objectId: selectedObjectId.value, fragmentId: fragmentId.value });
  }
}

function objectLabel(object) {
  return { block: '方块', spawn: '出生点', target: '目标' }[object.type] || object.type;
}
</script>

<style scoped>
.resource-panel {
  position: absolute;
  z-index: 8;
  top: 86px;
  right: 20px;
  width: 250px;
  max-height: calc(100vh - 110px);
  overflow: auto;
  padding: 14px;
  border: 1px solid var(--game-border, #443c2a);
  background: var(--game-panel, #171714);
  box-shadow: 0 10px 24px rgb(0 0 0 / 24%);
  color: var(--game-text, #e8e3d6);
  pointer-events: auto;
}

.panel-title,
.resource-row,
.object-row {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.panel-title {
  margin-bottom: 12px;
}

.panel-title h2 {
  margin: 0;
  font-size: 15px;
}

.panel-title span,
.field-label,
.subheading,
.empty-text,
.selection-info {
  color: var(--game-muted, #aaa594);
  font-size: 11px;
}

.field-label,
.subheading {
  display: block;
  margin: 8px 0 5px;
}

.resource-panel select,
.resource-panel input,
.action-button,
.danger-button {
  min-height: 32px;
  border: 1px solid var(--game-border, #443c2a);
  background: var(--game-panel-deep, #10100e);
  color: var(--game-text, #e8e3d6);
  font: inherit;
  font-size: 12px;
}

.resource-panel select,
.resource-panel input {
  width: 100%;
  padding: 0 8px;
}

.position-fields {
  margin-top: 8px;
}

.coordinate-grid {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 5px;
}

.coordinate-grid label span {
  display: block;
  margin-bottom: 3px;
  color: var(--game-muted, #aaa594);
  font-size: 10px;
}

.action-button,
.danger-button {
  width: 100%;
  margin-top: 8px;
  cursor: pointer;
}

.action-button:hover:not(:disabled),
.action-button:focus-visible,
.object-row:hover,
.object-row:focus-visible {
  border-color: var(--game-cyan, #e8ca80);
  outline: none;
}

.action-button:disabled,
.danger-button:disabled {
  cursor: not-allowed;
  opacity: 0.45;
}

.danger-button {
  border-color: #704e53;
  color: #d99a9a;
}

.object-list,
.binding-box,
.resource-list {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin-top: 10px;
}

.object-row {
  width: 100%;
  padding: 7px 8px;
  text-align: left;
}

.object-row.selected {
  border-color: var(--game-gold, #c6a15b);
}

.resource-row {
  padding: 5px 0;
  border-bottom: 1px solid rgb(107 91 54 / 55%);
  font-size: 12px;
}

.resource-row strong {
  color: var(--game-gold, #c6a15b);
  font-size: 11px;
}

.selection-info,
.error-message {
  margin: 10px 0 0;
}

.error-message {
  color: #e0a2a2;
  font-size: 11px;
}

@media (max-width: 760px) {
  .resource-panel {
    top: auto;
    right: 10px;
    bottom: 94px;
    left: 10px;
    width: auto;
    max-height: 42vh;
  }
}
</style>
