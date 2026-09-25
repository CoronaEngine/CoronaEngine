/** Project-mode presentation lifecycle. Mode policy lives in Web, not the native bridge. */
import { shallowReactive } from 'vue';
import { editorApi } from '../api/editorApi.js';

export const LAUNCHER_ROUTES = new Set(['/StartScreen', '/NewGame', '/JoinGame', '/RecentGames']);
export const normalizeWorldMode = (mode) => mode === 'story' ? 'story' : 'creative';
export const normalizeProjectPath = (path) => String(path || '').trim()
  .replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();

export function unwrapProjectInfo(response) {
  let info = response;
  for (let i = 0; i < 3 && info?.data && !info?.project_path; i++) info = info.data;
  if (typeof info?.project_path !== 'string' || !info.project_path.trim()) {
    throw new Error('无法读取当前世界的模式，请重新打开世界。');
  }
  return info;
}

// The pre-isolation native API defaults a portable scene with no [world] type
// to story. Read the actual marker through the existing Python file adapter;
// never migrate/save project files merely to decide which UI to render.
export function portableWorldMode(text, legacyMode = 'creative') {
  let section = '';
  let mode;
  const format = {};
  for (const line of String(text).replace(/^\uFEFF/, '').split(/\r?\n/)) {
    const value = line.trim();
    if (!value || value.startsWith(';') || value.startsWith('#')) continue;
    if (value.startsWith('[') && value.endsWith(']')) {
      section = value.slice(1, -1).trim().toLowerCase();
      continue;
    }
    const equals = value.indexOf('=');
    if (section === 'world' && equals >= 0 && value.slice(0, equals).trim().toLowerCase() === 'type') {
      mode = value.slice(equals + 1).trim();
    }
    if (section === 'format' && equals >= 0) {
      format[value.slice(0, equals).trim().toLowerCase()] = value.slice(equals + 1).trim();
    }
  }
  return normalizeWorldMode(format.type === 'corona_scene_folder' && format.version === '1' ? mode : legacyMode);
}
export async function readWorldProjectInfo(api) {
  const info = unwrapProjectInfo(await api.projectSettings.getActiveProjectInfo());
  // Other modes are already unambiguous, including legacy/multiplayer projects.
  if (info.mode !== 'story' || info.entrance_scene !== 'scene.ini') return info;
  const path = `${info.project_path.replace(/[\\/]+$/, '')}/scene.ini`;
  const response = await api.ai.readLocalFileAsBase64(path);
  const data = response?.data ?? response;
  if (typeof data !== 'string' || !/^data:[^,]*;base64,/.test(data)) {
    throw new Error('无法读取世界模式配置，请重新打开世界。');
  }
  const bytes = Uint8Array.from(atob(data.slice(data.indexOf(',') + 1)), char => char.charCodeAt(0));
  const text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  return { ...info, mode: portableWorldMode(text, info.mode) };
}

export function createWorldModeController({ readProjectInfo, publish = () => {} }) {
  const state = { status: 'idle', mode: null, projectPath: '', revision: 0, error: null };
  let pending = null;
  let opening = false;
  let openToken = 0;
  const emit = () => publish({ ...state });
  function invalidate(projectPath = '') {
    state.revision += 1;
    Object.assign(state, { status: 'loading', mode: null, projectPath, error: null });
    pending = null;
    emit();
    return state.revision;
  }
  async function resolve(expectedPath = '', { force = false } = {}) {
    const expected = normalizeProjectPath(expectedPath);
    if (!force && state.status === 'ready' && (!expected || normalizeProjectPath(state.projectPath) === expected)) {
      return { ...state };
    }
    if (pending && pending.expected === expected) return pending.promise;
    const revision = invalidate(expectedPath);
    const request = { expected, promise: null };
    request.promise = (async () => {
      try {
        const info = unwrapProjectInfo(await readProjectInfo());
        if (revision !== state.revision) return null;
        if (expected && normalizeProjectPath(info.project_path) !== expected) {
          throw new Error('当前世界已改变，请重新打开目标世界。');
        }
        Object.assign(state, { status: 'ready', mode: normalizeWorldMode(info.mode),
          projectPath: info.project_path, error: null });
        emit();
        return { ...state };
      } catch (error) {
        if (revision !== state.revision) return null;
        Object.assign(state, { status: 'error', mode: null, error });
        emit();
        throw error;
      } finally {
        if (pending === request) pending = null;
      }
    })();
    pending = request;
    return request.promise;
  }
  return {
    state, resolve, invalidate,
    fail(error) {
      pending = null;
      Object.assign(state, { status: 'error', mode: null, error });
      emit();
    },
    get opening() { return opening; },
    beginOpen(path) { opening = true; invalidate(path); return ++openToken; },
    finishOpen(token) {
      if (token !== openToken) return false;
      opening = false;
      return true;
    },
  };
}

export const worldModeState = shallowReactive({
  status: 'idle', mode: null, projectPath: '', revision: 0, error: null,
});
export const worldModeService = createWorldModeController({
  readProjectInfo: () => readWorldProjectInfo(editorApi),
  publish: (state) => Object.assign(worldModeState, state),
});
export const editorUiAllowed = () => worldModeState.status === 'ready' && worldModeState.mode === 'creative';

/** Injectable guard shared by the real router and lifecycle regression tests. */
export function createWorldModeRouteGuard({ controller, closePanel, notify }) {
  return async (to) => {
    if (LAUNCHER_ROUTES.has(to.path)) return true;
    if (controller.opening) return false;
    try {
      const session = await controller.resolve();
      if (!session) return false;
      if (session.mode === 'story' && to.path !== '/') {
        if (to.query?.standalone === '1') {
          await closePanel();
          return false;
        }
        return '/';
      }
      return true;
    } catch (error) {
      if (to.query?.standalone === '1') {
        await closePanel().catch(() => {});
        return false;
      }
      notify(error.message || '读取世界模式失败', error);
      return '/StartScreen';
    }
  };
}
