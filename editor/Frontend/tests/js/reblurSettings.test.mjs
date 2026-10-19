import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, reactive } from 'vue';
import { reblurDefaults, validateReblurSettings, useReblurSettings } from '../../src/utils/reblurSettings.js';

const settle = () => new Promise(resolve => setImmediate(resolve));
function fixture(api = {}) {
  const props = reactive({ sceneId: 'kitchen', cameraId: 'camera-a', enabled: true });
  const scope = effectScope();
  const sent = [];
  const state = scope.run(() => useReblurSettings(props, {
    getReblurSettings: async () => ({ settings: { max_history: 18, anti_firefly: false } }),
    setReblurSettings: async (...args) => { sent.push(args); return { settings: args[2], pending: true }; },
    ...api,
  }));
  return { props, state, sent, stop: () => scope.stop() };
}

test('loads effective values and applies an independent camera preference with denoising disabled', async t => {
  const f = fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(f.state.draft.value.max_history, 18);
  assert.equal(f.state.draft.value.anti_firefly, false);
  f.state.draft.value.max_history = 12;
  f.state.draft.value.reconstruction = 2;
  assert.equal(await f.state.apply(), true);
  assert.equal(f.sent[0][0], 'kitchen');
  assert.equal(f.sent[0][1], 'camera-a');
  assert.equal(f.sent[0][2].max_history, 12);
  assert.equal(f.sent[0][2].reconstruction, 2);
  assert.equal('enabled' in f.sent[0][2], false);
  assert.equal(f.state.saved.value, true);
});

test('rejects invalid and inconsistent input before sending changes', async t => {
  const f = fixture(); t.after(f.stop);
  for (const patch of [{ max_history: '' }, { diffuse_prepass: NaN }, { stabilization: 2 },
    { fast_history: 31 }, { min_radius: 31 }, { reconstruction: 1.5 }]) {
    f.state.draft.value = { ...reblurDefaults, ...patch };
    assert.equal(await f.state.apply(), false);
    assert.notEqual(f.state.error.value, '');
  }
  assert.equal(f.sent.length, 0);
});

test('unwraps native CEF responses when loading and applying parameters', async t => {
  const f = fixture({
    getReblurSettings: async () => ({ success: true, data: { settings: { max_history: 22, stabilization: 0.5,
      lobe_fraction: 0.15000000596046448, plane_sensitivity: 0.019999999552965164 } } }),
    setReblurSettings: async () => ({ success: true, data: { settings: { max_history: 20 }, pending: true } }),
  });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(f.state.draft.value.max_history, 22);
  assert.equal(f.state.draft.value.stabilization, 0.5);
  assert.equal(f.state.draft.value.lobe_fraction, 0.15);
  assert.equal(f.state.draft.value.plane_sensitivity, 0.02);
  assert.equal(await f.state.apply(), true);
  assert.equal(f.state.draft.value.max_history, 20);
  assert.equal(f.state.draft.value.stabilization, 0.5);
});

test('a late load cannot overwrite a newly bound camera', async t => {
  let finish;
  const f = fixture({ getReblurSettings: (_, camera) => camera === 'camera-a'
    ? new Promise(resolve => { finish = resolve; }) : Promise.resolve({ settings: { max_history: 40 } }) });
  t.after(f.stop);
  f.state.open.value = true;
  f.props.cameraId = 'camera-b'; await settle();
  finish({ settings: { max_history: 9 } }); await settle();
  assert.equal(f.state.draft.value.max_history, 40);
  assert.equal(f.state.busy.value, false);
});

test('a late save cannot affect a newly bound camera or closed component', async t => {
  let finish;
  const f = fixture({ setReblurSettings: () => new Promise(resolve => { finish = resolve; }) });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  const pending = f.state.apply();
  f.props.cameraId = 'camera-b'; await settle();
  finish({ settings: { max_history: 9 } });
  assert.equal(await pending, false);
  assert.equal(f.state.draft.value.max_history, 18);
  assert.equal(f.state.saved.value, false);
});

test('failed requests preserve the draft and reset only updates the draft until apply', async t => {
  const f = fixture({ setReblurSettings: async () => { throw new Error('renderer unavailable'); } });
  t.after(f.stop);
  f.state.draft.value.max_history = 21;
  assert.equal(await f.state.apply(), false);
  assert.equal(f.state.draft.value.max_history, 21);
  assert.match(f.state.error.value, /renderer unavailable/);
  f.state.reset();
  assert.equal(f.state.draft.value.max_history, 30);
  assert.equal(f.sent.length, 0);
  assert.equal(validateReblurSettings(f.state.draft.value), '');
});

test('switching away from REBLUR closes the panel and disables changes', async t => {
  const f = fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  f.props.enabled = false;
  assert.equal(f.state.open.value, false);
  assert.equal(await f.state.apply(), false);
  assert.equal(f.sent.length, 0);
});
