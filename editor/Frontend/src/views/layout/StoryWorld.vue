<script setup>
import { computed, onMounted, onUnmounted, ref, nextTick } from 'vue';
import { useRouter, onBeforeRouteLeave } from 'vue-router';
import { editorApi } from '@/api/editorApi.js';
import { worldModeState, normalizeProjectPath } from '@/services/worldModeService.js';
import { createStoryCameraController } from '@/utils/viewportStoryCamera.js';
import { projectLauncherService, cancelPendingProjectOpen, getProjectSelectionVersion } from '@/services/projectLauncherService.js';
import { trackWorldSessionWork, notifyWorldError, registerWorldSessionSave } from '@/services/worldSessionLifecycle.js';
import { createStoryNavigationController, NAVIGATION_KEY } from '../../../../../game/frontend/storyNavigation.mjs';

import { ensureStoryCharacters, syncPlacementActors } from '../../../../../game/frontend/storyActors.mjs';
import { PLACEMENT_SITES, exhibitItems as exhibitItemsFor, nextPlacementIndex,
  placementGuid, sanitizePlacements } from '../../../../../game/frontend/storyProps.mjs';
import { PROPHET_GUID } from '../../../../../game/frontend/storyCharacters.mjs';
import { PROPHET_INTERACTION, canTalkToProphet, normalizeDialogue,
  prophetAvailable } from '../../../../../game/frontend/prophetDialogue.mjs';
import { createPlayerController, VIEW_LABELS } from '../../../../../game/frontend/playerController.mjs';
import { createPlayerSave } from '../../../../../game/frontend/playerSave.mjs';
import { createStoryGameplay, worldBounds, distanceToBounds, pickupDistance } from '../../../../../game/frontend/storyGameplay.mjs';
import { STORY_CHARACTERS, resolveStoryAssetPath, sceneSnapshot } from '../../../../../game/frontend/storyCharacters.mjs';
import { ensureStoryCube, cubeTransformOf, playerFacingYaw } from '../../../../../game/frontend/storyCube.mjs';
import { collectSwayTargets, createWorldRuleRunner,
  findFragment } from '../../../../../game/frontend/storyWorldRules.mjs';
import { ADJUST_ACTIONS, PROPHET_SCREENS, adjustFeedback, fragmentPanel,
  nextObjectTransform, objectLabel, prophetKeyAction, selectableObjects,
  toggleFragment } from '../../../../../game/frontend/storyProphetActions.mjs';

const router = useRouter();
const surface = ref(null);
const inventoryOpen = ref(false);
const inventoryPanel = ref(null);
const selectedSlot = ref(0);
const navigationPending = ref(false);
// The gameplay object is a plain, non-reactive controller, so `computed` cannot
// track it. Mirror the confirmed world role into a ref instead of reading through it.
const roleRef = ref(null);
// The small-world roster: named small worlds the player owns. Mirrored from the
// backend (authoritative), independent of the combat state like the exhibits.
const subworlds = ref([]);
// Which roster entry the player last entered; its name surfaces in the HUD.
const activeSubworldId = ref(null);
// A small world's display name, read from the roster entry being visited.
const subworldName = computed(() => {
  const entry = subworlds.value.find(item => item.id === activeSubworldId.value);
  return entry?.name || (subworlds.value.length ? subworlds.value[0].name : '');
});
const initializing = ref(true);
const cooldownTick = ref(0);
let cooldownTimer = null;
const gameplayState = ref(null);
const bossNearby = ref(false);
const canPickup = ref(false);
const feedback = ref('');
const gameplayError = ref('');
const actionBusy = ref(false);
let feedbackTimer = null;
let bossActor = null;
let bossBounds = null;
let sceneId = null;
let visualSignature = null;
const revision = worldModeState.revision;
const projectPath = worldModeState.projectPath;
// Exhibits the small world shows. The backend is authoritative; this mirrors what it
// last confirmed so the panel and the scene can be reconciled from one source.
const placements = ref([]);
const exhibitItems = computed(() => {
  void viewTick.value;
  return exhibitItemsFor(gameplayState.value?.state?.inventory, placements.value);
});
// A roster entry opens only from the main world once the boss is defeated; both
// gates read from the confirmed role so the panel matches the travel button.
const canEnterRoster = computed(() => {
  const data = gameplayState.value;
  return Boolean(data && data.role === 'main' && data.state.boss.hp <= 0 && !navigation.busy);
});
const canAddSubworld = computed(() => {
  const data = gameplayState.value;
  return Boolean(data && data.role === 'main' && data.state.boss.hp <= 0);
});
const siteNames = PLACEMENT_SITES.map(site => site.name).join(' → ');
let visualPlacements = null;
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
  isInputLocked: () => initializing.value || !focused || document.hidden || navigation.busy || savingPlayer || inventoryOpen.value || dialogueOpen.value,
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
  // The prophet only exists in the small world, and only answers when actually near.
  prophetNearby.value = canTalkToProphet({
    role: data?.role, player, bounds: prophetBounds(), range: PROPHET_INTERACTION.range,
  }) && !dialogueOpen.value;
}
function showFeedback(message) {
  if (!current()) return;
  feedback.value = message;
  clearTimeout(feedbackTimer);
  feedbackTimer = setTimeout(() => { feedback.value = ''; }, 2200);
}
// Save finalizers may outlive the component; validate the actual native source,
// not the new route's revision, before reconciling any old-world actor.
async function assertSource() {
  const info = unwrap(await editorApi.projectSettings.getActiveProjectInfo());
  if (info?.mode !== 'story' || normalizeProjectPath(info.project_path) !== normalizeProjectPath(projectPath)) {
    throw new Error('当前世界已改变，已取消旧世界模型更新');
  }
}
const signature = data => JSON.stringify([data.role, data.state.boss.hp === 0, data.state.drop]);
// Exhibits change independently of combat state, so they get their own signature and
// only the ownership summary (not every coordinate) gates the scene work.
const placementSignature = list => JSON.stringify(sanitizePlacements(list)
  .map(entry => [placementGuid(entry.index), entry.propId, ...entry.position, ...entry.rotation, entry.scale]));
const gameplay = createStoryGameplay({
  api: editorApi, projectPath, readPlayer: camera.snapshotPlayer, readBoss: () => bossActor,
  trackWork: trackWorldSessionWork,
  onState: data => {
    // Mirror the confirmed role even when `current()` is false; the HUD must follow
    // the world the engine actually acknowledged, not a stale render guard.
    roleRef.value = data.role;
    if (current()) { gameplayState.value = data; updateProximity(); }
  },
  onFeedback: showFeedback,
  reconcile: async data => {
    const next = signature(data), nextPlacements = placementSignature(placements.value);
    if (next === visualSignature && nextPlacements === visualPlacements) return;
    if (next !== visualSignature) {
      await ensureStoryCharacters({ api: editorApi, sceneId, frontendUrl: window.location.href,
        gameplay: data, combatOnly: true, assertSource, projectPath });
      visualSignature = next;
    }
    // A main world has no exhibits, and the layout is authoritative: stale placement
    // actors are retired here, so a withdrawn item always leaves the scene. A world
    // that was replaced mid-flight aborts this, which is not a save failure.
    if (nextPlacements !== visualPlacements) {
      try {
        await syncPlacementActors({ api: editorApi, sceneId, frontendUrl: window.location.href,
          placements: placements.value, assertSource, isCurrent: current });
        visualPlacements = nextPlacements;
      } catch (error) {
        // An abort here means the world was replaced mid-flight. The combat branch
        // above is skipped in that same case, so failing the caller would be wrong.
        if (error.name !== 'AbortError' && current()) {
          gameplayError.value = `陈列同步失败：${error.message}`;
        }
      }
    }
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
// The camera controller is a plain object, so the hint label needs an explicit
// signal to recompute when the view mode changes.
const viewTick = ref(0);
function viewLabel() {
  void viewTick.value;
  const mode = camera.viewMode?.();
  return (mode && VIEW_LABELS?.[mode]) || '第三人称';
}
function cycleView() {
  if (!cameraReady || navigation.busy || savingPlayer || initializing.value) return;
  const label = viewLabel();
  camera.cycleViewMode?.();
  viewTick.value++;
  showFeedback(`视角：${label} → ${viewLabel()}（V 键切换）`);
}
// Talking to the prophet. Availability is a confirmed world role plus a live distance,
// exactly like the fragment pickup hint, so F never acts on a stale guess.
const dialogueOpen = ref(false);
const dialogueLine = ref(0);
const prophetNearby = ref(false);
const dialogue = normalizeDialogue(null);
let prophetActor = null;
let playerActor = null;
let cubeActor = null;
let cubeTransform = null;
// Derived per check rather than cached: the prophet is an engine-owned actor whose
// transform can change (a reload, a hand edit, or another world's restore), and a
// stale box would keep F answering from where the prophet used to stand.
const prophetBounds = () => worldBounds(prophetActor);
let dialoguePanel = null;
function openDialogue() {
  if (!prophetNearby.value || dialogueOpen.value) return;
  camera.resetInput();
  dialogueLine.value = 0;
  dialogueOpen.value = true;
  void nextTick(() => dialoguePanel?.focus());
}
function closeDialogue() {
  if (!dialogueOpen.value) return;
  leaveProphetScreen();
  exitAdjustMode({ reopenDialogue: false });
  dialogueOpen.value = false;
  dialogueLine.value = 0;
  // Re-arm the proximity hint immediately; nothing else moves the player here, so
  // without this F would appear dead until the next movement frame.
  updateProximity();
  void nextTick(() => surface.value?.focus());
}
// Enter/Space advance the script; the last line closes it so the player is never stuck.
function advanceDialogue() {
  if (!dialogueOpen.value) return;
  if (dialogueLine.value + 1 < dialogue.lines.length) dialogueLine.value++;
  else closeDialogue();
}
// F prioritises the prophet, then falls back to the fragment pickup it always had.
function interact() {
  if (prophetNearby.value) openDialogue();
  else void runAction(gameplay.pickup);
}

// ---- 「先知」旁的物体与世界规则（李淳珺任务）-----------------------------------
// The cube is a game-owned prop the prophet lets the player scale or move; the rule
// engine animates the small world once a fragment is installed. Both live in their own
// modules, so this page only wires them up: it renders the menu and applies what the
// pure functions return.
const installedRules = ref([]);
const actionFeedback = ref('');
// The saved records only say which fragments are installed; the semantics come from the
// catalogue, so a rewrite of a rule never invalidates an existing world.
const resolvedRules = computed(() => installedRules.value
  .map(record => findFragment(record?.fragmentId)?.rule).filter(Boolean));
// The panel has two main options. «碎片» is a screen inside the panel; «调整物体» leaves the
// panel for the overhead editing mode, and leaving that mode reopens the panel.
const prophetScreen = ref(null);
const adjustMode = ref(false);
const previousViewMode = ref(null);
const adjustSelection = ref(null);
const fragmentList = computed(() => fragmentPanel(installedRules.value));
const adjustHint = computed(() => (adjustSelection.value
  ? `已选中：${adjustSelection.value.name}`
  : '俯瞰视角：点击物体选中它，再用下面的按钮调整'));
function enterAdjustMode() {
  actionFeedback.value = '';
  prophetScreen.value = null;
  adjustSelection.value = null;
  // The overhead view is what makes the arrangement readable; remember where to return.
  previousViewMode.value = camera.viewMode?.() || null;
  camera.setViewMode?.('top');
  adjustMode.value = true;
  dialogueOpen.value = false;
  camera.resetInput();
  void nextTick(() => surface.value?.focus());
  void loadSelectableObjects();
}
// The list is the reliable way to choose an object: clicking a name cannot miss, while
// clicking a pixel depends on the engine's readback settling.
const selectableList = ref([]);
const isSelected = entry => Boolean(adjustSelection.value
  && adjustSelection.value.guid === entry?.guid);
async function loadSelectableObjects() {
  try {
    selectableList.value = selectableObjects(await editorApi.scene.getSnapshot(sceneId));
  } catch (error) {
    selectableList.value = [];
    actionFeedback.value = `读取物体列表失败：${error.message}`;
  }
}
function selectObjectFromList(entry) {
  adjustSelection.value = { guid: entry.guid, name: entry.name,
    route: entry.route, modelRef: entry.modelRef,
    actor: { actor_guid: entry.guid, name: entry.name, geometry: entry.transform },
    transform: entry.transform };
  actionFeedback.value = `已选中：${entry.name}`;
}
function exitAdjustMode({ reopenDialogue = true } = {}) {
  if (!adjustMode.value) return;
  if (previousViewMode.value) camera.setViewMode?.(previousViewMode.value);
  previousViewMode.value = null;
  adjustMode.value = false;
  adjustSelection.value = null;
  actionFeedback.value = '';
  camera.resetInput();
  if (reopenDialogue) {
    // Back to the conversation, positioned on the last line so the script is not replayed.
    dialogueOpen.value = true;
    dialogueLine.value = Math.max(0, dialogue.lines.length - 1);
    void nextTick(() => dialoguePanel?.focus());
  }
}
function openProphetScreen(id) {
  if (id === 'adjust') { enterAdjustMode(); return; }
  actionFeedback.value = '';
  prophetScreen.value = id;
}
function leaveProphetScreen() {
  prophetScreen.value = null;
  actionFeedback.value = '';
}
const worldRuleRunner = createWorldRuleRunner({
  bridge: () => (typeof window === 'undefined' ? null : window.coronaBridge),
  onError: error => showFeedback(`世界规则已停止：${error.message}`),
});
function rememberCube(actor) {
  cubeActor = actor || null;
  cubeTransform = cubeTransformOf(cubeActor);
  return cubeTransform;
}
// Rebuild the animated set from a fresh snapshot: reading the scene is an asynchronous
// CEF round trip, so the frame loop only replays the handles captured here. Passing no
// snapshot stops the rule, which is what the main world wants.
function applyWorldRules(snapshot) {
  worldRuleRunner.stop();
  if (!snapshot || !inSubworld.value || !resolvedRules.value.length) return false;
  worldRuleRunner.setTargets(collectSwayTargets(snapshot, resolvedRules.value[0]));
  worldRuleRunner.setRules(resolvedRules.value);
  return worldRuleRunner.start();
}
async function runProphetAction(chosen) {
  // Adjusting deliberately leaves the dialogue, so the guard has to accept either context.
  // Gating only on `dialogueOpen` silently dropped every adjustment action.
  if ((!dialogueOpen.value && !adjustMode.value) || !chosen) return;
  if (chosen.type === 'open-screen') { openProphetScreen(chosen.screen); return; }
  actionFeedback.value = '';
  if (chosen.type === 'toggle-fragment') {
    await runAction(async () => {
      const result = toggleFragment(installedRules.value, chosen.fragmentId);
      const saved = await gameplay.saveWorldRules(result.rules);
      installedRules.value = Array.isArray(saved?.rules) ? saved.rules : result.rules;
      actionFeedback.value = result.feedback;
      // A snapshot is what teaches the rule which objects to animate.
      applyWorldRules(await editorApi.scene.getSnapshot(sceneId));
    });
    return;
  }
  // An adjustment acts on whatever the player clicked; until then, on the prophet's cube.
  const target = adjustSelection.value?.actor || cubeActor;
  const current = cubeTransformOf(target)
    || (adjustSelection.value ? adjustSelection.value.transform : cubeTransform);
  if (!target || !current || !prophetActor || !playerActor) {
    actionFeedback.value = '物体尚未就绪，请先点击一个物体';
    return;
  }
  await runAction(async () => {
    const next = nextObjectTransform(chosen.id, { current, reference: prophetActor,
      facingYaw: playerFacingYaw(playerActor) });
    if (!next) return;
    // The light path: the setter does not re-import the resource, so it cannot add mesh work
    // to the geometry thread while the renderer is submitting. `persist: true` is explicit
    // because a runtime transform that is never persisted is exactly what made an earlier
    // adjustment look like it "did nothing"; the engine's own default is not relied on.
    const data = unwrap(await editorApi.scene.setActorTransform(sceneId, target.actor_guid,
      { ...next, persist: true }));
    if (!data || data.ok === false || !['success', 'loaded'].includes(data.status)) {
      throw new Error(data?.message || data?.diagnostics?.[0]?.message || '引擎未确认调整');
    }
    const actor = data.actor || { ...target, geometry: next };
    if (target === cubeActor) rememberCube(actor);
    if (adjustSelection.value) adjustSelection.value = { ...adjustSelection.value, actor, transform: next };
    // Re-anchor the sway, otherwise the next frame would undo the player's own move.
    worldRuleRunner.refreshBase(target.actor_guid, next.position);
    // Confirm against the engine instead of our own optimism. Reading the actor back is the
    // only way to separate "the engine refused the write" from "the engine took it but the
    // change is not on screen" — and those two need completely different fixes.
    const kept = (await readBackTransform(target.actor_guid))?.scale?.[0];
    const summary = adjustFeedback(chosen.id, next, prophetActor);
    actionFeedback.value = Number.isFinite(kept) && Math.abs(kept - next.scale[0]) < 1e-3
      ? `${summary}（引擎已接受）`
      : `${summary}｜引擎未保留，实际缩放 ${Number.isFinite(kept) ? kept.toFixed(2) : '读不到'}`;
  });
}
// Exhibits are laid out by the game, so the panel lists what the main world yielded
// rather than free-form placement. Site order is fixed and cycles per exhibit.
function nextSite() {
  return PLACEMENT_SITES[placements.value.length % PLACEMENT_SITES.length];
}
async function placementStep() {
  await gameplay.savePlacements(placements.value);
  if (!current()) return false;
  // Membership is now a confirmed input to reconcile(), which owns the scene work.
  await gameplay.flush();
  return true;
}
async function placeExhibit(propId) {
  if (!inSubworld.value || !current() || navigation.busy || savingPlayer || actionBusy.value) return;
  const item = exhibitItems.value.find(entry => entry.id === propId);
  if (!item?.available) return;
  const site = nextSite();
  placements.value = sanitizePlacements([...placements.value, {
    propId, index: nextPlacementIndex(placements.value), siteId: site.id,
    position: [...site.position], rotation: [0, 0, 0], scale: 1,
  }]);
  await runAction(async () => {
    try {
      await placementStep();
      showFeedback(`已陈列 ${item.name} · ${site.name}`);
    } catch (error) {
      // The backend is authoritative: re-read it instead of trusting a bad local guess.
      placements.value = sanitizePlacements(gameplay.placements?.placements);
      throw error;
    }
  });
}
async function withdrawExhibit() {
  if (!inSubworld.value || !current() || !placements.value.length
    || navigation.busy || savingPlayer || actionBusy.value) return;
  placements.value = placements.value.slice(0, -1);
  await runAction(async () => {
    try {
      await placementStep();
      showFeedback('已撤回最后一件陈列');
    } catch (error) {
      placements.value = sanitizePlacements(gameplay.placements?.placements);
      throw error;
    }
  });
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
// Sync the roster mirror from the authoritative backend. `seed` adds one entry the
// first time when the player has earned a small world (boss defeated) but the list
// is still empty, so killing the boss visibly opens the player's first small world.
async function syncSubworlds(seed = false) {
  if (!gameplay.subworlds) return;
  const list = gameplay.subworlds?.subworlds || [];
  subworlds.value = [...list];
  if (seed && !subworlds.value.length
    && gameplayState.value?.state?.inventory?.worldFragment > 0) {
    try {
      const grown = await gameplay.addSubworld();
      subworlds.value = [...(grown?.subworlds || [])];
      if (!activeSubworldId.value && subworlds.value.length) {
        activeSubworldId.value = subworlds.value[0].id;
      }
    } catch (error) { if (current()) gameplayError.value = error.message; }
  }
}
// Roster actions run inside the open bag panel, unlike `runAction` (which is for
// panel-closing combat/exhibit work), so they get their own busy guard.
async function runPanelAction(action) {
  if (!current() || navigation.busy || savingPlayer || actionBusy.value) return;
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
async function addSubworldEntry() {
  await runPanelAction(async () => {
    const grown = await gameplay.addSubworld();
    subworlds.value = [...(grown?.subworlds || [])];
    showFeedback('已记录一座小世界');
  });
}
async function renameSubworldEntry(entry) {
  const name = window.prompt(`为「${entry.name}」起一个新名字`, entry.name);
  if (!name || !name.trim()) return;
  await runPanelAction(async () => {
    const grown = await gameplay.renameSubworld(entry.id, name.trim());
    subworlds.value = [...(grown?.subworlds || [])];
    showFeedback('小世界已命名');
  });
}
// Enter a specific roster entry; remember which one so its name shows in the HUD.
async function enterSubworldEntry(entry) {
  const data = gameplayState.value;
  if (!current() || !inventoryOpen.value || !data || navigationPending.value || navigation.busy
    || data.role !== 'main' || data.state.boss.hp > 0) return;
  activeSubworldId.value = entry.id;
  await navigateWorld();
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
// A world switch tears this component down and re-mounts it for the target world, so
// pending/again-in-flight is the only window where the HUD must stay neutral. Without
// this the indicator would flip main -> sub (or back) in two visible steps.
const transitioning = computed(() => navigation.busy || navigationPending.value);
const inSubworld = computed(() => !transitioning.value && roleRef.value === 'child');
const worldLabel = computed(() => transitioning.value ? '切换中…'
  : inSubworld.value ? `小世界${subworldName.value ? ` · ${subworldName.value}` : ''}` : '主世界');
// The small world has no boss, no rage spend and no attack target, so the whole
// combat strip is hidden there instead of showing permanently disabled controls.
const showCombatHud = computed(() => !inSubworld.value);
const controlHints = computed(() => [
  ['WASD', '移动'],
  ['鼠标', '转向 · 滚轮缩放'],
  ['Space', '跳远'],
  ['Shift', '长按跑步 · 点按闪避'],
  ...(showCombatHud.value ? [
    ['左键', '攻击'],
    ['E / R', '技能'],
  ] : []),
  ...(prophetNearby.value ? [['F', `与${dialogue.name}交谈`]] : []),
  ...(canPickup.value ? [['F', '拾取 世界碎片']] : []),
  ['V', `视角 · ${viewLabel()}`],
  ['Tab', '背包'],
  ['Esc', inSubworld.value ? '保存离开' : '保存退出'],
]);
function onKeyDown(event) {
  if (!current()) return;
  // The dialogue panel owns the keyboard while it is open, and swallowing keys here
  // keeps F/Space from reaching the camera or another interaction.
  if (adjustMode.value) {
    // The editing mode owns the keyboard: only the adjustment steps and the way out get
    // through, so a stray key cannot cast a skill or swing while the player is arranging.
    event.preventDefault();
    event.stopPropagation();
    if (event.code === 'Escape' || event.key === 'Escape') {
      if (!event.repeat) exitAdjustMode();
      return;
    }
    if (!event.repeat) {
      const chosen = prophetKeyAction(event.key, 'adjust');
      if (chosen) void runProphetAction(chosen);
    }
    return;
  }
  if (dialogueOpen.value) {
    event.preventDefault();
    event.stopPropagation();
    if (event.code === 'Escape' || event.key === 'Escape') {
      // Escape walks back one level before it closes the panel.
      if (!event.repeat) {
        if (prophetScreen.value) leaveProphetScreen();
        else closeDialogue();
      }
      return;
    }
    if (!prophetScreen.value && !event.repeat
      && ['Enter', 'Space', 'NumpadEnter'].includes(event.code)) advanceDialogue();
    // Number keys pick an option on the current screen; the script keeps Enter and Space.
    if (!event.repeat) {
      const chosen = prophetKeyAction(event.key, prophetScreen.value);
      if (chosen) void runProphetAction(chosen);
    }
    return;
  }
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
  if (event.code === 'KeyV' || event.key?.toLowerCase() === 'v') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat) cycleView();
    return;
  }
  if (event.code === 'KeyF' || event.key?.toLowerCase() === 'f') {
    event.preventDefault(); event.stopPropagation();
    if (!event.repeat && focused) interact();
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
/** Read one actor's transform back from the engine, to confirm that a write was kept. */
async function readBackTransform(guid) {
  const actors = sceneSnapshot(await editorApi.scene.getSnapshot(sceneId))?.actors;
  const actor = (Array.isArray(actors) ? actors : []).find(entry => entry.actor_guid === guid);
  return actor ? cubeTransformOf(actor) : null;
}
function onPointerDown(event) {
  if (inventoryOpen.value) return;
  surface.value?.focus();
  // While adjusting, the click that focuses the viewport must not be swallowed by the gate.
  if (!focused && !adjustMode.value) return;
  if (event.button !== 0) { camera.pointerDown(event); return; }
  event.preventDefault();
  // Adjusting is label-driven: the left button neither attacks nor picks an object, it only
  // focuses the viewport so dragging and the wheel still let the player look at the object.
  if (adjustMode.value) return;
  void runAction(gameplay.attack);
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
    // A main world reports an empty layout, so this is a no-op there and the exhibits
    // are already known before the scene is reconciled below.
    placements.value = sanitizePlacements(gameplay.placements?.placements);
    await syncSubworlds(true);
    const { snapshot, player, targetOffset } = await ensureStoryCharacters({
      api: editorApi, sceneId, frontendUrl: window.location.href, gameplay: loadedGameplay, assertSource, projectPath,
      isCurrent: () => current() && worldModeState.projectPath === projectPath,
    });
    if (!current() || worldModeState.projectPath !== projectPath) return;
    bossActor = snapshot.actors?.find(actor => actor.actor_guid === STORY_CHARACTERS[1].guid) || null;
    bossBounds = worldBounds(bossActor);
    // The prophet is created on demand in the small world, so it may legitimately be
    // absent here; interaction simply stays unavailable until it exists.
    prophetActor = snapshot.actors?.find(actor => actor.actor_guid === PROPHET_GUID) || null;
    playerActor = player;
    // The prophet's cube and the rules this small world already carries (李淳珺任务).
    // A main world has no prophet, so the cube is hidden rather than created there.
    // The cube is decoration: if it cannot be created the world must still load and the
    // prophet must still talk, so a failure here is reported rather than thrown.
    try {
      rememberCube(prophetActor && playerActor ? await ensureStoryCube({
        api: editorApi, sceneId, frontendUrl: window.location.href,
        role: loadedGameplay?.role, prophetActor, playerActor, resolveAsset: resolveStoryAssetPath,
      }) : null);
    } catch (error) {
      rememberCube(null);
      actionFeedback.value = `物体未能创建：${error.message}`;
    }
    installedRules.value = Array.isArray(gameplay.worldRules?.rules) ? gameplay.worldRules.rules : [];
    // The sway needs a fresh read so it also covers the cube that was just created; a rule
    // that cannot start is reported, never fatal to loading the world.
    try {
      applyWorldRules(installedRules.value.length && inSubworld.value
        ? await editorApi.scene.getSnapshot(sceneId) : null);
    } catch (error) {
      worldRuleRunner.stop();
      actionFeedback.value = `世界规则未能启动：${error.message}`;
    }
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
  clearInterval(cooldownTimer);
  document.removeEventListener('focusin', onInventoryFocus);
  cameraReady = false;
  navigation.dispose();
  // Leaving the world must not keep the overhead view: a non-standard camera pose would
  // otherwise be the one flushed into the scene on the way out.
  exitAdjustMode({ reopenDialogue: false });
  worldRuleRunner.dispose();
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
        <symbol id="story-shard" viewBox="0 0 48 48"><path d="m24 3 14 18-14 24L10 21Z"/><path d="m24 3-4 18 4 24 5-24ZM10 21h28"/></symbol>
        <symbol id="story-heavy" viewBox="0 0 48 48"><path d="M36 5 17 24l7 7L43 12V5ZM13 22l13 13M19 29 8 40M5 37l6 6"/></symbol>
        <symbol id="story-sweep" viewBox="0 0 48 48"><path d="M7 30C-1 8 28-2 41 15M6 34C17 45 40 38 42 25M35 12l7 4 1-9M12 30l20-17-13 24Z"/></symbol>
      </defs>
    </svg>
    <div v-if="initializing" class="story-loading" role="status">{{ gameplayError || '剧情世界加载中…（Esc 取消）' }}</div>
    <div
      v-if="gameplayState && !initializing"
      class="story-hud"
      :aria-label="inSubworld ? '小世界界面' : '战斗界面'">
      <div class="world-indicator" role="status" aria-live="polite">{{ worldLabel }}</div>
      <div
        v-if="dialogueOpen || adjustMode"
        class="dialogue-overlay"
        :class="{ 'dialogue-overlay-passive': adjustMode }"
        @pointerdown.stop
        @pointermove.stop
        @wheel.stop.prevent
        @contextmenu.prevent>
        <section
          v-if="dialogueOpen"
          ref="dialoguePanel"
          class="dialogue-panel"
          role="dialog"
          aria-modal="true"
          aria-labelledby="dialogue-title"
          tabindex="-1">
          <header class="dialogue-header">
            <strong id="dialogue-title">{{ dialogue.name }}</strong>
            <span v-if="dialogue.title">{{ dialogue.title }}</span>
          </header>
          <p class="dialogue-line">{{ dialogue.lines[dialogueLine] }}</p>
          <p v-if="dialogueLine + 1 >= dialogue.lines.length && dialogue.closing" class="dialogue-closing">
            {{ dialogue.closing }}
          </p>
          <div class="prophet-actions" aria-label="先知的两个主选项">
            <template v-if="!prophetScreen">
              <button v-for="screen in PROPHET_SCREENS" :key="screen.id" class="prophet-action"
                :data-prophet-screen="screen.id" @click="openProphetScreen(screen.id)">
                <span class="prophet-action-key">{{ screen.key }}</span>
                <span class="prophet-action-label">{{ screen.label }}</span>
                <span class="prophet-action-detail">{{ screen.detail }}</span>
              </button>
            </template>

            <template v-else>
              <button v-for="(entry, index) in fragmentList" :key="entry.id" class="prophet-action"
                :data-prophet-fragment="entry.id" :disabled="!entry.owned"
                @click="runProphetAction({ type: 'toggle-fragment', fragmentId: entry.id })">
                <span class="prophet-action-key">{{ index + 1 }}</span>
                <span class="prophet-action-label">{{ entry.name }}</span>
                <span class="prophet-action-state" :data-state="entry.state">{{ entry.stateLabel }}</span>
                <span class="prophet-action-detail">{{ entry.description }} · {{ entry.details }}</span>
              </button>
            </template>

            <button v-if="prophetScreen" class="prophet-action prophet-action-back"
              data-prophet-back @click="leaveProphetScreen">
              <span class="prophet-action-label">返回</span>
            </button>
            <p v-if="actionFeedback" class="prophet-action-feedback" role="status">{{ actionFeedback }}</p>
          </div>
          <footer class="dialogue-footer">
            <span class="dialogue-progress">{{ dialogueLine + 1 }} / {{ dialogue.lines.length }}</span>
            <span class="dialogue-hint">{{ dialogue.hint }}</span>
            <button class="dialogue-next" data-dialogue-next @click="advanceDialogue">
              {{ dialogueLine + 1 >= dialogue.lines.length ? '结束交谈' : '继续' }}
            </button>
          </footer>
        </section>
        <section v-if="adjustMode" class="dialogue-panel" data-adjust-panel role="group"
          aria-label="调整物体" tabindex="-1">
          <strong>调整物体 · 俯瞰视角</strong>
          <p class="dialogue-line">{{ adjustHint }}</p>
          <div class="prophet-actions" style="flex-basis: 100%; max-height: 150px; overflow-y: auto;">
            <button v-for="entry in selectableList" :key="entry.guid" class="prophet-action"
              :data-adjust-object="entry.guid"
              :style="isSelected(entry) ? { borderColor: '#a08750', background: '#2a2f22' } : null"
              @click="selectObjectFromList(entry)">
              <span class="prophet-action-label">{{ objectLabel(entry) }}</span>
            </button>
            <button class="prophet-action" data-adjust-refresh @click="loadSelectableObjects">
              <span class="prophet-action-label">刷新列表</span>
            </button>
            <p v-if="!selectableList.length" class="prophet-action-detail">
              场景里没有读出可调整的物体，点「刷新列表」重试
            </p>
          </div>
          <div class="prophet-actions">
            <button v-for="action in ADJUST_ACTIONS" :key="action.id" class="prophet-action"
              :data-prophet-action="action.id" @click="runProphetAction({ type: 'adjust', id: action.id })">
              <span class="prophet-action-key">{{ action.key }}</span>
              <span class="prophet-action-group">{{ action.group }}</span>
              <span class="prophet-action-label">{{ action.label }}</span>
            </button>
            <button class="prophet-action prophet-action-back" data-adjust-exit @click="exitAdjustMode()">
              <span class="prophet-action-label">退出调整，回到对话</span>
            </button>
            <p v-if="actionFeedback" class="prophet-action-feedback" role="status">{{ actionFeedback }}</p>
          </div>
        </section>
      </div>
      <section v-if="bossNearby && !inventoryOpen && showCombatHud" class="boss-status" aria-label="Boss 血条">
        <div class="boss-name">巨龙</div>
        <div
          class="boss-bar"
          role="progressbar"
          aria-label="Boss 生命"
          :aria-valuenow="gameplayState.state.boss.hp"
          :aria-valuemax="gameplayState.config.bossHp"
          aria-valuemin="0">
          <span :style="{ width: `${100 * gameplayState.state.boss.hp / gameplayState.config.bossHp}%` }" />
        </div>
        <div class="boss-number">{{ gameplayState.state.boss.hp }} / {{ gameplayState.config.bossHp }}</div>
      </section>
      <aside v-if="!inventoryOpen" class="story-controls" aria-label="操作提示">
        <span v-for="[key, text] in controlHints" :key="key"
          :class="{ 'pickup-control': key === 'F' }" :role="key === 'F' ? 'status' : undefined">
          <kbd>{{ key }}</kbd> {{ text }}
        </span>
      </aside>
      <div v-if="!inventoryOpen" class="story-bottom-stack">
        <div v-if="gameplayError" class="gameplay-error" role="alert" @pointerdown.stop @wheel.stop @pointermove.stop="camera.resetInput">
          <span>{{ gameplayError }}</span><button :disabled="actionBusy" @click.stop="runAction(saveRegistration.flush)">重试保存</button>
        </div>
        <div v-if="feedback" class="story-feedback" role="status" aria-live="polite">{{ feedback }}</div>
        <section
          v-if="showCombatHud"
          class="player-vitals"
          aria-label="人物生命与怒气">
          <div class="vitals-content">
            <div class="vital-label"><span>生命</span><span>{{ gameplayState.config.playerHp }} <em>/ {{ gameplayState.config.playerHp }}</em></span></div>
            <div class="vital-bar health" role="progressbar" aria-label="生命" :aria-valuenow="gameplayState.config.playerHp" :aria-valuemax="gameplayState.config.playerHp" aria-valuemin="0"><span /></div>
            <div class="vital-label"><span>怒气</span><span>{{ gameplayState.state.rage }} <em>/ {{ gameplayState.config.rageMax }}</em></span></div>
            <div class="vital-bar rage" role="progressbar" aria-label="怒气" :aria-valuenow="gameplayState.state.rage" :aria-valuemax="gameplayState.config.rageMax" aria-valuemin="0">
              <span :style="{ width: `${100 * gameplayState.state.rage / gameplayState.config.rageMax}%` }" />
            </div>
          </div>
          <div class="skill-strip" aria-label="怒气技能" @pointerdown.stop @pointermove.stop="camera.resetInput">
            <div v-for="(skill, id) in gameplayState.config.skills" :key="id" class="skill-tile hint-anchor" tabindex="0"
              :class="{ unavailable: !skillStatus(id).available }" :aria-label="`${skill.key} ${skill.name}，消耗 ${skill.rageCost} 怒气`">
              <svg aria-hidden="true"><use :href="`#story-${id}`" /></svg><kbd>{{ skill.key }}</kbd>
              <span v-if="skillStatus(id).remainingMs > 0" class="skill-cooldown">{{ (skillStatus(id).remainingMs / 1000).toFixed(1) }}</span>
              <span class="ui-tooltip" role="tooltip">{{ skill.name }} · {{ skill.rageCost }} 怒气</span>
            </div>
          </div>
        </section>
        <section v-else class="subworld-notice" aria-label="小世界状态">
          <strong>{{ worldLabel }}</strong>
          <span>此处为沙盒空间 · 无战斗目标</span>
        </section>
      </div>
    </div>
    <div v-if="inventoryOpen && gameplayState" class="inventory-overlay" @pointerdown.stop @pointermove.stop @wheel.stop.prevent @contextmenu.prevent>
      <section ref="inventoryPanel" class="inventory-panel" role="dialog" aria-modal="true" aria-labelledby="inventory-title" @keydown="onInventoryKey">
        <header class="inventory-header">
          <h1 id="inventory-title">{{ inSubworld ? '背包 · 小世界' : '背包' }}</h1>
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
        <section class="roster-section" aria-label="小世界名册">
          <header class="roster-header">
            <h2>小世界名册</h2>
            <span class="roster-total">共 {{ subworlds.length }} 座</span>
          </header>
          <p v-if="!subworlds.length" class="roster-empty">
            还没有小世界。击败巨龙夺取世界碎片后，将开启你的第一座小世界。
          </p>
          <ul v-else class="roster-list">
            <li v-for="entry in subworlds" :key="entry.id" class="roster-row">
              <div class="roster-info">
                <strong>{{ entry.name }}</strong>
                <span class="roster-id">{{ entry.id }}</span>
              </div>
              <div class="roster-actions">
                <button class="roster-action" :disabled="!canEnterRoster || navigationPending"
                  :aria-label="`进入 ${entry.name}`" @click="enterSubworldEntry(entry)">进入</button>
                <button class="roster-action rename" :disabled="navigationPending || actionBusy"
                  :aria-label="`给 ${entry.name} 改名`" @click="renameSubworldEntry(entry)">改名</button>
              </div>
            </li>
          </ul>
          <button v-if="canAddSubworld" class="roster-add" :disabled="navigationPending || actionBusy"
            @click="addSubworldEntry">记录一座新小世界</button>
        </section>
        <section v-if="inSubworld" class="exhibit-section" aria-label="小世界陈列">
          <header class="exhibit-header">
            <h2>陈列主世界的物质</h2>
            <span class="exhibit-total">{{ placements.length }} 件在展</span>
          </header>
          <p v-if="!exhibitItems.some(item => item.owned)" class="exhibit-empty">
            还没有从小世界之外带回物质。击败巨龙取得世界碎片后即可陈列。
          </p>
          <ul v-else class="exhibit-list">
            <li v-for="item in exhibitItems" :key="item.id" :class="{ empty: !item.owned }">
              <div class="exhibit-info">
                <strong>{{ item.name }}</strong>
                <span class="exhibit-count">持有 {{ item.owned }} · 已陈列 {{ item.placed }}</span>
                <span class="exhibit-description">{{ item.description }}</span>
              </div>
              <button
                class="exhibit-action"
                :disabled="!item.available || actionBusy || navigationPending"
                :aria-label="item.available ? `将${item.name}陈列到小世界` : `${item.name}已全部陈列`"
                @click="placeExhibit(item.id)">
                {{ item.available ? '陈列' : '已全部陈列' }}
              </button>
            </li>
          </ul>
          <p v-if="placements.length" class="exhibit-hint">
            陈列点按顺序循环：{{ siteNames }}
          </p>
          <button v-if="placements.length" class="exhibit-withdraw" :disabled="actionBusy || navigationPending" @click="withdrawExhibit">
            撤回最后一件
          </button>
        </section>
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
      </section>
    </div>
  </div>
</template>

<style scoped>
.story-loading { position: absolute; inset: 0; display: grid; place-items: center; background: #10151de8; }
.story-world-viewport { position: fixed; inset: 0; overflow: hidden; outline: none; background: transparent; color: #eee3ca; font-family: 'Microsoft YaHei', sans-serif; }
.hud-symbols { position: absolute; width: 0; height: 0; overflow: hidden; }
.story-hud { position: absolute; inset: 0; pointer-events: none; }
.boss-status { position: absolute; top: 26px; left: 50%; transform: translateX(-50%); width: min(440px, 45vw); text-align: center; text-shadow: 0 2px 4px #000; }
.boss-name { font-size: 17px; letter-spacing: 5px; margin-bottom: 9px; }
.boss-number { margin-top: 5px; font-size: 12px; font-variant-numeric: tabular-nums; }
.boss-bar, .vital-bar { padding: 2px; border: 1px solid #9b8049; background: #14120fe8; height: 11px; }
.boss-bar span, .vital-bar span { display: block; width: 100%; height: 100%; background: linear-gradient(90deg, #661e26, #be4b45); transition: width 150ms ease-out; }
.story-controls { position: absolute; left: 24px; top: 50%; transform: translateY(-50%); display: grid; gap: 9px; padding: 14px 16px; border-left: 1px solid #806b3d; background: linear-gradient(90deg, #13130dd4, #13130d00); font-size: 12px; text-shadow: 0 1px 3px #000; }
.story-controls span { display: flex; align-items: center; gap: 10px; }
kbd { font-family: inherit; font-size: 11px; color: #dfcb94; }
.story-controls kbd { min-width: 42px; padding: 2px 3px; border: 1px solid #706441; text-align: center; background: #171813b8; }
.pickup-control { color: #ffdf8d; }
.pickup-control kbd { border-color: #d9b45c; }
.world-indicator { position: absolute; top: 26px; right: 24px; padding: 6px 12px; border: 1px solid #806b3d; background: #13130dd4; font-size: 12px; letter-spacing: 2px; text-shadow: 0 1px 3px #000; }
.story-bottom-stack { position: absolute; bottom: 22px; left: 50%; transform: translateX(-50%); width: min(490px, calc(100vw - 32px)); display: grid; gap: 9px; }
.subworld-notice { display: grid; gap: 5px; justify-items: center; padding: 14px 16px; border: 1px solid #8f7846; background: linear-gradient(120deg, #151711f5, #090c09ec); box-shadow: 0 5px 24px #0007; text-align: center; }
.subworld-notice strong { font-size: 14px; font-weight: 500; letter-spacing: 3px; color: #f0d69a; }
.subworld-notice span { font-size: 11px; color: #a89d85; }
.dialogue-overlay { pointer-events: auto; position: absolute; inset: 0; display: grid; align-items: end; justify-items: center; padding: 0 20px 92px; background: #05080766; }
/* While adjusting an object the overlay must not cover the viewport: the click has to
   reach the surface to pick something. Only the panel itself stays interactive. */
.dialogue-overlay-passive { pointer-events: none; background: none; padding: 0; }
.dialogue-overlay-passive > * { pointer-events: auto; }
.dialogue-panel { width: min(560px, 100%); box-sizing: border-box; padding: 18px 22px; border: 1px solid #a08750; background: linear-gradient(145deg, #20231cec, #0f1411fa); box-shadow: 0 18px 60px #0009; outline: none; }
.dialogue-header { display: flex; align-items: baseline; gap: 12px; padding-bottom: 12px; border-bottom: 1px solid #4e513d; }
.dialogue-header strong { font-size: 15px; font-weight: 500; letter-spacing: 3px; color: #f0d69a; }
.dialogue-header span { font-size: 11px; color: #a89d85; }
.dialogue-line { margin: 14px 0 0; font-size: 14px; line-height: 1.9; color: #eee3ca; min-height: 2.6em; }
.dialogue-closing { margin: 8px 0 0; font-size: 12px; color: #b39252; }
.dialogue-footer { display: flex; align-items: center; gap: 12px; padding-top: 16px; }
.dialogue-progress { font-size: 11px; color: #8e8a78; font-variant-numeric: tabular-nums; }
.dialogue-hint { flex: 1; font-size: 11px; color: #8e8a78; }
.prophet-actions { display: flex; flex-wrap: wrap; gap: 6px; margin: 12px 0 0; }
.prophet-action { display: inline-flex; align-items: center; gap: 6px; padding: 5px 9px; cursor: pointer;
  border: 1px solid #6d6350; background: #171a15; color: #ded4bb; font-size: 12px; }
.prophet-action:hover:not(:disabled) { border-color: #a08750; background: #22261d; }
.prophet-action:disabled { cursor: not-allowed; opacity: 0.45; }
.prophet-action-key { min-width: 14px; padding: 0 3px; border: 1px solid #6d6350; text-align: center;
  font-size: 11px; color: #cbbd97; }
.prophet-action-group { color: #8e8a78; font-size: 11px; }
.prophet-action-feedback { flex-basis: 100%; margin: 4px 0 0; font-size: 11px; color: #d8c98f; }
.prophet-action-detail { flex-basis: 100%; font-size: 11px; color: #8e8a78; }
.prophet-action-state { padding: 0 4px; border: 1px solid #6d6350; font-size: 11px; color: #cbbd97; }
.prophet-action-state[data-state="installed"] { border-color: #7f9a5c; color: #cbe0a6; }
.prophet-action-state[data-state="locked"] { opacity: 0.5; }
.prophet-action-back { margin-left: auto; }
.dialogue-next { flex-shrink: 0; padding: 7px 16px; border: 1px solid #bfa167; color: #f0d69a; background: #343820; font-size: 12px; cursor: pointer; }
.player-vitals { display: flex; align-items: center; gap: 22px; padding: 12px 16px; border: 1px solid #8f7846; background: linear-gradient(120deg, #151711f5, #090c09ec); box-shadow: 0 5px 24px #0007; }
.vitals-content { flex: 1; min-width: 0; }
.vital-label { display: flex; justify-content: space-between; margin: 0 0 5px; font-size: 12px; font-variant-numeric: tabular-nums; }
.vital-label em { font-style: normal; color: #a89d85; }
.vital-bar { box-sizing: border-box; height: 9px; padding: 1px; }
.vital-bar.health { margin-bottom: 10px; }
.vital-bar.rage span { background: linear-gradient(90deg, #aa501e, #f2b750); }
.skill-strip { display: flex; gap: 10px; pointer-events: auto; }
.skill-tile { position: relative; width: 46px; height: 49px; border: 1px solid #b99a55; background: #27251b; }
.skill-tile > svg { width: 33px; height: 33px; margin: 4px 6px; fill: none; stroke: #f0d08d; stroke-width: 2; }
.skill-tile > kbd { position: absolute; bottom: -5px; left: -4px; min-width: 15px; padding: 0 3px; border: 1px solid #8e7950; background: #141510; text-align: center; }
.skill-tile.unavailable > svg { opacity: .35; filter: grayscale(1); }
.skill-tile.unavailable { border-color: #68624e; background: #171914; }
.skill-cooldown { position: absolute; inset: 0; display: grid; place-items: center; background: #111b; color: #f8e6b2; font-size: 17px; font-variant-numeric: tabular-nums; }
.story-feedback { justify-self: center; padding: 6px 12px; background: #10150be6; color: #e6cc8e; font-size: 13px; }
.gameplay-error { pointer-events: auto; display: flex; align-items: center; gap: 10px; padding: 10px; color: #ffcac0; background: #341713f2; font-size: 13px; overflow-wrap: anywhere; }
.gameplay-error button { flex-shrink: 0; padding: 5px 8px; border: 1px solid #b78b66; }
.inventory-overlay { pointer-events: auto; position: absolute; inset: 0; display: grid; place-items: center; padding: 20px; background: #050807b5; }
.inventory-panel { pointer-events: auto; width: min(454px, 100%); box-sizing: border-box; max-height: calc(100vh - 40px); overflow-y: auto; padding: 22px 24px; background: linear-gradient(145deg, #20231cec, #0f1411fa); border: 1px solid #a08750; box-shadow: 0 18px 80px #0009; }
.inventory-header { display: flex; justify-content: space-between; align-items: center; padding-bottom: 20px; }
.inventory-header h1 { margin: 0; font-size: 20px; font-weight: 500; letter-spacing: 3px; }
.close-inventory { width: 30px; height: 30px; font-size: 24px; color: #cabb9b; background: transparent; border: 1px solid #746744; cursor: pointer; line-height: 1; }
.inventory-grid { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 8px; }
.inventory-slot { position: relative; aspect-ratio: 1; border: 1px solid #4e513d; background: #0b110f99; color: #e4d5b2; cursor: pointer; min-width: 0; }
.inventory-slot.occupied { border-color: #b39252; background: radial-gradient(ellipse, #51432680, #111812); }
.fragment-icon { width: 60%; height: 60%; margin: auto; fill: #7d976653; stroke: #d7ddb0; stroke-width: 1.5; }
.slot-count { position: absolute; bottom: 4px; right: 6px; font-size: 12px; }
.inventory-footer { display: flex; justify-content: center; padding-top: 22px; }
.exhibit-section { margin-top: 22px; padding-top: 18px; border-top: 1px solid #4e513d; }
.roster-section { margin-top: 22px; padding-top: 18px; border-top: 1px solid #4e513d; }
.roster-header { display: flex; align-items: baseline; justify-content: space-between; margin-bottom: 10px; }
.roster-header h2 { margin: 0; font-size: 14px; font-weight: 500; letter-spacing: 2px; color: #dfcb94; }
.roster-total { font-size: 11px; color: #a89d85; }
.roster-empty { margin: 0; font-size: 12px; color: #a89d85; line-height: 1.8; }
.roster-list { display: grid; gap: 8px; margin: 0; padding: 0; list-style: none; }
.roster-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 10px 12px; border: 1px solid #4e513d; background: linear-gradient(120deg, #151711f5, #090c09ec); }
.roster-info { display: grid; gap: 2px; min-width: 0; }
.roster-info strong { font-size: 13px; font-weight: 500; color: #f0d69a; }
.roster-id { font-size: 10px; color: #8e8a78; }
.roster-actions { display: flex; gap: 8px; flex-shrink: 0; }
.roster-action { padding: 5px 12px; border: 1px solid #bfa167; color: #f0d69a; background: #343820; font-size: 12px; cursor: pointer; }
.roster-action.rename { border-color: #706441; background: #171813; }
.roster-action:disabled { opacity: .45; cursor: default; }
.roster-add { margin-top: 10px; padding: 7px 14px; width: 100%; border: 1px dashed #bfa167; color: #dfcb94; background: #23261c; font-size: 12px; cursor: pointer; }
.roster-add:disabled { opacity: .45; cursor: default; }
.exhibit-header { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; }
.exhibit-header h2 { margin: 0; font-size: 14px; font-weight: 500; letter-spacing: 2px; }
.exhibit-total { font-size: 11px; color: #a89d85; font-variant-numeric: tabular-nums; }
.exhibit-empty, .exhibit-hint { margin: 10px 0 0; font-size: 11px; color: #a89d85; line-height: 1.6; }
.exhibit-list { list-style: none; margin: 12px 0 0; padding: 0; display: grid; gap: 10px; }
.exhibit-list li { display: flex; align-items: center; gap: 12px; padding: 10px 12px; border: 1px solid #4e513d; background: #0b110f99; }
.exhibit-list li.empty { opacity: .5; }
.exhibit-info { flex: 1; min-width: 0; display: grid; gap: 3px; }
.exhibit-info strong { font-size: 13px; font-weight: 500; color: #e4d5b2; }
.exhibit-count { font-size: 11px; color: #b39252; font-variant-numeric: tabular-nums; }
.exhibit-description { font-size: 11px; color: #8e8a78; line-height: 1.5; }
.exhibit-action, .exhibit-withdraw { flex-shrink: 0; padding: 6px 12px; border: 1px solid #bfa167; color: #f0d69a; background: #343820; font-size: 12px; cursor: pointer; }
.exhibit-withdraw { margin-top: 10px; border-color: #746744; color: #cabb9b; background: transparent; }
.world-travel-button { min-width: 158px; height: 38px; border: 1px solid #bfa167; color: #f0d69a; background: #343820; font-size: 14px; cursor: pointer; }
button:disabled { color: #929384; border-color: #4e5148; background: #232820; cursor: not-allowed; }
button:focus-visible, .hint-anchor:focus-visible { outline: 2px solid #f3cf79; outline-offset: 3px; }
.hint-anchor { position: relative; }
.ui-tooltip { display: none; position: absolute; bottom: calc(100% + 9px); left: 50%; transform: translateX(-50%); padding: 7px 10px; border: 1px solid #8f7846; background: #0c120ff5; color: #efe0bc; font-size: 12px; font-weight: 400; white-space: nowrap; z-index: 3; pointer-events: none; }
.hint-anchor:hover > .ui-tooltip, .hint-anchor:focus > .ui-tooltip, .hint-anchor:focus-within > .ui-tooltip { display: block; }
@media (max-height: 760px) { .story-bottom-stack { bottom: 14px; } .story-controls { gap: 7px; } .boss-status { top: 18px; } .world-indicator { top: 18px; } }
@media (max-width: 800px) { .story-controls { left: 12px; padding: 10px; font-size: 11px; } .player-vitals { gap: 14px; padding: 10px 12px; } .world-indicator { right: 12px; font-size: 11px; letter-spacing: 1px; } }
@media (prefers-reduced-motion: reduce) { .vital-bar span, .boss-bar span { transition: none; } }
</style>
