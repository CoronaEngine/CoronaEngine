/**
 * Reads the baked map height grid off disk and installs it in `storyMapHeight.mjs`.
 *
 * Kept apart from the mapping itself on purpose: that module is pure arithmetic (and is tested
 * as such in Node), while this one is the only place in the story frontend that knows a file
 * exists. The grid is data the world may not have — a missing or unreadable file must leave the
 * ground on the constant engine floor rather than break entering the world — so callers are
 * expected to catch the rejection and carry on.
 *
 * ## Why this is not a bare `fetch`
 *
 * The story page is the CEF view of `<root>/CabbageEditor/Frontend/dist/index.html`, i.e. a
 * `file:` document (`cef_app.cpp` sets `allow-file-access-from-files` for exactly this reason).
 * Chromium's Fetch API rejects `file:` URLs outright — only http(s)/data/blob are fetchable —
 * so a fetch-only loader reads nothing on the real page, fails into the "no grid" fallback, and
 * leaves every character on the flat engine floor while looking perfectly wired. That is the
 * failure this module was written into and then fell into. `file:` reads therefore go through
 * `XMLHttpRequest`, which the switch above does allow; http(s) documents keep using `fetch`.
 */

import { setHeightGrid } from './storyMapHeight.mjs';

/**
 * Relative to the page (`CabbageEditor/Frontend/dist/index.html`), which is the same two
 * levels up that `resolveStoryAssetPath` walks for `game/art/models`.
 */
export const HEIGHT_GRID_PATH = '../../game/data/story-map-height.json';

/** Absolute URL of the deployed grid for a page URL. */
export function mapHeightUrl(frontendUrl, path = HEIGHT_GRID_PATH) {
  return new URL(path, frontendUrl).href;
}

/**
 * One `XMLHttpRequest` read of a JSON document.
 *
 * `responseType = 'json'` is honoured first (CEF parses the 291 KB off the IO thread), with a
 * `JSON.parse` of `responseText` as the fallback for a request object that ignores it. A
 * failure — missing file, bad JSON — rejects, so the caller keeps the constant floor.
 */
export function readJsonByXhr(url, createXhr = () => new XMLHttpRequest()) {
  return new Promise((resolve, reject) => {
    let request;
    try { request = createXhr(); } catch (error) { reject(error); return; }
    try {
      request.open('GET', url, true);
      request.responseType = 'json';
    } catch (error) { reject(error); return; }
    request.onload = () => {
      if (request.response && typeof request.response === 'object') { resolve(request.response); return; }
      try { resolve(JSON.parse(String(request.responseText ?? ''))); }
      catch (error) { reject(new Error(`地图高度网格解析失败：${error.message}`)); }
    };
    request.onerror = () => reject(new Error('地图高度网格读取失败：网络错误'));
    request.send();
  });
}

/** One `fetch` read of a JSON document, for an http(s) page. */
export async function readJsonByFetch(url, fetchImpl = globalThis.fetch) {
  if (typeof fetchImpl !== 'function') throw new Error('当前环境不支持读取地图高度网格');
  const response = await fetchImpl(url);
  if (response?.ok === false) {
    throw new Error(`地图高度网格读取失败（${response.status || '未知状态'}）`);
  }
  return typeof response?.json === 'function' ? await response.json() : response;
}

/**
 * Read + validate + install. Resolves to the parsed grid; rejects when it cannot be read.
 *
 * @param url absolute grid URL (`mapHeightUrl`)
 * @param options.fetchImpl injectable for tests; defaults to the global `fetch`
 * @param options.createXhr injectable for tests; defaults to `new XMLHttpRequest()`
 */
export async function loadStoryMapHeight(url, options = {}) {
  const { fetchImpl = globalThis.fetch,
    createXhr = typeof globalThis.XMLHttpRequest === 'function' ? undefined : null } = options;
  // A `file:` page cannot be served by `fetch`; anything else (http, a test double) can.
  const document = /^file:/i.test(String(url)) && createXhr !== null
    ? await readJsonByXhr(url, createXhr)
    : await readJsonByFetch(url, fetchImpl);
  return setHeightGrid(document);
}
