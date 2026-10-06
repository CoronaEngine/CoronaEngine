import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import { computed, ref } from 'vue';
import { babelParse, parse } from 'vue/compiler-sfc';
import * as visionModes from '../../src/utils/visionRenderModes.js';
const { visionRenderModes } = visionModes;

function declarations(path, names) {
  const { descriptor } = parse(fs.readFileSync(new URL(path, import.meta.url), 'utf8'));
  const source = descriptor.scriptSetup.content;
  return babelParse(source, { sourceType: 'module' }).program.body
    .filter(node => node.type === 'VariableDeclaration'
      && node.declarations.some(item => names.includes(item.id.name)))
    .map(node => source.slice(node.start, node.end)).join('\n');
}

function mainHost({ fail = false, pending = false, denoiseFail = false, denoiseReply,
  accumulationFail = false, accumulationReply } = {}) {
  const calls = [];
  const source = declarations('../../src/views/layout/MainPage.vue', [
    'mainRenderModeOptions', 'mainRenderModeLabel', 'pendingMainRenderSelection',
    'currentMainCameraId', 'selectMainRenderMode',
    'currentMainSceneId', 'currentMainCamera', 'mainVisionDenoise', 'mainVisionDenoiseBusy',
    'mainVisionDenoiseError', 'pendingMainDenoiseSelection', 'toggleMainVisionDenoise',
    'mainVisionAccumulation', 'mainVisionAccumulationBusy', 'mainVisionAccumulationError',
    'pendingMainAccumulationSelection', 'toggleMainVisionAccumulation',
    'isVector3', 'sceneGridEnabledFromSnapshot', 'applySceneSnapshot',
  ]);
  const api = {
    setVisionRenderMode: async (...args) => {
      calls.push(['mode', ...args]);
      if (fail) throw new Error('mode unavailable');
      return { mode: pending ? 'path_tracing' : args[2], pending };
    },
    setRenderBackend: async (...args) => { calls.push(['backend', ...args]); return { mode: args[0] }; },
    setOutputMode: async (...args) => { calls.push(['output', ...args]); },
    setVisionDenoise: async (...args) => {
      calls.push(['denoise', ...args]);
      if (denoiseFail) throw new Error('denoiser unavailable');
      return denoiseReply ? denoiseReply(args) : { enabled: pending ? !args[2] : args[2], pending };
    },
    setVisionAccumulation: async (...args) => {
      calls.push(['accumulation', ...args]);
      if (accumulationFail) throw new Error('accumulation unavailable');
      return accumulationReply ? accumulationReply(args) : { enabled: pending ? !args[2] : args[2], pending };
    },
  };
  const host = new Function('ref', 'computed', 'visionModes', 'editorApi', `
    const { visionRenderModes, normalizeVisionRenderMode, visionDenoiseFromCamera, visionAccumulationFromCamera } = visionModes;
    const tabs = ref([{ id: 'test.scene' }]), activeTab = ref(0), activeMenu = ref(null);
    const cameraBindingState = ref({ sceneId: 'test.scene', cameraId: 'camera-2' });
    const mainRenderBackend = ref('native'), mainVisionRenderMode = ref('path_tracing');
    const DEFAULT_SCENE_NAME = 'default.scene';
    const unwrapBridgeData = value => value?.data ?? value;
    const syncSceneCameraBinding = async () => {}, logError = () => {};
    const resetRealtimeCameraInput = () => {}, scheduleCameraViewportSync = () => {};
    const syncViewportUiMode = () => {}, scheduleCameraUpdate = () => {};
    const isRealtimeCameraInputActive = () => false, indexActorsByHandle = () => new Map();
    const sceneGridEnabled = ref(true), sceneLightSettings = { direction: {} };
    const cameraState = ref({ position: [0, 0, 0], forward: [0, 0, 1], up: [0, 1, 0], fov: 60 });
    let actorPickIndex, lastCameraViewportSignature;
    ${source}
    return { selectMainRenderMode, mainRenderModeOptions, mainRenderModeLabel,
      mainRenderBackend, mainVisionRenderMode, cameraBindingState, tabs, applySceneSnapshot,
      mainVisionDenoise, mainVisionDenoiseError, currentMainCamera, toggleMainVisionDenoise,
      mainVisionAccumulation: typeof mainVisionAccumulation === 'undefined' ? undefined : mainVisionAccumulation,
      mainVisionAccumulationError: typeof mainVisionAccumulationError === 'undefined' ? undefined : mainVisionAccumulationError,
      toggleMainVisionAccumulation: typeof toggleMainVisionAccumulation === 'undefined' ? undefined : toggleMainVisionAccumulation };
  `)(ref, computed, visionModes, { sceneTools: api });
  return { ...host, calls };
}

test('main menu selects ReSTIR on the current camera using the Vision backend', async () => {
  const host = mainHost();
  const option = host.mainRenderModeOptions.find(item => item.value === 'restir');
  assert.equal(option.backend, 'vision');
  assert.equal(await host.selectMainRenderMode(option.value), true);
  assert.deepEqual(host.calls, [
    ['mode', 'test.scene', 'camera-2', 'restir'],
    ['output', 'test.scene', 'camera-2', 'final_color'],
    ['backend', 'vision', 'test.scene', 'camera-2'],
  ]);
  assert.equal(host.mainVisionRenderMode.value, 'restir');
  assert.match(host.mainRenderModeLabel.value, /ReSTIR/);
  await host.selectMainRenderMode('path_tracing');
  assert.equal(host.mainVisionRenderMode.value, 'path_tracing');
  await host.selectMainRenderMode('native');
  assert.equal(host.mainRenderModeLabel.value, 'Native');
});

test('failed main viewport selection restores the previous displayed mode', async () => {
  const host = mainHost({ fail: true });
  assert.equal(await host.selectMainRenderMode('restir'), false);
  assert.equal(host.mainRenderBackend.value, 'native');
  assert.equal(host.mainVisionRenderMode.value, 'path_tracing');
  assert.equal(host.mainRenderModeLabel.value, 'Native');
});

test('queued native mode acknowledgement keeps the requested ReSTIR label', async () => {
  const host = mainHost({ pending: true });
  assert.equal(await host.selectMainRenderMode('restir'), true);
  assert.equal(host.mainVisionRenderMode.value, 'restir');
});

test('detached camera selects ReSTIR and stores the returned camera mode', async () => {
  const source = declarations('../../src/views/tools/CameraView.vue', ['selectVisionRenderMode']);
  const calls = [];
  const host = new Function('ref', 'editorApi', `
    const sceneId = 'test.scene', cameraId = 'camera-3';
    const visionModeMenuOpen = ref(true), visionRenderMode = ref('path_tracing');
    const backend = ref('vision'), outputMode = ref('final_color'), errorText = ref('');
    const camera = ref({ vision_render_mode: 'path_tracing' });
    const unwrap = value => value?.data ?? value;
    ${source}
    return { selectVisionRenderMode, visionRenderMode, camera, visionModeMenuOpen };
  `)(ref, { sceneTools: { setVisionRenderMode: async (...args) => {
    calls.push(args); return { data: { mode: 'path_tracing', pending: true } };
  } } });
  await host.selectVisionRenderMode('restir');
  assert.deepEqual(calls, [['test.scene', 'camera-3', 'restir']]);
  assert.equal(host.camera.value.vision_render_mode, 'restir');
  assert.equal(host.visionRenderMode.value, 'restir');
  assert.equal(host.visionModeMenuOpen.value, false);
});

const cameraSnapshot = (fields = {}, cameraId = 'camera-2') => ({
  scene_id: 'test.scene', active_camera_name: cameraId,
  cameras: [{ camera_id: cameraId, name: cameraId, handle: cameraId === 'camera-2' ? 2 : 3,
    render_backend: 'vision', vision_render_mode: 'path_tracing', ...fields }],
});

function detachedHost({ fields = {}, fail = false, pending = false, accumulationFail = false } = {}) {
  const calls = [];
  const source = declarations('../../src/views/tools/CameraView.vue', [
    'camera', 'cameraName', 'backend', 'visionRenderMode', 'visionDenoise', 'visionDenoiseBusy',
    'outputMode', 'shadowCascadeDebug', 'ssaoEnabled', 'moveSpeed', 'renderWidth', 'renderHeight',
    'visionAvailable', 'errorText', 'visionModeMenuOpen', 'loadCamera', 'selectVisionRenderMode',
    'toggleVisionDenoise',
    'visionAccumulation', 'visionAccumulationBusy', 'toggleVisionAccumulation',
  ]);
  const host = new Function('ref', 'visionModes', 'editorApi', `
    const { normalizeVisionRenderMode, visionDenoiseFromCamera, visionAccumulationFromCamera } = visionModes;
    const sceneId = 'test.scene', cameraId = 'camera-3', unwrap = value => value?.data ?? value;
    ${source}
    return { loadCamera, selectVisionRenderMode, camera, backend, visionRenderMode, errorText,
      visionDenoise, toggleVisionDenoise,
      visionAccumulation: typeof visionAccumulation === 'undefined' ? undefined : visionAccumulation,
      toggleVisionAccumulation: typeof toggleVisionAccumulation === 'undefined' ? undefined : toggleVisionAccumulation };
  `)(ref, visionModes, { sceneTools: {
    listCameraViews: async () => ({ data: { cameras: cameraSnapshot(fields, 'camera-3').cameras } }),
    isVisionAvailable: async () => ({ available: true }),
    setVisionRenderMode: async (...args) => { calls.push(['mode', ...args]); return { mode: args[2] }; },
    setVisionDenoise: async (...args) => {
      calls.push(['denoise', ...args]);
      if (fail) throw new Error('denoiser unavailable');
      return { data: { enabled: pending ? !args[2] : args[2], pending } };
    },
    setVisionAccumulation: async (...args) => {
      calls.push(['accumulation', ...args]);
      if (accumulationFail) throw new Error('accumulation unavailable');
      return { data: { enabled: pending ? !args[2] : args[2], pending } };
    },
  } });
  return { ...host, calls };
}

test('algorithm menus keep accumulation and SVGF as separate settings', () => {
  const host = mainHost();
  assert.equal(host.mainRenderModeOptions.some(mode => mode.value === 'svgf'), false);
  assert.deepEqual(visionRenderModes.map(mode => mode.value), [
    'path_tracing', 'restir', 'ssat',
  ]);
});

for (const [name, fields, mode, enabled] of [
  ['new camera defaults off', {}, 'path_tracing', false],
  ['ReSTIR persisted on', { vision_render_mode: 'restir', vision_denoise: true }, 'restir', true],
  ['string true', { vision_denoise: 'true' }, 'path_tracing', true],
  ['string false', { vision_denoise: 'false' }, 'path_tracing', false],
  ['legacy SVGF', { vision_render_mode: 'svgf' }, 'path_tracing', true],
  ['explicit false overrides legacy', { vision_render_mode: 'svgf', vision_denoise: false }, 'path_tracing', false],
  ['string false overrides legacy', { vision_render_mode: 'svgf', vision_denoise: 'false' }, 'path_tracing', false],
]) {
  test(`main and detached cameras read ${name}`, async () => {
    const main = mainHost();
    main.applySceneSnapshot('test.scene', cameraSnapshot(fields));
    const detached = detachedHost({ fields });
    await detached.loadCamera();
    assert.equal(main.mainVisionRenderMode.value, mode);
    assert.equal(main.mainVisionDenoise.value, enabled);
    assert.equal(detached.visionRenderMode.value, mode);
    assert.equal(detached.visionDenoise.value, enabled);
  });
}

test('main denoise toggles the bound camera and survives queued snapshots and algorithm changes', async () => {
  const host = mainHost({ pending: true });
  host.applySceneSnapshot('test.scene', cameraSnapshot());
  host.tabs.value = [{ id: 'stale-tab.scene' }];
  assert.equal(typeof host.toggleMainVisionDenoise, 'function');
  assert.equal(await host.toggleMainVisionDenoise(), true);
  assert.deepEqual(host.calls, [['denoise', 'test.scene', 'camera-2', true]]);
  assert.equal(host.mainVisionDenoise.value, true);
  assert.equal(host.currentMainCamera.value.vision_denoise, true);
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_denoise: false }));
  assert.equal(host.mainVisionDenoise.value, true);
  for (const mode of ['restir', 'path_tracing', 'ssat']) {
    await host.selectMainRenderMode(mode);
    assert.equal(host.mainVisionDenoise.value, true);
  }
  assert.equal(await host.toggleMainVisionDenoise(), false);
  assert.equal(host.calls.filter(call => call[0] === 'denoise').length, 1);
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_render_mode: 'restir', vision_denoise: false }, 'camera-3'));
  assert.equal(host.mainVisionRenderMode.value, 'restir');
  assert.equal(host.mainVisionDenoise.value, false);
});

test('main denoise failure restores its snapshot and displays the error', async () => {
  const host = mainHost({ denoiseFail: true });
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_denoise: false }));
  assert.equal(typeof host.toggleMainVisionDenoise, 'function');
  assert.equal(await host.toggleMainVisionDenoise(), false);
  assert.equal(host.mainVisionDenoise.value, false);
  assert.equal(host.currentMainCamera.value.vision_denoise, false);
  assert.match(host.mainVisionDenoiseError.value, /denoiser unavailable/);
});

test('native cameras cannot implicitly enable Vision by toggling denoise', async () => {
  const main = mainHost();
  main.applySceneSnapshot('test.scene', cameraSnapshot({ render_backend: 'native' }));
  const detached = detachedHost({ fields: { render_backend: 'native' } });
  await detached.loadCamera();
  assert.equal(typeof main.toggleMainVisionDenoise, 'function');
  assert.equal(typeof detached.toggleVisionDenoise, 'function');
  await main.toggleMainVisionDenoise();
  await detached.toggleVisionDenoise();
  assert.deepEqual(main.calls, []);
  assert.deepEqual(detached.calls, []);
  assert.equal(main.mainRenderBackend.value, 'native');
  assert.equal(detached.backend.value, 'native');
});

test('detached denoise is retained across PT, ReSTIR and SSAT and accepts queued acknowledgements', async () => {
  const host = detachedHost({ pending: true });
  await host.loadCamera();
  assert.equal(typeof host.toggleVisionDenoise, 'function');
  assert.equal(await host.toggleVisionDenoise(), true);
  assert.deepEqual(host.calls[0], ['denoise', 'test.scene', 'camera-3', true]);
  assert.equal(host.visionDenoise.value, true);
  assert.equal(host.camera.value.vision_denoise, true);
  for (const mode of ['restir', 'path_tracing', 'ssat']) {
    await host.selectVisionRenderMode(mode);
    assert.equal(host.visionDenoise.value, true);
    assert.equal(host.camera.value.vision_denoise, true);
  }
  assert.equal(await host.toggleVisionDenoise(), false);
  assert.equal(host.calls.filter(call => call[0] === 'denoise').length, 1);
});

test('detached denoise failure restores the old value and reports the error', async () => {
  const host = detachedHost({ fields: { vision_denoise: true }, fail: true });
  await host.loadCamera();
  assert.equal(typeof host.toggleVisionDenoise, 'function');
  assert.equal(await host.toggleVisionDenoise(), false);
  assert.equal(host.visionDenoise.value, true);
  assert.equal(host.camera.value.vision_denoise, true);
  assert.match(host.errorText.value, /denoiser unavailable/);
});

test('progressive PT allows SVGF on and off in both viewports', async () => {
  const fields = { vision_render_mode: 'progressive_path_tracing' };
  const main = mainHost();
  main.applySceneSnapshot('test.scene', cameraSnapshot(fields));
  const detached = detachedHost({ fields });
  await detached.loadCamera();
  for (const enabled of [true, false]) {
    assert.equal(await main.toggleMainVisionDenoise(), true);
    assert.equal(await detached.toggleVisionDenoise(), true);
    assert.equal(main.currentMainCamera.value.vision_denoise, enabled);
    assert.equal(detached.camera.value.vision_denoise, enabled);
  }
});

test('a late main denoise response cannot modify the newly bound camera', async () => {
  let reply;
  const host = mainHost({ denoiseReply: () => new Promise(resolve => { reply = resolve; }) });
  host.applySceneSnapshot('test.scene', cameraSnapshot());
  const request = host.toggleMainVisionDenoise();
  assert.equal(host.mainVisionDenoise.value, true);
  assert.equal(await host.toggleMainVisionDenoise(), false);
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_denoise: false }, 'camera-3'));
  reply({ enabled: true, pending: true });
  assert.equal(await request, true);
  assert.equal(host.mainVisionDenoise.value, false);
  assert.equal(host.currentMainCamera.value.camera_id, 'camera-3');
  assert.equal(host.currentMainCamera.value.vision_denoise, false);
});

for (const [name, fields, mode, enabled] of [
  ['new camera defaults off', {}, 'path_tracing', false],
  ['PT persisted on', { vision_accumulation: true }, 'path_tracing', true],
  ['ReSTIR persisted on', { vision_render_mode: 'restir', vision_accumulation: true }, 'restir', true],
  ['string true', { vision_accumulation: 'true' }, 'path_tracing', true],
  ['string false', { vision_accumulation: 'false' }, 'path_tracing', false],
  ['legacy progressive PT', { vision_render_mode: 'progressive_path_tracing' }, 'path_tracing', true],
  ['explicit false overrides legacy', { vision_render_mode: 'progressive_path_tracing', vision_accumulation: false }, 'path_tracing', false],
  ['string false overrides legacy', { vision_render_mode: 'progressive_path_tracing', vision_accumulation: 'false' }, 'path_tracing', false],
]) {
  test(`main and detached cameras read accumulation: ${name}`, async () => {
    const main = mainHost();
    main.applySceneSnapshot('test.scene', cameraSnapshot(fields));
    const detached = detachedHost({ fields });
    await detached.loadCamera();
    assert.equal(main.mainVisionRenderMode.value, mode);
    assert.equal(detached.visionRenderMode.value, mode);
    assert.equal(main.mainVisionAccumulation?.value, enabled);
    assert.equal(detached.visionAccumulation?.value, enabled);
    assert.equal(main.currentMainCamera.value.vision_accumulation, enabled);
    assert.equal(detached.camera.value.vision_accumulation, enabled);
  });
}

test('both viewports independently toggle accumulation and SVGF in PT and ReSTIR', async () => {
  const main = mainHost({ pending: true });
  main.applySceneSnapshot('test.scene', cameraSnapshot());
  main.tabs.value = [{ id: 'stale-tab.scene' }];
  const detached = detachedHost({ pending: true });
  await detached.loadCamera();
  assert.equal(typeof main.toggleMainVisionAccumulation, 'function');
  assert.equal(typeof detached.toggleVisionAccumulation, 'function');
  for (const mode of ['path_tracing', 'restir']) {
    await main.selectMainRenderMode(mode);
    await detached.selectVisionRenderMode(mode);
    for (const enabled of [true, false]) {
      assert.equal(await main.toggleMainVisionAccumulation(), true);
      assert.equal(await detached.toggleVisionAccumulation(), true);
      assert.equal(main.mainVisionAccumulation.value, enabled);
      assert.equal(detached.visionAccumulation.value, enabled);
      assert.equal(main.currentMainCamera.value.vision_accumulation, enabled);
      assert.equal(detached.camera.value.vision_accumulation, enabled);
      assert.equal(main.mainVisionDenoise.value, false);
      assert.equal(detached.visionDenoise.value, false);
    }
    await main.toggleMainVisionDenoise();
    await detached.toggleVisionDenoise();
    assert.equal(main.mainVisionAccumulation.value, false);
    assert.equal(detached.visionAccumulation.value, false);
    await main.toggleMainVisionDenoise();
    await detached.toggleVisionDenoise();
  }
  assert.deepEqual(main.calls.filter(call => call[0] === 'accumulation'), [
    ['accumulation', 'test.scene', 'camera-2', true], ['accumulation', 'test.scene', 'camera-2', false],
    ['accumulation', 'test.scene', 'camera-2', true], ['accumulation', 'test.scene', 'camera-2', false],
  ]);
  assert.deepEqual(detached.calls.filter(call => call[0] === 'accumulation'), [
    ['accumulation', 'test.scene', 'camera-3', true], ['accumulation', 'test.scene', 'camera-3', false],
    ['accumulation', 'test.scene', 'camera-3', true], ['accumulation', 'test.scene', 'camera-3', false],
  ]);
});

test('main accumulation survives queued snapshots, mode changes and preserves camera scope', async () => {
  const host = mainHost({ pending: true });
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_denoise: true }));
  assert.equal(typeof host.toggleMainVisionAccumulation, 'function');
  await host.toggleMainVisionAccumulation();
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_accumulation: false, vision_denoise: true }));
  assert.equal(host.mainVisionAccumulation.value, true);
  for (const mode of ['restir', 'path_tracing', 'ssat', 'native']) {
    await host.selectMainRenderMode(mode);
    assert.equal(host.mainVisionAccumulation.value, true);
    assert.equal(host.mainVisionDenoise.value, true);
    if (mode === 'ssat' || mode === 'native') assert.equal(await host.toggleMainVisionAccumulation(), false);
  }
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_render_mode: 'restir', vision_accumulation: false }, 'camera-3'));
  assert.equal(host.mainVisionAccumulation.value, false);
  assert.equal(host.mainVisionDenoise.value, false);
});

for (const fields of [{ render_backend: 'native' }, { vision_render_mode: 'ssat' }]) {
  test(`both toggles are disabled but preserved for ${fields.render_backend || fields.vision_render_mode}`, async () => {
    const settings = { ...fields, vision_accumulation: true, vision_denoise: true };
    const main = mainHost();
    main.applySceneSnapshot('test.scene', cameraSnapshot(settings));
    const detached = detachedHost({ fields: settings });
    await detached.loadCamera();
    assert.equal(typeof main.toggleMainVisionAccumulation, 'function');
    assert.equal(typeof detached.toggleVisionAccumulation, 'function');
    assert.equal(await main.toggleMainVisionAccumulation(), false);
    assert.equal(await detached.toggleVisionAccumulation(), false);
    assert.equal(await main.toggleMainVisionDenoise(), false);
    assert.equal(await detached.toggleVisionDenoise(), false);
    assert.equal(main.mainVisionAccumulation.value, true);
    assert.equal(detached.visionAccumulation.value, true);
    assert.equal(main.mainVisionDenoise.value, true);
    assert.equal(detached.visionDenoise.value, true);
    assert.deepEqual(main.calls, []);
    assert.deepEqual(detached.calls, []);
  });
}

test('failed accumulation changes restore each camera value and show the error', async () => {
  const fields = { vision_accumulation: true, vision_denoise: true };
  const main = mainHost({ accumulationFail: true });
  main.applySceneSnapshot('test.scene', cameraSnapshot(fields));
  const detached = detachedHost({ fields, accumulationFail: true });
  await detached.loadCamera();
  assert.equal(typeof main.toggleMainVisionAccumulation, 'function');
  assert.equal(typeof detached.toggleVisionAccumulation, 'function');
  assert.equal(await main.toggleMainVisionAccumulation(), false);
  assert.equal(await detached.toggleVisionAccumulation(), false);
  assert.equal(main.mainVisionAccumulation.value, true);
  assert.equal(detached.visionAccumulation.value, true);
  assert.equal(main.currentMainCamera.value.vision_accumulation, true);
  assert.equal(detached.camera.value.vision_accumulation, true);
  assert.match(main.mainVisionAccumulationError.value, /accumulation unavailable/);
  assert.match(detached.errorText.value, /accumulation unavailable/);
});

test('a late accumulation response cannot modify the newly bound camera', async () => {
  let reply;
  const host = mainHost({ accumulationReply: () => new Promise(resolve => { reply = resolve; }) });
  host.applySceneSnapshot('test.scene', cameraSnapshot());
  assert.equal(typeof host.toggleMainVisionAccumulation, 'function');
  const request = host.toggleMainVisionAccumulation();
  assert.equal(host.mainVisionAccumulation.value, true);
  assert.equal(await host.toggleMainVisionAccumulation(), false);
  host.applySceneSnapshot('test.scene', cameraSnapshot({ vision_accumulation: false }, 'camera-3'));
  reply({ enabled: true, pending: true });
  assert.equal(await request, true);
  assert.equal(host.mainVisionAccumulation.value, false);
  assert.equal(host.currentMainCamera.value.camera_id, 'camera-3');
  assert.equal(host.currentMainCamera.value.vision_accumulation, false);
});
