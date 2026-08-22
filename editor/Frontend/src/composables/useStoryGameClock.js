import { computed, onUnmounted, ref, unref, watch } from 'vue';

import { editorApi } from '@/api/editorApi.js';
import { resolveSceneSnapshot } from '@/utils/nativeSceneViewport.js';
import {
  advanceStoryGameTime,
  formatStoryClock,
  normalizeStoryClockDocument,
  STORY_CLOCK_VERSION,
  STORY_INITIAL_GAME_TIME_MS,
  storyClockStorageKey,
  storyLightingAtTime,
} from '@/utils/storyGameClock.js';

const CLOCK_TICK_MS = 250;
const CLOCK_SAVE_MS = 5000;

function browserStorage() {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

function finiteDirection(value, fallback = [1, 10, 1]) {
  if (!Array.isArray(value) || value.length < 3) return [...fallback];
  const direction = value.slice(0, 3).map(Number);
  return direction.every(Number.isFinite) ? direction : [...fallback];
}

export function useStoryGameClock({ sceneId, projectKey, enabled } = {}) {
  const totalGameTimeMs = ref(STORY_INITIAL_GAME_TIME_MS);
  const loaded = ref(false);
  const lightingError = ref('');

  let disposed = false;
  let storageKey = '';
  let tickTimer = null;
  let saveTimer = null;
  let previousTickAt = 0;
  let baselineLighting = null;
  let baselineSceneId = '';
  let lightingInFlight = null;
  let pendingLighting = null;

  const clock = computed(() => formatStoryClock(totalGameTimeMs.value));
  const dayNumber = computed(() => clock.value.dayNumber);
  const dayText = computed(() => clock.value.dayText);
  const timeText = computed(() => clock.value.timeText);
  const phase = computed(() => clock.value.phase);
  const isNight = computed(() => clock.value.isNight);

  const persist = (storage = browserStorage()) => {
    if (!storageKey || !storage || !loaded.value) return false;
    try {
      storage.setItem(
        storageKey,
        JSON.stringify({
          version: STORY_CLOCK_VERSION,
          totalGameTimeMs: totalGameTimeMs.value,
          updatedAt: Date.now(),
        })
      );
      return true;
    } catch (error) {
      console.warn('[StoryMode] failed to persist the story clock', error);
      return false;
    }
  };

  const load = (key, storage = browserStorage()) => {
    storageKey = key ? storyClockStorageKey(key) : '';
    let document = null;
    if (storageKey && storage) {
      try {
        const serialized = storage.getItem(storageKey);
        if (serialized) document = JSON.parse(serialized);
      } catch (error) {
        console.warn('[StoryMode] failed to load the story clock', error);
      }
    }
    totalGameTimeMs.value = normalizeStoryClockDocument(document).totalGameTimeMs;
    loaded.value = Boolean(storageKey);
    previousTickAt = Date.now();
  };

  const captureBaselineLighting = async (activeSceneId) => {
    if (!activeSceneId || baselineSceneId === activeSceneId || disposed) return;
    try {
      let environment = {};
      if (typeof editorApi.sceneTools.getEnvironment === 'function') {
        environment = resolveSceneSnapshot(
          await editorApi.sceneTools.getEnvironment(activeSceneId)
        );
      }
      if (!environment?.sun) {
        const snapshot = resolveSceneSnapshot(await editorApi.scene.getSnapshot(activeSceneId));
        environment = snapshot?.environment ?? snapshot;
      }
      const sun = environment?.sun ?? {};
      const enabledValue = sun.enabled !== false;
      baselineLighting = {
        enabled: enabledValue,
        direction: finiteDirection(sun.direction ?? sun.sun_direction),
        sunIntensity: Math.max(
          0,
          Number(sun.sun_intensity ?? sun.sunIntensity ?? (enabledValue ? 10 : 0)) || 0
        ),
        skyIntensity: Math.max(
          0,
          Number(sun.sky_intensity ?? sun.skyIntensity ?? (enabledValue ? 20 : 0)) || 0
        ),
      };
      baselineSceneId = activeSceneId;
    } catch (error) {
      console.warn('[StoryMode] failed to capture scene lighting', error);
      baselineLighting = {
        enabled: true,
        direction: [1, 10, 1],
        sunIntensity: 10,
        skyIntensity: 20,
      };
      baselineSceneId = activeSceneId;
    }
  };

  const flushLighting = async () => {
    if (lightingInFlight || !pendingLighting || disposed) return lightingInFlight;
    const next = pendingLighting;
    pendingLighting = null;
    lightingInFlight = editorApi.sceneTools
      .sunDirection(next.sceneId, true, next.direction, {
        sunIntensity: next.sunIntensity,
        skyIntensity: next.skyIntensity,
        persist: false,
      })
      .then(() => {
        lightingError.value = '';
      })
      .catch((error) => {
        console.warn('[StoryMode] failed to update runtime day/night lighting', error);
        lightingError.value = '昼夜光照暂时无法同步。';
      })
      .finally(() => {
        lightingInFlight = null;
        if (pendingLighting && !disposed) void flushLighting();
      });
    return lightingInFlight;
  };

  const updateLighting = async () => {
    const activeSceneId = String(unref(sceneId) || '').trim();
    if (!activeSceneId || !unref(enabled) || disposed) return false;
    await captureBaselineLighting(activeSceneId);
    pendingLighting = { sceneId: activeSceneId, ...storyLightingAtTime(totalGameTimeMs.value) };
    void flushLighting();
    return true;
  };

  const tick = () => {
    const now = Date.now();
    if (!previousTickAt) previousTickAt = now;
    const elapsed = now - previousTickAt;
    previousTickAt = now;
    if (!loaded.value || !unref(enabled)) return;
    totalGameTimeMs.value = advanceStoryGameTime(totalGameTimeMs.value, elapsed);
    void updateLighting();
  };

  const startTimers = () => {
    if (tickTimer === null) tickTimer = window.setInterval(tick, CLOCK_TICK_MS);
    if (saveTimer === null) saveTimer = window.setInterval(() => persist(), CLOCK_SAVE_MS);
  };

  const restoreLighting = async () => {
    pendingLighting = null;
    if (lightingInFlight) await lightingInFlight.catch(() => {});
    const scene = baselineSceneId;
    const baseline = baselineLighting;
    baselineLighting = null;
    baselineSceneId = '';
    if (!scene || !baseline) return false;
    try {
      await editorApi.sceneTools.sunDirection(scene, baseline.enabled, baseline.direction, {
        sunIntensity: baseline.sunIntensity,
        skyIntensity: baseline.skyIntensity,
        persist: false,
      });
      return true;
    } catch (error) {
      console.warn('[StoryMode] failed to restore scene lighting', error);
      return false;
    }
  };

  const shutdown = async () => {
    persist();
    if (tickTimer !== null) window.clearInterval(tickTimer);
    if (saveTimer !== null) window.clearInterval(saveTimer);
    tickTimer = null;
    saveTimer = null;
    await restoreLighting();
  };

  watch(
    () => String(unref(projectKey) || '').trim(),
    (nextKey) => load(nextKey),
    { immediate: true }
  );

  watch(
    [() => Boolean(unref(enabled)), () => String(unref(sceneId) || '')],
    ([isEnabled, activeSceneId], [wasEnabled, previousSceneId] = []) => {
      previousTickAt = Date.now();
      if (
        previousSceneId &&
        previousSceneId !== activeSceneId &&
        baselineSceneId === previousSceneId
      ) {
        void restoreLighting();
      }
      if (isEnabled && activeSceneId) void updateLighting();
      else if (wasEnabled && !isEnabled) persist();
    },
    { immediate: true }
  );

  startTimers();

  onUnmounted(() => {
    disposed = true;
    persist();
    if (tickTimer !== null) window.clearInterval(tickTimer);
    if (saveTimer !== null) window.clearInterval(saveTimer);
    tickTimer = null;
    saveTimer = null;
    pendingLighting = null;
    const scene = baselineSceneId;
    const baseline = baselineLighting;
    if (scene && baseline) {
      editorApi.sceneTools
        .sunDirection(scene, baseline.enabled, baseline.direction, {
          sunIntensity: baseline.sunIntensity,
          skyIntensity: baseline.skyIntensity,
          persist: false,
        })
        .catch(() => {});
    }
  });

  return {
    totalGameTimeMs,
    loaded,
    dayNumber,
    dayText,
    timeText,
    phase,
    isNight,
    lightingError,
    persist,
    updateLighting,
    restoreLighting,
    shutdown,
  };
}
