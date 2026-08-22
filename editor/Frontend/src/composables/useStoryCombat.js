import { computed, onMounted, onUnmounted, ref, unref, watch } from 'vue';

import { editorApi } from '@/api/editorApi.js';
import {
  createStoryMonsterActorData,
  STORY_MONSTER_DEFINITIONS,
  STORY_MONSTER_PREFIX,
  STORY_PLAYER_ATTACK_COOLDOWN_MS,
  STORY_PLAYER_ATTACK_DAMAGE,
  STORY_PLAYER_ATTACK_RANGE,
  STORY_PLAYER_DAMAGE_INVULNERABILITY_MS,
  STORY_PLAYER_MAX_HEALTH,
  STORY_PLAYER_RESPAWN_PROTECTION_MS,
} from '@/config/storyCombat.js';
import {
  editorCallErrorMessage,
  editorCallSucceeded,
  resolveStoryWorldAssetRoot,
} from '@/services/storyWorldBootstrapService.js';
import {
  applyStoryDamage,
  canStoryAttack,
  canStoryReceiveDamage,
  moveStoryPointTowards,
  normalizeStoryCombatProgress,
  shouldRespawnStoryBoss,
  storyCombatStorageKey,
  storyDistance3,
  storyHorizontalDistance,
  storyMonsterAiState,
  storyWanderPoint,
  storyYawTowards,
} from '@/utils/storyCombat.js';
import { storyDayId } from '@/utils/storyGameClock.js';
import { resolveSceneSnapshot } from '@/utils/nativeSceneViewport.js';

const FIXED_STEP_SECONDS = 1 / 20;
const MAX_FRAME_SECONDS = 0.2;
const AIM_REFRESH_MS = 250;
const PICK_TIMEOUT_MS = 1500;
const SNAPSHOT_RETRY_COUNT = 12;
const SNAPSHOT_RETRY_MS = 160;

function browserStorage() {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

function actorGuid(actor) {
  return String(actor?.actor_guid || actor?.guid || '')
    .trim()
    .toLowerCase();
}

function actorName(actor) {
  return String(actor?.name || actor?.actor_name || '')
    .trim()
    .toLowerCase();
}

function payloadValue(payload) {
  if (payload?.data?.data && typeof payload.data.data === 'object') return payload.data.data;
  if (payload?.data && typeof payload.data === 'object') return payload.data;
  return payload || {};
}

function delay(ms) {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

export function useStoryCombat({
  sceneId,
  projectKey,
  enabled,
  paused,
  totalGameTimeMs,
  playerState,
  cameraBinding,
  viewportRef,
  onActorsReady,
} = {}) {
  const playerHealth = ref(STORY_PLAYER_MAX_HEALTH);
  const playerDead = ref(false);
  const monstersReady = ref(false);
  const warningMessage = ref('');
  const notice = ref(null);
  const aimedMonster = ref(null);
  const attackPulse = ref(0);
  const hitPulse = ref(0);
  const damageFlash = ref(0);
  const damageNumber = ref(null);
  const monsterRevision = ref(0);

  const runtimes = new Map();
  const pendingPicks = new Map();
  let disposed = false;
  let initializedSceneId = '';
  let initializationPromise = null;
  let initializationGeneration = 0;
  let pickCallbackToken = null;
  let frameId = null;
  let aimTimer = null;
  let previousFrameAt = 0;
  let accumulatorSeconds = 0;
  let simulationTimeMs = 0;
  let pickSequence = 0;
  let lastAttackAt = Number.NEGATIVE_INFINITY;
  let lastPlayerDamageAt = Number.NEGATIVE_INFINITY;
  let playerProtectedUntil = 0;
  let combatStorageKey = '';
  let bossDefeatedAtGameTimeMs = null;
  let bossEngagedUntil = 0;
  let noticeTimer = null;
  let damageTimer = null;

  const isActive = () => !disposed && Boolean(unref(enabled));
  const isPaused = () => Boolean(unref(paused)) || playerDead.value;
  const bridge = () => window.coronaBridge;

  const publishMonsters = () => {
    monsterRevision.value += 1;
  };

  const showNotice = (message, kind = 'info', duration = 2600) => {
    if (noticeTimer !== null) window.clearTimeout(noticeTimer);
    notice.value = message
      ? { id: Date.now() + monsterRevision.value, message: String(message), kind }
      : null;
    if (!notice.value) return;
    noticeTimer = window.setTimeout(() => {
      noticeTimer = null;
      notice.value = null;
    }, duration);
  };

  const showDamage = (amount) => {
    if (damageTimer !== null) window.clearTimeout(damageTimer);
    damageNumber.value = { id: Date.now(), amount };
    damageFlash.value += 1;
    damageTimer = window.setTimeout(() => {
      damageTimer = null;
      damageNumber.value = null;
    }, 720);
  };

  const persistBossProgress = (storage = browserStorage()) => {
    if (!combatStorageKey || !storage) return false;
    try {
      storage.setItem(
        combatStorageKey,
        JSON.stringify({
          version: 1,
          bossDefeatedAtGameTimeMs,
          updatedAt: Date.now(),
        })
      );
      return true;
    } catch (error) {
      console.warn('[StoryMode] failed to persist combat progress', error);
      return false;
    }
  };

  const loadBossProgress = (key, storage = browserStorage()) => {
    combatStorageKey = key ? storyCombatStorageKey(key) : '';
    let document = null;
    if (combatStorageKey && storage) {
      try {
        const serialized = storage.getItem(combatStorageKey);
        if (serialized) document = JSON.parse(serialized);
      } catch (error) {
        console.warn('[StoryMode] failed to load combat progress', error);
      }
    }
    bossDefeatedAtGameTimeMs = normalizeStoryCombatProgress(document).bossDefeatedAtGameTimeMs;
  };

  const setRuntimeVisible = (runtime, visible) => {
    const next = Boolean(visible);
    if (!runtime?.handle || runtime.visible === next) return;
    runtime.visible = next;
    const nativeBridge = bridge();
    if (!nativeBridge || typeof nativeBridge.setProperty !== 'function') return;
    try {
      nativeBridge.setProperty(runtime.handle, 3, next);
    } catch (error) {
      console.warn('[StoryMode] failed to change monster visibility', error);
    }
  };

  const publishRuntimeTransform = (runtime, { position = true, rotation = true } = {}) => {
    if (!runtime?.handle) return;
    const nativeBridge = bridge();
    if (!nativeBridge || typeof nativeBridge.actorTransform !== 'function') return;
    try {
      if (position) nativeBridge.actorTransform(runtime.handle, 0, [...runtime.position]);
      if (rotation) nativeBridge.actorTransform(runtime.handle, 1, [0, runtime.rotationY, 0]);
    } catch (error) {
      console.warn('[StoryMode] failed to move monster', error);
    }
  };

  const resetRuntimeToSpawn = (runtime, { visible = true } = {}) => {
    runtime.position = [...runtime.definition.position];
    runtime.rotationY = 0;
    runtime.wanderTarget = null;
    runtime.state = 'idle';
    publishRuntimeTransform(runtime);
    setRuntimeVisible(runtime, visible);
  };

  const restoreAllRuntimeActors = () => {
    for (const runtime of runtimes.values()) resetRuntimeToSpawn(runtime, { visible: true });
    publishMonsters();
  };

  const snapshotActors = async (activeSceneId) => {
    const snapshot = resolveSceneSnapshot(await editorApi.scene.getSnapshot(activeSceneId));
    return Array.isArray(snapshot?.actors) ? snapshot.actors : [];
  };

  const resolveMonsterActors = async (activeSceneId) => {
    for (let attempt = 0; attempt < SNAPSHOT_RETRY_COUNT; attempt += 1) {
      const actors = await snapshotActors(activeSceneId);
      const resolved = STORY_MONSTER_DEFINITIONS.map((definition) => {
        const guid = definition.guid.toLowerCase();
        const name = definition.name.toLowerCase();
        return (
          actors.find((actor) => actorGuid(actor) === guid || actorName(actor) === name) || null
        );
      });
      if (resolved.every((actor) => Number(actor?.handle || 0) > 0)) return resolved;
      if (attempt + 1 < SNAPSHOT_RETRY_COUNT) await delay(SNAPSHOT_RETRY_MS);
    }
    return [];
  };

  const buildRuntime = (definition, actor) => ({
    definition,
    id: definition.id,
    name: definition.name,
    displayName: definition.displayName,
    kind: definition.kind,
    handle: Number(actor?.handle || 0),
    position: [...definition.position],
    rotationY: 0,
    health: definition.maxHealth,
    alive: true,
    visible: true,
    state: 'idle',
    lastAttackAt: Number.NEGATIVE_INFINITY,
    lastSpawnDayId: null,
    deadDayId: null,
    wanderTarget: null,
    wanderSeed:
      definition.kind === 'boss' ? 99991 : 1709 + Number(definition.id.split('-')[1] || 0),
  });

  const runtimeContextMatches = (generation, activeSceneId, activeProjectKey) =>
    !disposed &&
    generation === initializationGeneration &&
    String(unref(sceneId) || '').trim() === activeSceneId &&
    String(unref(projectKey) || '').trim() === activeProjectKey;

  const invalidateRuntimeContext = ({ persistProgress = true } = {}) => {
    if (persistProgress) persistBossProgress();
    initializationGeneration += 1;
    initializationPromise = null;
    pendingPicks.clear();
    aimedMonster.value = null;
    restoreAllRuntimeActors();
    runtimes.clear();
    initializedSceneId = '';
    monstersReady.value = false;
    publishMonsters();
  };

  const initialize = async () => {
    const activeSceneId = String(unref(sceneId) || '').trim();
    const activeProjectKey = String(unref(projectKey) || '').trim();
    if (!activeSceneId || !activeProjectKey || !isActive()) return false;
    if (initializedSceneId === activeSceneId && monstersReady.value) return true;
    if (initializationPromise) return initializationPromise;
    const generation = initializationGeneration;

    const operation = (async () => {
      warningMessage.value = '';
      monstersReady.value = false;
      loadBossProgress(activeProjectKey);
      const assetRoot = resolveStoryWorldAssetRoot({
        frontendLocation: window.location?.href || '',
        activeProjectPath: activeProjectKey,
      });
      if (!assetRoot) throw new Error('无法定位剧情怪物资源目录。');

      const creationWarnings = [];
      for (const definition of STORY_MONSTER_DEFINITIONS) {
        try {
          const response = await editorApi.sceneTools.createActor(
            activeSceneId,
            `${assetRoot}/${definition.asset}`,
            definition.actorType,
            createStoryMonsterActorData(definition)
          );
          if (!runtimeContextMatches(generation, activeSceneId, activeProjectKey)) return false;
          if (!editorCallSucceeded(response)) {
            creationWarnings.push(
              editorCallErrorMessage(response, `${definition.displayName} 创建失败`)
            );
          }
        } catch (error) {
          creationWarnings.push(
            `${definition.displayName} 创建失败：${error?.message || '未知错误'}`
          );
        }
      }

      const actors = await resolveMonsterActors(activeSceneId);
      if (!runtimeContextMatches(generation, activeSceneId, activeProjectKey)) return false;
      runtimes.clear();
      for (let index = 0; index < STORY_MONSTER_DEFINITIONS.length; index += 1) {
        const definition = STORY_MONSTER_DEFINITIONS[index];
        const actor = actors[index];
        if (!actor || Number(actor.handle || 0) <= 0) {
          creationWarnings.push(`${definition.displayName} 尚未加载完成。`);
          continue;
        }
        runtimes.set(definition.id, buildRuntime(definition, actor));
      }

      const boss = runtimes.get('boss');
      if (boss && bossDefeatedAtGameTimeMs !== null) {
        if (shouldRespawnStoryBoss(bossDefeatedAtGameTimeMs, unref(totalGameTimeMs))) {
          bossDefeatedAtGameTimeMs = null;
          persistBossProgress();
        } else {
          boss.health = 0;
          boss.alive = false;
          setRuntimeVisible(boss, false);
        }
      }

      warningMessage.value = creationWarnings[0] || '';
      initializedSceneId = activeSceneId;
      monstersReady.value = runtimes.size > 0;
      publishMonsters();
      if (typeof onActorsReady === 'function') await onActorsReady();
      return monstersReady.value;
    })().catch((error) => {
      if (runtimeContextMatches(generation, activeSceneId, activeProjectKey)) {
        console.warn('[StoryMode] failed to initialize monsters', error);
        warningMessage.value = String(error?.message || '怪物系统初始化失败。');
      }
      return false;
    });
    initializationPromise = operation;
    operation.finally(() => {
      if (initializationPromise === operation) initializationPromise = null;
    });
    return operation;
  };

  const damagePlayer = (amount, sourceName) => {
    const now = Date.now();
    if (
      playerDead.value ||
      isPaused() ||
      !canStoryReceiveDamage(
        lastPlayerDamageAt,
        playerProtectedUntil,
        now,
        STORY_PLAYER_DAMAGE_INVULNERABILITY_MS
      )
    ) {
      return false;
    }
    const result = applyStoryDamage(playerHealth.value, amount, STORY_PLAYER_MAX_HEALTH);
    if (result.damage <= 0) return false;
    lastPlayerDamageAt = now;
    playerHealth.value = result.health;
    showDamage(result.damage);
    showNotice(`${sourceName} 对你造成了 ${result.damage} 点伤害。`, 'danger', 1500);
    if (result.dead) {
      playerDead.value = true;
      aimedMonster.value = null;
      showNotice('你倒下了。', 'danger', 3200);
    }
    return true;
  };

  const killMonster = (runtime) => {
    runtime.health = 0;
    runtime.alive = false;
    runtime.state = 'dead';
    setRuntimeVisible(runtime, false);
    if (runtime.kind === 'minion') {
      runtime.deadDayId = storyDayId(unref(totalGameTimeMs));
      showNotice(`击败了 ${runtime.displayName}。`, 'success');
    } else {
      bossDefeatedAtGameTimeMs = Math.max(Number(unref(totalGameTimeMs)) || 0, 0);
      persistBossProgress();
      showNotice('山魈王已被击败，两个游戏日后它会再次出现。', 'success', 4200);
    }
  };

  const damageMonster = (runtime, amount) => {
    if (!runtime?.alive || !runtime.visible) return false;
    const result = applyStoryDamage(runtime.health, amount, runtime.definition.maxHealth);
    if (result.damage <= 0) return false;
    runtime.health = result.health;
    hitPulse.value += 1;
    if (runtime.kind === 'boss') bossEngagedUntil = Date.now() + 7000;
    if (result.dead) killMonster(runtime);
    else showNotice(`${runtime.displayName} 受到 ${result.damage} 点伤害。`, 'success', 1100);
    publishMonsters();
    return true;
  };

  const requestCenterPick = (purpose) => {
    if (!isActive() || isPaused()) return false;
    const now = Date.now();
    for (const [requestId, pending] of pendingPicks) {
      if (now - pending.requestedAt > PICK_TIMEOUT_MS) pendingPicks.delete(requestId);
    }
    if ([...pendingPicks.values()].some((pending) => pending.purpose === purpose)) return false;
    const binding = unref(cameraBinding);
    const viewport = unref(viewportRef);
    const nativeBridge = bridge();
    const cameraHandle = Number(binding?.cameraHandle || 0);
    const activeSceneId = String(binding?.sceneId || unref(sceneId) || '').trim();
    if (
      !nativeBridge ||
      typeof nativeBridge.pickActor !== 'function' ||
      !viewport ||
      cameraHandle <= 0 ||
      !activeSceneId
    ) {
      return false;
    }
    const rect = viewport.getBoundingClientRect?.();
    const width = Number(rect?.width || viewport.clientWidth || 0);
    const height = Number(rect?.height || viewport.clientHeight || 0);
    if (width <= 0 || height <= 0) return false;
    const requestId = `story-combat-${purpose}-${Date.now()}-${++pickSequence}`;
    pendingPicks.set(requestId, {
      purpose,
      requestedAt: now,
      generation: initializationGeneration,
      sceneId: activeSceneId,
      projectKey: String(unref(projectKey) || '').trim(),
    });
    try {
      nativeBridge.pickActor(
        cameraHandle,
        activeSceneId,
        requestId,
        width * 0.5,
        height * 0.5,
        width,
        height
      );
      return requestId;
    } catch (error) {
      pendingPicks.delete(requestId);
      console.warn('[StoryMode] failed to request combat pick', error);
      return false;
    }
  };

  const attack = () => {
    const now = Date.now();
    if (
      !isActive() ||
      isPaused() ||
      !canStoryAttack(lastAttackAt, now, STORY_PLAYER_ATTACK_COOLDOWN_MS)
    ) {
      return false;
    }
    lastAttackAt = now;
    attackPulse.value += 1;
    const request = requestCenterPick('attack');
    if (!request) showNotice('当前无法发动攻击。', 'warning', 1100);
    return Boolean(request);
  };

  const handlePickResult = (rawPayload) => {
    const payload = payloadValue(rawPayload);
    const requestId = String(payload?.requestId || '');
    const pending = pendingPicks.get(requestId);
    if (!pending) return;
    pendingPicks.delete(requestId);
    if (Date.now() - pending.requestedAt > PICK_TIMEOUT_MS) return;
    if (
      pending.generation !== initializationGeneration ||
      pending.sceneId !== String(unref(sceneId) || '').trim() ||
      pending.projectKey !== String(unref(projectKey) || '').trim() ||
      !isActive() ||
      isPaused()
    ) {
      return;
    }
    const handle = Number(payload?.actorHandle || 0);
    const runtime = [...runtimes.values()].find((candidate) => candidate.handle === handle) || null;
    if (pending.purpose === 'aim') {
      aimedMonster.value = runtime?.alive && runtime.visible ? runtime : null;
      return;
    }
    if (pending.purpose !== 'attack') return;
    if (!runtime || !runtime.alive || !runtime.visible) {
      aimedMonster.value = null;
      return;
    }
    aimedMonster.value = runtime;
    const distance = storyDistance3(unref(playerState)?.position, runtime.position);
    if (distance > STORY_PLAYER_ATTACK_RANGE) {
      showNotice(`${runtime.displayName} 距离过远。`, 'warning', 1000);
      return;
    }
    damageMonster(runtime, STORY_PLAYER_ATTACK_DAMAGE);
  };

  const respawnPlayer = () => {
    playerHealth.value = STORY_PLAYER_MAX_HEALTH;
    playerDead.value = false;
    lastPlayerDamageAt = Number.NEGATIVE_INFINITY;
    playerProtectedUntil = Date.now() + STORY_PLAYER_RESPAWN_PROTECTION_MS;
    showNotice('你已在云溪村村口醒来，获得 3 秒保护。', 'success', 3000);
    return true;
  };

  const updateMonsterLifecycle = (runtime) => {
    const currentGameTime = Number(unref(totalGameTimeMs)) || 0;
    if (runtime.kind === 'boss') {
      if (!runtime.alive && shouldRespawnStoryBoss(bossDefeatedAtGameTimeMs, currentGameTime)) {
        bossDefeatedAtGameTimeMs = null;
        runtime.health = runtime.definition.maxHealth;
        runtime.alive = true;
        resetRuntimeToSpawn(runtime, { visible: true });
        persistBossProgress();
        showNotice('山魈王重新出现在村外。', 'danger', 3600);
      } else {
        setRuntimeVisible(runtime, runtime.alive);
      }
      return;
    }

    const currentDayId = storyDayId(currentGameTime);
    if (runtime.lastSpawnDayId !== currentDayId) {
      runtime.lastSpawnDayId = currentDayId;
      runtime.deadDayId = null;
      runtime.health = runtime.definition.maxHealth;
      runtime.alive = true;
      resetRuntimeToSpawn(runtime, { visible: true });
    } else {
      setRuntimeVisible(runtime, runtime.alive && runtime.deadDayId !== currentDayId);
    }
  };

  const fixedStep = (deltaSeconds) => {
    const player = unref(playerState);
    const playerPosition = player?.position || [0, 0, 0];
    for (const runtime of runtimes.values()) {
      updateMonsterLifecycle(runtime);
      if (!runtime.alive || !runtime.visible) continue;
      if (isPaused()) continue;

      const definition = runtime.definition;
      const distanceToPlayer = storyHorizontalDistance(runtime.position, playerPosition);
      const distanceFromSpawn = storyHorizontalDistance(runtime.position, definition.position);
      const heightDifference = Math.abs(runtime.position[1] - Number(playerPosition?.[1] || 0));
      const state = storyMonsterAiState({
        kind: runtime.kind,
        alive: runtime.alive,
        distanceToPlayer,
        distanceFromSpawn,
        attackRange: definition.attackRange,
        detectionRange: definition.detectionRange,
        leashRange: definition.leashRange,
      });
      runtime.state = state;

      if (state === 'attack') {
        if (
          heightDifference <= 3.5 &&
          canStoryAttack(runtime.lastAttackAt, simulationTimeMs, definition.attackCooldownMs)
        ) {
          runtime.lastAttackAt = simulationTimeMs;
          damagePlayer(definition.damage, runtime.displayName);
          if (runtime.kind === 'boss') bossEngagedUntil = Date.now() + 7000;
        }
        continue;
      }
      if (runtime.kind === 'boss' || state === 'idle') continue;

      let target = null;
      if (state === 'chase') target = playerPosition;
      else if (state === 'return') target = definition.position;
      else {
        if (
          !runtime.wanderTarget ||
          storyHorizontalDistance(runtime.position, runtime.wanderTarget) < 0.35
        ) {
          const next = storyWanderPoint(
            definition.position,
            definition.wanderRadius,
            runtime.wanderSeed
          );
          runtime.wanderSeed = next.seed;
          runtime.wanderTarget = next.point;
        }
        target = runtime.wanderTarget;
      }

      const moved = moveStoryPointTowards(
        runtime.position,
        target,
        definition.moveSpeed * deltaSeconds
      );
      if (storyHorizontalDistance(runtime.position, moved.position) > 1e-5) {
        runtime.rotationY = storyYawTowards(runtime.position, moved.position);
        runtime.position = moved.position;
        publishRuntimeTransform(runtime);
      }
    }
    publishMonsters();
  };

  const frame = (timestamp) => {
    frameId = null;
    if (disposed) return;
    if (!previousFrameAt) previousFrameAt = timestamp;
    const frameSeconds = Math.min(
      Math.max((timestamp - previousFrameAt) / 1000, 0),
      MAX_FRAME_SECONDS
    );
    previousFrameAt = timestamp;
    if (isActive() && monstersReady.value) {
      accumulatorSeconds += frameSeconds;
      while (accumulatorSeconds >= FIXED_STEP_SECONDS) {
        if (!isPaused()) simulationTimeMs += FIXED_STEP_SECONDS * 1000;
        fixedStep(FIXED_STEP_SECONDS);
        accumulatorSeconds -= FIXED_STEP_SECONDS;
      }
    }
    frameId = window.requestAnimationFrame(frame);
  };

  const aimScan = () => {
    if (!isActive() || isPaused()) {
      aimedMonster.value = null;
      return;
    }
    requestCenterPick('aim');
  };

  const monsterMarkers = computed(() => {
    void monsterRevision.value;
    return [...runtimes.values()]
      .filter((runtime) => runtime.alive && runtime.visible)
      .map((runtime) => ({
        id: runtime.definition.guid,
        name: runtime.displayName,
        type: runtime.kind === 'boss' ? 'boss' : 'monster',
        semanticRole: runtime.definition.semanticRole,
        position: [...runtime.position],
        kind: runtime.kind === 'boss' ? 'boss' : 'danger',
      }));
  });

  const aimedMonsterHud = computed(() => {
    void monsterRevision.value;
    const runtime = aimedMonster.value;
    if (!runtime?.alive || !runtime.visible) return null;
    return {
      name: runtime.displayName,
      health: runtime.health,
      maxHealth: runtime.definition.maxHealth,
      kind: runtime.kind,
      distance: storyDistance3(unref(playerState)?.position, runtime.position),
    };
  });

  const bossHud = computed(() => {
    void monsterRevision.value;
    const boss = runtimes.get('boss');
    if (!boss?.alive || !boss.visible) return null;
    const distance = storyDistance3(unref(playerState)?.position, boss.position);
    if (distance > 16 && Date.now() > bossEngagedUntil) return null;
    return {
      name: boss.displayName,
      health: boss.health,
      maxHealth: boss.definition.maxHealth,
    };
  });

  watch(
    () => String(unref(projectKey) || '').trim(),
    (nextKey, previousKey) => {
      if (previousKey !== undefined && previousKey !== nextKey) invalidateRuntimeContext();
      loadBossProgress(nextKey);
    },
    { immediate: true }
  );

  watch(
    () => String(unref(sceneId) || '').trim(),
    (nextSceneId, previousSceneId) => {
      if (previousSceneId && previousSceneId !== nextSceneId) invalidateRuntimeContext();
    }
  );

  watch(
    [
      () => Boolean(unref(enabled)),
      () => String(unref(sceneId) || ''),
      () => String(unref(projectKey) || ''),
    ],
    ([active, activeSceneId, activeProjectKey]) => {
      if (active && activeSceneId && activeProjectKey) void initialize();
    },
    { immediate: true }
  );

  onMounted(async () => {
    try {
      pickCallbackToken = await editorApi.events.onActorPickResult(handlePickResult);
    } catch (error) {
      console.warn('[StoryMode] failed to subscribe to combat picking', error);
      warningMessage.value = '战斗拾取暂时不可用。';
    }
    frameId = window.requestAnimationFrame(frame);
    aimTimer = window.setInterval(aimScan, AIM_REFRESH_MS);
  });

  const shutdown = async () => {
    persistBossProgress();
    pendingPicks.clear();
    aimedMonster.value = null;
    restoreAllRuntimeActors();
    if (frameId !== null) window.cancelAnimationFrame(frameId);
    if (aimTimer !== null) window.clearInterval(aimTimer);
    frameId = null;
    aimTimer = null;
    if (pickCallbackToken) {
      await editorApi.off(pickCallbackToken).catch(() => {});
      pickCallbackToken = null;
    }
  };

  onUnmounted(() => {
    disposed = true;
    persistBossProgress();
    pendingPicks.clear();
    restoreAllRuntimeActors();
    if (frameId !== null) window.cancelAnimationFrame(frameId);
    if (aimTimer !== null) window.clearInterval(aimTimer);
    if (noticeTimer !== null) window.clearTimeout(noticeTimer);
    if (damageTimer !== null) window.clearTimeout(damageTimer);
    frameId = null;
    aimTimer = null;
    noticeTimer = null;
    damageTimer = null;
    if (pickCallbackToken) editorApi.off(pickCallbackToken).catch(() => {});
    pickCallbackToken = null;
  });

  return {
    playerHealth,
    playerMaxHealth: STORY_PLAYER_MAX_HEALTH,
    playerDead,
    monstersReady,
    warningMessage,
    notice,
    aimedMonster: aimedMonsterHud,
    bossHud,
    monsterMarkers,
    attackPulse,
    hitPulse,
    damageFlash,
    damageNumber,
    attack,
    damagePlayer,
    respawnPlayer,
    persist: persistBossProgress,
    shutdown,
    refresh: initialize,
  };
}
