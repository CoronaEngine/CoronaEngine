export const STORY_CLOCK_VERSION = 1;
export const STORY_CLOCK_STORAGE_PREFIX = 'corona.story.clock.v1:';
export const STORY_REAL_DAY_MS = 10 * 60 * 1000;
export const STORY_GAME_DAY_MS = 24 * 60 * 60 * 1000;
export const STORY_GAME_HOUR_MS = 60 * 60 * 1000;
export const STORY_CLOCK_RATE = STORY_GAME_DAY_MS / STORY_REAL_DAY_MS;
export const STORY_INITIAL_GAME_TIME_MS = 6 * STORY_GAME_HOUR_MS;

const clamp = (value, minimum, maximum) => Math.min(Math.max(value, minimum), maximum);
const smoothstep = (edge0, edge1, value) => {
  if (edge0 === edge1) return value >= edge1 ? 1 : 0;
  const t = clamp((value - edge0) / (edge1 - edge0), 0, 1);
  return t * t * (3 - 2 * t);
};

export function normalizeStoryGameProjectKey(projectPath) {
  const normalized = String(projectPath || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/\/+$/, '')
    .toLowerCase();
  return normalized || 'active-project';
}

export function storyClockStorageKey(projectPath) {
  return `${STORY_CLOCK_STORAGE_PREFIX}${encodeURIComponent(normalizeStoryGameProjectKey(projectPath))}`;
}

export function normalizeStoryClockDocument(document) {
  const source =
    document && typeof document === 'object' && !Array.isArray(document) ? document : {};
  const totalGameTimeMs = Number(source.totalGameTimeMs);
  return {
    version: STORY_CLOCK_VERSION,
    totalGameTimeMs:
      Number.isFinite(totalGameTimeMs) && totalGameTimeMs >= 0
        ? totalGameTimeMs
        : STORY_INITIAL_GAME_TIME_MS,
    updatedAt: Number.isFinite(Number(source.updatedAt)) ? Number(source.updatedAt) : 0,
  };
}

export function advanceStoryGameTime(totalGameTimeMs, realElapsedMs) {
  const current = Math.max(Number(totalGameTimeMs) || 0, 0);
  const elapsed = clamp(Number(realElapsedMs) || 0, 0, 1000);
  return current + elapsed * STORY_CLOCK_RATE;
}

export function storyClockParts(totalGameTimeMs) {
  const total = Math.max(Number(totalGameTimeMs) || 0, 0);
  const dayIndex = Math.floor(total / STORY_GAME_DAY_MS);
  const timeOfDayMs = total % STORY_GAME_DAY_MS;
  const totalMinutes = Math.floor(timeOfDayMs / 60000);
  const hour = Math.floor(totalMinutes / 60);
  const minute = totalMinutes % 60;
  return {
    dayIndex,
    dayNumber: dayIndex + 1,
    timeOfDayMs,
    hour,
    minute,
    hourValue: timeOfDayMs / STORY_GAME_HOUR_MS,
  };
}

export function storyDayId(totalGameTimeMs) {
  return storyClockParts(totalGameTimeMs).dayIndex;
}

export function storyTimePhase(totalGameTimeMs) {
  const { hourValue } = storyClockParts(totalGameTimeMs);
  if (hourValue >= 5 && hourValue < 7) return 'dawn';
  if (hourValue >= 7 && hourValue < 18) return 'day';
  if (hourValue >= 18 && hourValue < 20) return 'dusk';
  return 'night';
}

export function isStoryNight(totalGameTimeMs) {
  return storyTimePhase(totalGameTimeMs) === 'night';
}

export function storyNightId(totalGameTimeMs) {
  const parts = storyClockParts(totalGameTimeMs);
  if (parts.hourValue >= 20) return parts.dayIndex;
  if (parts.hourValue < 5) return parts.dayIndex - 1;
  return null;
}

export function formatStoryClock(totalGameTimeMs) {
  const parts = storyClockParts(totalGameTimeMs);
  return {
    ...parts,
    phase: storyTimePhase(totalGameTimeMs),
    isNight: isStoryNight(totalGameTimeMs),
    timeText: `${String(parts.hour).padStart(2, '0')}:${String(parts.minute).padStart(2, '0')}`,
    dayText: `第 ${parts.dayNumber} 天`,
  };
}

export function storyLightingAtTime(totalGameTimeMs) {
  const { hourValue } = storyClockParts(totalGameTimeMs);
  const phase = storyTimePhase(totalGameTimeMs);
  let daylight = 0;
  if (hourValue >= 5 && hourValue < 7) daylight = smoothstep(5, 7, hourValue);
  else if (hourValue >= 7 && hourValue < 18) daylight = 1;
  else if (hourValue >= 18 && hourValue < 20) daylight = 1 - smoothstep(18, 20, hourValue);

  const orbit = ((hourValue - 6) / 24) * Math.PI * 2;
  const horizontalX = Math.cos(orbit);
  const horizontalZ = Math.sin(orbit);
  const height = Math.max(1.25, Math.abs(Math.sin(orbit)) * 10);
  const moonSide = phase === 'night' ? -1 : 1;
  return {
    phase,
    direction: [horizontalX * 5 * moonSide, height, horizontalZ * 5 * moonSide],
    sunIntensity: 0.35 + (10 - 0.35) * daylight,
    skyIntensity: 1.8 + (20 - 1.8) * daylight,
  };
}
