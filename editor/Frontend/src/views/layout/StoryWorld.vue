<script setup>
import { onMounted, onUnmounted, ref, nextTick, watch } from 'vue';
import { useRouter, onBeforeRouteLeave } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState, normalizeProjectPath } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';
import { projectLauncherService, cancelPendingProjectOpen, getProjectSelectionVersion } from '@/services/projectLauncherService.js';
import { trackWorldSessionWork, notifyWorldError, registerWorldSessionSave } from '@/services/worldSessionLifecycle.js';
import { createStoryNavigationController, NAVIGATION_KEY } from '../../../../../game/frontend/storyNavigation.mjs';

import { ensureStoryCharacters } from '../../../../../game/frontend/storyActors.mjs';
import { createPlayerController } from '../../../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../../../game/frontend/playerSave.mjs';
import { createStoryGameplay, worldBounds, distanceToBounds, pickupDistance } from '../../../../../game/frontend/storyGameplay.mjs';
import { STORY_CHARACTERS } from '../../../../../game/frontend/storyCharacters.mjs';

const router = useRouter();
const surface = ref(null);
const inventoryOpen = ref(false);
const inventoryPanel = ref(null);
const selectedSlot = ref(0);
const navigationPending = ref(false);
const initializing = ref(true);
const cooldownTick = ref(0);
let cooldownTimer = null;
const gameplayState = ref(null);
const bossNearby = ref(false);
const canPickup = ref(false);
const feedback = ref('');
const gameplayError = ref('');
const toastText = ref('');
const fxLayer = ref(null);
const actionBusy = ref(false);
let feedbackTimer = null;
let toastTimer = null;
let bossActor = null;
let bossBounds = null;
let sceneId = null;
let visualSignature = null;
let lastHitHp = null;
let lastHitRage = null;
let lastHitRevision = null;
const revision = worldModeState.revision;
const projectPath = worldModeState.projectPath;
let disposed = false;
let cameraReady = false;
let playerSave = null;
let savingPlayer = false;
let initialization = Promise.resolve();
let exitPending = null;
let resizeObserver = null;
let leaving = false;
let dpiQuery = null;
let focused = document.hasFocus();
const current = () => !disposed && !leaving && worldModeState.status === 'ready'
  && worldModeState.mode === 'story' && worldModeState.revision === revision;
const camera = createStoryCameraController({
  createControls: createPlayerController,
  onError: error => {
    if (current()) gameplayError.value = error.message;
    notifyWorldError(error);
  },
  getBridge: () => current() ? window.coronaBridge : null,
  isCurrent: current,
  isInputLocked: () => initializing.value || !focused || document.hidden || navigation.busy || savingPlayer || inventoryOpen.value,
  onPlayerChanged: updateProximity,
  getRect: () => surface.value?.getBoundingClientRect(),
  getPixelRatio: () => window.devicePixelRatio,
});
const unwrap = (response) => response?.data ?? response;
function updateProximity() {
  if (!current()) return;
  const data = gameplayState.value, player = camera.snapshotPlayer();
  bossNearby.value = Boolean(data?.role === 'main' && data.state.boss.hp > 0
    && distanceToBounds(player?.position, bossBounds) <= data.config.bossBarRadius);
  canPickup.value = Boolean(data?.role === 'main' && data.state.drop && !data.state.drop.collected
    && pickupDistance(player, data.state.drop) <= data.config.pickupRange);
}
function showFeedback(message) {
  if (!current()) return;
  feedback.value = message;
  clearTimeout(feedbackTimer);
  feedbackTimer = setTimeout(() => { feedback.value = ''; }, 2200);
}
// 提示条：只做显示，不改动任何玩法/存档流程
function showToast(message) {
  if (!message) return;
  toastText.value = message;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { toastText.value = ''; }, 1600);
}
// 受击飘字：只做显示。按 revision 判断本帧是否发生新的命中，
// 用怒气下降确认“打中但未掉血”的技能，再取掉血差值作为数字。
function spawnDamageText(value, kind) {
  const layer = fxLayer.value;
  if (!layer) return;
  const el = document.createElement('span');
  el.className = kind === 'miss' ? 'dmg dmg--miss' : 'dmg';
  el.style.left = `${(34 + Math.random() * 32).toFixed(1)}%`;
  el.style.top = `${(40 + Math.random() * 10).toFixed(1)}%`;
  el.style.setProperty('--dx', `${Math.round(Math.random() * 46 - 23)}px`);
  el.style.setProperty('--rise', `${Math.round(46 + Math.random() * 36)}px`);
  el.style.setProperty('--rot', `${(Math.random() * 12 - 6).toFixed(1)}deg`);
  el.textContent = kind === 'miss' ? '未中' : String(value);
  layer.appendChild(el);
  el.addEventListener('animationend', () => el.remove(), { once: true });
  setTimeout(() => el.remove(), 1400);
}
function syncCombatFeedback(data) {
  if (!data) return;
  const { rage, revision: stateRevision } = data.state;
  const hp = data.state.boss.hp;
  const skillsUsed = lastHitRage !== null && lastHitRage > rage;
  if (stateRevision !== lastHitRevision) {
    const delta = lastHitHp === null ? 0 : Math.max(0, lastHitHp - hp);
    if (delta > 0) spawnDamageText(delta, 'hit');
    else if (skillsUsed) spawnDamageText(0, 'miss');
  }
  lastHitRevision = stateRevision;
  lastHitHp = hp;
  lastHitRage = rage;
}
watch(feedback, value => showToast(value));
watch(() => gameplayState.value?.state.revision, () => syncCombatFeedback(gameplayState.value));
// Save finalizers may outlive the component; validate the actual native source,
// not the new route's revision, before reconciling any old-world actor.
async function assertSource() {
  const info = unwrap(await editorApi.projectSettings.getActiveProjectInfo());
  if (info?.mode !== 'story' || normalizeProjectPath(info.project_path) !== normalizeProjectPath(projectPath)) {
    throw new Error('当前世界已改变，已取消旧世界模型更新');
  }
}
const signature = data => JSON.stringify([data.role, data.state.boss.hp === 0, data.state.drop]);
const gameplay = createStoryGameplay({
  api: editorApi, projectPath, readPlayer: camera.snapshotPlayer, readBoss: () => bossActor,
  trackWork: trackWorldSessionWork,
  onState: data => { if (current()) { gameplayState.value = data; updateProximity(); } },
  onFeedback: showFeedback,
  reconcile: async data => {
    const next = signature(data);
    if (next === visualSignature) return;
    await ensureStoryCharacters({ api: editorApi, sceneId, frontendUrl: window.location.href,
      gameplay: data, combatOnly: true, assertSource, projectPath });
    visualSignature = next;
    if (current()) updateProximity();
  },
});
async function runAction(action) {
  if (!current() || !focused || document.hidden || inventoryOpen.value
    || navigation.busy || savingPlayer || actionBusy.value || !cameraReady) return;
  actionBusy.value = true;
  try {
    await action();
    if (current()) gameplayError.value = '';
  } catch (error) {
    if (current()) gameplayError.value = error.message || '保存失败，请重试';
  } finally {
    actionBusy.value = false;
    if (current()) updateProximity();
  }
}
function toggleInventory() {
  if (initializing.value || navigation.busy || navigationPending.value || savingPlayer) return;
  camera.resetInput();
  inventoryOpen.value = !inventoryOpen.value;
  void nextTick(() => {
    if (!current()) return;
    if (inventoryOpen.value) inventoryPanel.value?.querySelector('[data-inventory-close]')?.focus();
    else surface.value?.focus();
  });
}
function onInventoryFocus(event) {
  if (inventoryOpen.value && inventoryPanel.value && !inventoryPanel.value.contains(event.target)) {
    inventoryPanel.value.querySelector('[data-inventory-close]')?.focus();
  }
}
function onInventoryKey(event) {
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(event.code)) return;
  event.preventDefault(); event.stopPropagation();
  const panel = inventoryPanel.value;
  const slots = [...(panel?.querySelectorAll('.inventory-slot') || [])];
  if (!slots.length || navigationPending.value) return;
  const close = panel.querySelector('[data-inventory-close]');
  const travel = panel.querySelector('[data-inventory-travel]:not(:disabled)')
    || panel.querySelector('[data-inventory-travel-focus]');
  const active = document.activeElement;
  const index = slots.indexOf(active);
  let target;
  if (event.code === 'Home') target = 0;
  else if (event.code === 'End') target = slots.length - 1;
  else if (index < 0) {
    target = active === close ? 0 : slots.length - 5 + selectedSlot.value % 5;
  } else {
    const step = { ArrowLeft: -1, ArrowRight: 1, ArrowUp: -5, ArrowDown: 5 }[event.code];
    target = index + step;
    // Tab/Esc still close the bag. Arrows reach both actions without a mouse.
    if (target < 0) { close?.focus(); return; }
    if (target >= slots.length) { travel?.focus(); return; }
  }
  selectedSlot.value = target;
  slots[target]?.focus();
}
function skillStatus(id) {
  void cooldownTick.value;
  return gameplay.skillStatus(id);
}
function cooldownSeconds(id) {
  const remaining = skillStatus(id).remainingMs;
  return remaining > 120 ? (remaining / 1000).toFixed(1) : '';
}
function ragePercent() {
  const data = gameplayState.value;
  if (!data?.config.rageMax) return 0;
  return Math.max(0, Math.min(100, 100 * data.state.rage / data.config.rageMax));
}
function bossPercent() {
  const data = gameplayState.value;
  if (!data?.config.bossHp) return 0;
  return Math.max(0, Math.min(100, 100 * data.state.boss.hp / data.config.bossHp));
}
async function navigateWorld() {
  const data = gameplayState.value;
  if (!current() || !inventoryOpen.value || !data || navigationPending.value || navigation.busy
    || (data.role === 'main' && data.state.boss.hp > 0)) return;
  navigationPending.value = true;
  gameplayError.value = '';
  try { await navigation.navigate(data.role === 'main' ? 'enter' : 'exit'); }
  finally { navigationPending.value = false; }
}
const saveRegistration = registerWorldSessionSave(async () => {
  camera.resetInput();
  // Keep input locked until the actual acknowledgement, even after a UI timeout.
  // A retry must not release the scene with movement newer than the pending save.
  savingPlayer = true;
  try {
    // Initialization owns its error UI; finish any already-submitted native call.
    await initialization.catch(() => {});
    await gameplay.flush();
    await playerSave?.save();
    if (current()) gameplayError.value = '';
  } catch (error) {
    if (current()) gameplayError.value = error.message || '保存失败，请重试';
    throw error;
  } finally { savingPlayer = false; }
});
async function saveBeforeLeave() {
  if (exitPending) return exitPending;
  leaving = true;
  camera.resetInput();
  exitPending = (async () => {
    try {
      await saveRegistration.flush();
      saveRegistration.release();
      return true;
    } catch (error) {
      leaving = false;
      if (current()) gameplayError.value = error.message || '保存失败，请重试';
      if (current() && gameplay.data) {
        gameplayState.value = gameplay.data;
        updateProximity();
      }
      notifyWorldError(error, '保存剧情进度失败');
      return false;
    } finally { exitPending = null; }
  })();
  return exitPending;
}
onBeforeRouteLeave(saveBeforeLeave);
async function exitStory() {
  navigation.interrupt();
  if (!await saveBeforeLeave()) return;
  navigation.cancel();
  camera.dispose();
  await router.replace('/StartScreen');
}
const navigation = createStoryNavigationController({
  projectPath,
  isReady: () => cameraReady && focused && !document.hidden && !savingPlayer,
  isSourceCurrent: current,
  getSelectionVersion: getProjectSelectionVersion,
  readSession: () => worldModeState,
  resetInput: camera.resetInput,
  flushCamera: async () => {
    await saveRegistration.flush();
    const pose = camera.snapshotPose();
    if (!pose?.cameraName) throw new Error('当前相机尚未绑定，无法保存视角');
    const result = unwrap(await editorApi.viewport.setCameraPose(pose.sceneId, pose.cameraName, pose.camera));
    if (result?.status !== 'success') throw new Error(result?.message || '保存相机视角失败');
  },
  prepare: direction => editorApi.scratch.sendKeyEvent(NAVIGATION_KEY, '', JSON.stringify({ projectPath, direction })),
  trackPreparation: trackWorldSessionWork,
  openProject: path => projectLauncherService.openProject(path),
  cancelProjectOpen: cancelPendingProjectOpen,
  leave: () => router.replace('/StartScreen'),
  notify: error => { if (current()) gameplayError.value = error.message; notifyWorldError(error); },
});
function onKeyDown(event) {
  if (!current()) return;
  if (event.code === 'Escape' || event.key === 'Escape') {
    event.preventDefault();
    event.stopPropagation();
    if (navigation.busy) { if (!event.repeat) void exitStory(); }
    else if (inventoryOpen.value) toggleInventory();
    else if (!event.repeat) void exitStory();
    return;
  }
  if (event.ctrlKey || event.altKey || event.metaKey || event.isComposing) return;
  if (event.code === 'Tab' || event.key === 'Tab') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused && !document.hidden && cameraReady && !navigation.busy && !savingPlayer) toggleInventory();
    return;
  }
  if (inventoryOpen.value) {
    if (!['Enter', 'Space'].includes(event.code)) event.preventDefault();
    event.stopPropagation(); return;
  }
  if (event.code === 'KeyF' || event.key?.toLowerCase() === 'f') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused) void runAction(gameplay.pickup);
    return;
  }
  if ((event.code === 'Space' || event.key === ' ' || event.key === 'Spacebar')
    && (actionBusy.value || gameplay.busy || gameplay.needsSave)) {
    event.preventDefault(); event.stopPropagation(); return;
  }
  if (event.code === 'KeyE' || event.code === 'KeyR' || ['e', 'r'].includes(event.key?.toLowerCase())) {
    event.preventDefault(); event.stopPropagation();
    const skill = event.code === 'KeyE' || event.key?.toLowerCase() === 'e' ? 'heavy' : 'sweep';
    if (!event.repeat && focused) void runAction(() => gameplay.castSkill(skill));
    return;
  }
  if (focused && camera.keyDown(event)) event.stopPropagation?.();
}
function onBlur() { focused = false; camera.resetInput(); }
function onFocus() { focused = !document.hidden; }
function onVisibilityChange() {
  if (document.hidden) onBlur();
  else focused = document.hasFocus();
}
function watchDpi() {
  dpiQuery?.removeEventListener('change', onDpiChanged);
  dpiQuery = window.matchMedia?.(`(resolution: ${window.devicePixelRatio}dppx)`);
  dpiQuery?.addEventListener('change', onDpiChanged);
}
function onDpiChanged() { camera.syncViewport(); watchDpi(); }
function onPointerDown(event) {
  if (inventoryOpen.value) return;
  surface.value?.focus();
  if (!focused) return;
  if (event.button === 0) {
    event.preventDefault();
    void runAction(gameplay.attack);
  } else camera.pointerDown(event);
}

onMounted(async () => {
  // Escape must work even while the native scene/snapshot request is pending.
  document.addEventListener('keydown', onKeyDown);
  document.addEventListener('focusin', onInventoryFocus);
  cooldownTimer = setInterval(() => { cooldownTick.value++; }, 100);
  window.addEventListener('blur', onBlur);
  window.addEventListener('focus', onFocus);
  document.addEventListener('visibilitychange', onVisibilityChange);
  initialization = trackWorldSessionWork((async () => {
    const init = unwrap(await editorApi.main.onInit());
    if (!current()) return;
    const scenes = init?.scenes || [];
    const index = Math.min(Math.max(Number(init?.active_index) || 0, 0), Math.max(0, scenes.length - 1));
    sceneId = scenes[index]?.path || init?.path;
    if (!sceneId) throw new Error('当前世界没有可用场景');
    const loadedGameplay = await gameplay.load();
    if (!current()) return;
    lastHitHp = loadedGameplay.state.boss.hp;
    lastHitRage = loadedGameplay.state.rage;
    lastHitRevision = loadedGameplay.state.revision;
    const { snapshot, player, targetOffset } = await ensureStoryCharacters({
      api: editorApi, sceneId, frontendUrl: window.location.href, gameplay: loadedGameplay, assertSource, projectPath,
      isCurrent: () => current() && worldModeState.projectPath === projectPath,
    });
    if (!current() || worldModeState.projectPath !== projectPath) return;
    bossActor = snapshot.actors?.find(actor => actor.actor_guid === STORY_CHARACTERS[1].guid) || null;
    bossBounds = worldBounds(bossActor);
    visualSignature = signature(loadedGameplay);
    cameraReady = camera.bind(snapshot, sceneId);
    camera.bindPlayer(player, targetOffset);
    updateProximity();
    playerSave = createPlayerSave({ api: editorApi, sceneId,
      readPlayer: camera.snapshotPlayer, stopInput: camera.resetInput,
      assertSource, onSaved: camera.acknowledgePlayerSave });
    if (typeof ResizeObserver !== 'undefined') {
      resizeObserver = new ResizeObserver(camera.syncViewport);
      resizeObserver.observe(surface.value);
    }
    window.addEventListener('resize', camera.syncViewport);
    watchDpi();
    window.addEventListener('pointermove', camera.pointerMove);
    window.addEventListener('pointerup', camera.pointerUp);
    window.addEventListener('pointercancel', camera.resetInput);
    document.addEventListener('keyup', camera.keyUp);
    initializing.value = false;
    if (focused) surface.value?.focus();
  })());
  try { await initialization; } catch (error) {
    if (!current()) return;
    await router.replace('/StartScreen');
    window.alert(error.message || '剧情世界加载失败');
  }
});
onUnmounted(() => {
  disposed = true;
  clearTimeout(feedbackTimer);
  clearTimeout(toastTimer);
  clearInterval(cooldownTimer);
  document.removeEventListener('focusin', onInventoryFocus);
  cameraReady = false;
  navigation.dispose();
  camera.dispose();
  saveRegistration.retire();
  resizeObserver?.disconnect();
  window.removeEventListener('resize', camera.syncViewport);
  window.removeEventListener('blur', onBlur);
  window.removeEventListener('focus', onFocus);
  dpiQuery?.removeEventListener('change', onDpiChanged);
  window.removeEventListener('pointermove', camera.pointerMove);
  window.removeEventListener('pointerup', camera.pointerUp);
  window.removeEventListener('pointercancel', camera.resetInput);
  document.removeEventListener('keydown', onKeyDown);
  document.removeEventListener('keyup', camera.keyUp);
  document.removeEventListener('visibilitychange', onVisibilityChange);
});
</script>

<template>
  <div ref="surface" class="story-world-viewport" tabindex="0" data-story-viewport
    @pointerdown="onPointerDown" @pointerleave="camera.resetInput" @wheel.prevent="camera.wheel" @contextmenu.prevent>
    <svg class="hud-symbols" aria-hidden="true" xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="story-brush" x="-25%" y="-45%" width="150%" height="190%">
          <feTurbulence type="fractalNoise" baseFrequency="0.055 0.09" numOctaves="3" seed="11" result="noise" />
          <feDisplacementMap in="SourceGraphic" in2="noise" scale="4.6" xChannelSelector="R" yChannelSelector="G" />
        </filter>
        <symbol id="story-shard" viewBox="0 0 48 48"><path d="m24 3 14 18-14 24L10 21Z"/><path d="m24 3-4 18 4 24 5-24ZM10 21h28"/></symbol>
        <symbol id="story-heavy" viewBox="0 0 48 48"><path d="M36 5 17 24l7 7L43 12V5ZM13 22l13 13M19 29 8 40M5 37l6 6"/></symbol>
        <symbol id="story-sweep" viewBox="0 0 48 48"><path d="M7 30C-1 8 28-2 41 15M6 34C17 45 40 38 42 25M35 12l7 4 1-9M12 30l20-17-13 24Z"/></symbol>
      </defs>
    </svg>
    <div v-if="initializing" class="story-loading" role="status">{{ gameplayError || '剧情世界加载中…（Esc 取消）' }}</div>
    <div v-if="gameplayState && !initializing" class="story-hud" aria-label="战斗界面">
      <section v-if="bossNearby && !inventoryOpen" class="boss-status plate" aria-label="Boss 血条">
        <div class="plate__body">
          <span class="haze haze--r" aria-hidden="true"></span>
          <div class="boss-head">
            <span class="boss-seal" aria-hidden="true">
              <svg viewBox="0 0 24 24"><path d="M4 8h16M8 15h8"/></svg>
            </span>
            <span class="boss-name">巨龙</span>
            <span class="boss-number"><b>{{ gameplayState.state.boss.hp }}</b><i>/</i><span>{{ gameplayState.config.bossHp }}</span></span>
          </div>
          <div class="boss-bar bar bar--boss" role="progressbar" aria-label="Boss 生命"
            :aria-valuenow="gameplayState.state.boss.hp" :aria-valuemax="gameplayState.config.bossHp" aria-valuemin="0">
            <span class="bar__lag"></span>
            <span class="bar__fill" :style="{ width: `${bossPercent()}%` }"></span>
            <span class="bar__flash"></span>
            <span class="bar__tick" style="left:20%"></span>
            <span class="bar__tick" style="left:40%"></span>
            <span class="bar__tick" style="left:60%"></span>
            <span class="bar__tick" style="left:80%"></span>
          </div>
        </div>
      </section>
      <aside v-if="!inventoryOpen" class="story-controls tip" aria-label="操作提示">
        <svg class="tip__seal" viewBox="0 0 24 24" aria-hidden="true">
          <path d="M3 6.5h18M3 12h18M3 17.5h18"/>
        </svg>
        <span><kbd>WASD</kbd> 移动</span>
        <span><kbd>鼠标</kbd> 转向 · 滚轮缩放</span>
        <span><kbd>Space</kbd> 跳远</span>
        <span><kbd>Shift</kbd> 长按跑步 · 点按闪避</span>
        <span><kbd>左键</kbd> 攻击</span>
        <span><kbd>E / R</kbd> 技能</span>
        <span v-if="canPickup" class="pickup-control" role="status"><kbd>F</kbd> 拾取 世界碎片</span>
        <span><kbd>Tab</kbd> 背包</span>
        <span><kbd>Esc</kbd> 保存退出</span>
      </aside>
      <div v-if="!inventoryOpen" class="story-bottom-stack">
        <div v-if="gameplayError" class="gameplay-error" role="alert" @pointerdown.stop @wheel.stop @pointermove.stop="camera.resetInput">
          <span>{{ gameplayError }}</span><button :disabled="actionBusy" @click.stop="runAction(saveRegistration.flush)">重试保存</button>
        </div>
        <div v-if="feedback" class="story-feedback" role="status" aria-live="polite">{{ feedback }}</div>
      </div>
      <section v-if="!inventoryOpen" class="player-vitals vitals plate" aria-label="人物生命与怒气">
        <div class="plate__body">
          <span class="haze" aria-hidden="true"></span>
          <div class="vitals__row">
            <span class="vitals__seal" aria-hidden="true">
              <svg viewBox="0 0 24 24">
                <path d="M4 7.2c4.6.7 7.6-.4 11.4-.2" />
                <path d="M4.8 12c3.9.6 6.6-.3 9.6-.2" />
                <path d="M3.6 16.6c4.4.7 7.8-.4 11.8-.2" />
              </svg>
            </span>
            <div class="vitals__bars">
              <div class="stat">
                <span class="stat__name">生命</span>
                <div class="vital-bar bar bar--hp" role="progressbar" aria-label="生命"
                  :aria-valuenow="gameplayState.config.playerHp" :aria-valuemax="gameplayState.config.playerHp" aria-valuemin="0">
                  <!-- 生命值恒满：与原界面一致，不伪造掉落 -->
                  <span class="bar__lag" style="width:100%"></span>
                  <span class="bar__fill" style="width:100%"></span>
                  <span class="bar__flash"></span>
                </div>
                <span class="stat__num">{{ gameplayState.config.playerHp }}</span>
              </div>
              <div class="stat">
                <span class="stat__name">怒气</span>
                <div class="vital-bar bar bar--rage" role="progressbar" aria-label="怒气"
                  :aria-valuenow="gameplayState.state.rage" :aria-valuemax="gameplayState.config.rageMax" aria-valuemin="0">
                  <span class="bar__lag" :style="{ width: `${ragePercent()}%` }"></span>
                  <span class="bar__fill" :style="{ width: `${ragePercent()}%` }"></span>
                  <span class="bar__flash"></span>
                </div>
                <span class="stat__num">{{ gameplayState.state.rage }}<i>/{{ gameplayState.config.rageMax }}</i></span>
              </div>
            </div>
          </div>
        </div>
      </section>
      <section v-if="!inventoryOpen" class="skill-strip skills" aria-label="怒气技能" @pointerdown.stop @pointermove.stop="camera.resetInput">
        <div v-for="(skill, id) in gameplayState.config.skills" :key="id" class="skill-wrap">
          <button type="button" class="skill-tile skill hint-anchor" tabindex="0"
            :class="{ unavailable: !skillStatus(id).available, 'is-cd': skillStatus(id).remainingMs > 0 }"
            :aria-label="`${skill.key} ${skill.name}，消耗 ${skill.rageCost} 怒气`">
            <span class="skill__ring" aria-hidden="true"></span>
            <svg class="skill__glyph" aria-hidden="true"><use :href="`#story-${id}`" /></svg>
            <span class="skill__cost">{{ skill.rageCost }}</span>
            <span class="skill__left">{{ cooldownSeconds(id) }}</span>
            <span class="skill__key" aria-hidden="true">{{ skill.key }}</span>
            <span class="ui-tooltip" role="tooltip">{{ skill.name }} · {{ skill.rageCost }} 怒气</span>
          </button>
          <span class="skill__name">{{ skill.name }}</span>
        </div>
      </section>
    </div>
    <div v-if="inventoryOpen && gameplayState" class="inventory-overlay" @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent>
      <section ref="inventoryPanel" class="inventory-panel plate" role="dialog" aria-modal="true" aria-labelledby="inventory-title" @keydown="onInventoryKey">
        <div class="plate__body inventory-inner">
          <span class="haze" aria-hidden="true"></span>
          <header class="inventory-header">
            <h1 id="inventory-title">背包</h1>
            <button class="close-inventory" data-inventory-close aria-label="关闭背包" :disabled="navigationPending" @click="toggleInventory">×</button>
          </header>
          <div class="inventory-grid" aria-label="物品格子">
            <button v-for="slot in 20" :key="slot" class="inventory-slot hint-anchor"
              :class="{ occupied: slot === 1 && gameplayState.state.inventory.worldFragment }"
              :tabindex="selectedSlot === slot - 1 ? 0 : -1" :disabled="navigationPending"
              :aria-label="slot === 1 && gameplayState.state.inventory.worldFragment ? `世界碎片，数量 ${gameplayState.state.inventory.worldFragment}` : `空格 ${slot}`"
              @focus="selectedSlot = slot - 1" @click="selectedSlot = slot - 1">
              <template v-if="slot === 1 && gameplayState.state.inventory.worldFragment">
                <svg class="fragment-icon" aria-hidden="true"><use href="#story-shard" /></svg>
                <span class="slot-count">{{ gameplayState.state.inventory.worldFragment }}</span>
                <span class="ui-tooltip" role="tooltip">世界碎片</span>
              </template>
            </button>
          </div>
          <div v-if="gameplayError" class="gameplay-error" role="alert">{{ gameplayError }}</div>
          <footer class="inventory-footer">
            <span data-inventory-travel-focus class="world-button-hint hint-anchor" :tabindex="gameplayState.role === 'main' && gameplayState.state.boss.hp > 0 ? 0 : -1"
              :aria-label="gameplayState.role === 'main' && gameplayState.state.boss.hp > 0 ? '击败 Boss 后开启小世界' : undefined">
              <button data-inventory-travel class="world-travel-button" :disabled="navigationPending || (gameplayState.role === 'main' && gameplayState.state.boss.hp > 0)" @click="navigateWorld">
                {{ gameplayState.role === 'main' ? '小世界' : '返回主世界' }}
              </button>
              <span v-if="gameplayState.role === 'main' && gameplayState.state.boss.hp > 0" class="ui-tooltip" role="tooltip">击败 Boss 后开启</span>
            </span>
          </footer>
        </div>
      </section>
    </div>
    <div ref="fxLayer" class="fx-layer" aria-hidden="true"></div>
    <div class="toast" :class="{ 'is-on': Boolean(toastText) }" role="status" aria-live="polite">{{ toastText }}</div>
  </div>
</template>

<style scoped>
/* =========================================================
   设计令牌：东方暗黑神话色板 / 切角金边 / 墨色浓度
   ========================================================= */
.story-world-viewport {
  /* —— 颜色 —— */
  --c-paper: #e9e0cc;
  --c-paper-dim: #bdb298;
  --c-gold: #c8a659;
  --c-gold-dim: #8a7038;
  --c-gold-bright: #f0d68f;
  --c-cinnabar: #b3382a;
  --c-cinnabar-bright: #e0523b;
  --c-bronze: #5f8a72;
  --c-bronze-dim: #2c4a3c;

  /* —— 结构 —— */
  --gold-line: rgba(200, 166, 89, .42);
  --gold-line-soft: rgba(200, 166, 89, .22);
  --panel-a: .88;
  --pattern: rgba(233, 224, 204, .08);
  --haze: .12;
  --slot: clamp(50px, 6.2vw, 74px);
  --gap-edge: clamp(10px, 2.1vw, 30px);
  --cut: 14px;
  --edge: 2px;
  --shape: polygon(var(--cut) 0, 100% 0, 100% calc(100% - var(--cut)), calc(100% - var(--cut)) 100%, 0 100%, 0 var(--cut));
  --boss-top: clamp(10px, 1.7vh, 22px);
  --toast-top: calc(var(--boss-top) + 64px);
  --font-serif: 'Noto Serif SC', 'Source Han Serif SC', 'Source Han Serif CN', 'Songti SC', 'STSong', 'SimSun', serif;
  --font-brush: 'LXGW WenKai', '霞鹜文楷', 'Kaiti SC', 'STKaiti', 'KaiTi', '楷体', var(--font-serif);

  position: fixed;
  inset: 0;
  overflow: hidden;
  outline: none;
  background: transparent;
  color: var(--c-paper);
  font-family: var(--font-serif);
  -webkit-font-smoothing: antialiased;
  text-rendering: optimizeLegibility;
}

/* =========================================================
   通用面板：切角金边「框 + 芯」，浅色花纹打在芯上
   ========================================================= */
.plate {
  position: relative;
  padding: var(--edge);
  clip-path: var(--shape);
  background-image:
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .18) 0 1px, transparent 1px 4px),
    linear-gradient(148deg,
      rgba(250, 232, 182, .95) 0%, rgba(200, 168, 98, .78) 16%,
      rgba(122, 98, 52, .46) 38%, rgba(96, 78, 42, .58) 58%,
      rgba(208, 176, 106, .82) 82%, rgba(250, 232, 182, .92) 100%);
  filter: drop-shadow(0 10px 24px rgba(0, 0, 0, .62));
}

.plate__body {
  position: relative;
  height: 100%;
  box-sizing: border-box;
  clip-path: var(--shape);
  background-color: rgba(26, 23, 18, .94);
  background-image:
    repeating-linear-gradient(90deg, var(--pattern) 0 1px, transparent 1px 9px),
    repeating-linear-gradient(90deg, rgba(233, 224, 204, .05) 0 2px, transparent 2px 16px),
    repeating-linear-gradient(58deg, rgba(233, 224, 204, .03) 0 1px, transparent 1px 13px),
    repeating-linear-gradient(-58deg, rgba(233, 224, 204, .022) 0 1px, transparent 1px 17px),
    repeating-linear-gradient(114deg, rgba(233, 224, 204, .035) 0 1px, transparent 1px 6px),
    radial-gradient(120% 92% at 8% 0%, rgba(233, 224, 204, .11), transparent 62%),
    radial-gradient(92% 120% at 100% 112%, rgba(179, 56, 42, .11), transparent 62%),
    linear-gradient(180deg, rgba(70, 61, 48, var(--panel-a)), rgba(25, 22, 18, .94));
  background-size: 100% 5px, 100% 3px, auto, auto, auto, auto, auto, auto;
  background-position: left top, left bottom, 0 0, 0 0, 0 0, 0 0, 0 0, 0 0;
  background-repeat: no-repeat, no-repeat, repeat, repeat, repeat, no-repeat, no-repeat, no-repeat;
  box-shadow: inset 0 0 0 1px rgba(0, 0, 0, .62), inset 0 0 30px rgba(0, 0, 0, .5);
}

.plate__body > * { position: relative; z-index: 1; }

/* 朱砂小菱：贴在切角处，作为点睛 */
.plate__body::before {
  content: '';
  position: absolute;
  left: 8px;
  top: 8px;
  width: 5px;
  height: 5px;
  z-index: 2;
  background: linear-gradient(135deg, var(--c-cinnabar-bright), var(--c-cinnabar));
  transform: rotate(45deg);
  opacity: .9;
  box-shadow: 0 0 9px rgba(179, 56, 42, .5);
}

/* 局部墨迹晕染（只贴在控件边缘，不铺满屏幕） */
.haze {
  position: absolute !important;
  z-index: 0 !important;
  pointer-events: none;
  left: -16%;
  top: -45%;
  width: 64%;
  height: 190%;
  background: radial-gradient(closest-side, rgba(233, 224, 204, var(--haze)), rgba(233, 224, 204, 0) 72%);
  filter: blur(10px);
}

.haze--r {
  left: auto;
  right: -20%;
  top: -30%;
  width: 56%;
  height: 170%;
  background: radial-gradient(closest-side, rgba(179, 56, 42, calc(var(--haze) * .9)), rgba(179, 56, 42, 0) 72%);
}

/* =========================================================
   HUD 总容器
   ========================================================= */
.story-loading {
  position: absolute;
  inset: 0;
  z-index: 30;
  display: grid;
  place-items: center;
  background: radial-gradient(circle at 50% 46%, rgba(24, 21, 17, .72), rgba(5, 5, 4, .94) 78%);
  color: var(--c-paper-dim);
  font-size: clamp(12px, 1.2vw, 15px);
  letter-spacing: .34em;
  text-indent: .34em;
}

.hud-symbols { position: absolute; width: 0; height: 0; overflow: hidden; }

.story-hud {
  position: absolute;
  inset: 0;
  pointer-events: none;
  z-index: 20;
}

/* ------------------ 顶部中央：首领血条 ------------------ */
.boss-status {
  position: absolute;
  left: 50%;
  transform: translateX(-50%);
  top: var(--boss-top);
  width: min(600px, 44vw);
}

.boss-status .plate__body { padding: clamp(7px, .9vh, 10px) clamp(10px, 1vw, 14px) clamp(8px, 1vh, 11px); }
.boss-status .plate__body::before { left: auto; right: 8px; top: 8px; }

.boss-head { display: flex; align-items: center; gap: 8px; margin-bottom: 6px; }
.boss-seal { flex: none; width: 16px; height: 16px; display: grid; place-items: center; opacity: .9; }
.boss-seal svg { width: 16px; height: 16px; }
.boss-seal path { fill: none; stroke: var(--c-cinnabar-bright); stroke-width: 3; stroke-linecap: round; }

.boss-name {
  font-family: var(--font-brush);
  font-size: clamp(11px, 1.15vw, 14px);
  letter-spacing: .2em;
  color: #e7d9b6;
  text-shadow: 0 1px 3px rgba(0, 0, 0, .95), 0 0 12px rgba(179, 56, 42, .25);
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.boss-number {
  margin-left: auto;
  flex: none;
  font-size: clamp(9px, .95vw, 11px);
  font-variant-numeric: tabular-nums;
  color: var(--c-paper-dim);
  opacity: .85;
  text-shadow: 0 1px 2px #000;
}

.boss-number b { color: var(--c-gold-bright); font-weight: 600; }
.boss-number i { font-style: normal; opacity: .45; margin: 0 2px; }

/* ------------------ 通用状态条 ------------------ */
.bar {
  position: relative;
  flex: 1;
  min-width: 0;
  height: var(--bar-h, 13px);
  overflow: hidden;
  border-radius: 2px 12px 12px 2px;
  background: linear-gradient(180deg, #1c1813 0%, #110f0c 34%, #080706 72%, #050504 100%);
  border: var(--edge) solid var(--gold-line-soft);
  box-shadow: inset 0 2px 5px rgba(0, 0, 0, .85), inset 0 -1px 0 rgba(233, 224, 204, .07);
}

.bar__lag, .bar__fill { position: absolute; left: 0; top: 0; bottom: 0; width: 0; }

.bar__lag {
  width: 100%;
  background: linear-gradient(180deg,
    rgba(233, 224, 204, .42) 0%, rgba(233, 224, 204, .26) 38%,
    rgba(233, 224, 204, .12) 72%, rgba(233, 224, 204, .05) 100%);
  transition: width .62s cubic-bezier(.25, .8, .3, 1) .34s;
}

.bar__fill {
  transition: width .13s linear;
  border-right: 1px solid rgba(255, 240, 205, .5);
  box-shadow:
    inset 0 1px 0 rgba(255, 255, 255, .55),
    inset 0 5px 7px -4px rgba(255, 255, 255, .30),
    inset 0 -2px 3px rgba(0, 0, 0, .55),
    inset 0 -6px 8px -4px rgba(0, 0, 0, .75);
}

.bar__fill::after {
  content: '';
  position: absolute;
  left: 1px;
  right: 1px;
  top: 1px;
  height: 1px;
  background: linear-gradient(90deg, rgba(255, 255, 255, .6), rgba(255, 255, 255, .06));
  opacity: .5;
}

.bar--hp .bar__fill {
  background: linear-gradient(180deg, #e97a60 0%, #cb4a36 30%, #a3321f 58%, #71200f 82%, #420f07 100%);
  animation: breathe 3.6s ease-in-out infinite;
}

.bar--rage .bar__fill {
  background: linear-gradient(180deg, #f7e6b4 0%, #d8b569 30%, #b08c3f 58%, #7d6023 82%, #48370f 100%);
  animation: breathe 3s ease-in-out infinite .8s;
}

.bar--boss { --bar-h: 14px; border-color: rgba(200, 166, 89, .38); }

.bar--boss .bar__fill {
  background: linear-gradient(180deg, #f0805f 0%, #cf4d38 30%, #a3311f 58%, #701f0f 82%, #3f0e07 100%);
}

.bar--boss .bar__lag {
  transition: width .8s cubic-bezier(.3, .7, .2, 1) .38s;
  background: linear-gradient(180deg,
    rgba(233, 224, 204, .46) 0%, rgba(233, 224, 204, .28) 38%,
    rgba(233, 224, 204, .13) 72%, rgba(233, 224, 204, .05) 100%);
}

/* 人物状态条比首领血条窄一号 */
.vital-bar { --bar-h: 12px; }

.bar__flash {
  position: absolute;
  inset: 0;
  opacity: 0;
  pointer-events: none;
  background: linear-gradient(180deg, rgba(255, 255, 255, .96), rgba(255, 255, 255, .42));
}

/* 首领血条的刻度：斜切浅线 */
.bar__tick {
  position: absolute;
  top: 0;
  bottom: 0;
  width: 2px;
  transform: skewX(-18deg);
  background: linear-gradient(180deg, rgba(233, 224, 204, .42), rgba(233, 224, 204, .08));
  box-shadow: 0 0 5px rgba(0, 0, 0, .9);
}

/* ------------------ 左下：生命 / 怒气 ------------------ */
.player-vitals {
  position: absolute;
  left: var(--gap-edge);
  bottom: var(--gap-edge);
  width: clamp(196px, 25vw, 318px);
  pointer-events: auto;
}

.player-vitals .plate__body { padding: clamp(9px, 1.2vh, 13px) clamp(11px, 1.2vw, 16px); }
.vitals__row { display: flex; align-items: center; gap: clamp(8px, 1vw, 12px); }

/* 朱砂小印（抽象三笔） */
.vitals__seal {
  flex: none;
  width: 26px;
  height: 26px;
  display: grid;
  place-items: center;
  border: var(--edge) solid rgba(214, 96, 72, .8);
  background: linear-gradient(180deg, rgba(179, 56, 42, .28), rgba(179, 56, 42, .06));
  border-radius: 2px 8px 2px 2px;
  box-shadow: 0 0 12px rgba(179, 56, 42, .26), inset 0 0 8px rgba(0, 0, 0, .6);
}

.vitals__seal svg { width: 16px; height: 16px; }
.vitals__seal path { fill: none; stroke: var(--c-cinnabar-bright); stroke-width: 1.7; stroke-linecap: round; }
.vitals__bars { flex: 1; display: grid; gap: clamp(6px, .9vh, 10px); min-width: 0; }

.stat { display: flex; align-items: center; gap: clamp(6px, .8vw, 10px); min-width: 0; }

.stat__name {
  flex: none;
  min-width: 2.4em;
  white-space: nowrap;
  font-family: var(--font-brush);
  font-size: clamp(10px, 1.02vw, 13px);
  letter-spacing: .18em;
  color: var(--c-paper-dim);
  text-shadow: 0 1px 2px rgba(0, 0, 0, .9);
}

.stat__num {
  flex: none;
  min-width: 2.9em;
  text-align: right;
  white-space: nowrap;
  font-size: clamp(9px, .92vw, 12px);
  font-variant-numeric: tabular-nums;
  color: var(--c-gold);
  opacity: .88;
  text-shadow: 0 1px 2px rgba(0, 0, 0, .9);
}

.stat__num i { font-style: normal; opacity: .5; }

/* ------------------ 右下：技能槽 ------------------ */
.skills {
  position: absolute;
  right: var(--gap-edge);
  bottom: var(--gap-edge);
  display: flex;
  align-items: flex-end;
  gap: clamp(9px, 1.2vw, 18px);
  pointer-events: auto;
}

.skill-wrap { display: grid; justify-items: center; gap: 6px; }

.skill {
  position: relative;
  width: var(--slot);
  height: var(--slot);
  padding: 0;
  border-radius: 50%;
  cursor: pointer;
  color: #ded2b6;
  pointer-events: auto;
  border: var(--edge) solid rgba(200, 166, 89, .55);
  background:
    radial-gradient(circle at 34% 26%, rgba(233, 224, 204, .10), transparent 56%),
    radial-gradient(circle at 72% 80%, rgba(179, 56, 42, .14), transparent 56%),
    linear-gradient(180deg, rgba(26, 24, 20, .94), rgba(8, 8, 8, .96));
  box-shadow:
    inset 0 0 14px rgba(0, 0, 0, .9),
    inset 0 1px 0 rgba(233, 224, 204, .07),
    0 4px 16px rgba(0, 0, 0, .65),
    0 0 0 3px rgba(0, 0, 0, .35);
  transition: transform .16s ease, box-shadow .22s ease, border-color .22s ease;
  outline: none;
}

.skill__ring { position: absolute; inset: 4px; border-radius: 50%; border: 1px solid rgba(200, 166, 89, .24); }
.skill__ring::after { content: ''; position: absolute; inset: 4px; border-radius: 50%; border: 1px dashed rgba(233, 224, 204, .10); }

/* 岩石底纹（局部材质） */
.skill::after {
  content: '';
  position: absolute;
  inset: 0;
  border-radius: 50%;
  pointer-events: none;
  opacity: .5;
  mix-blend-mode: screen;
  background-image:
    repeating-linear-gradient(70deg, rgba(233, 224, 204, .05) 0 1px, transparent 1px 6px),
    radial-gradient(circle at 30% 70%, rgba(233, 224, 204, .05), transparent 45%);
}

.skill__glyph {
  position: absolute;
  inset: 0;
  display: grid;
  place-items: center;
  pointer-events: none;
  transition: opacity .25s ease, transform .25s ease;
}

/* 图标本体是按钮的直属子节点，这里统一笔触与颜色（居中由 .skill__glyph 负责） */
.skill-tile > svg {
  width: 52%;
  height: 52%;
  overflow: visible;
  color: #f0d08d;
  fill: none;
  stroke: currentColor;
  stroke-width: 2.1;
  stroke-linecap: round;
  stroke-linejoin: round;
}

.skill__cost {
  position: absolute;
  left: 12%;
  top: 6%;
  font-size: clamp(8px, .85vw, 10px);
  letter-spacing: .06em;
  color: rgba(143, 184, 160, .9);
  text-shadow: 0 1px 2px #000;
  pointer-events: none;
}

.skill__key {
  position: absolute;
  right: -5px;
  bottom: -5px;
  min-width: 21px;
  height: 21px;
  padding: 0 3px;
  display: grid;
  place-items: center;
  font-size: 11px;
  letter-spacing: .04em;
  color: #f2e6c8;
  background: linear-gradient(180deg, #7a3326, #2c110c);
  border: var(--edge) solid rgba(214, 182, 116, .75);
  box-shadow: 0 2px 9px rgba(0, 0, 0, .8), inset 0 1px 0 rgba(255, 255, 255, .14);
  pointer-events: none;
}

.skill__name {
  font-family: var(--font-brush);
  font-size: clamp(10px, 1vw, 12px);
  letter-spacing: .3em;
  text-indent: .3em;
  color: var(--c-paper-dim);
  opacity: .9;
  text-shadow: 0 1px 2px rgba(0, 0, 0, .9);
}

.skill__left {
  position: absolute;
  inset: 0;
  display: grid;
  place-items: center;
  font-size: clamp(14px, 1.7vw, 20px);
  font-variant-numeric: tabular-nums;
  color: #f2e8ce;
  text-shadow: 0 0 9px rgba(0, 0, 0, .95), 0 1px 0 rgba(0, 0, 0, .9);
  pointer-events: none;
}

/* 冷却中压暗符文，让倒计时数字清晰可读 */
.skill.is-cd .skill__glyph { opacity: .22; transform: scale(.9); }

.skill-tile.unavailable { border-color: #68624e; }
.skill-tile.unavailable .skill__glyph { opacity: .35; filter: grayscale(1); }
.skill-tile.unavailable .skill__glyph path { stroke: #cfc7ae; }

.skill:hover {
  transform: translateY(-2px);
  border-color: rgba(240, 214, 143, .6);
  box-shadow: inset 0 0 14px rgba(0, 0, 0, .9), 0 6px 18px rgba(0, 0, 0, .7), 0 0 18px rgba(200, 166, 89, .22), 0 0 0 3px rgba(0, 0, 0, .35);
}

/* ------------------ 左上：操作提示 ------------------ */
.tip {
  position: absolute;
  left: var(--gap-edge);
  top: clamp(12px, 1.8vh, 22px);
  display: grid;
  grid-template-columns: auto 1fr;
  align-items: center;
  gap: 6px 9px;
  max-width: min(300px, 30vw);
  padding: 9px 13px 9px 10px;
  font-size: clamp(10px, 1vw, 12.5px);
  line-height: 1.5;
  color: rgba(233, 224, 204, .72);
  letter-spacing: .06em;
  clip-path: polygon(8px 0, 100% 0, 100% calc(100% - 8px), calc(100% - 8px) 100%, 0 100%, 0 8px);
  border: var(--edge) solid transparent;
  background:
    linear-gradient(180deg, rgba(46, 40, 32, .80), rgba(22, 19, 16, .70)) padding-box,
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .18) 0 1px, transparent 1px 4px) border-box,
    linear-gradient(140deg, rgba(240, 214, 143, .85), rgba(122, 98, 52, .42) 52%, rgba(240, 214, 143, .75)) border-box;
  box-shadow: inset 0 0 22px rgba(0, 0, 0, .45);
  animation: breathe-soft 5.2s ease-in-out infinite;
}

.tip__seal { grid-row: span 9; flex: none; width: 15px; height: 15px; align-self: start; }
.tip__seal path { fill: none; stroke: var(--c-cinnabar-bright); stroke-width: 1.9; stroke-linecap: round; }
.tip span { display: flex; align-items: center; gap: 10px; white-space: nowrap; }

kbd {
  font-family: inherit;
  font-size: 11px;
  color: #dfcb94;
}

.tip kbd {
  min-width: 42px;
  padding: 2px 3px;
  border: 1px solid rgba(200, 166, 89, .45);
  background: rgba(200, 166, 89, .10);
  text-align: center;
  color: var(--c-gold-bright);
}

.pickup-control { color: #ffdf8d; }
.pickup-control kbd { border-color: #d9b45c; background: rgba(217, 180, 92, .18); }

/* ------------------ 底部中央：错误 / 反馈 ------------------ */
.story-bottom-stack {
  position: absolute;
  left: 50%;
  transform: translateX(-50%);
  bottom: calc(var(--gap-edge) + clamp(72px, 11vh, 122px));
  width: min(490px, calc(100vw - 32px));
  display: grid;
  gap: 9px;
  justify-items: center;
}

.gameplay-error {
  pointer-events: auto;
  display: flex;
  align-items: center;
  gap: 10px;
  width: 100%;
  padding: 10px 12px;
  font-size: 13px;
  color: #ffcac0;
  overflow-wrap: anywhere;
  clip-path: polygon(10px 0, 100% 0, 100% calc(100% - 10px), calc(100% - 10px) 100%, 0 100%, 0 10px);
  border: var(--edge) solid rgba(224, 82, 59, .6);
  background: linear-gradient(180deg, rgba(58, 22, 18, .92), rgba(26, 12, 10, .9));
  box-shadow: 0 8px 22px rgba(0, 0, 0, .55), inset 0 0 20px rgba(0, 0, 0, .5);
}

.gameplay-error button {
  flex-shrink: 0;
  padding: 5px 10px;
  font-family: inherit;
  color: #f3d9b4;
  background: rgba(179, 56, 42, .28);
  border: 1px solid rgba(214, 182, 116, .6);
  cursor: pointer;
}

.story-feedback {
  justify-self: center;
  padding: 5px 16px;
  font-family: var(--font-brush);
  font-size: clamp(12px, 1.25vw, 15px);
  letter-spacing: .24em;
  text-indent: .24em;
  color: #efdfba;
  white-space: nowrap;
  clip-path: polygon(9px 0, 100% 0, 100% calc(100% - 9px), calc(100% - 9px) 100%, 0 100%, 0 9px);
  border: var(--edge) solid transparent;
  background:
    linear-gradient(180deg, rgba(40, 34, 26, .88), rgba(18, 16, 13, .84)) padding-box,
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .18) 0 1px, transparent 1px 4px) border-box,
    linear-gradient(140deg, rgba(240, 214, 143, .85), rgba(122, 98, 52, .42) 50%, rgba(240, 214, 143, .7)) border-box;
  box-shadow: 0 8px 22px rgba(0, 0, 0, .55);
}

/* ------------------ 飘字 / 提示条 ------------------ */
.fx-layer { position: absolute; inset: 0; overflow: hidden; pointer-events: none; z-index: 40; }

.dmg {
  position: absolute;
  transform: translate(-50%, -50%);
  font-family: var(--font-serif);
  font-weight: 700;
  font-size: clamp(18px, 2.1vw, 27px);
  color: var(--c-gold-bright);
  text-shadow: 0 0 12px rgba(200, 166, 89, .5), 0 2px 0 rgba(0, 0, 0, .85), 0 0 3px rgba(0, 0, 0, .9);
  animation: dmg-float .95s cubic-bezier(.2, .7, .3, 1) forwards;
  will-change: transform, opacity;
  pointer-events: none;
}

.dmg--miss { font-size: clamp(13px, 1.4vw, 17px); color: rgba(233, 224, 204, .68); }

.toast {
  position: absolute;
  left: 50%;
  top: var(--toast-top);
  transform: translate(-50%, -8px);
  padding: 5px 16px;
  font-family: var(--font-brush);
  font-size: clamp(12px, 1.25vw, 15px);
  letter-spacing: .24em;
  text-indent: .24em;
  color: #efdfba;
  white-space: nowrap;
  clip-path: polygon(9px 0, 100% 0, 100% calc(100% - 9px), calc(100% - 9px) 100%, 0 100%, 0 9px);
  border: var(--edge) solid transparent;
  background:
    linear-gradient(180deg, rgba(40, 34, 26, .88), rgba(18, 16, 13, .84)) padding-box,
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .18) 0 1px, transparent 1px 4px) border-box,
    linear-gradient(140deg, rgba(240, 214, 143, .85), rgba(122, 98, 52, .42) 50%, rgba(240, 214, 143, .7)) border-box;
  box-shadow: 0 8px 22px rgba(0, 0, 0, .55);
  opacity: 0;
  pointer-events: none;
  z-index: 45;
  transition: opacity .3s ease, transform .3s ease;
}

.toast.is-on { opacity: 1; transform: translate(-50%, 0); }

/* =========================================================
   背包
   ========================================================= */
.inventory-overlay {
  pointer-events: auto;
  position: absolute;
  inset: 0;
  z-index: 50;
  display: grid;
  place-items: center;
  padding: 20px;
  background: radial-gradient(circle at 50% 46%, rgba(24, 21, 17, .58), rgba(0, 0, 0, .88) 78%);
}

.inventory-panel {
  pointer-events: auto;
  --cut: 22px;
  position: relative;
  width: min(92vw, 480px);
  box-sizing: border-box;
  max-height: calc(100vh - 40px);
  overflow: hidden;
}

.inventory-inner {
  box-sizing: border-box;
  max-height: calc(100vh - 44px);
  overflow-y: auto;
  padding: clamp(20px, 3.2vh, 30px) clamp(18px, 2.6vw, 28px) clamp(16px, 2.4vh, 24px);
}

.inventory-header { display: flex; justify-content: space-between; align-items: center; padding-bottom: 18px; }

.inventory-header h1 {
  margin: 0;
  font-family: var(--font-brush);
  font-size: clamp(18px, 2vw, 22px);
  font-weight: 400;
  letter-spacing: .34em;
  color: #f0e3c2;
  text-shadow: 0 0 2px rgba(0, 0, 0, .9), 0 2px 0 #000;
}

.close-inventory {
  width: 30px;
  height: 30px;
  font-family: inherit;
  font-size: 22px;
  line-height: 1;
  color: #cabb9b;
  background: rgba(200, 166, 89, .08);
  border: var(--edge) solid rgba(200, 166, 89, .5);
  cursor: pointer;
  transition: color .22s ease, border-color .22s ease;
}

.close-inventory:hover { color: var(--c-gold-bright); border-color: rgba(240, 214, 143, .7); }

.inventory-grid { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 8px; }

.inventory-slot {
  position: relative;
  aspect-ratio: 1;
  min-width: 0;
  color: #e4d5b2;
  cursor: pointer;
  clip-path: polygon(7px 0, 100% 0, 100% calc(100% - 7px), calc(100% - 7px) 100%, 0 100%, 0 7px);
  border: var(--edge) solid rgba(200, 166, 89, .22);
  background: linear-gradient(180deg, rgba(24, 21, 17, .72), rgba(8, 8, 7, .82));
  box-shadow: inset 0 0 14px rgba(0, 0, 0, .8);
}

.inventory-slot.occupied {
  border-color: rgba(200, 166, 89, .6);
  background: radial-gradient(ellipse at 50% 40%, rgba(81, 67, 38, .5), rgba(17, 24, 18, .9));
  box-shadow: inset 0 0 18px rgba(0, 0, 0, .7), 0 0 14px rgba(200, 166, 89, .18);
}

.fragment-icon { width: 60%; height: 60%; margin: auto; display: block; fill: rgba(125, 151, 102, .32); stroke: #d7ddb0; stroke-width: 1.5; }
.slot-count { position: absolute; bottom: 4px; right: 6px; font-size: 12px; color: var(--c-gold-bright); }
.inventory-footer { display: flex; justify-content: center; padding-top: 22px; }

.world-travel-button {
  min-width: 158px;
  height: 38px;
  font-family: inherit;
  font-size: 14px;
  letter-spacing: .16em;
  color: #f0d69a;
  cursor: pointer;
  clip-path: polygon(9px 0, 100% 0, 100% calc(100% - 9px), calc(100% - 9px) 100%, 0 100%, 0 9px);
  border: var(--edge) solid transparent;
  background:
    linear-gradient(180deg, rgba(52, 56, 32, .92), rgba(20, 22, 14, .92)) padding-box,
    repeating-linear-gradient(58deg, rgba(255, 242, 210, .16) 0 1px, transparent 1px 4px) border-box,
    linear-gradient(140deg, rgba(240, 214, 143, .8), rgba(122, 98, 52, .4) 52%, rgba(240, 214, 143, .7)) border-box;
}

.world-travel-button:hover:not(:disabled) { color: #fdf2d2; box-shadow: 0 0 22px rgba(200, 166, 89, .28); }

button:disabled {
  color: #929384;
  border-color: rgba(120, 116, 100, .5);
  background: rgba(35, 40, 32, .8);
  cursor: not-allowed;
}

button:focus-visible, .hint-anchor:focus-visible { outline: 2px solid #f3cf79; outline-offset: 3px; }
.hint-anchor { position: relative; }

.ui-tooltip {
  display: none;
  position: absolute;
  bottom: calc(100% + 9px);
  left: 50%;
  transform: translateX(-50%);
  padding: 7px 10px;
  border: var(--edge) solid rgba(143, 120, 70, .8);
  background: rgba(12, 18, 15, .96);
  color: #efe0bc;
  font-family: var(--font-serif);
  font-size: 12px;
  font-weight: 400;
  white-space: nowrap;
  z-index: 3;
  pointer-events: none;
}

.hint-anchor:hover > .ui-tooltip,
.hint-anchor:focus > .ui-tooltip,
.hint-anchor:focus-within > .ui-tooltip { display: block; }

/* =========================================================
   关键帧
   ========================================================= */
@keyframes breathe {
  0%, 100% { filter: brightness(.93) saturate(.95); }
  50% { filter: brightness(1.14) saturate(1.06); }
}

@keyframes breathe-soft {
  0%, 100% { opacity: .62; }
  50% { opacity: 1; }
}

@keyframes dmg-float {
  0% { opacity: 0; transform: translate(-50%, -50%) scale(.62) rotate(var(--rot, 0deg)); }
  20% { opacity: 1; transform: translate(-50%, -58%) scale(1.12) rotate(var(--rot, 0deg)); }
  60% { opacity: .95; }
  100% { opacity: 0; transform: translate(calc(-50% + var(--dx, 0px)), calc(-50% - var(--rise, 60px))) scale(.98) rotate(var(--rot, 0deg)); }
}

/* =========================================================
   响应式
   ========================================================= */
@media (max-width: 870px) {
  .boss-status { width: min(600px, 88vw); }
  .tip { max-width: min(64vw, 420px); font-size: 11px; }
}

@media (max-width: 900px) {
  .story-world-viewport { --slot: clamp(46px, 7.6vw, 62px); }
  .player-vitals { width: clamp(180px, 36vw, 250px); }
  .boss-name { font-size: 12px; letter-spacing: .12em; }
}

@media (max-width: 660px) {
  .story-world-viewport { --slot: clamp(44px, 12vw, 58px); --gap-edge: 12px; }
  .boss-name { max-width: 46vw; }
  .tip { max-width: 88vw; font-size: 10.5px; line-height: 1.45; }
  .skill__name { display: none; }
  .player-vitals { width: min(58vw, 220px); }
}

@media (max-width: 400px) {
  .story-world-viewport { --slot: 38px; }
  .skills { gap: 6px; }
  .player-vitals { width: min(50vw, 170px); }
  .vitals__seal { display: none; }
  .stat__name { letter-spacing: .04em; min-width: 2em; }
  .skill__cost { display: none; }
}

@media (prefers-reduced-motion: reduce) {
  .bar--hp .bar__fill, .bar--rage .bar__fill, .tip { animation: none; }
  .bar__lag, .bar__fill { transition: none; }
  .dmg { animation-duration: 1.15s; }
}
</style>
