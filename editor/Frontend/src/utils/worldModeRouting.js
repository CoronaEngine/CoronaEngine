export const STORY_MODE = 'story';
export const STORY_MODE_ROUTE = '/StoryMode';
export const CREATIVE_MODE_ROUTE = '/';

export function normalizeWorldMode(mode) {
  return String(mode || '')
    .trim()
    .toLocaleLowerCase('en-US');
}

export function destinationForWorldMode(mode) {
  return normalizeWorldMode(mode) === STORY_MODE ? STORY_MODE_ROUTE : CREATIVE_MODE_ROUTE;
}
