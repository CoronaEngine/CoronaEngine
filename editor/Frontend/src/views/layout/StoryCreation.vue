<template>
  <main class="story-creation" aria-label="剧情模式创作空间">
    <div ref="viewportRef" class="story-creation__viewport" tabindex="-1"></div>
    <header class="story-creation__header">
      <div>
        <p>STORY CREATION</p>
        <h1>{{ demo.document.name }}</h1>
        <span>{{ editMode ? '编辑模式' : '试玩模式' }} · {{ worldBallId }}</span>
      </div>
      <div class="story-creation__actions">
        <button type="button" @click="toggleMode">{{ editMode ? '进入试玩' : '返回编辑' }}</button>
        <button type="button" :disabled="demo.syncing" @click="save">保存 Demo</button>
        <button type="button" :disabled="demo.syncing" @click="exportDemo">导出 Demo</button>
        <button type="button" @click="backToStory">返回主世界</button>
      </div>
    </header>

    <section v-if="editMode" class="story-creation__panel" aria-label="创作工具">
      <h2>组件库</h2>
      <p class="story-creation__hint">选择已附魔组件，然后放置到小世界。</p>
      <div class="story-creation__components">
        <button
          v-for="component in demo.componentList"
          :key="component.id"
          type="button"
          :class="{ selected: demo.selectedComponent === component.id }"
          @click="demo.chooseComponent(component.id)"
        >
          {{ component.name }}
        </button>
      </div>
      <div class="story-creation__tools">
        <button type="button" :disabled="demo.syncing" @click="demo.placeSelected">放置组件</button>
        <button type="button" :disabled="!demo.history.length || demo.syncing" @click="demo.undo">撤销</button>
      </div>

      <h2>已放置内容</h2>
      <div v-if="demo.document.actors.length" class="story-creation__actors">
        <div v-for="actor in demo.document.actors" :key="actor.id" class="story-creation__actor">
          <button
            type="button"
            class="story-creation__actor-name"
            :class="{ selected: demo.selectedActorId === actor.id }"
            @click="selectActor(actor)"
          >
            {{ actor.componentType }} · {{ actor.name }}
          </button>
          <button type="button" class="story-creation__delete" @click="demo.deleteActor(actor.id)">删除</button>
        </div>
      </div>
      <p v-else class="story-creation__empty">还没有放置组件。</p>

      <div v-if="selectedActor" class="story-creation__transform">
        <h2>变换</h2>
        <label v-for="axis in axes" :key="`position-${axis}`">
          位置 {{ axis }}
          <input v-model.number="selectedTransform.position[axisIndex(axis)]" type="number" step="0.1" />
        </label>
        <label v-for="axis in axes" :key="`rotation-${axis}`">
          旋转 {{ axis }}
          <input v-model.number="selectedTransform.rotation[axisIndex(axis)]" type="number" step="1" />
        </label>
        <label v-for="axis in axes" :key="`scale-${axis}`">
          缩放 {{ axis }}
          <input v-model.number="selectedTransform.scale[axisIndex(axis)]" type="number" min="0.01" step="0.05" />
        </label>
        <button type="button" :disabled="demo.syncing" @click="saveSelectedTransform">保存变换</button>
      </div>

      <h2>世界核心</h2>
      <p class="story-creation__hint">附魔后的碎片安装到核心后，才会成为 Demo 组件。</p>
      <div class="story-creation__slots">
        <div v-for="slot in slotTypes" :key="slot.id">
          <span>{{ slot.label }}</span>
          <button
            type="button"
            :disabled="!findEnchanted(slot.id) || demo.syncing"
            @click="installSlot(slot.id)"
          >
            {{ demo.document.slots[slot.id] ? '已安装' : (findEnchanted(slot.id) ? '安装' : '需要附魔碎片') }}
          </button>
        </div>
      </div>
    </section>

    <div v-if="demo.notice || demo.error || exportMessage" class="story-creation__notice">
      {{ demo.notice || demo.error || exportMessage }}
    </div>
    <div v-if="!editMode" class="story-creation__play-hint">试玩模式 · 点击返回编辑继续搭建</div>
  </main>
</template>

<script setup>
import { computed, onMounted, reactive, ref } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { useNativeSceneViewport } from '@/composables/useNativeSceneViewport.js';
import { useStoryCameraControls } from '@/composables/useStoryCameraControls.js';
import { useStoryDemoBuilder } from '@/composables/useStoryDemoBuilder.js';
import { useStoryInventoryStore } from '@/stores/storyInventory.js';
import { useStoryProgressStore } from '@/stores/storyProgress.js';
import { STORY_DEMO_SLOT_TYPES } from '@/config/storyDemo.js';
import { exportPlayableStoryDemo } from '@/services/storyDemoExportService.js';

const route = useRoute();
const router = useRouter();
const viewportRef = ref(null);
const inventory = useStoryInventoryStore();
const progress = useStoryProgressStore();
const projectKey = String(
  route.query.projectKey ||
  window.localStorage?.getItem('corona.activeProjectPath') ||
  'active-project'
);
const worldBallId = String(route.query.worldBallId || progress.unlockedWorldBalls[0] || 'demo-1');
const sourceScene = ref(String(route.query.sourceScene || '').trim());
const demo = useStoryDemoBuilder({ projectKey, worldBallId });
const {
  status: viewportStatus,
  cameraBinding,
  refreshCameraBinding,
  retry: retryViewport,
} = useNativeSceneViewport(viewportRef);
const storyCameraEnabled = computed(() =>
  Boolean(demo.sceneReady.value) && viewportStatus.value === 'ready' && !demo.syncing.value
);
const { stop: stopCameraControls } = useStoryCameraControls({
  viewportRef,
  cameraBinding,
  enabled: storyCameraEnabled,
  refreshCameraBinding,
});
const exportMessage = ref('');
const axes = ['X', 'Y', 'Z'];
const slotTypes = [
  { id: 'terrain', label: '地形槽' },
  { id: 'object', label: '物体槽' },
  { id: 'enemy', label: '敌人槽' },
  { id: 'objective', label: '目标槽' },
];

const editMode = computed(() => demo.editMode.value);
const selectedActor = computed(() => demo.document.value.actors.find((actor) => actor.id === demo.selectedActorId.value) || null);
const selectedTransform = reactive({ position: [0, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] });
const syncSelectedTransform = () => {
  const actor = selectedActor.value;
  if (!actor) return;
  selectedTransform.position = [...actor.position];
  selectedTransform.rotation = [...actor.rotation];
  selectedTransform.scale = [...actor.scale];
};
const selectActor = (actor) => {
  if (!actor) return;
  demo.selectedActorId.value = actor.id;
  selectedTransform.position = [...actor.position];
  selectedTransform.rotation = [...actor.rotation];
  selectedTransform.scale = [...actor.scale];
};
const axisIndex = (axis) => axes.indexOf(axis);
const findEnchanted = (type) => inventory.slots.find((slot) => slot?.itemId === `enchanted_${type}_fragment`) || null;
const installSlot = async (type) => {
  const slot = findEnchanted(type);
  if (!slot) return;
  const installed = await demo.installSlot(type, slot);
  if (installed) inventory.removeItem(slot.itemId, 1);
};
const saveSelectedTransform = async () => {
  if (!selectedActor.value) return;
  await demo.updateActor(selectedActor.value.id, {
    position: selectedTransform.position,
    rotation: selectedTransform.rotation,
    scale: selectedTransform.scale,
  });
};
const save = () => demo.save();
const exportDemo = async () => {
  exportMessage.value = '';
  try {
    const target = await editorApi.project.choosePortableSceneTarget();
    const path = target?.data?.path || target?.path || target?.data?.targetDirectory || target?.targetDirectory;
    if (!path) throw new Error('未选择导出目录。');
    const result = await exportPlayableStoryDemo(demo.document.value, path, projectKey);
    const exported = result?.data?.data || result?.data || result || {};
    exportMessage.value = exported.message || `Demo 资源包已导出到：${exported.packagePath || path}`;
  } catch (error) {
    exportMessage.value = error?.message || '导出失败。';
  }
};
const toggleMode = async () => {
  demo.toggleMode();
  await demo.ensureScene();
};
const backToStory = async () => {
  try {
    if (!sourceScene.value) {
      const init = await editorApi.main.onInit();
      const data = init?.data?.data || init?.data || init || {};
      sourceScene.value = String(data.scene_id || data.sceneId || data.path || data.name || '').trim();
    }
    await stopCameraControls({ persist: true });
    await Promise.resolve(editorApi.main.sceneSave(demo.sceneName)).catch(() => {});
    if (sourceScene.value) await editorApi.sceneTools.reloadScene(sourceScene.value, projectKey);
  } catch (error) {
    console.warn('[StoryCreation] failed to restore the main Story scene', error);
  } finally {
    await router.push({ path: '/StoryMode' });
  }
};

onMounted(async () => {
  inventory.resetForProject(projectKey);
  progress.load(projectKey);
  await demo.ensureScene();
  syncSelectedTransform();
  void retryViewport();
});
</script>

<style scoped>
.story-creation { position: relative; width: 100vw; height: 100vh; overflow: hidden; color: #f2e6c9; background: #080b0b; font-family: Inter, 'Microsoft YaHei', sans-serif; }
.story-creation__viewport { position: absolute; inset: 0; }
.story-creation__header { position: absolute; z-index: 5; inset: 0 0 auto; display: flex; align-items: center; justify-content: space-between; gap: 18px; padding: 22px 30px; background: linear-gradient(180deg, rgba(4, 6, 5, .86), transparent); pointer-events: none; }
.story-creation__header > div, .story-creation__actions { pointer-events: auto; }
.story-creation__header p { margin: 0 0 4px; color: #c7a863; font-size: 10px; letter-spacing: .25em; }
.story-creation__header h1 { margin: 0; font-size: 25px; }
.story-creation__header span { color: #aaa18e; font-size: 12px; }
.story-creation__actions { display: flex; flex-wrap: wrap; gap: 8px; }
.story-creation button { border: 1px solid rgba(216,184,108,.35); border-radius: 8px; padding: 9px 13px; color: #f4e6c4; background: rgba(16,18,15,.82); cursor: pointer; }
.story-creation button:hover, .story-creation button.selected { border-color: #e0bd6d; background: rgba(216,184,108,.18); }
.story-creation button:disabled { cursor: not-allowed; opacity: .45; }
.story-creation__panel { position: absolute; z-index: 4; top: 112px; right: 24px; width: min(350px, calc(100vw - 48px)); max-height: calc(100vh - 136px); overflow: auto; padding: 20px; border: 1px solid rgba(216,184,108,.32); border-radius: 14px; background: rgba(10,13,11,.9); backdrop-filter: blur(10px); }
.story-creation__panel h2 { margin: 17px 0 8px; font-size: 15px; }
.story-creation__panel h2:first-child { margin-top: 0; }
.story-creation__hint, .story-creation__empty { color: #9c927e; font-size: 12px; }
.story-creation__components, .story-creation__tools, .story-creation__slots { display: grid; gap: 7px; }
.story-creation__actors > div, .story-creation__slots > div { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 8px 0; border-bottom: 1px solid rgba(255,255,255,.08); font-size: 12px; }
.story-creation__actor-name { flex: 1; overflow: hidden; padding: 5px 8px !important; border-color: transparent !important; text-align: left; text-overflow: ellipsis; white-space: nowrap; }
.story-creation__delete { padding: 5px 8px !important; color: #f08d85 !important; }
.story-creation__transform { display: grid; gap: 7px; }
.story-creation__transform h2 { grid-column: 1 / -1; }
.story-creation__transform label { display: flex; align-items: center; justify-content: space-between; gap: 8px; color: #b9ae97; font-size: 12px; }
.story-creation__transform input { width: 120px; padding: 6px 8px; border: 1px solid rgba(216,184,108,.22); border-radius: 6px; color: #f4e6c4; background: rgba(0,0,0,.26); }
.story-creation__notice, .story-creation__play-hint { position: absolute; z-index: 6; bottom: 28px; left: 50%; transform: translateX(-50%); padding: 10px 15px; border-radius: 999px; background: rgba(7,9,7,.84); color: #e8d29d; font-size: 12px; }
.story-creation__play-hint { right: 24px; left: auto; transform: none; }
</style>
