import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(__dirname, '..', '..', '..');

const read = (relativePath) => fs.readFileSync(path.join(root, relativePath), 'utf8');
const exists = (relativePath) => fs.existsSync(path.join(root, relativePath));

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

if (!exists('include/corona/systems/ui/viewport_gizmo_manager.h')) {
  throw new Error('Native viewport gizmo manager header must be restored.');
}
if (!exists('src/systems/ui/viewport_gizmo_manager.cpp')) {
  throw new Error('Native viewport gizmo manager implementation must be restored.');
}
if (exists('editor/Frontend/src/utils/viewportGizmo.js')) {
  throw new Error('Frontend viewport gizmo drag controller must be removed when native ImGuizmo is active.');
}
if (exists('editor/Frontend/src/utils/viewportGizmoView.js')) {
  throw new Error('Frontend SVG viewport gizmo renderer must be removed when native ImGuizmo is active.');
}

const cmake = read('src/systems/ui/CMakeLists.txt');
const browserUi = read('src/systems/ui/cef/browser_ui.cpp');
const bridge = read('src/systems/ui/cef/cef_realtime_bridge.cpp');
const overlay = read('editor/Frontend/src/components/viewport/ViewportGizmoOverlay.vue');
const mainPage = read('editor/Frontend/src/views/layout/MainPage.vue');
const cefSubprocess = read('examples/cef_subprocess/main.cpp');

assertIncludes(
  cmake,
  'viewport_gizmo_manager.cpp',
  'CMake must compile the native viewport gizmo manager.',
);
assertIncludes(
  cmake,
  'ImGuizmo.cpp',
  'CMake must compile ImGuizmo for native viewport gizmo rendering.',
);
assertIncludes(
  cmake,
  'vision/src/ext/window/imgui/gizmo',
  'CMake must add the ImGuizmo include directory.',
);

assertIncludes(
  browserUi,
  '#include <corona/systems/ui/viewport_gizmo_manager.h>',
  'browser_ui must include the native viewport gizmo manager.',
);
assertIncludes(
  browserUi,
  'ViewportGizmoManager::instance().render',
  'browser_ui must render the native gizmo over ImGui viewport images.',
);
assertIncludes(
  browserUi,
  'ImGui::GetItemRectMin()',
  'Native gizmo must use the actual rendered ImGui image rect for hit testing.',
);
assertIncludes(
  browserUi,
  'ImGui::GetItemRectSize()',
  'Native gizmo must use the actual rendered ImGui image size for hit testing.',
);
assertIncludes(
  browserUi,
  'emit_native_viewport_gizmo_events',
  'browser_ui must drain native selection/transform events back to the frontend.',
);

assertIncludes(
  bridge,
  'ViewportGizmoMode',
  'Realtime bridge must accept native gizmo mode messages.',
);
assertIncludes(
  bridge,
  'ViewportGizmoSelection',
  'Realtime bridge must accept native gizmo selection messages.',
);
assertIncludes(
  bridge,
  'ViewportGizmoClearSelection',
  'Realtime bridge must accept native gizmo clear-selection messages.',
);
assertIncludes(
  cefSubprocess,
  'setViewportGizmoMode',
  'CEF renderer must expose setViewportGizmoMode on window.coronaBridge.',
);
assertIncludes(
  cefSubprocess,
  'setViewportGizmoSelection',
  'CEF renderer must expose setViewportGizmoSelection on window.coronaBridge.',
);
assertIncludes(
  cefSubprocess,
  'clearViewportGizmoSelection',
  'CEF renderer must expose clearViewportGizmoSelection on window.coronaBridge.',
);

assertNotIncludes(
  overlay,
  '<svg',
  'Vue viewport gizmo overlay must not draw a second SVG gizmo.',
);
assertNotIncludes(
  overlay,
  "defineEmits(['mode-change', 'drag-start'])",
  'Vue viewport gizmo overlay must not start frontend drag sessions.',
);
assertNotIncludes(
  mainPage,
  'createViewportGizmoController',
  'MainPage must not use the frontend gizmo drag controller when native ImGuizmo is active.',
);
assertNotIncludes(
  mainPage,
  '@drag-start="handleGizmoDragStart"',
  'MainPage must not wire frontend drag-start handlers for the old Vue gizmo.',
);
