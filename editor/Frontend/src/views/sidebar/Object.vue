<template>
  <div class="object-panel-shell flex flex-col flex-1 min-h-0 h-full w-full rounded-lg overflow-hidden relative">
    <DockTitleBar
      v-if="!isDocked"
      title="对象"
      extraClass="bg-[#D8B86C] rounded-t-md text-sm"
      routePath="/Object"
      @close="closeFloat"
    />

    <div v-if="loading" class="object-empty">正在读取对象属性…</div>
    <div v-else-if="!selectedActorName" class="object-empty">
      <strong>还没有选中对象</strong>
      <span>在 3D 视口或场景管理中选择一个模型后，这里会显示可调整的属性。</span>
    </div>

    <div v-else class="object-scroll">
      <header class="object-heading">
        <div>
          <span class="object-type">{{ actor.type || 'model' }}</span>
          <h2>{{ actor.name }}</h2>
        </div>
        <button type="button" class="save-button" :disabled="saving" @click="saveActor">
          {{ saving ? '保存中…' : '保存对象' }}
        </button>
      </header>

      <div
        v-if="actor.loadStatus !== 'loaded'"
        data-testid="actor-placeholder-warning"
        class="placeholder-warning"
      >
        <strong>资源未加载，当前显示为占位项</strong>
        <span>{{ actor.loadError?.message || actor.loadError || '模型资源不可用' }}</span>
        <button
          type="button"
          class="inline-button"
          :disabled="placeholderRebinding"
          @click="rebindPlaceholderResource"
        >
          {{ placeholderRebinding ? '重新绑定中…' : '重新绑定资源' }}
        </button>
      </div>

      <section class="property-section" data-assistant-title="对象名称" data-assistant-description="修改后可用新的对象名称在节点积木中准确引用这个模型。">
        <div class="section-title">模型</div>
        <div class="property-row">
          <label for="actor-alias">名称</label>
          <input id="actor-alias" v-model="aliasDraft" type="text" :disabled="aliasSaving" @keydown.enter.prevent="commitAlias" @keydown.esc.prevent="resetAlias" />
          <button type="button" class="inline-button" :disabled="!aliasDirty || aliasSaving" @click="commitAlias">应用</button>
        </div>
        <p v-if="aliasError" class="property-error">{{ aliasError }}</p>

        <div class="property-row">
          <label>渲染空间</label>
          <div class="segmented">
            <button type="button" :class="{ active: !actor.followCamera }" @click="setRenderSpace(false)">场景</button>
            <button type="button" :class="{ active: actor.followCamera }" @click="setRenderSpace(true)">屏幕 UI</button>
          </div>
        </div>

        <div class="property-row property-row-wide">
          <label>模型资源</label>
          <input :value="actor.modelPath" type="text" readonly placeholder="未设置模型资源" />
          <button type="button" class="inline-button" @click="selectModelFile">浏览</button>
        </div>
      </section>

      <section class="property-section property-section-collapsible" data-guidance="object-transform" data-assistant-title="对象变换" data-assistant-description="修改模型在场景中的位置、旋转和大小。">
        <button
          type="button"
          class="section-toggle"
          :aria-expanded="!collapsedSections.transform"
          @click="togglePropertySection('transform')"
        >
          <span>变换</span>
          <span class="section-chevron" :class="{ expanded: !collapsedSections.transform }">&#8964;</span>
        </button>
        <div v-show="!collapsedSections.transform" class="section-collapsible-body">
          <div v-for="group in transformGroups" :key="group.key" class="vector-group">
            <span>{{ group.label }}</span>
            <label v-for="axis in axes" :key="axis" :class="`axis-${axis}`">
              <b>{{ axis.toUpperCase() }}</b>
              <input
                v-model.number="actor.transform[group.key][axis]"
                type="number"
                :step="group.step"
                :data-guidance="guidanceKeyForTransform(group.key, axis)"
                :data-assistant-title="`${group.label} ${axis.toUpperCase()}`"
                @input="scheduleTransform(group.operation)"
                @change="applyTransform(group.operation, axis)"
              />
            </label>
          </div>
        </div>
      </section>

      <section class="property-section" data-assistant-title="摄像机跟随" data-assistant-description="启用后模型会按照偏移值跟随编辑器或游戏摄像机。">
        <div class="section-title section-title-row">
          <span>摄像机跟随</span>
          <label class="switch-label"><input v-model="actor.cameraLock.enabled" type="checkbox" @change="updateCameraLock" />启用</label>
        </div>
        <div v-if="actor.cameraLock.enabled" class="vector-group">
          <span>位置偏移</span>
          <label v-for="axis in axes" :key="axis" :class="`axis-${axis}`">
            <b>{{ axis.toUpperCase() }}</b>
            <input v-model.number="actor.cameraLock.position[axis]" type="number" step="0.1" @change="updateCameraLockOffset" />
          </label>
        </div>
      </section>

      <section v-if="actor.loadStatus === 'loaded'" class="property-section" data-assistant-title="碰撞设置" data-assistant-description="选择模型参与碰撞检测时使用的形状。">
        <div class="section-title">碰撞</div>
        <div class="property-row">
          <label for="actor-collision">碰撞形状</label>
          <div id="actor-collision" class="collision-options" role="radiogroup" aria-label="碰撞形状">
            <label
              v-for="option in collisionOptions"
              :key="option.value"
              :class="{ active: actor.collision === option.value }"
            >
              <input
                v-model="actor.collision"
                type="radio"
                name="actor-collision-shape"
                :value="option.value"
                @change="updateCollision"
              />
              <span>{{ option.label }}</span>
            </label>
          </div>
        </div>
      </section>

      <section
        v-if="actor.loadStatus === 'loaded'"
        class="property-section property-section-collapsible"
        data-guidance="object-physics"
        data-assistant-title="物理设置"
        data-assistant-description="控制模型是否参与物理模拟，以及质量、弹性、阻尼和轴向锁定。"
      >
        <button
          type="button"
          class="section-toggle"
          :aria-expanded="!collapsedSections.physics"
          @click="togglePropertySection('physics')"
        >
          <span>物理</span>
          <span class="section-chevron" :class="{ expanded: !collapsedSections.physics }">&#8964;</span>
        </button>
        <div v-show="!collapsedSections.physics" class="section-collapsible-body">
          <div class="physics-enable-row">
            <span>物理模拟</span>
            <label class="switch-label"><input v-model="actor.mechanics.physicsEnabled" data-guidance="object-physics-enabled" type="checkbox" @change="updateMechanic('SetPhysicsEnabled', actor.mechanics.physicsEnabled)" />启用</label>
          </div>
          <div class="physics-grid" :class="{ disabled: !actor.mechanics.physicsEnabled }">
            <label>质量<input v-model.number="actor.mechanics.mass" data-guidance="object-physics-mass" type="number" min="0" step="0.1" :disabled="!actor.mechanics.physicsEnabled" @change="updateMechanic('SetMass', actor.mechanics.mass)" /></label>
            <label>弹性<input v-model.number="actor.mechanics.restitution" type="number" min="0" max="1" step="0.05" :disabled="!actor.mechanics.physicsEnabled" @change="updateMechanic('SetRestitution', actor.mechanics.restitution)" /></label>
            <label>阻尼<input v-model.number="actor.mechanics.damping" type="number" min="0" max="1" step="0.01" :disabled="!actor.mechanics.physicsEnabled" @change="updateMechanic('SetDamping', actor.mechanics.damping)" /></label>
          </div>
          <div class="lock-row">
            <span>锁定移动</span>
            <label v-for="(axis, index) in axes" :key="axis"><input v-model="actor.mechanics.linearLock[index]" type="checkbox" @change="updateLocks('SetLinearLock', actor.mechanics.linearLock)" />{{ axis.toUpperCase() }}</label>
          </div>
          <div class="lock-row">
            <span>锁定旋转</span>
            <label v-for="(axis, index) in axes" :key="axis"><input v-model="actor.mechanics.angularLock[index]" type="checkbox" @change="updateLocks('SetAngularLock', actor.mechanics.angularLock)" />{{ axis.toUpperCase() }}</label>
          </div>
        </div>
      </section>

    </div>
  </div>
</template>

<script setup>
import { computed, onMounted, onUnmounted, reactive, ref } from 'vue';
import DockTitleBar from '@/components/ui/DockTitleBar.vue';
import { useDockPanel } from '@/composables/useDockPanel.js';
import { useErrorHandler } from '@/composables/useErrorHandler.js';
import { editorApi } from '@/api/editorApi.js';
import { DEFAULT_SCENE_NAME } from '@/utils/constants.js';
import { getActorContext } from '@/blockly/composables/useActorContext.js';
import { cabbageContextService } from '@/services/cabbageAssistantContextService.js';

const { closePanel, isDocked } = useDockPanel();
const { error: logError } = useErrorHandler('Object');
const axes = ['x', 'y', 'z'];
const collapsedSections = reactive({
  transform: true,
  physics: true,
});

function togglePropertySection(section) {
  if (Object.prototype.hasOwnProperty.call(collapsedSections, section)) {
    collapsedSections[section] = !collapsedSections[section];
  }
}
const collisionOptions = [
  { value: 'none', label: '无' },
  { value: 'box', label: '包围盒' },
  { value: 'mesh', label: '模型网格' },
];
const transformGroups = [
  { key: 'position', label: '位置', operation: 'SetPosition', step: 0.1 },
  { key: 'rotation', label: '旋转', operation: 'SetRotation', step: 1 },
  { key: 'scale', label: '缩放', operation: 'SetScale', step: 0.05 },
];

function guidanceKeyForTransform(groupKey, axis) {
  const keys = {
    'position:x': 'object-position-x',
    'rotation:y': 'object-rotation-y',
    'scale:x': 'object-scale-x',
  };
  return keys[`${groupKey}:${axis}`] || undefined;
}

const selectedSceneName = ref(DEFAULT_SCENE_NAME);
const selectedActorName = ref('');
const loading = ref(false);
const saving = ref(false);
const aliasDraft = ref('');
const aliasSaving = ref(false);
const aliasError = ref('');
const placeholderRebinding = ref(false);
let selectionToken = null;
let transformToken = null;
let loadSequence = 0;
const updateTimers = new Map();
const pendingTransformUpdates = new Map();
let transformFrameId = null;
let transformFlushPromise = Promise.resolve();
let lastSavedCollision = 'none';
const TRANSFORM_EPSILON = 1e-5;
const viewportTransformBaseline = {
  actorKey: '',
  position: null,
  rotation: null,
  scale: null,
};

const actor = reactive({
  name: '',
  type: '',
  handle: 0,
  actorGuid: '',
  loadStatus: 'loaded',
  loadError: '',
  modelPath: '',
  followCamera: false,
  transform: {
    position: { x: 0, y: 0, z: 0 },
    rotation: { x: 0, y: 0, z: 0 },
    scale: { x: 1, y: 1, z: 1 },
  },
  collision: 'none',
  mechanics: {
    physicsEnabled: true,
    mass: 1,
    restitution: 0.8,
    damping: 0.99,
    linearLock: [false, false, false],
    angularLock: [false, false, false],
  },
  cameraLock: {
    enabled: false,
    position: { x: 0, y: 0, z: 2 },
  },
});

const unwrap = (value) => value?.data ?? value ?? {};
const aliasDirty = computed(() => aliasDraft.value.trim() !== actor.name);
const normalizeCollisionType = (value) => {
  const raw = value?.type ?? value?.shape ?? value;
  if (raw === false || raw === 0) return 'none';
  if (raw === true || raw === 1) return 'box';
  if (raw === 2) return 'mesh';
  const candidate = String(raw ?? '').trim().toLowerCase();
  if (['none', 'disabled', 'off'].includes(candidate)) return 'none';
  if (['mesh', 'model_mesh', 'model-mesh'].includes(candidate)) return 'mesh';
  if (['box', 'aabb', 'bounding_box', 'bounding-box'].includes(candidate)) return 'box';
  return 'box';
};
const readFollowCamera = (data) => data?.render_space === 'ui' || data?.follow_camera === true || data?.follow_camera === 1 || data?.follow_camera === 'true' || data?.follow_camera === '1';

function assignVector(target, value, fallback) {
  target.x = Number(value?.x ?? value?.[0] ?? fallback.x);
  target.y = Number(value?.y ?? value?.[1] ?? fallback.y);
  target.z = Number(value?.z ?? value?.[2] ?? fallback.z);
}

function cloneVector(value) {
  return { x: Number(value?.x) || 0, y: Number(value?.y) || 0, z: Number(value?.z) || 0 };
}

function currentActorKey() {
  return `${selectedSceneName.value}::${selectedActorName.value}`;
}

function syncViewportTransformBaseline() {
  viewportTransformBaseline.actorKey = currentActorKey();
  viewportTransformBaseline.position = cloneVector(actor.transform.position);
  viewportTransformBaseline.rotation = cloneVector(actor.transform.rotation);
  viewportTransformBaseline.scale = cloneVector(actor.transform.scale);
}

function clearViewportTransformBaseline() {
  viewportTransformBaseline.actorKey = '';
  viewportTransformBaseline.position = null;
  viewportTransformBaseline.rotation = null;
  viewportTransformBaseline.scale = null;
}

function vectorChanged(previous, next) {
  if (!previous || !next) return false;
  return ['x', 'y', 'z'].some((axis) => Math.abs(Number(previous[axis] || 0) - Number(next[axis] || 0)) > TRANSFORM_EPSILON);
}

async function loadActor(sceneName, actorName) {
  if (!sceneName || !actorName) return;
  const sequence = ++loadSequence;
  loading.value = true;
  selectedSceneName.value = sceneName;
  selectedActorName.value = actorName;
  try {
    const data = unwrap(await editorApi.scene.getActor(sceneName, actorName));
    if (sequence !== loadSequence || selectedActorName.value !== actorName) return;
    if (!data || data.status === 'error') throw new Error(data?.message || '无法读取对象属性');
    actor.name = String(data.name || actorName);
    actor.type = String(data.actor_type || data.type || 'model');
    actor.handle = Number(data.handle || 0);
    actor.actorGuid = String(data.actor_guid || '');
    actor.loadStatus = String(data.load_status || 'loaded');
    actor.loadError = data.load_error || '';
    actor.modelPath = String(data.model || data.path || data.file || '');
    actor.followCamera = readFollowCamera(data);
    const geometry = data.geometry || {};
    assignVector(actor.transform.position, geometry.position, { x: 0, y: 0, z: 0 });
    assignVector(actor.transform.rotation, geometry.rotation, { x: 0, y: 0, z: 0 });
    assignVector(actor.transform.scale, geometry.scale, { x: 1, y: 1, z: 1 });
    const mechanics = data.mechanics || {};
    actor.collision = normalizeCollisionType(
      data.collision
      ?? data.collision_type
      ?? mechanics.collision_type
      ?? mechanics.collision_shape
    );
    lastSavedCollision = actor.collision;
    actor.mechanics.physicsEnabled = mechanics.physics_enabled !== false;
    actor.mechanics.mass = Number(mechanics.mass ?? 1);
    actor.mechanics.restitution = Number(mechanics.restitution ?? 0.8);
    actor.mechanics.damping = Number(mechanics.damping ?? 0.99);
    actor.mechanics.linearLock = axes.map((_, index) => Boolean(mechanics.linear_lock?.[index]));
    actor.mechanics.angularLock = axes.map((_, index) => Boolean(mechanics.angular_lock?.[index]));
    const cameraLock = data.camera_lock || {};
    actor.cameraLock.enabled = Boolean(cameraLock.enabled ?? cameraLock.lock_to_camera);
    assignVector(actor.cameraLock.position, cameraLock.position_offset, { x: 0, y: 0, z: 2 });
    aliasDraft.value = actor.name;
    aliasError.value = '';
    syncViewportTransformBaseline();
  } catch (error) {
    if (sequence === loadSequence) logError('加载对象数据失败', error);
  } finally {
    if (sequence === loadSequence) loading.value = false;
  }
}

function resetAlias() {
  aliasDraft.value = actor.name;
  aliasError.value = '';
}

async function commitAlias() {
  const nextName = aliasDraft.value.trim();
  const currentName = selectedActorName.value;
  if (!nextName || !currentName || aliasSaving.value) {
    if (!nextName) aliasError.value = '名称不能为空';
    return;
  }
  if (nextName === actor.name) return resetAlias();
  aliasSaving.value = true;
  aliasError.value = '';
  try {
    const result = unwrap(await editorApi.sceneTools.renameActor(selectedSceneName.value, currentName, nextName));
    if (result?.status === 'error') throw new Error(result.message || '修改名称失败');
    const savedName = String(result?.actor?.name || result?.new_name || nextName);
    selectedActorName.value = savedName;
    actor.name = savedName;
    aliasDraft.value = savedName;
  } catch (error) {
    aliasError.value = error?.message || '修改名称失败';
    aliasDraft.value = actor.name;
    logError('修改对象名称失败', error);
  } finally {
    aliasSaving.value = false;
  }
}

function vectorFor(operation) {
  const key = operation === 'SetPosition' ? 'position' : operation === 'SetRotation' ? 'rotation' : 'scale';
  const value = actor.transform[key];
  return [Number(value.x) || 0, Number(value.y) || 0, Number(value.z) || 0];
}

function schedule(key, callback, delay = 120) {
  clearTimeout(updateTimers.get(key));
  updateTimers.set(key, window.setTimeout(() => {
    updateTimers.delete(key);
    callback();
  }, delay));
}

function flushTransformUpdates() {
  transformFrameId = null;
  const updates = Array.from(pendingTransformUpdates.values());
  pendingTransformUpdates.clear();
  if (!selectedSceneName.value || !selectedActorName.value || updates.length === 0) return;

  transformFlushPromise = Promise.all(
    updates.map((update) => {
      const key = update.operation === 'SetPosition'
        ? 'position'
        : update.operation === 'SetRotation'
          ? 'rotation'
          : 'scale';
      return editorApi.scene.setActorTransform(
        selectedSceneName.value,
        selectedActorName.value,
        { [key]: update.vector, persist: false },
      );
    }),
  ).catch((error) => {
    logError('更新对象变换失败', error);
  });
}

function scheduleTransform(operation) {
  if (!selectedActorName.value || !['SetPosition', 'SetRotation', 'SetScale'].includes(operation)) return;
  pendingTransformUpdates.set(operation, {
    operation,
    vector: vectorFor(operation),
  });
  if (transformFrameId === null) {
    transformFrameId = window.requestAnimationFrame(flushTransformUpdates);
  }
}

async function applyTransform(operation, axis = '') {
  if (!selectedActorName.value) return;
  // The native transform update emitted by this Dock is an echo, not a second viewport edit.
  syncViewportTransformBaseline();
  scheduleTransform(operation);
  clearTimeout(updateTimers.get(`save:${operation}`));
  updateTimers.delete(`save:${operation}`);
  schedule(`save:${operation}`, async () => {
    try {
      await transformFlushPromise;
      await editorApi.sceneTools.saveActor(selectedSceneName.value, selectedActorName.value);
      const eventType = operation === 'SetPosition'
        ? 'transform_position'
        : operation === 'SetRotation'
          ? 'transform_rotation'
          : 'transform_scale';
      void cabbageContextService.recordEvent({
        type: eventType,
        category: 'scene',
        success: true,
        details: {
          sceneName: selectedSceneName.value,
          actorName: selectedActorName.value,
          actorType: actor.type || 'model',
          axis: String(axis || '').toLowerCase(),
          value: axis ? Number(actor.transform[operation === 'SetPosition' ? 'position' : operation === 'SetRotation' ? 'rotation' : 'scale']?.[axis]) : null,
          source: 'property_panel',
        },
      });
    } catch (error) {
      logError('保存对象变换失败', error);
    }
  }, 180);
}

async function setRenderSpace(enabled) {
  if (!selectedActorName.value || actor.followCamera === enabled) return;
  const previous = actor.followCamera;
  actor.followCamera = enabled;
  try {
    await editorApi.sceneTools.setActorState(
      selectedSceneName.value,
      selectedActorName.value,
      { follow_camera: Boolean(enabled) },
    );
    if (enabled) actor.mechanics.physicsEnabled = false;
  } catch (error) {
    actor.followCamera = previous;
    logError('更新对象渲染空间失败', error);
  }
}

async function selectModelFile() {
  if (!selectedActorName.value) return;
  try {
    const raw = await editorApi.sceneTools.selectModelFile(selectedSceneName.value, selectedActorName.value, 'model');
    const payload = unwrap(raw);
    const path = typeof payload === 'string' ? payload : payload?.path || payload?.data || '';
    if (path) actor.modelPath = String(path);
  } catch (error) {
    logError('选择模型资源失败', error);
  }
}

async function rebindPlaceholderResource() {
  if (!selectedActorName.value || !actor.actorGuid || placeholderRebinding.value) return;
  placeholderRebinding.value = true;
  try {
    const selected = unwrap(
      await editorApi.sceneTools.selectModelFile(
        selectedSceneName.value,
        selectedActorName.value,
        'model'
      )
    );
    const path = typeof selected === 'string'
      ? selected
      : selected?.path || selected?.data || '';
    if (!path) return;
    const result = unwrap(
      await editorApi.sceneTools.rebindActorResource(
        selectedSceneName.value,
        actor.actorGuid,
        path
      )
    );
    if (!result || result.status === 'error' || result.ok === false) {
      throw new Error(result?.message || '重新绑定资源失败');
    }
    await loadActor(selectedSceneName.value, selectedActorName.value);
  } catch (error) {
    logError('重新绑定占位资源失败', error);
  } finally {
    placeholderRebinding.value = false;
  }
}

function applyCollisionFast(collisionType) {
  return editorApi.sceneTools.setActorPhysics(
    selectedSceneName.value,
    selectedActorName.value,
    { collision_shape: normalizeCollisionType(collisionType) },
  );
}

async function updateCollision() {
  if (!selectedActorName.value) return;
  const previous = lastSavedCollision;
  const selected = normalizeCollisionType(actor.collision);
  actor.collision = selected;
  try {
    await applyCollisionFast(selected);
    lastSavedCollision = selected;
    void cabbageContextService.recordEvent({
      type: 'physics_changed',
      category: 'physics',
      success: true,
      details: {
        sceneName: selectedSceneName.value,
        actorName: selectedActorName.value,
        operation: 'SetCollision',
        collisionType: selected,
      },
    });
  } catch (error) {
    actor.collision = previous;
    applyCollisionFast(previous);
    logError('更新对象碰撞失败', error);
  }
}

async function updateMechanic(operation, value) {
  if (!selectedActorName.value) return;
  const fieldByOperation = {
    SetPhysicsEnabled: 'physics_enabled',
    SetMass: 'mass',
    SetRestitution: 'restitution',
    SetDamping: 'damping',
  };
  const field = fieldByOperation[operation];
  if (!field) {
    logError('更新对象物理属性失败', new Error(`Unsupported physics operation: ${operation}`));
    return;
  }
  try {
    await editorApi.sceneTools.setActorPhysics(
      selectedSceneName.value,
      selectedActorName.value,
      { [field]: value },
    );
    void cabbageContextService.recordEvent({
      type: 'physics_changed',
      category: 'physics',
      success: true,
      details: {
        sceneName: selectedSceneName.value,
        actorName: selectedActorName.value,
        operation,
        value,
        source: 'property_panel',
      },
    });
  } catch (error) {
    logError('更新对象物理属性失败', error);
  }
}

async function updateLocks(operation, values) {
  if (!selectedActorName.value) return;
  const field = operation === 'SetLinearLock' ? 'linear_lock' : operation === 'SetAngularLock' ? 'angular_lock' : null;
  if (!field) {
    logError('更新对象轴锁失败', new Error(`Unsupported lock operation: ${operation}`));
    return;
  }
  try {
    await editorApi.sceneTools.setActorPhysics(
      selectedSceneName.value,
      selectedActorName.value,
      { [field]: values.map((value) => Boolean(value)) },
    );
    void cabbageContextService.recordEvent({
      type: 'physics_changed',
      category: 'physics',
      success: true,
      details: {
        sceneName: selectedSceneName.value,
        actorName: selectedActorName.value,
        operation,
      },
    });
  } catch (error) {
    logError('更新对象轴锁失败', error);
  }
}

async function updateCameraLock() {
  try {
    await editorApi.sceneTools.setActorCameraLock(
      selectedSceneName.value,
      selectedActorName.value,
      { enabled: actor.cameraLock.enabled },
    );
  } catch (error) {
    logError('更新摄像机跟随失败', error);
  }
}

async function updateCameraLockOffset() {
  const value = actor.cameraLock.position;
  const toFinite = (input, fallback) => {
    const number = Number(input);
    return Number.isFinite(number) ? number : fallback;
  };
  try {
    await editorApi.sceneTools.setActorCameraLock(
      selectedSceneName.value,
      selectedActorName.value,
      {
        enabled: true,
        position_offset: [
          toFinite(value.x, 0),
          toFinite(value.y, 0),
          toFinite(value.z, 2),
        ],
      },
    );
  } catch (error) {
    logError('更新摄像机偏移失败', error);
  }
}

async function saveActor() {
  if (!selectedActorName.value || saving.value) return;
  saving.value = true;
  try {
    await editorApi.sceneTools.saveActor(selectedSceneName.value, selectedActorName.value);
  } catch (error) {
    logError('保存对象失败', error);
  } finally {
    saving.value = false;
  }
}

function handleSelection(payload = {}) {
  const type = String(payload.actor_type || payload.type || '');
  const sceneName = String(payload.scene || selectedSceneName.value || DEFAULT_SCENE_NAME);
  const actorName = String(payload.actor || '');
  if (type === 'scene' || !actorName) {
    selectedSceneName.value = sceneName;
    selectedActorName.value = '';
    actor.name = '';
    aliasDraft.value = '';
    clearViewportTransformBaseline();
    return;
  }
  loadActor(sceneName, actorName);
}

function handleTransform(payload = {}) {
  if (!selectedActorName.value || payload.actor !== selectedActorName.value || payload.scene !== selectedSceneName.value) return;
  const actorKey = currentActorKey();
  if (viewportTransformBaseline.actorKey !== actorKey) {
    syncViewportTransformBaseline();
    return;
  }

  const next = {
    position: cloneVector(actor.transform.position),
    rotation: cloneVector(actor.transform.rotation),
    scale: cloneVector(actor.transform.scale),
  };
  assignVector(next.position, payload.position, next.position);
  assignVector(next.rotation, payload.rotation, next.rotation);
  assignVector(next.scale, payload.scale, next.scale);

  const changedKeys = ['position', 'rotation', 'scale'].filter((key) => (
    vectorChanged(viewportTransformBaseline[key], next[key])
  ));
  assignVector(actor.transform.position, next.position, actor.transform.position);
  assignVector(actor.transform.rotation, next.rotation, actor.transform.rotation);
  assignVector(actor.transform.scale, next.scale, actor.transform.scale);
  syncViewportTransformBaseline();

  for (const key of changedKeys) {
    const eventType = key === 'position' ? 'transform_position' : key === 'rotation' ? 'transform_rotation' : 'transform_scale';
    void cabbageContextService.recordEvent({
      type: eventType,
      category: 'scene',
      success: true,
      details: {
        sceneName: selectedSceneName.value,
        actorName: selectedActorName.value,
        actorType: actor.type || 'model',
        source: 'viewport',
      },
    });
  }
}

function handleGuidancePrepare(event) {
  if (event?.detail?.panelId !== 'Object') return;
  const selectorKey = String(event.detail.selectorKey || '');
  if (selectorKey === 'object-transform' || selectorKey.startsWith('object-position-') || selectorKey.startsWith('object-rotation-') || selectorKey.startsWith('object-scale-')) collapsedSections.transform = false;
  if (selectorKey === 'object-physics' || selectorKey.startsWith('object-physics-')) collapsedSections.physics = false;
}

function closeFloat() {
  closePanel();
}

onMounted(async () => {
  // Actor selection is stored before this panel opens. Read it without invoking
  // MainView.on_init, which would reinitialize the scene and main camera.
  const actorContext = getActorContext();
  selectedSceneName.value = actorContext.scene || DEFAULT_SCENE_NAME;
  if (actorContext.actor) {
    await loadActor(selectedSceneName.value, actorContext.actor);
  }
  selectionToken = await editorApi.events.onActorSelectionChanged(handleSelection);
  transformToken = await editorApi.events.onActorTransformUpdated(handleTransform);
  window.addEventListener('cabbage-guidance-prepare', handleGuidancePrepare);
});

onUnmounted(() => {
  loadSequence += 1;
  for (const timer of updateTimers.values()) clearTimeout(timer);
  updateTimers.clear();
  pendingTransformUpdates.clear();
  if (transformFrameId !== null) window.cancelAnimationFrame(transformFrameId);
  transformFrameId = null;
  if (selectionToken) editorApi.off(selectionToken).catch(() => {});
  if (transformToken) editorApi.off(transformToken).catch(() => {});
  selectionToken = null;
  transformToken = null;
  clearViewportTransformBaseline();
  window.removeEventListener('cabbage-guidance-prepare', handleGuidancePrepare);
});
</script>

<style scoped>
.object-panel-shell {
  color: #e5e7eb;
  background: rgba(40, 40, 40, 0.42);
  border: 1px solid rgba(58, 58, 58, 0.72);
}
.object-empty {
  display: flex;
  flex: 1;
  min-height: 180px;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 8px;
  padding: 24px;
  color: #9ca3af;
  text-align: center;
  font-size: 12px;
  line-height: 1.65;
  background: rgba(8, 8, 6, 0.42);
}
.object-empty strong { color: #f2ead5; font-size: 14px; }
.object-scroll { min-height: 0; flex: 1; overflow-y: auto; padding: 10px; background: rgba(8, 8, 6, 0.42); scrollbar-color:#8c6f36 #11100d; }
.object-heading { display:flex; align-items:center; justify-content:space-between; gap:10px; margin-bottom:10px; padding:10px 11px; border:1px solid rgba(216,184,108,.28); border-radius:7px; background:#15130d; }
.object-heading h2 { margin:2px 0 0; color:#f2ead5; font-size:15px; overflow-wrap:anywhere; }
.object-type { color:#d6b66b; font-size:10px; text-transform:uppercase; }
.placeholder-warning { display:grid; gap:6px; margin-bottom:8px; padding:9px 10px; border:1px solid rgba(217,119,6,.7); border-radius:7px; background:rgba(69,26,3,.55); color:#fde68a; font-size:10px; }
.placeholder-warning span { overflow-wrap:anywhere; color:#fcd34d; opacity:.82; }
.placeholder-warning button { justify-self:start; }
.save-button,.inline-button { border:1px solid rgba(216,184,108,.28); border-radius:5px; background:#211d12; color:#e9dfc5; padding:5px 9px; font-size:11px; transition:background .15s ease,border-color .15s ease; }
.save-button:hover:not(:disabled),.inline-button:hover:not(:disabled) { border-color:#D8B86C; background:#2b230f; color:#fff7dc; }
.save-button { border-color:#b8924a; background:#4b391c; color:#fff7dc; }
.save-button:hover:not(:disabled) { background:#8c6f36; }
.save-button:disabled,.inline-button:disabled { opacity:.45; cursor:not-allowed; }
.property-section { margin-bottom:8px; padding:10px; border:1px solid rgba(216,184,108,.24); border-radius:7px; background:#15130d; }
.section-title { margin-bottom:8px; color:#f2ead5; font-size:12px; font-weight:700; }
.section-title-row { display:flex; align-items:center; justify-content:space-between; }
.property-section-collapsible { padding:0; overflow:hidden; }
.section-toggle { width:100%; min-height:38px; display:flex; align-items:center; justify-content:space-between; padding:9px 10px; border:0; background:#191711; color:#f2ead5; font-size:12px; font-weight:700; text-align:left; cursor:pointer; transition:background .15s ease,color .15s ease; }
.section-toggle:hover { background:#242016; color:#e5c77f; }
.section-toggle:focus-visible { outline:1px solid #d8b86c; outline-offset:-2px; }
.section-chevron { color:#b9ad8f; transform:rotate(0deg); transition:transform .15s ease,color .15s ease; }
.section-chevron.expanded { color:#e5c77f; transform:rotate(180deg); }
.section-collapsible-body { padding:0 10px 10px; border-top:1px solid rgba(216,184,108,.18); background:#11100d; }
.physics-enable-row { display:flex; align-items:center; justify-content:space-between; padding:9px 0 7px; color:#b9ad8f; font-size:11px; }
.property-row { display:grid; grid-template-columns:72px minmax(0,1fr) auto; align-items:center; gap:7px; margin-top:7px; }
.property-row-wide { grid-template-columns:72px minmax(0,1fr) auto; }
.property-row>label { color:#b9ad8f; font-size:11px; }
.collision-options { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:5px; }
.collision-options label { display:flex; align-items:center; justify-content:center; min-width:0; padding:5px 4px; border:1px solid rgba(216,184,108,.2); border-radius:4px; background:#0f0e0a; color:#b9ad8f; font-size:10px; cursor:pointer; transition:border-color .15s,background .15s,color .15s; }
.collision-options label:hover { border-color:#8c6f36; color:#f2ead5; }
.collision-options label.active { border-color:#D8B86C; background:#4b391c; color:#fff; }
.collision-options input { position:absolute; width:1px; height:1px; opacity:0; pointer-events:none; }
input[type='text'],input[type='number'],select { min-width:0; width:100%; border:1px solid rgba(216,184,108,.22); border-radius:4px; background:#0f0e0a; color:#f2ead5; padding:5px 6px; font-size:11px; outline:none; }
input:focus,select:focus { border-color:#D8B86C; box-shadow:0 0 0 1px rgba(216,184,108,.18); }
.property-error { margin:5px 0 0 79px; color:#ff9e91; font-size:10px; }
.segmented { display:flex; width:max-content; padding:2px; border:1px solid rgba(216,184,108,.24); border-radius:5px; background:#0f0e0a; }
.segmented button { border-radius:4px; color:#9ca3af; padding:4px 8px; font-size:10px; }
.segmented button:hover { color:#f3f4f6; }
.segmented button.active { background:#4b391c; color:#fff; }
.vector-group { display:grid; grid-template-columns:58px repeat(3,minmax(0,1fr)); align-items:center; gap:5px; margin-top:7px; }
.vector-group>span { color:#b9ad8f; font-size:11px; }
.vector-group label { display:grid; grid-template-columns:12px minmax(0,1fr); align-items:center; gap:3px; }
.vector-group b { font-size:9px; }
.axis-x b { color:#f28b82; }.axis-y b { color:#8ab4f8; }.axis-z b { color:#81c995; }
.switch-label { display:flex; align-items:center; gap:5px; color:#d2c6a7; font-size:10px; }
.physics-grid { display:grid; grid-template-columns:repeat(3,minmax(0,1fr)); gap:6px; }
.physics-grid label { color:#b9ad8f; font-size:10px; }
.physics-grid input { margin-top:3px; }
.physics-grid.disabled { opacity:.56; }
.lock-row { display:flex; align-items:center; gap:10px; margin-top:9px; color:#b9ad8f; font-size:10px; }
.lock-row>span { min-width:64px; }
.lock-row label { display:flex; align-items:center; gap:3px; }
</style>
