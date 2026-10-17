// Transport-level contracts for src/api/editorApi.js.
//
// These tests intentionally import ONLY node builtins plus the module under test, so they
// run without node_modules: `node --test tests/js/editorApiTransport.test.mjs`.
// The fake host below mimics the parts of the CEF surface editorApi.js actually uses.

import test from 'node:test';
import assert from 'node:assert/strict';

// Mirrors the shape of the C++ Editor API manifest closely enough for the JS validator:
// every entry needs `allowed_callers` to include the CEF caller bit (1).
const CALLER_CEF = 1;

const MANIFEST_METHODS = [
  {
    api: 'SceneTools.get_model_nodes',
    js_wrapper: 'sceneTools.getModelNodes',
    params: [{ name: 'scene_name', type: 'string' }, { name: 'actor_name', type: 'string' },
      { name: 'parent_index', type: 'integer', optional: true }, { name: 'offset', type: 'integer', optional: true },
      { name: 'limit', type: 'integer', optional: true }],
    return: 'object', allowed_callers: CALLER_CEF,
  },
  {
    api: 'SceneTools.set_model_node_state',
    js_wrapper: 'sceneTools.setModelNodeState',
    params: [{ name: 'scene_name', type: 'string' }, { name: 'actor_name', type: 'string' },
      { name: 'node_index', type: 'integer' }, { name: 'state', type: 'object' }],
    return: 'object', allowed_callers: CALLER_CEF,
  },
  { api: 'EditorApi.list_methods', params: [], return: 'object', allowed_callers: CALLER_CEF },
  { api: 'EditorApi.list_events', params: [], return: 'object', allowed_callers: CALLER_CEF },
  {
    api: 'EditorApi.register_callback',
    params: [
      { name: 'event', type: 'string' },
      { name: 'spec', type: 'object', optional: true },
    ],
    return: 'object',
    allowed_callers: CALLER_CEF,
  },
  {
    api: 'EditorApi.unregister_callback',
    params: [{ name: 'token', type: 'integer' }],
    return: 'object',
    allowed_callers: CALLER_CEF,
  },
  {
    api: 'SceneTools.list_scene_tree',
    js_wrapper: 'sceneTools.listSceneTree',
    params: [{ name: 'sceneName', type: 'string' }],
    return: 'object',
    allowed_callers: CALLER_CEF,
  },
];

const MANIFEST_EVENTS = [
  {
    event: 'events.onActorChanged',
    js_wrapper: 'events.onActorChanged',
    payload: 'object',
    allowed_callers: CALLER_CEF,
  },
];

function createFakeCefHost() {
  const failFor = new Set();
  const hangFor = new Set();
  const requests = [];
  const pending = new Map();

  return {
    requests,
    failFor,
    hangFor,
    pending,
    cefQuery({ request, onSuccess, onFailure }) {
      let payload = null;
      try {
        payload = JSON.parse(request);
      } catch (error) {
        onFailure(-1, `unparseable request: ${error.message}`);
        return;
      }

      requests.push(payload);

      // Emulates a native handler that accepts the request and never answers it.
      if (hangFor.has(payload.api)) {
        pending.set(payload.api, { onSuccess, onFailure });
        return;
      }
      if (failFor.has(payload.api)) {
        onFailure(-2, `${payload.api} refused by native`);
        return;
      }
      if (payload.api === 'EditorApi.list_methods') {
        onSuccess(JSON.stringify({ status: 'success', data: { methods: MANIFEST_METHODS } }));
        return;
      }
      if (payload.api === 'EditorApi.list_events') {
        onSuccess(JSON.stringify({ status: 'success', data: { events: MANIFEST_EVENTS } }));
        return;
      }
      if (payload.api === 'EditorApi.register_callback') {
        onSuccess(JSON.stringify({ status: 'success', data: { callback_token: 42 } }));
        return;
      }
      onSuccess(JSON.stringify({ status: 'success', data: {} }));
    },
  };
}

// A fresh module instance per test so `editorApiCallbacks` starts empty.
async function loadEditorApi() {
  const host = createFakeCefHost();
  globalThis.window = host;
  const module = await import(`../../src/api/editorApi.js?fresh=${Date.now()}${Math.random()}`);
  return { editorApi: module.editorApi, host, limits: module.editorApiTransportLimits };
}

test('model node wrappers preserve actor and numeric node identity in native requests', async () => {
  const { editorApi, host } = await loadEditorApi();
  await editorApi.sceneTools.getModelNodes('Scene/main.scene', 'Temple', -1, 0, 50);
  const patch = { visible: false, position: [1, 2, 3] };
  await editorApi.sceneTools.setModelNodeState('Scene/main.scene', 'Temple', 12, patch);
  assert.deepEqual(host.requests.find((item) => item.api === 'SceneTools.get_model_nodes').args,
    ['Scene/main.scene', 'Temple', -1, 0, 50]);
  assert.deepEqual(host.requests.find((item) => item.api === 'SceneTools.set_model_node_state').args,
    ['Scene/main.scene', 'Temple', 12, patch]);
});

// Runs `body` with a short transport wait so timeout behaviour is exercised without
// making the suite wait for the production 30s limit.
async function withShortTimeout(limits, body) {
  const original = limits.responseTimeoutMs;
  limits.responseTimeoutMs = 20;
  try {
    return await body();
  } finally {
    limits.responseTimeoutMs = original;
  }
}

test('a callback whose unregister failed must not stay reachable', async () => {
  const { editorApi, host } = await loadEditorApi();
  const seen = [];

  const token = await editorApi.events.onActorChanged((payload) => seen.push(payload));
  assert.equal(token, 42, 'the registration must return the native callback token');

  // Sanity: while registered, the event reaches the handler.
  globalThis.window.__coronaEditorApiDispatch({
    token,
    event: 'events.onActorChanged',
    payload: { n: 1 },
  });
  assert.deepEqual(seen, [{ n: 1 }]);

  // Native refuses the unregister. The local registration must still be dropped, otherwise
  // the handler (and everything it closes over) leaks for the lifetime of the page.
  host.failFor.add('EditorApi.unregister_callback');
  await assert.rejects(() => editorApi.off(token));

  globalThis.window.__coronaEditorApiDispatch({
    token,
    event: 'events.onActorChanged',
    payload: { n: 2 },
  });
  assert.deepEqual(seen, [{ n: 1 }], 'a handler whose unregister failed must not stay reachable');
});

test('a request with no native reply must fail with a diagnosable timeout', async () => {
  const { editorApi, host, limits } = await loadEditorApi();

  await withShortTimeout(limits, async () => {
    host.hangFor.add('EditorApi.list_methods');

    await assert.rejects(
      () => editorApi.listMethods(),
      (error) => {
        assert.equal(error.code, 'EDITOR_API_TIMEOUT');
        assert.match(error.message, /EditorApi\.list_methods/);
        // The native side may still be working; the message must not claim it was cancelled.
        assert.match(error.message, /may still be in progress/i);
        return true;
      }
    );
  });
});

test('a reply that arrives within the limit must still resolve', async () => {
  const { editorApi, limits } = await loadEditorApi();

  await withShortTimeout(limits, async () => {
    const response = await editorApi.listMethods();
    assert.ok(response, 'a prompt native reply must resolve normally');
  });
});

test('a late native reply after a timeout must be ignored', async () => {
  const { editorApi, host, limits } = await loadEditorApi();

  await withShortTimeout(limits, async () => {
    host.hangFor.add('EditorApi.list_methods');
    await assert.rejects(() => editorApi.listMethods());

    // The native handler finally answers. This must neither throw nor try to settle twice.
    const late = host.pending.get('EditorApi.list_methods');
    assert.ok(late, 'the fake host must have captured the hung request');
    assert.doesNotThrow(() =>
      late.onSuccess(JSON.stringify({ status: 'success', data: { methods: MANIFEST_METHODS } }))
    );
  });
});

// Characterization tests for wrapper -> manifest resolution. These pin behaviour so the
// lookup can be refactored (reverse map instead of a linear scan per call) without risk.
test('a wrapper path resolves through the manifest to its native api name', async () => {
  const { editorApi, host } = await loadEditorApi();

  await editorApi.sceneTools.listSceneTree('SceneA');

  const sent = host.requests.find((entry) => entry.api === 'SceneTools.list_scene_tree');
  assert.ok(
    sent,
    `wrapper must resolve to the manifest api, saw: ${host.requests.map((r) => r.api).join(', ')}`
  );
  assert.deepEqual(sent.args, ['SceneA']);
});

test('an unknown wrapper path is rejected instead of being sent to native', async () => {
  const { editorApi, host } = await loadEditorApi();

  await assert.rejects(() => editorApi.notARealNamespace.notARealMethod());

  assert.equal(
    host.requests.some((entry) => entry.api === 'notARealNamespace.notARealMethod'),
    false,
    'an unresolvable wrapper must never reach native'
  );
});

import {
  flattenLoadedNodes,
  MODEL_NODE_DISPLAY_MAX_SKIP,
  resolveModelNodeDisplayParent,
  virtualNodeWindow,
} from '../../src/utils/modelHierarchy.js';

test('loaded model nodes flatten only expanded branches and cap the render window', () => {
  const pages = {
    '-1': { items: [{ node_index: 0, name: 'root', child_count: 2 }] },
    '0': { items: [{ node_index: 1, name: 'a', child_count: 0 }, { node_index: 2, name: 'b', child_count: 0 }] },
  };
  const rows = flattenLoadedNodes(pages, new Set([0]));
  assert.deepEqual(rows.map((row) => [row.node.node_index, row.depth]), [[0, 0], [1, 1], [2, 1]]);

  const window = virtualNodeWindow(rows, 28, 28, 28, 1);
  assert.equal(window.start, 0);
  assert.equal(window.end, 3);
  assert.equal(window.topSpacer, 0);
  assert.equal(window.bottomSpacer, 0);
});

test('collapsed nodes do not contribute descendant rows', () => {
  const pages = {
    '-1': { items: [{ node_index: 0, name: 'root', child_count: 1 }] },
    '0': { items: [{ node_index: 1, name: 'child', child_count: 0 }] },
  };
  assert.deepEqual(flattenLoadedNodes(pages, new Set()).map((row) => row.node.node_index), [0]);
});

test('a lone synthetic root is skipped so nodes hang directly under the actor', () => {
  assert.equal(
    resolveModelNodeDisplayParent({ items: [{ node_index: 0, name: 'root', child_count: 12 }] }),
    0,
  );
  assert.equal(
    resolveModelNodeDisplayParent({ items: [{ node_index: 7, name: 'Scene', child_count: 3 }] }, 1),
    7,
  );
});

test('a single root that carries content of its own is kept', () => {
  assert.equal(
    resolveModelNodeDisplayParent({ items: [{ node_index: 0, name: 'temple', child_count: 0 }] }),
    -1,
  );
  assert.equal(resolveModelNodeDisplayParent({ items: [] }), -1);
});

test('several roots are real content, not a container to skip', () => {
  assert.equal(
    resolveModelNodeDisplayParent({
      items: [{ node_index: 0, child_count: 2 }, { node_index: 1, child_count: 0 }],
    }),
    -1,
  );
});

test('the synthetic root skip is bounded instead of walking away', () => {
  const page = { items: [{ node_index: 5, child_count: 9 }] };
  assert.equal(resolveModelNodeDisplayParent(page, MODEL_NODE_DISPLAY_MAX_SKIP), -1);
});

test('the flattened tree can start at the displayed parent instead of the roots', () => {
  const pages = {
    '-1': { items: [{ node_index: 0, name: 'root', child_count: 1 }] },
    '0': { items: [{ node_index: 1, name: 'a', child_count: 1 }] },
    '1': { items: [{ node_index: 2, name: 'b', child_count: 0 }] },
  };
  const rows = flattenLoadedNodes(pages, new Set([1]), '0');
  assert.deepEqual(rows.map((row) => [row.node.node_index, row.depth]), [[1, 0], [2, 1]]);
});
