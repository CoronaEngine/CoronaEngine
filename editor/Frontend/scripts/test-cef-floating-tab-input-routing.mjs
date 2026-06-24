import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(__dirname, '..', '..', '..');

const read = (relativePath) => fs.readFileSync(path.join(root, relativePath), 'utf8');

const assertIncludes = (source, needle, message) => {
  if (!source.includes(needle)) {
    throw new Error(message);
  }
};

const assertNotIncludes = (source, needle, message) => {
  if (source.includes(needle)) {
    throw new Error(message);
  }
};

const imguiUi = read('src/systems/ui/imgui/imgui_ui.cpp');
const browserUi = read('src/systems/ui/cef/browser_ui.cpp');

assertIncludes(
  imguiUi,
  'auto route_browser_platform_window = [&](SDL_WindowID window_id)',
  'Keyboard/text events from ImGui platform windows must route by SDL window ID for every browser tab.',
);
assertIncludes(
  imguiUi,
  'tab->platform_window_id == window_id',
  'Focused platform-window routing must match BrowserTab::platform_window_id.',
);
assertIncludes(
  imguiUi,
  'tab->platform_handle_raw == foreground',
  'Foreground native-window routing must match BrowserTab::platform_handle_raw.',
);
assertIncludes(
  browserUi,
  'sync_tab_platform_window(tab);',
  'Every rendered browser tab must publish its ImGui platform window before input routing runs next frame.',
);
assertIncludes(
  browserUi,
  'void sync_tab_platform_window(BrowserTab* tab)',
  'Platform-window synchronization must be shared by camera and floating CEF tabs.',
);

assertIncludes(
  browserUi,
  'SetFocus(false)',
  'Focusing one CEF tab must blur the previously focused tab so frontend focus locks can release.',
);

assertNotIncludes(
  imguiUi,
  'if (context.window && window_id == SDL_GetWindowID(context.window)) {\n            route_main_window();',
  'Main SDL-window keyboard events must not force active_tab_id back to the main CEF tab; embedded dock tabs share that window.',
);

assertNotIncludes(
  imguiUi,
  'if (focused_window == context.window) {\n            route_main_window();',
  'Main SDL keyboard focus must preserve the active embedded CEF tab selected by mouse hit-testing.',
);

assertNotIncludes(
  imguiUi,
  'if (main_hwnd && foreground == main_hwnd) {\n            route_main_window();',
  'Main native foreground window must not override active embedded CEF tab routing.',
);
