import assert from 'node:assert/strict';
import test from 'node:test';

import {
  advanceStoryGameTime,
  formatStoryClock,
  isStoryNight,
  normalizeStoryClockDocument,
  normalizeStoryGameProjectKey,
  STORY_CLOCK_RATE,
  STORY_GAME_DAY_MS,
  STORY_GAME_HOUR_MS,
  STORY_INITIAL_GAME_TIME_MS,
  STORY_REAL_DAY_MS,
  storyClockParts,
  storyClockStorageKey,
  storyLightingAtTime,
  storyNightId,
  storyTimePhase,
} from '../../src/utils/storyGameClock.js';

test('story clock starts at day one 06:00 and formats stable project keys', () => {
  const initial = normalizeStoryClockDocument(null);
  assert.equal(initial.totalGameTimeMs, STORY_INITIAL_GAME_TIME_MS);
  assert.deepEqual(formatStoryClock(initial.totalGameTimeMs), {
    dayIndex: 0,
    dayNumber: 1,
    timeOfDayMs: 6 * STORY_GAME_HOUR_MS,
    hour: 6,
    minute: 0,
    hourValue: 6,
    phase: 'dawn',
    isNight: false,
    timeText: '06:00',
    dayText: '第 1 天',
  });
  assert.equal(normalizeStoryGameProjectKey(' D:\\Worlds\\StoryOne\\ '), 'd:/worlds/storyone');
  assert.equal(
    storyClockStorageKey('D:\\Worlds\\StoryOne'),
    storyClockStorageKey('d:/worlds/storyone/')
  );
});

test('ten real minutes advance exactly one complete game day', () => {
  assert.equal(STORY_REAL_DAY_MS * STORY_CLOCK_RATE, STORY_GAME_DAY_MS);
  let total = 0;
  for (let elapsed = 0; elapsed < STORY_REAL_DAY_MS; elapsed += 1000) {
    total = advanceStoryGameTime(total, 1000);
  }
  assert.equal(total, STORY_GAME_DAY_MS);
  assert.equal(storyClockParts(total).dayNumber, 2);
});

test('clock advancement clamps delayed ticks and rejects invalid saves', () => {
  assert.equal(advanceStoryGameTime(100, 5000), 100 + 1000 * STORY_CLOCK_RATE);
  assert.equal(
    normalizeStoryClockDocument({ totalGameTimeMs: -1 }).totalGameTimeMs,
    STORY_INITIAL_GAME_TIME_MS
  );
  assert.equal(
    normalizeStoryClockDocument({ totalGameTimeMs: 'broken' }).totalGameTimeMs,
    STORY_INITIAL_GAME_TIME_MS
  );
});

test('time phases and night identifiers remain correct across midnight', () => {
  const at = (hour) => hour * STORY_GAME_HOUR_MS;
  assert.equal(storyTimePhase(at(5)), 'dawn');
  assert.equal(storyTimePhase(at(7)), 'day');
  assert.equal(storyTimePhase(at(18)), 'dusk');
  assert.equal(storyTimePhase(at(20)), 'night');
  assert.equal(isStoryNight(at(4.5)), true);
  assert.equal(isStoryNight(at(12)), false);
  assert.equal(storyNightId(at(23)), 0);
  assert.equal(storyNightId(STORY_GAME_DAY_MS + at(2)), 0);
  assert.equal(storyNightId(STORY_GAME_DAY_MS + at(12)), null);
  assert.equal(formatStoryClock(at(23.5)).timeText, '23:30');
});

test('night lighting remains visible while daytime reaches configured maxima', () => {
  const midnight = storyLightingAtTime(0);
  assert.equal(midnight.phase, 'night');
  assert.ok(midnight.sunIntensity >= 0.35);
  assert.ok(midnight.skyIntensity >= 1.8);
  assert.ok(midnight.direction.every(Number.isFinite));

  const noon = storyLightingAtTime(12 * STORY_GAME_HOUR_MS);
  assert.equal(noon.phase, 'day');
  assert.equal(noon.sunIntensity, 10);
  assert.equal(noon.skyIntensity, 20);
});
