import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import { createSSRApp, effectScope, reactive } from 'vue';
import { parse } from 'vue/compiler-sfc';
import { compile } from '@vue/compiler-ssr';
import { renderToString } from '@vue/server-renderer';
import { ptDefaults, ptGroups, usePtSettings } from '../../src/utils/ptSettings.js';

const { descriptor } = parse(fs.readFileSync(new URL('../../src/components/ui/PtSettings.vue', import.meta.url), 'utf8'));
const { code } = compile(descriptor.template.content, { mode: 'function' });
const ssrRender = new Function('require', code)(createRequire(import.meta.url));
async function renderPanel(f) {
  const context = {};
  const app = createSSRApp({ ssrRender, setup: () => ({ ...f.props, ...f.state, ptGroups, trigger: null }) });
  const html = await renderToString(app, context);
  return html + (context.teleports?.body || '');
}

const settle = () => new Promise(resolve => setImmediate(resolve));
function fixture(api = {}) {
  const props = reactive({ sceneId: 'room', cameraId: 'camera-a', enabled: true });
  const scope = effectScope();
  const sent = [];
  const state = scope.run(() => usePtSettings(props, {
    getPtSettings: async () => ({ success: true, data: {
      settings: { max_depth: 12, min_depth: 3, rr_threshold: 0.75 },
      adaptive: { enabled: false, threshold: 0.002, start_index: 256 },
    } }),
    setPtSettings: async (...args) => { sent.push(args); return { success: true, data: { settings: args[2], pending: true } }; },
    ...api,
  }));
  return { props, state, sent, stop: () => scope.stop() };
}

test('loads effective PT values and submits only working parameters to the bound camera', async t => {
  const f = fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.deepEqual(f.state.draft.value, { max_depth: 12, min_depth: 3, rr_threshold: 0.75 });
  assert.deepEqual(f.state.adaptive.value, { enabled: false, threshold: 0.002, start_index: 256 });
  f.state.draft.value.max_depth = 8;
  f.state.draft.value.rr_threshold = 0;
  assert.equal(await f.state.apply(), true);
  assert.deepEqual(f.sent, [['room', 'camera-a', { max_depth: 8, min_depth: 3, rr_threshold: 0 }]]);
  assert.equal(f.state.saved.value, true);
});

test('rejects non-finite, fractional and out-of-range parameters before submitting', async t => {
  const f = fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  for (const patch of [{ max_depth: '' }, { max_depth: 129 }, { max_depth: 1.5 },
    { min_depth: -1 }, { min_depth: 21 }, { min_depth: 2.5 },
    { rr_threshold: NaN }, { rr_threshold: Infinity }, { rr_threshold: 1.1 }]) {
    f.state.draft.value = { ...ptDefaults, ...patch };
    assert.equal(await f.state.apply(), false);
    assert.notEqual(f.state.error.value, '');
  }
  assert.equal(f.sent.length, 0);
});

test('preserves ImGui zero-depth and independent RR-depth boundaries', async t => {
  const f = fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  f.state.draft.value = { max_depth: 0, min_depth: 20, rr_threshold: 0 };
  assert.equal(await f.state.apply(), true);
  assert.deepEqual(f.sent[0][2], { max_depth: 0, min_depth: 20, rr_threshold: 0 });
});

test('loading then applying a float32 threshold preserves its actual rendering value', async t => {
  const threshold = Math.fround(0.12345678);
  const f = fixture({ getPtSettings: async () => ({ settings: { rr_threshold: threshold } }) });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(await f.state.apply(), true);
  assert.equal(Math.fround(f.sent[0][2].rr_threshold), threshold);
});

test('reset changes only the draft and failed saves preserve edits', async t => {
  const f = fixture({ setPtSettings: async () => { throw new Error('renderer unavailable'); } });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  f.state.draft.value.max_depth = 9;
  assert.equal(await f.state.apply(), false);
  assert.equal(f.state.draft.value.max_depth, 9);
  assert.match(f.state.error.value, /renderer unavailable/);
  f.state.reset();
  assert.deepEqual(f.state.draft.value, { max_depth: 16, min_depth: 5, rr_threshold: 1 });
  assert.equal(f.sent.length, 0);
});

test('late loads and saves cannot overwrite a different camera', async t => {
  let finishLoad, finishSave;
  const f = fixture({
    getPtSettings: (_, camera) => camera === 'camera-a'
      ? new Promise(resolve => { finishLoad = resolve; })
      : Promise.resolve({ settings: { max_depth: 6 } }),
    setPtSettings: () => new Promise(resolve => { finishSave = resolve; }),
  });
  t.after(f.stop);
  f.state.open.value = true;
  f.props.cameraId = 'camera-b'; await settle();
  finishLoad({ settings: { max_depth: 99 } }); await settle();
  assert.equal(f.state.draft.value.max_depth, 6);
  const pending = f.state.apply();
  f.props.cameraId = 'camera-c'; await settle();
  finishSave({ settings: { max_depth: 98 } });
  assert.equal(await pending, false);
  assert.equal(f.state.draft.value.max_depth, 6);
  assert.equal(f.state.saved.value, false);
});

test('switching away from PT closes the panel and blocks changes', async t => {
  const f = fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  f.props.enabled = false;
  assert.equal(f.state.open.value, false);
  assert.equal(await f.state.apply(), false);
  assert.equal(f.sent.length, 0);
});

test('waits for actual scene settings before allowing an initial apply', async t => {
  let reads = 0;
  const f = fixture({ getPtSettings: async () => ++reads === 1
    ? { settings: {}, ready: false }
    : { settings: { max_depth: 2, min_depth: 5, rr_threshold: 1 }, ready: true } });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(await f.state.apply(), false);
  assert.equal(f.sent.length, 0);
  await new Promise(resolve => setTimeout(resolve, 200));
  assert.equal(f.state.draft.value.max_depth, 2);
  assert.equal(await f.state.apply(), true);
  assert.equal(f.sent[0][2].max_depth, 2);
});

test('a failed initial read cannot submit fallback defaults as scene settings', async t => {
  const f = fixture({ getPtSettings: async () => { throw new Error('not ready'); } });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.match(f.state.error.value, /not ready/);
  assert.equal(await f.state.apply(), false);
  assert.equal(f.sent.length, 0);
});

test('closing a panel waiting for its first GPU view cancels further reads', async t => {
  let reads = 0;
  const f = fixture({ getPtSettings: async () => { reads++; return { ready: false, settings: {} }; } });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  f.state.open.value = false;
  await new Promise(resolve => setTimeout(resolve, 200));
  assert.equal(reads, 1);
  assert.equal(await f.state.apply(), false);
});

test('the actual panel renders the three PT inputs and disables the adaptive controls', async t => {
  const f = fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  const html = await renderPanel(f);
  assert.match(html, /<input(?=[^>]*aria-label="最大路径深度")(?=[^>]*value="12")(?=[^>]*max="128")[^>]*>/);
  assert.match(html, /<input(?=[^>]*aria-label="RR 起始深度")(?=[^>]*value="3")[^>]*>/);
  assert.match(html, /<input(?=[^>]*aria-label="RR 阈值")(?=[^>]*value="0.75")[^>]*>/);
  assert.match(html, /<fieldset disabled aria-label="自适应采样（暂不可用）">/);
  for (const label of ['启用自适应采样', '收敛阈值', '最少样本数']) assert.ok(html.includes(`aria-label="${label}"`));
  assert.doesNotMatch(html, /<button type="submit" disabled/);
});

test('the actual panel blocks submission while waiting for scene parameters', async t => {
  const f = fixture({ getPtSettings: async () => ({ ready: false, settings: {} }) });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  const html = await renderPanel(f);
  assert.match(html, /正在读取当前相机参数/);
  assert.match(html, /<button type="submit" disabled/);
});
