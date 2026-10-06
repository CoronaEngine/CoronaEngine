import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import test from 'node:test';
import { createSSRApp } from 'vue';
import { parse } from 'vue/compiler-sfc';
import { compile } from '@vue/compiler-ssr';
import { renderToString } from '@vue/server-renderer';
import { visionRenderModes } from '../../src/utils/visionRenderModes.js';

const source = fs.readFileSync(new URL('../../src/views/layout/MainPage.vue', import.meta.url), 'utf8');
const { descriptor } = parse(source);
const { code } = compile(descriptor.template.content, { mode: 'function' });
const ssrRender = new Function('require', code)(createRequire(import.meta.url));

async function renderMain(settings = {}) {
  const noop = () => {};
  const app = createSSRApp({
    ssrRender,
    setup: () => ({
      nativeViewportCursorEnabled: false, viewportUiMode: 'mono',
      translate: key => key, sceneLightSettings: { enabled: true, direction: { x: 0, y: -1, z: 0 } },
      sceneLightBusy: false, showDialog: false, projectResourceLoadStatus: null, showLocalModal: false,
      sceneAxisVectors: [], dockShortcuts: [], cabbageAssistant: { tasks: [], attentionToken: 0 },
      cabbageChatDetached: true, handleViewportFocus: noop, handleViewportPointer: noop,
      handleViewportPointerDown: noop, handleViewportPointerCancel: noop, handleViewportPointerLeave: noop,
      handleViewportClick: noop, handleWheel: noop, updateSceneLight: noop,
      mainRenderBackend: 'vision', mainVisionRenderMode: 'restir', visionAvailable: true,
      mainRenderModeLabel: 'Vision ReSTIR', activeMenu: null, toggleMenu: noop,
      currentMainCameraId: () => 'camera-1', mainVisionAccumulation: true, mainVisionDenoise: false,
      mainVisionAccumulationBusy: false, mainVisionDenoiseBusy: false,
      mainVisionAccumulationError: '', mainVisionDenoiseError: '',
      mainRenderModeOptions: [{ value: 'native', backend: 'native', label: 'Native' }, ...visionRenderModes],
      selectMainRenderMode: noop, toggleMainVisionAccumulation: noop, toggleMainVisionDenoise: noop,
      ...settings,
    }),
  });
  for (const name of ['CabbageReviewAssistant', 'CabbageChatPanel', 'CabbageGuidanceOverlay']) {
    app.component(name, { render: () => null });
  }
  return renderToString(app);
}

test('main viewport actually renders a reachable mode selector and both independent checkboxes', async () => {
  const html = await renderMain();
  assert.match(html, /<button[^>]*aria-label="渲染模式"[^>]*aria-expanded="false"/);
  assert.match(html, /Vision ReSTIR/);
  assert.match(html, /<input[^>]*aria-label="累积样本"[^>]*checked/);
  assert.match(html, /<input[^>]*aria-label="SVGF"/);
  assert.doesNotMatch(html, /<input[^>]*aria-label="SVGF"[^>]*checked/);
});

test('mode selector stays visible on Native while both checkbox preferences are disabled and retained', async () => {
  const html = await renderMain({ mainRenderBackend: 'native', mainRenderModeLabel: 'Native', mainVisionDenoise: true });
  assert.match(html, /<button[^>]*aria-label="渲染模式"/);
  assert.match(html, /Native/);
  assert.match(html, /<input[^>]*aria-label="累积样本"[^>]*checked[^>]*disabled/);
  assert.match(html, /<input[^>]*aria-label="SVGF"[^>]*checked[^>]*disabled/);
});

test('expanded render options are painted in the page without a native CEF popup', async () => {
  const html = await renderMain({ activeMenu: 'render' });
  assert.match(html, /<button[^>]*aria-label="渲染模式"[^>]*aria-expanded="true"/);
  assert.match(html, /role="menu"/);
  assert.match(html, /<button[^>]*role="menuitemradio"[^>]*aria-checked="true"[^>]*>\s*Vision ReSTIR/);
  assert.match(html, /<button[^>]*role="menuitemradio"[^>]*aria-checked="false"[^>]*>\s*Vision PT/);
});
