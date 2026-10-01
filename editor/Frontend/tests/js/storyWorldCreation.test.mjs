import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import { setImmediate } from 'node:timers';
import { ref } from 'vue';
import { babelParse, compileScript, parse } from 'vue/compiler-sfc';
import { editorApi } from '../../src/api/editorApi.js';
import * as launcher from '../../src/services/projectLauncherService.js';
import * as worldMode from '../../src/services/worldModeService.js';
import * as lifecycle from '../../src/services/worldSessionLifecycle.js';
import lanchat from '../../src/stores/lanchat.js';

// Compile the real creation page rather than duplicating its orchestration here.
// Only lifecycle/router/UI dependencies are injected; the launcher is production code.
const { descriptor } = parse(fs.readFileSync(new URL('../../src/views/layout/NewGame.vue', import.meta.url), 'utf8'));
const compiled = compileScript(descriptor, { id: 'story-create-regression', genDefaultAs: 'NewGame' });
const imports = babelParse(compiled.content, { sourceType: 'module' }).program.body
  .filter(node => node.type === 'ImportDeclaration');
let setupSource = compiled.content;
for (const node of [...imports].reverse()) {
  const bindings = node.specifiers.map(specifier => {
    const key = specifier.type === 'ImportDefaultSpecifier' ? 'default' : specifier.imported.name;
    return `${JSON.stringify(key)}: ${specifier.local.name}`;
  }).join(', ');
  setupSource = setupSource.slice(0, node.start)
    + `const { ${bindings} } = modules[${JSON.stringify(node.source.value)}];`
    + setupSource.slice(node.end);
}
const makeComponent = new Function('modules', `${setupSource}; return NewGame;`);
const idle = () => ({ status: 'stopped', restored: false, snapshotCaptured: false, restoreStatus: 'idle' });
const deferred = () => {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  return { promise, resolve };
};

async function mountCreationPage(t, { creative = true, stopResult = idle(), wrapped = true } = {}) {
  const calls = [], alerts = [], saves = [], mounted = [], unmounted = [];
  let activePath = creative ? 'creative-world' : '';
  let activeMode = creative ? 'creative' : '';
  let route = '/NewGame';
  let sequence = 0;
  let scriptResult = stopResult;
  const wrap = result => wrapped ? { data: result } : result;
  const previous = { window: globalThis.window, CustomEvent: globalThis.CustomEvent, alert: globalThis.alert };
  const timers = new Map();
  globalThis.window = {
    localStorage: { setItem: (...args) => saves.push(args) },
    dispatchEvent() {},
    alert: message => alerts.push(message),
    setInterval: callback => { const id = timers.size + 1; timers.set(id, callback); return id; },
    clearInterval: id => timers.delete(id),
  };
  globalThis.alert = globalThis.window.alert;
  globalThis.CustomEvent = class { constructor(type, options) { Object.assign(this, { type }, options); } };
  t.after(() => {
    for (const hook of unmounted) hook();
    assert.equal(timers.size, 0);
    Object.assign(globalThis, previous);
    worldMode.worldModeService.invalidate();
  });
  t.mock.method(console, 'error', () => {});
  t.mock.method(editorApi.project, 'getProjectLoadStatus', async () => wrap({ archive_service_ready: true }));
  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => wrap({ project_path: activePath, mode: activeMode }));
  t.mock.method(editorApi.project, 'createWorldProject', async payload => {
    calls.push(['create', payload]);
    return wrap({ name: `剧情世界_${++sequence}`, path: `story-world-${sequence}` });
  });
  t.mock.method(editorApi.scratch, 'stopGamePreview', async () => {
    calls.push(['preview']);
    return wrap({ status: 'stopped' });
  });
  t.mock.method(editorApi.scratch, 'getGamePreviewStatus', assert.fail);
  t.mock.method(editorApi.scratch, 'stopScriptExecution', async restore => {
    calls.push(['script', restore]);
    return wrap(await scriptResult);
  });
  t.mock.method(editorApi.project, 'openProject', async path => {
    calls.push(['open', path]);
    activePath = path;
    activeMode = 'story';
    return wrap({ ok: true, path });
  });
  t.mock.method(lanchat, 'openRoom', assert.fail);
  t.mock.method(lanchat, 'setWorkspaceMode', assert.fail);
  worldMode.worldModeService.invalidate();
  if (creative) await worldMode.worldModeService.resolve(activePath, { force: true });
  const router = {
    push: async path => { calls.push(['route', path]); route = path; },
    replace: async path => { calls.push(['replace', path]); route = path; },
  };
  const component = makeComponent({
    vue: { ref, onMounted: hook => mounted.push(hook), onUnmounted: hook => unmounted.push(hook) },
    'vue-router': { useRouter: () => router },
    '@/api/editorApi.js': { editorApi },
    '@/services/worldModeService.js': worldMode,
    '@/services/projectLauncherService.js': launcher,
    '@/services/worldSessionLifecycle.js': lifecycle,
    '@/services/cabbageAssistantContextService.js': { initializeWorldTasks: assert.fail },
    '@/stores/lanchat.js': { default: lanchat },
    '@/i18n/domTranslator.js': { translateUiText: text => text },
  });
  const page = component.setup({}, { expose() {} });
  for (const hook of mounted) hook();
  await page.refreshArchiveReady();
  assert.equal(page.mode.value, 'story');
  assert.match(descriptor.template.content, /@click="handleCreate"/);
  return {
    page, calls, alerts, saves,
    setStopResult: result => { scriptResult = result; },
    activePath: () => activePath,
    route: () => route,
  };
}

test('creation button opens story worlds with idle, restored, and repeated-stop results', async t => {
  for (const wrapped of [true, false]) {
    for (const stopResult of [idle(), { status: 'stopped', restored: true, snapshotCaptured: false, restoreStatus: 'restored' }]) {
      await t.test(JSON.stringify({ wrapped, stopResult }), async t => {
        const h = await mountCreationPage(t, { wrapped, stopResult });
        h.page.worldPrompt.value = '  云海故事  ';
        await h.page.handleCreate();
        assert.deepEqual(h.calls, [
          ['create', { mode: 'story', prompt: '云海故事' }],
          ['preview'], ['script', true], ['open', 'story-world-1'], ['route', '/'],
        ]);
        assert.equal(h.activePath(), 'story-world-1');
        assert.equal(worldMode.worldModeState.mode, 'story');
        assert.equal(h.page.creating.value, false);
        assert.deepEqual(h.alerts, []);
        assert.ok(h.saves.some(([key, value]) => key === 'corona.activeProjectPath' && value === 'story-world-1'));
        h.setStopResult(idle());
        await launcher.stopWorldRuntimeBeforeOpen(editorApi.scratch);
        await launcher.stopWorldRuntimeBeforeOpen(editorApi.scratch);
        assert.deepEqual(h.alerts, []);
      });
    }
  }
});

test('fresh launcher creates a story without an old runtime to restore', async t => {
  const h = await mountCreationPage(t, { creative: false });
  await h.page.handleCreate();
  assert.deepEqual(h.calls, [
    ['create', { mode: 'story', prompt: '' }], ['open', 'story-world-1'], ['route', '/'],
  ]);
  assert.deepEqual(h.alerts, []);
});

test('creation keeps the old world on real failures and the button can retry', async t => {
  for (const stopResult of [
    { status: 'stopped', restored: false, snapshotCaptured: true, restoreStatus: 'error', restoreError: 'scene restore failed' },
    { status: 'error', restored: false, message: 'cooperative stop timed out', pendingThreads: ['child-worker'] },
  ]) {
    await t.test(stopResult.restoreError || stopResult.message, async t => {
      const h = await mountCreationPage(t, { stopResult });
      await h.page.handleCreate();
      assert.deepEqual(h.calls, [
        ['create', { mode: 'story', prompt: '' }], ['preview'], ['script', true], ['replace', '/StartScreen'],
      ]);
      assert.equal(h.activePath(), 'creative-world');
      assert.equal(h.alerts.length, 1);
      assert.ok(h.alerts[0].includes(stopResult.restoreError || stopResult.message));
      assert.equal(h.page.creating.value, false);
      assert.equal(worldMode.worldModeService.opening, false);
      assert.deepEqual(h.saves, []);
      h.setStopResult(idle());
      h.calls.length = 0;
      await h.page.handleCreate();
      assert.deepEqual(h.calls, [
        ['create', { mode: 'story', prompt: '' }], ['preview'], ['script', true], ['open', 'story-world-2'], ['route', '/'],
      ]);
      assert.equal(h.activePath(), 'story-world-2');
      assert.equal(h.route(), '/');
      assert.equal(h.alerts.length, 1);
    });
  }
});

test('creation waits for cleanup, disables duplicate requests, and respects service readiness', async t => {
  const stop = deferred();
  const h = await mountCreationPage(t, { stopResult: stop.promise });
  h.page.archiveReady.value = false;
  await h.page.handleCreate();
  assert.deepEqual(h.calls, []);
  h.page.archiveReady.value = true;
  const creation = h.page.handleCreate();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(h.page.creating.value, true);
  assert.deepEqual(h.calls, [['create', { mode: 'story', prompt: '' }], ['preview'], ['script', true]]);
  await h.page.handleCreate();
  assert.equal(h.calls.filter(([call]) => call === 'create').length, 1);
  stop.resolve(idle());
  await creation;
  assert.equal(h.route(), '/');
  assert.equal(h.page.creating.value, false);
});
