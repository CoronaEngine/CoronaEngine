import assert from 'node:assert/strict';
import test from 'node:test';
import { createStoryNavigationController, storyNavigationKey } from '../../frontend/storyNavigation.mjs';

const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
const event = (props = {}) => ({ code: 'KeyO', preventDefault() {}, stopPropagation() {}, ...props });
const turn = () => new Promise(resolve => setImmediate(resolve));
const prepared = (source = 'D:/main', target = 'D:/main/.game/subworld', direction = 'enter') => ({
  status: 'ok', navigation: { source, target, direction, mode: 'story' },
});
function fixture(options = {}) {
  let version = 0, ready = true, current = true;
  let state = { status: 'ready', mode: 'story', projectPath: 'D:/main' };
  const calls = [], errors = [], tracked = [];
  let controller;
  controller = createStoryNavigationController({
    projectPath: 'D:/main', isReady: () => ready, isSourceCurrent: () => current,
    getSelectionVersion: () => version, readSession: () => state,
    resetInput: () => calls.push('reset'),
    flushCamera: async () => { calls.push('pose'); await options.pose?.(); },
    prepare: async key => { calls.push(['prepare', key]); return options.prepare ? options.prepare(key) : prepared(); },
    trackPreparation: promise => { tracked.push(promise); return promise; },
    openProject: path => {
      version++; current = false;
      state = { status: 'loading', mode: null, projectPath: path };
      calls.push(['open', path]);
      // Production App.vue unmounts the source on beginOpen.
      controller.dispose();
      return (async () => {
        const response = options.open ? await options.open(path) : { ok: true, path };
        if (response?.ok) state = { status: 'ready', mode: 'story', projectPath: path };
        return response;
      })();
    },
    cancelProjectOpen: () => { version++; calls.push('cancel'); },
    leave: () => { calls.push('leave'); }, notify: error => errors.push(error.message),
  });
  return { controller, calls, errors, tracked,
    setReady: value => { ready = value; },
    supersede: () => { version++; current = false; },
    opens: () => calls.filter(call => Array.isArray(call) && call[0] === 'open').map(call => call[1]),
  };
}

test('O/P selection filters repeats, composition, modifiers, editable/shadow targets', () => {
  assert.equal(storyNavigationKey(event()), 'KeyO');
  assert.equal(storyNavigationKey(event({ code: 'KeyP' })), 'KeyP');
  assert.equal(storyNavigationKey(event({ code: '', key: 'o' })), 'KeyO');
  assert.equal(storyNavigationKey(event({ shiftKey: true })), 'KeyO');
  for (const props of [{ repeat: true }, { defaultPrevented: true }, { isComposing: true },
    { keyCode: 229 }, { ctrlKey: true }, { altKey: true }, { metaKey: true },
    { target: { isContentEditable: true } }, { target: { closest: () => ({}) } },
    { composedPath: () => [{}, { isContentEditable: true }] }, { code: 'KeyW' }]) {
    assert.equal(storyNavigationKey(event(props)), '', JSON.stringify(props));
  }
});

test('no navigation until camera is bound and focus/current source are confirmed', async () => {
  const f = fixture();
  f.setReady(false);
  assert.equal(f.controller.keyDown(event()), false);
  f.setReady(true); f.supersede();
  assert.equal(f.controller.keyDown(event()), false);
  assert.deepEqual(f.calls, []);
});

test('flushes pose before Python save and tracks preparation, not launcher opening', async () => {
  const f = fixture();
  await f.controller.keyDown(event());
  assert.deepEqual(f.calls, ['reset', 'pose', ['prepare', 'KeyO'], ['open', 'D:/main/.game/subworld']]);
  assert.equal(f.tracked.length, 1);
  assert.equal((await f.tracked[0]).status, 'ok');
  assert.deepEqual(f.errors, []);
});

test('P is sent through the same existing key adapter', async () => {
  const f = fixture({ prepare: () => prepared('D:/main', 'D:/parent', 'exit') });
  await f.controller.keyDown(event({ code: 'KeyP' }));
  assert.deepEqual(f.calls[2], ['prepare', 'KeyP']);
  assert.deepEqual(f.opens(), ['D:/parent']);
});

test('busy locks repeated presses until both preparation and open finish', async () => {
  const preparation = deferred(), opened = deferred();
  const f = fixture({ prepare: () => preparation.promise, open: () => opened.promise });
  const pending = f.controller.keyDown(event());
  assert.equal(f.controller.busy, true);
  assert.equal(f.controller.keyDown(event({ code: 'KeyP' })), true);
  assert.equal(f.controller.keyDown(event({ repeat: true })), false);
  await turn();
  preparation.resolve(prepared());
  await turn();
  assert.equal(f.controller.busy, true);
  opened.resolve({ ok: true });
  await pending;
  assert.equal(f.controller.busy, false);
  assert.equal(f.calls.filter(call => Array.isArray(call) && call[0] === 'prepare').length, 1);
});

test('no-op does not open and releases input lock', async () => {
  const f = fixture({ prepare: () => ({ status: 'noop' }) });
  await f.controller.keyDown(event());
  assert.deepEqual(f.opens(), []);
  assert.equal(f.controller.busy, false);
  await f.controller.keyDown(event());
  assert.equal(f.tracked.length, 2);
});

test('save/copy and camera-ack errors keep the source and report the reason', async () => {
  for (const options of [
    { prepare: () => ({ status: 'error', message: 'copy failed' }) },
    { prepare: () => { throw new Error('copy failed'); } },
    { pose: () => { throw new Error('copy failed'); } },
  ]) {
    const f = fixture(options);
    await f.controller.keyDown(event());
    assert.deepEqual(f.opens(), []);
    assert.deepEqual(f.errors, ['copy failed']);
    assert.equal(f.controller.busy, false);
  }
});

test('Escape before pose acknowledgement prevents the Python save request', async () => {
  const pose = deferred();
  const f = fixture({ pose: () => pose.promise });
  const pending = f.controller.keyDown(event());
  f.controller.cancel();
  pose.resolve();
  await pending;
  assert.deepEqual(f.calls, ['reset', 'pose', 'reset', 'cancel']);
  assert.deepEqual(f.opens(), []);
});

test('Escape/new world/unmount during prepare ignores late success and failure', async () => {
  for (const fail of [true, false]) {
    for (const invalidate of [f => f.controller.cancel(), f => f.supersede(), f => f.controller.dispose()]) {
      const response = deferred();
      const f = fixture({ prepare: () => response.promise });
      const pending = f.controller.keyDown(event());
      await turn(); invalidate(f);
      if (fail) response.reject(new Error('late')); else response.resolve(prepared());
      await pending;
      assert.deepEqual(f.opens(), []);
      assert.deepEqual(f.errors, []);
    }
  }
});

test('open failure recovers source despite source-page unmount', async () => {
  const f = fixture({ open: path => path.endsWith('subworld')
    ? { ok: false, message: 'missing asset' } : { ok: true } });
  await f.controller.keyDown(event());
  assert.deepEqual(f.opens(), ['D:/main/.game/subworld', 'D:/main']);
  assert.match(f.errors[0], /已恢复来源世界.*missing asset/);
  assert.ok(!f.calls.includes('leave'));
});

test('recovery failure returns to launcher with both reasons', async () => {
  const f = fixture({ open: path => { throw new Error(`cannot open ${path}`); } });
  await f.controller.keyDown(event());
  assert.deepEqual(f.opens(), ['D:/main/.game/subworld', 'D:/main']);
  assert.match(f.errors[0], /恢复来源世界也失败/);
  assert.equal(f.calls.at(-1), 'leave');
});

test('Escape/new selection during open never reopens the old source', async () => {
  for (const fail of [true, false]) {
    const opened = deferred();
    const f = fixture({ open: () => opened.promise });
    const pending = f.controller.keyDown(event());
    await turn(); f.supersede();
    if (fail) opened.reject(new Error('late open failure')); else opened.resolve({ status: 'superseded' });
    await pending;
    assert.deepEqual(f.opens(), ['D:/main/.game/subworld']);
    assert.deepEqual(f.errors, []);
    assert.ok(!f.calls.includes('leave'));
  }
});

test('a superseded recovery never changes the new route or displays stale alerts', async () => {
  const recovery = deferred();
  const f = fixture({ open: path => path.endsWith('subworld')
    ? { ok: false, message: 'failed' } : recovery.promise });
  const pending = f.controller.keyDown(event());
  await turn(); f.supersede();
  recovery.reject(new Error('late recovery failure'));
  await pending;
  assert.deepEqual(f.errors, []);
  assert.ok(!f.calls.includes('leave'));
});

test('malformed or wrong-source navigation cannot open an arbitrary stale target', async () => {
  for (const response of [{ status: 'ok' }, prepared('D:/old'), prepared('D:/main', 'D:/main'),
    prepared('D:/main', 'D:/target', 'exit'), { ...prepared(), navigation: { ...prepared().navigation, mode: 'creative' } }]) {
    const f = fixture({ prepare: () => response });
    await f.controller.keyDown(event());
    assert.deepEqual(f.opens(), []);
    assert.equal(f.errors.length, 1);
  }
});


test('interrupted preparation cannot revive after an exit save fails; later keys still work', async () => {
  const prep = deferred(); let attempts = 0;
  const f = fixture({ prepare: () => ++attempts === 1 ? prep.promise : prepared() });
  const pending = f.controller.keyDown(event()); await turn();
  f.controller.interrupt();
  assert.equal(f.controller.busy, true);
  prep.resolve(prepared()); await pending;
  assert.deepEqual(f.opens(), []);
  assert.deepEqual(f.errors, []);
  assert.equal(f.controller.busy, false);
  await f.controller.keyDown(event());
  assert.deepEqual(f.opens(), ['D:/main/.game/subworld']);
});
