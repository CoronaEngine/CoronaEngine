import assert from 'node:assert/strict';
import test from 'node:test';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import { setImmediate } from 'node:timers';
import { createSSRApp, effectScope, reactive } from 'vue';
import { parse } from 'vue/compiler-sfc';
import { compile } from '@vue/compiler-ssr';
import { renderToString } from '@vue/server-renderer';

const moduleUrl = new URL('../../src/utils/restirSettings.js', import.meta.url);
const componentUrl = new URL('../../src/components/ui/RestirSettings.vue', import.meta.url);
const expectedDefaults = {
  max_depth: 15, min_depth: 5, rr_threshold: 1,
  direct: { open: true, M_light: 10, M_bsdf: 1, max_age: 30, max_recursion: 5,
    diffuse_factor: 0.3, pairwise: true,
    temporal: { open: true, history_limit: 5, theta: 35, depth: 0.3, radius: 8 },
    spatial: { open: true, theta: 35, depth: 0.3, radius: 30 } },
  indirect: { open: true, max_age: 30, diffuse_factor: 0.3,
    temporal: { open: true, history_limit: 5, theta: 35, depth: 0.3, radius: 8 },
    spatial: { open: true, theta: 35, depth: 0.3, radius: 12 } },
};
const fresh = () => structuredClone(expectedDefaults);
const settle = () => new Promise(resolve => setImmediate(resolve));
const delay = () => new Promise(resolve => setTimeout(resolve, 200));
async function fixture(api = {}) {
  assert.ok(fs.existsSync(moduleUrl), 'ReSTIR must provide its own settings state module');
  const module = await import(moduleUrl.href);
  const props = reactive({ sceneId: 'room', cameraId: 'camera-a', enabled: true });
  const scope = effectScope(), sent = [], reads = [];
  const state = scope.run(() => module.useRestirSettings(props, {
    getRestirSettings: async (...args) => { reads.push(args); return { success: true, data: { settings: fresh(), ready: true } }; },
    setRestirSettings: async (...args) => { sent.push(args); return { success: true, data: { settings: args[2], pending: true } }; },
    ...api,
  }));
  return { props, state, sent, reads, module, stop: () => scope.stop() };
}
async function renderPanel(f) {
  assert.ok(fs.existsSync(componentUrl), 'ReSTIR must provide a separate panel');
  const { descriptor } = parse(fs.readFileSync(componentUrl, 'utf8'));
  const { code } = compile(descriptor.template.content, { mode: 'function' });
  const ssrRender = new Function('require', code)(createRequire(import.meta.url));
  const context = {};
  const app = createSSRApp({ ssrRender, setup: () => ({ ...f.props, ...f.state, ...f.module, trigger: null }) });
  return await renderToString(app, context) + (context.teleports?.body || '');
}

test('loads lazily and sends complete nested settings only to the bound camera', async t => {
  const f = await fixture(); t.after(f.stop);
  assert.equal(f.reads.length, 0);
  assert.equal(await f.state.apply(), false);
  f.state.open.value = true; await settle();
  assert.deepEqual(f.state.draft.value, expectedDefaults);
  f.state.draft.value.direct.temporal.radius = 9.5;
  f.state.draft.value.direct.open = false;
  f.state.draft.value.indirect.spatial.open = false;
  f.state.draft.value.indirect.ratio = 4;
  f.state.draft.value.stable_planes = true;
  assert.equal(await f.state.apply(), true);
  const expected = fresh();
  expected.direct.temporal.radius = 9.5;
  expected.direct.open = false;
  expected.indirect.spatial.open = false;
  assert.deepEqual(f.sent, [['room', 'camera-a', expected]]);
  assert.equal(f.state.saved.value, true);
});

test('validates all nested number ranges, integer fields, finite values and strict booleans', async t => {
  const f = await fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  const invalid = [
    ['max_depth', 129], ['max_depth', 1.5], ['min_depth', 21], ['rr_threshold', '0.5'],
    ['rr_threshold', NaN], ['direct.M_light', -1], ['direct.M_light', 101],
    ['direct.M_bsdf', 1.5], ['direct.max_age', 101], ['direct.max_recursion', 0],
    ['direct.max_recursion', 101], ['direct.diffuse_factor', 1.1], ['direct.pairwise', 1],
    ['direct.open', 'false'], ['indirect.open', null], ['indirect.max_age', -1],
    ['indirect.diffuse_factor', Infinity], ['direct.temporal.history_limit', 51],
    ['direct.temporal.history_limit', 1.5], ['indirect.temporal.theta', 91],
    ['direct.spatial.depth', -0.1], ['indirect.spatial.radius', 50.1],
    ['indirect.temporal.open', 0], ['direct.spatial.open', 'true'], ['indirect.temporal', null],
  ];
  for (const [path, value] of invalid) {
    f.state.draft.value = fresh();
    const keys = path.split('.'), key = keys.pop();
    keys.reduce((object, name) => object[name], f.state.draft.value)[key] = value;
    assert.equal(await f.state.apply(), false, path);
    assert.notEqual(f.state.error.value, '', path);
  }
  assert.equal(f.sent.length, 0);
  f.state.draft.value = fresh();
  Object.assign(f.state.draft.value, { max_depth: 0, min_depth: 20, rr_threshold: 0 });
  Object.assign(f.state.draft.value.direct, { M_light: 0, M_bsdf: 100, max_age: 0, max_recursion: 1, diffuse_factor: 0, pairwise: false });
  Object.assign(f.state.draft.value.indirect, { max_age: 100, diffuse_factor: 1 });
  Object.assign(f.state.draft.value.direct.temporal, { history_limit: 0, theta: 90, depth: 1, radius: 50 });
  assert.equal(await f.state.apply(), true);
});

test('nested load merging, reset and per-panel drafts never share mutable defaults', async t => {
  const f = await fixture({ getRestirSettings: async () => ({ settings: { direct: { temporal: { radius: 4 } } }, ready: true }) });
  const other = await fixture(); t.after(f.stop); t.after(other.stop);
  f.state.open.value = true; other.state.open.value = true; await settle();
  assert.equal(f.state.draft.value.direct.temporal.radius, 4);
  assert.equal(f.state.draft.value.direct.temporal.theta, 35);
  f.state.draft.value.indirect.spatial.radius = 48;
  assert.equal(other.state.draft.value.indirect.spatial.radius, 12);
  f.state.reset();
  assert.deepEqual(f.state.draft.value, expectedDefaults);
  assert.equal(f.sent.length, 0);
});

test('late loads and saves cannot overwrite a newly bound camera or scene', async t => {
  let finishLoad, finishSave;
  const f = await fixture({
    getRestirSettings: (_, camera) => camera === 'camera-a' ? new Promise(resolve => { finishLoad = resolve; })
      : Promise.resolve({ settings: { direct: { M_light: 6 } }, ready: true }),
    setRestirSettings: () => new Promise(resolve => { finishSave = resolve; }),
  }); t.after(f.stop);
  f.state.open.value = true;
  f.props.cameraId = 'camera-b'; await settle();
  finishLoad({ settings: { direct: { M_light: 99 } }, ready: true }); await settle();
  assert.equal(f.state.draft.value.direct.M_light, 6);
  const pending = f.state.apply();
  f.props.sceneId = 'new-room'; await settle();
  finishSave({ settings: { direct: { M_light: 98 } }, pending: true });
  assert.equal(await pending, false);
  assert.equal(f.state.draft.value.direct.M_light, 6);
  assert.equal(f.state.saved.value, false);
});

test('waits for ready before applying and cancels polling when camera changes or panel closes', async t => {
  let reads = 0;
  const f = await fixture({ getRestirSettings: async () => ++reads === 1
    ? { settings: {}, ready: false } : { settings: { max_depth: 2 }, ready: true } }); t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(await f.state.apply(), false);
  await delay();
  assert.equal(f.state.draft.value.max_depth, 2);
  assert.equal(await f.state.apply(), true);
  assert.equal(f.sent[0][2].max_depth, 2);
  const waiting = await fixture({ getRestirSettings: async (...args) => {
    waiting.reads.push(args); return { settings: {}, ready: false };
  } }); t.after(waiting.stop);
  waiting.state.open.value = true; await settle();
  waiting.props.cameraId = 'camera-b'; await settle();
  waiting.state.open.value = false;
  await delay();
  assert.deepEqual(waiting.reads, [['room', 'camera-a'], ['room', 'camera-b']]);
  assert.equal(await waiting.state.apply(), false);
});

test('blocks duplicate applies and preserves edits after server errors', async t => {
  let finish;
  const f = await fixture({ setRestirSettings: (...args) => { f.sent.push(args); return new Promise(resolve => { finish = resolve; }); } });
  t.after(f.stop);
  f.state.open.value = true; await settle();
  f.state.draft.value.direct.max_recursion = 7;
  const pending = f.state.apply();
  assert.equal(await f.state.apply(), false);
  assert.equal(f.sent.length, 1);
  finish({ status: 'error', message: 'compile failed' });
  assert.equal(await pending, false);
  assert.equal(f.state.draft.value.direct.max_recursion, 7);
  assert.match(f.state.error.value, /compile failed/);
  f.props.enabled = false;
  assert.equal(f.state.open.value, false);
  assert.equal(await f.state.apply(), false);
});

test('a failed read never permits submitting fallback defaults', async t => {
  const f = await fixture({ getRestirSettings: async () => { throw new Error('renderer offline'); } }); t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(await f.state.apply(), false);
  assert.match(f.state.error.value, /renderer offline/);
  assert.equal(f.sent.length, 0);
});

test('nested float32 values stay readable without changing the renderer value', async t => {
  const threshold = Math.fround(0.12345678);
  const f = await fixture({ getRestirSettings: async () => ({ ready: true,
    settings: { direct: { diffuse_factor: Math.fround(0.3), temporal: { depth: threshold } } } }) }); t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(f.state.draft.value.direct.diffuse_factor, 0.3);
  assert.equal(await f.state.apply(), true);
  assert.equal(Math.fround(f.sent[0][2].direct.temporal.depth), threshold);
});

test('renders the actual nested panel with independent switches, hints and bounded fields', async t => {
  const f = await fixture(); t.after(f.stop);
  f.state.open.value = true; await settle();
  const html = await renderPanel(f);
  for (const label of ['最大间接路径深度', 'DI 光源候选数', 'DI BSDF 候选数', 'DI 最大递归次数',
    'DI 漫反射差异阈值', 'GI 漫反射差异阈值',
    '启用直接光照（DI）', '启用间接光照（GI）', '启用 DI 时间复用', '启用 GI 空间复用'])
    assert.ok(html.includes(`aria-label="${label}"`), label);
  assert.match(html, /<input(?=[^>]*aria-label="DI 最大递归次数")(?=[^>]*min="1")(?=[^>]*max="100")[^>]*>/);
  assert.match(html, /<input(?=[^>]*aria-label="GI 空间复用半径")(?=[^>]*value="12")[^>]*>/);
  assert.match(html, /可能需要重新编译/);
  assert.match(html, /<input(?=[^>]*aria-label="GI 分辨率比例（暂不可用）")(?=[^>]*value="2")(?=[^>]*min="1")(?=[^>]*max="4")(?=[^>]*disabled)[^>]*>/);
  assert.match(html, /当前尚未接入渲染/);
  assert.doesNotMatch(html, /StablePlane|降噪|resize|GI ratio/);
  assert.doesNotMatch(html, /<button type="submit" disabled/);
  f.state.draft.value.direct.open = false;
  const disabled = await renderPanel(f);
  assert.match(disabled, /<fieldset disabled aria-label="DI 参数">/);
  assert.equal(f.state.draft.value.direct.M_light, 10);
});

test('the actual panel disables apply until settings are ready', async t => {
  const f = await fixture({ getRestirSettings: async () => ({ ready: false, settings: {} }) }); t.after(f.stop);
  f.state.open.value = true; await settle();
  const html = await renderPanel(f);
  assert.match(html, /正在准备 ReSTIR 并读取当前相机参数/);
  assert.match(html, /<button type="submit" disabled/);
});

test('a cold ReSTIR view can become ready after more than thirty seconds', async t => {
  let elapsed = 0, reads = 0;
  t.mock.method(Date, 'now', () => elapsed);
  const f = await fixture({ getRestirSettings: async () => {
    reads++;
    if (reads <= 2) {
      elapsed = 45000;
      return { ready: false, settings: {} };
    }
    return { ready: true, settings: { max_depth: 7 } };
  } }); t.after(f.stop);
  f.state.open.value = true; await settle();
  assert.equal(await f.state.apply(), false);
  await new Promise(resolve => setTimeout(resolve, 400));
  assert.equal(f.state.error.value, '');
  assert.equal(f.state.loaded.value, true);
  assert.equal(f.state.draft.value.max_depth, 7);
  assert.equal(await f.state.apply(), true);
});
