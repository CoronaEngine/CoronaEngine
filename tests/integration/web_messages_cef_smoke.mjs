// Real-engine smoke test. CDP uses JSON; application messages use structured clone.
// Run with Node 22+: node tests/integration/web_messages_cef_smoke.mjs <runtime-directory>
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawn } from 'node:child_process';
import { readFile, mkdir, writeFile, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';

if (process.argv.includes('--ctest') && process.env.CORONA_RUN_GPU_SMOKE !== '1') {
  console.log('CEF Web messages smoke skipped; set CORONA_RUN_GPU_SMOKE=1 to enable');
  process.exit(77);
}
if (typeof WebSocket === 'undefined') throw new Error('CEF smoke requires Node 22+ with native WebSocket');

const runtime = path.resolve(process.argv.slice(2).find(arg => !arg.startsWith('--')) || 'build/conan/examples/relwithdebinfo/examples/engine/RelWithDebInfo');
const testNativeBindings = process.argv.includes('--native-bindings');
const output = path.resolve('build/web-messages-smoke');
await mkdir(output, { recursive: true });
await rm(path.join(output, 'result.json'), { force: true });
const server = createServer();
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const port = server.address().port;
await new Promise(resolve => server.close(resolve));
const child = spawn(path.join(runtime, 'corona_engine.exe'), [], {
  cwd: runtime, windowsHide: true, stdio: 'ignore',
  env: { ...process.env, CORONA_CEF_REMOTE_DEBUG_PORT: String(port),
    CORONA_CEF_ROOT_CACHE: path.join(output, `cache-${Date.now()}`) },
});
let launchError;
child.on('error', error => { launchError = error; });
const clients = [];
let stage = 'starting engine';
const waitFor = async (fn, timeout = 90_000) => {
  const end = Date.now() + timeout;
  while (Date.now() < end) {
    if (launchError) throw launchError;
    const result = await fn();
    if (result) return result;
    await delay(150);
  }
  throw new Error('CEF smoke deadline exceeded');
};
const targets = async () => {
  try { return await (await fetch(`http://127.0.0.1:${port}/json/list`, { signal: AbortSignal.timeout(2000) })).json(); }
  catch { return []; }
};
async function connect(target) {
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  let sequence = 0;
  const pending = new Map();
  socket.onclose = event => {
    for (const entry of pending.values()) {
      clearTimeout(entry.timer);
      entry.reject(new Error(`CDP socket closed during ${stage}: ${event.code} ${event.reason}`));
    }
    pending.clear();
  };
  socket.onmessage = event => {
    const message = JSON.parse(event.data);
    const entry = pending.get(message.id);
    if (!entry) return;
    pending.delete(message.id);
    clearTimeout(entry.timer);
    if (message.error) entry.reject(new Error(message.error.message));
    else entry.resolve(message.result);
  };
  const call = (method, params) => new Promise((resolve, reject) => {
    const id = ++sequence;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error(`CDP timeout during ${stage}: ${method}`)); }, 20_000);
    pending.set(id, { resolve, reject, timer });
    socket.send(JSON.stringify({ id, method, params }));
  });
  const api = {
    call,
    async evaluate(expression, awaitPromise = true) {
      const reply = await call('Runtime.evaluate', { expression, awaitPromise, returnByValue: true });
      if (reply.exceptionDetails) throw new Error(reply.exceptionDetails.exception?.description || reply.exceptionDetails.text);
      return reply.result.value;
    },
    close() { for (const entry of pending.values()) { clearTimeout(entry.timer); entry.reject(new Error('CDP closed')); } socket.close(); },
  };
  clients.push(api);
  return api;
}

try {
  console.log('CEF smoke: waiting for main document');
  const mainTarget = await waitFor(async () => (await targets()).find(target => target.type === 'page' && target.url.includes('index.html')));
  const main = await connect(mainTarget);
  await waitFor(() => main.evaluate('typeof window.corona?.window?.getCurrent === "function"'));
  assert.equal(await main.evaluate('typeof corona.viewport?.pick'), 'function');
  assert.equal(await main.evaluate('typeof corona.input?.dispatch'), 'function');
  // Use a file-origin fixture in the real BrowserManager windows. The editor
  // correctly retires panels without an active world, so loading its Vue app
  // would test world policy rather than channel reachability.
  const fixture = path.join(output, 'index.html');
  await writeFile(fixture, '<!doctype html><meta charset="utf-8"><title>CEF Web messages smoke</title>');
  await main.call('Page.navigate', { url: pathToFileURL(fixture).href });
  await waitFor(() => main.evaluate('document.title === "CEF Web messages smoke" && typeof window.corona?.window?.getCurrent === "function"'));
  // Manifest-backed fixture calls use the same typed V8 endpoint as the application.
  await main.evaluate(`(() => {
    window.__fixtureApi = ({api, args}) => corona.editor.invoke(api, args);
    return true;
  })()`);
  assert.match(await main.evaluate("window.__fixtureApi({api:'SceneTools.read_archive_scene',args:[{}]}).catch(error=>error.message)"), /not allowed/);
  for (const method of ['fuzzy_search', 'get_stats', 'image_search', 'list_types', 'mark_index_dirty', 'prepare_index', 'rebuild_index']) {
    assert.match(await main.evaluate(`window.__fixtureApi({api:'ResourceSearch.${method}',args:[]}).catch(error=>error.message)`), /not allowed/);
  }
  const scope = `smoke-${Date.now()}`;
  stage = 'direct native scene reads without a scene';
  for (const method of ['getFileTree', 'createFolder', 'createActorTemplate', 'renameItem', 'deleteItem', 'openFile']) {
    assert.equal(await main.evaluate(`typeof corona.files?.${method}`), 'function', `files.${method} requires a direct V8 binding`);
  }
  for (const method of ['readProjectDocument', 'commitProjectDocument', 'getActiveProjectInfo', 'saveActiveProjectInfo']) {
    assert.equal(await main.evaluate(`typeof corona.projectSettings?.${method}`), 'function', `projectSettings.${method} requires a direct V8 binding`);
  }
  for (const method of ['save', 'load']) {
    assert.equal(await main.evaluate(`typeof corona.projectFiles?.${method}`), 'function', `projectFiles.${method} requires a direct V8 binding`);
  }
  for (const method of ['runFile', 'getRun', 'stop', 'getStatus']) {
    assert.equal(await main.evaluate(`typeof corona.scripts?.${method}`), 'function', `scripts.${method} requires a direct V8 binding`);
  }
  assert.equal(await main.evaluate('typeof corona.scene?.readActive'), 'function');
  assert.equal(await main.evaluate('typeof corona.scene?.readCameras'), 'function');
  assert.equal(await main.evaluate('typeof corona.scene?.selectActor'), 'function');
  for (const method of ['getEnvironment', 'setEnvironment']) {
    assert.equal(await main.evaluate(`typeof corona.scene?.${method}`), 'function', 'environment requires the same direct V8 endpoint');
  }
  for (const method of ['readActorSkeleton', 'readActorIkChains', 'setActorIkChains']) {
    assert.equal(await main.evaluate(`typeof corona.scene?.${method}`), 'function', `${method} requires the direct scoped V8 endpoint`);
  }
  assert.equal(await main.evaluate('typeof corona.scene?.readActorTree'), 'function', 'assistant actor context requires a direct scoped V8 endpoint');
  assert.deepEqual(await main.evaluate(`(async () => {
    JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); };
    return corona.scene.readActive();
  })()`), { archive_session: '' });
  assert.equal(await main.evaluate("corona.scene.getEnvironment('scene.ini',{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.equal(await main.evaluate("corona.scene.readActorIndex('scene.ini',{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.equal(await main.evaluate("corona.scene.readActorProperties('scene.ini','missing',{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.equal(await main.evaluate("corona.scene.readActorTree('scene.ini',{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.equal(await main.evaluate("corona.scene.readActorSkeleton('scene.ini','actor',{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.equal(await main.evaluate("corona.scene.readActorIkChains('scene.ini','actor',{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.equal(await main.evaluate("corona.scene.setActorIkChains('scene.ini','actor',[],{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.equal(await main.evaluate("corona.scene.readCameras('scene.ini',{archive_session:'missing'}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
  assert.deepEqual(await main.evaluate('corona.scene.readActive()'), { archive_session: '' });
  assert.equal(typeof (await main.evaluate('corona.scene.isVisionAvailable()')).available, 'boolean');
  assert.match(await main.evaluate("corona.editor.invoke('SceneTools.is_vision_available', []).catch(error=>error.message)"), /not allowed/);
  for (const method of ['readActive', 'selectActor']) {
    assert.equal(await main.evaluate(`corona.scene.${method}(null).catch(error=>error.code)`), 'INVALID_ARGUMENT');
  }
  const docs = [{ label: 'main', client: main }];
  // Launcher routes survive without opening/mutating a user world. World routes
  // correctly close stale standalone documents and are not valid smoke fixtures.
  const createdPanels = [];
  for (const [label, detached, route] of [
    ['docked', false, '/StartScreen'],
    ['detached', true, '/StartScreen'],
  ]) {
    stage = `creating ${label}`;
    console.log(`CEF smoke: ${stage}`);
    const options = { detached, panelId: `smoke-${label}`, title: label, nativeDocked: true,
      routePath: `${route}?uiOwner=${scope}&uiSession=smoke&uiWindow=${label}`, width: 320, height: 240,
    };
    const created = await main.evaluate(`(async () => {
      JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); };
      return corona.window.createPanel(${JSON.stringify(options)});
    })()`);
    assert.equal(Number.isInteger(created.tab_id), true);
    assert.equal(created.panel_id, options.panelId);
    createdPanels.push(created);
    const target = await waitFor(async () => (await targets()).find(item => item.url.includes(`uiWindow=${label}`)), 30_000);
    docs.push({ label, client: await connect(target) });
  }
  const identitySource = (await readFile('CoronaEditor/editor/Frontend/src/api/webDocument.js', 'utf8'))
    .replaceAll('export ', '');
  const source = identitySource + '\n' + (await readFile('CoronaEditor/editor/Frontend/src/api/webMessages.js', 'utf8'))
    .replace("import { getWebDocumentIdentity } from './webDocument.js';", '')
    .replaceAll('export function ', 'function ');
  for (const doc of docs) {
    stage = `installing module in ${doc.label}`;
    console.log(`CEF smoke: ${stage}`);
    assert.equal(await doc.client.evaluate('typeof corona.viewport?.configure'), 'function');
    for (const method of ['setCameraViewport', 'setViewportUiMode', 'setViewportUiCalibration', 'viewportUiPointer', 'setViewportSystemCursorHidden', 'moveActorMesh', 'pickActor']) {
      assert.equal(await doc.client.evaluate(`typeof window.coronaBridge?.${method}`), 'undefined');
    }
    await doc.client.evaluate(`(() => { ${source}\n window.__smoke = createWebMessages({scope:${JSON.stringify(scope)},documentId:${JSON.stringify(doc.label)},role:${JSON.stringify(doc.label === 'main' ? 'main' : 'peer')},
      channelFactory:name=>{const channel=new BroadcastChannel(name), post=channel.postMessage.bind(channel);
        channel.postMessage=message=>{if(window.__slowDiscovery && message.kind==='presence' && message.discoveryId) setTimeout(()=>post(message),180);else post(message)};
        return channel;}}); window.__received=[]; window.__smoke.on('state', data=>window.__received.push(data)); window.__smoke.handle('echo', data=>data); return true; })()`);
  }
  const evidence = [];
  for (const doc of docs) {
    stage = `roundtrip from ${doc.label}`;
    console.log(`CEF smoke: ${stage}`);
    const value = await doc.client.evaluate(`window.__smoke.request('main','echo',{id:'18446744073709551615',bytes:new Uint8Array([1,255])}).then(x=>({id:x.id,bytes:Array.from(x.bytes),typed:x.bytes instanceof Uint8Array}))`);
    assert.deepEqual(value, { id: '18446744073709551615', bytes: [1, 255], typed: true });
    evidence.push({ document: doc.label, origin: await doc.client.evaluate('location.origin'), roundtrip: 'passed' });
  }
  stage = 'archive-scoped graph save coordination across real CEF documents';
  console.log(`CEF smoke: ${stage}`);
  const graphSaveSource = (await readFile('CoronaEditor/editor/Frontend/src/services/nodeGraphSaves.js', 'utf8'))
    .replace(/^import .*;\r?\n/gm, '').replaceAll('export function ', 'function ');
  for (const doc of docs) {
    await doc.client.evaluate(`(() => { ${graphSaveSource}
      JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); };
      window.__graphCalls = 0; window.__graphSucceeded = true;
      window.__graphSaves = createNodeGraphSaves({messages:window.__smoke});
      window.__graphRegistration = window.__graphSaves.register({
        getSession:()=>${JSON.stringify(doc.label === 'detached' ? 'other-archive' : 'old-archive')},
        flush:async ()=>{ window.__graphCalls++; return window.__graphSucceeded; },
      });
      return true;
    })()`);
  }
  const allEchoes = await main.evaluate(`window.__smoke.requestAll('echo',new Uint8Array([4,255]))
    .then(values=>values.map(value=>({bytes:Array.from(value),typed:value instanceof Uint8Array})))`);
  assert.equal(allEchoes.length, 3);
  assert.ok(allEchoes.every(value => value.typed && value.bytes[0] === 4 && value.bytes[1] === 255));
  await docs[1].client.evaluate('window.__slowDiscovery = true');
  assert.equal(await main.evaluate(`window.__graphSaves.flush('old-archive').then(values=>values.reduce((sum,value)=>sum+value.saved,0))`), 2);
  await docs[1].client.evaluate('window.__slowDiscovery = false');
  assert.equal(await docs[2].client.evaluate('window.__graphCalls'), 0, 'another archive must never be saved');
  await docs[1].client.evaluate('window.__graphSucceeded = false; true');
  assert.equal(await docs[1].client.evaluate('window.__graphRegistration.retire().catch(error=>error.code)'), 'GRAPH_SAVE_FAILED');
  assert.equal(await main.evaluate(`window.__graphSaves.flush('old-archive').catch(error=>error.code)`), 'GRAPH_SAVE_FAILED');
  await docs[1].client.evaluate('window.__graphSucceeded = true; true');
  assert.equal(await main.evaluate(`window.__graphSaves.flush('old-archive').then(values=>values.reduce((sum,value)=>sum+value.saved,0))`), 2);
  const savedCalls = await docs[1].client.evaluate('window.__graphCalls');
  assert.equal(await main.evaluate(`window.__graphSaves.flush('old-archive').then(values=>values.reduce((sum,value)=>sum+value.saved,0))`), 1);
  assert.equal(await docs[1].client.evaluate('window.__graphCalls'), savedCalls, 'success removes the retired graph closure');
  evidence[0].graphSaveCoordination = 'real CEF documents with JSON disabled: requestAll typed arrays, delayed known participant discovery, archive isolation, failed unmount retained, explicit retry and removal';
  if (testNativeBindings) {
    stage = 'native window task burst';
    // Several documents share this queue with input. A frame must drain its
    // existing batch rather than serializing every request behind another frame.
    const burst = await main.evaluate(`(async () => {
      let timer;
      try {
        return await Promise.race([
          Promise.all(Array.from({length: 512}, () => corona.window.getCurrent())),
          new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Native task queue starved')), 5000); }),
        ]).then(values => ({count: values.length, sameTab: values.every(value => value.tabId === values[0].tabId)}));
      } finally { clearTimeout(timer); }
    })()`);
    assert.deepEqual(burst, {count: 512, sameTab: true});
    evidence[0].nativeTaskBurst = '512 queued native operations complete without frame-per-request starvation';
    stage = 'native window event subscriptions';
    for (const doc of docs) {
      assert.equal(await doc.client.evaluate('typeof corona.window.onClosed'), 'function', 'native close events require direct V8 callbacks');
      assert.equal(await doc.client.evaluate('typeof corona.window.onCloseRequested'), 'function');
      for (const method of ['close', 'closeTab', 'focusTab', 'createPanel', 'detachPanel', 'redockPanel']) {
        assert.equal(await doc.client.evaluate(`typeof corona.window.${method}`), 'function', `${method} requires a direct native V8 operation`);
      }
      await doc.client.evaluate(`(() => {
        window.__nativeClosed = []; window.__unsubscribedEvents = [];
        corona.window.onClosed(() => { throw new Error('subscriber failure must be isolated'); });
        corona.window.onClosed(value => window.__nativeClosed.push(value));
        const stop = corona.window.onClosed(value => window.__unsubscribedEvents.push(value));
        stop(); stop();
        let invalid = false;
        try { corona.window.onClosed(null); } catch { invalid = true; }
        if (!invalid) throw new Error('invalid callback accepted');
        return true;
      })()`);
    }
    stage = 'removed legacy Dock transport and typed camera creation';
    assert.equal(await main.evaluate('typeof window.coronaBridge?.dockCommand'), 'undefined');
    assert.equal(await main.evaluate('typeof window.__dockCallback'), 'undefined');
    assert.equal(await main.evaluate('typeof corona.window.createCameraView'), 'function');
    const cameraOptions = {archiveSession:'missing-archive',sceneId:'scene.ini',cameraId:'camera-main',routePath:'/StartScreen'};
    const cameraResult = await main.evaluate(`(async () => {
      JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); };
      try { await corona.window.createCameraView(${JSON.stringify(cameraOptions)}); }
      catch(error) { return error.code; }
    })()`);
    assert.equal(cameraResult, 'PROJECT_CONTEXT_CHANGED');
    for (const options of ['null', '[]', '{}', '{archiveSession:1}',
      '{archiveSession:"a",sceneId:"s",cameraId:"c",cameraHandle:1}',
      '{archiveSession:"a",sceneId:"s",cameraId:"c",width:0}']) {
      assert.equal(await main.evaluate(`corona.window.createCameraView(${options}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    for (const doc of docs) {
      stage = `direct V8 binding in ${doc.label}`;
      assert.equal(await doc.client.evaluate('typeof window.coronaBridge?.actorTransform'), 'undefined');
      assert.equal(await doc.client.evaluate('typeof window.coronaBridge?.setProperty'), 'undefined');
      assert.equal(await doc.client.evaluate('typeof window.coronaBridge?.computeActorFocusPose'), 'undefined');
      assert.equal(await doc.client.evaluate('typeof window.coronaBridge?.cameraMove'), 'undefined');
      console.log(`CEF smoke: ${stage}`);
      assert.equal(await doc.client.evaluate('typeof window.corona?.window?.getCurrent'), 'function', 'direct V8 binding is required');
      const result = await doc.client.evaluate(`(async () => {
        JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); };
        const before = await corona.window.getCurrent();
        await corona.window.setDragRegions([{x:2,y:3,w:40,h:12}]);
        const after = await corona.window.getCurrent();
        let errorCode;
        try { await corona.window.setDragRegions([{x:0,y:0,w:-1,h:10}]); } catch(error) { errorCode=error.code; }
        for (const args of [[], [null], [{}], [[null]], [[{x:NaN,y:0,w:1,h:1}]],
          [[{x:0,y:0,w:Infinity,h:1}]], [[{x:0,y:0,w:1,h:-1}]], [Array(257).fill({x:0,y:0,w:1,h:1})]]) {
          let invalid;
          try { await corona.window.setDragRegions(...args); } catch(error) { invalid = error.code; }
          if (invalid !== 'INVALID_ARGUMENT') throw new Error('invalid drag arguments accepted');
        }
        try { await corona.window.getCurrent(null); throw new Error('unexpected argument accepted'); }
        catch(error) { if (error.code !== 'INVALID_ARGUMENT') throw error; }
        const unchanged = await corona.window.getCurrent();
        if (unchanged.dragRegions[0]?.w !== 40) throw new Error('invalid arguments mutated native state');
        return {same:before.browserId === after.browserId, idType:typeof after.browserId,
          frameType:typeof after.frameId, regions:after.dragRegions, errorCode};
      })()`);
      assert.deepEqual(result, { same: true, idType: 'string', frameType: 'string',
        regions: [{ x: 2, y: 3, w: 40, h: 12 }], errorCode: 'INVALID_ARGUMENT' });
      evidence.find(item => item.document === doc.label).nativeBindings = 'passed';
    }
    const identities = await Promise.all(docs.map(doc => doc.client.evaluate('corona.window.getCurrent().then(x=>x.browserId)')));
    assert.equal(new Set(identities).size, docs.length, 'native contexts must identify different windows');
    for (let index = 1; index < docs.length; index++) {
      assert.equal(await docs[index].client.evaluate('corona.window.getCurrent().then(x=>x.tabId)'), createdPanels[index - 1].tab_id);
    }
    stage = 'typed panel validation and docking intents';
    assert.deepEqual(await main.evaluate('corona.window.suspendCameraViews("")'), {closed:0});
    assert.deepEqual(await docs[1].client.evaluate('corona.window.suspendCameraViews("")'), {closed:0});
    const barrierOrder = await main.evaluate(`(async () => {
      const order = [];
      await Promise.all([
        corona.window.getCurrent().then(() => order.push('before')),
        corona.window.suspendCameraViews('').then(() => order.push('barrier')),
        corona.window.getCurrent().then(() => order.push('after')),
      ]);
      return order;
    })()`);
    assert.deepEqual(barrierOrder, ['before', 'after', 'barrier'], 'barrier acknowledgment must occupy a second main-thread turn');
    evidence[0].twoTurnNativeBarrier = barrierOrder;
    for (const method of ['toggleCameraMaximized', 'cycleCameraWindowMode', 'toggleCameraBorderless']) {
      assert.equal(await main.evaluate(`corona.window.${method}('missing-scene','missing-camera')`), false);
      assert.equal(await docs[1].client.evaluate(`corona.window.${method}('','')`), false, 'non-camera panels must be unchanged');
      for (const args of ['', 'null', '1', '"scene"', '"","",1', '"scene",""']) {
        assert.equal(await main.evaluate(`corona.window.${method}(${args}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
      }
    }
    assert.equal(await main.evaluate('corona.window.resizeCameraView(800,600,"missing-scene","missing-camera")'), false);
    for (const args of ['', '0,600,"",""', '800,NaN,"",""', '800.5,600,"",""', '800,600,null,null', '800,600,"","",1']) {
      assert.equal(await main.evaluate(`corona.window.resizeCameraView(${args}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    assert.equal(await docs[1].client.evaluate('corona.window.toggleMainBorderless().catch(e=>e.code)'), 'PERMISSION_DENIED');
    assert.equal(await main.evaluate('corona.window.toggleMainBorderless(null).catch(e=>e.code)'), 'INVALID_ARGUMENT');
    assert.equal(await main.evaluate('corona.window.toggleMainBorderless()'), true);
    assert.equal(await main.evaluate('corona.window.toggleMainBorderless()'), true);
    evidence[0].cameraWindowControls = 'passed validation, missing camera and non-camera isolation; positive lifecycle recorded under real-camera below';
    evidence[0].mainBorderless = 'passed toggle and restore with JSON disabled';
    assert.deepEqual(await main.evaluate('corona.window.closeCameraView("missing-scene","missing-camera")'), {closed:false});
    for (const args of ['', 'null', '1', '"scene"', '"scene",null', '"scene","camera",1']) {
      assert.equal(await main.evaluate(`corona.window.closeCameraView(${args}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    for (const args of ['', 'null', '1', '"",1', '{}']) {
      assert.equal(await main.evaluate(`corona.window.suspendCameraViews(${args}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    for (const options of ['null', '[]', '{width:0}', '{height:-1}', '{width:1.5}', '{detached:1}', '{x:Infinity}',
      '{panelId:4}', '{dockingPos:"main"}', '{nativeDocked:"true"}', '{width:2147483648}']) {
      assert.equal(await main.evaluate(`corona.window.createPanel(${options}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    const mainId = await main.evaluate('corona.window.getCurrent().then(x=>x.tabId)');
    assert.equal(await main.evaluate(`corona.window.detachPanel({tabId:${mainId}}).catch(e=>e.code)`), 'PERMISSION_DENIED');
    assert.equal(await docs[1].client.evaluate(`corona.window.detachPanel({tabId:${createdPanels[1].tab_id}}).catch(e=>e.code)`), 'PERMISSION_DENIED');
    assert.equal(await main.evaluate('corona.window.detachPanel({tabId:2147483647})'), false);
    assert.equal(await main.evaluate('corona.window.redockPanel({tabId:2147483647})'), false);
    const initialDock = await docs[1].client.evaluate('corona.window.getCurrent()');
    assert.ok(['docked', 'detaching', 'detached', 'redocking'].includes(initialDock.dockState), 'native snapshot must expose actual Dock state');
    // The existing frame runner may have restored a floating viewport from its
    // layout. Establish Docked through the real redock operation before detach.
    await main.evaluate(`corona.window.redockPanel({tabId:${createdPanels[0].tab_id}})`);
    await main.evaluate('corona.window.suspendCameraViews("")');
    assert.equal(await docs[1].client.evaluate('corona.window.getCurrent().then(x=>x.dockState)'), 'docked');
    assert.equal(await main.evaluate(`corona.window.detachPanel({tabId:${createdPanels[0].tab_id},x:120,y:120,width:320,height:240})`), true);
    await main.evaluate('corona.window.suspendCameraViews("")');
    const detachedState = await docs[1].client.evaluate('corona.window.getCurrent()');
    assert.equal(detachedState.dockState, 'detached');
    assert.equal(detachedState.detached, true);
    assert.equal(await main.evaluate(`corona.window.detachPanel({tabId:${createdPanels[0].tab_id}})`), false, 'double detach must be rejected');
    assert.equal(await main.evaluate(`corona.window.redockPanel({tabId:${createdPanels[0].tab_id}})`), true);
    for (const method of ['detachPanel', 'redockPanel']) {
      for (const options of ['null', '[]', '{tabId:-1}', '{tabId:"3"}', '{width:-1}', '{maximized:1}', '{x:1.5}']) {
        assert.equal(await main.evaluate(`corona.window.${method}(${options}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
      }
    }
    evidence[0].panelCreationAndDocking = 'passed with JSON disabled; existing native Dock state machine retained';
    await docs[1].client.evaluate('corona.window.setDragRegions([])');
    assert.deepEqual(await main.evaluate('corona.window.getCurrent().then(x=>x.dragRegions)'), [{ x: 2, y: 3, w: 40, h: 12 }]);

  }
  await main.evaluate(`window.__smoke.emit('state',{speed:2})`);
  await delay(500);
  for (const doc of docs) assert.deepEqual(await doc.client.evaluate('window.__received'), [{ speed: 2 }]);
  if (testNativeBindings) {
    stage = 'refresh with pending native reads';
    const dock = docs[1].client;
    const before = await dock.evaluate('corona.window.getCurrent()');
    await dock.evaluate(`(() => { window.__oldNativeContext = true;
      for(let i=0;i<8;i++) corona.window.getCurrent().catch(()=>{});
      return true; })()`);
    await dock.call('Page.reload', {});
    await waitFor(() => dock.evaluate('!window.__oldNativeContext && typeof window.corona?.window?.getCurrent === "function"'));
    const after = await dock.evaluate('corona.window.getCurrent()');
    assert.equal(after.browserId, before.browserId);
    assert.deepEqual(after.dragRegions, []);
    assert.equal(typeof after.frameId, 'string');
    evidence[1].refreshWithPendingReads = 'passed';
    assert.deepEqual(await main.evaluate('corona.window.getCurrent().then(x=>x.dragRegions)'), [{ x: 2, y: 3, w: 40, h: 12 }]);
    stage = 'actual native window removal event';
    const closedTab = await docs[2].client.evaluate('corona.window.getCurrent()');
    const mainTab = await main.evaluate('corona.window.getCurrent()');
    assert.equal(await main.evaluate(`corona.window.focusTab(${closedTab.tabId})`), true);
    assert.equal(await main.evaluate('corona.window.focusTab(2147483647)'), false);
    assert.equal(await main.evaluate(`corona.window.closeTab(${mainTab.tabId}).catch(e=>e.code)`), 'PERMISSION_DENIED');
    assert.equal(await main.evaluate('corona.window.close().catch(e=>e.code)'), 'PERMISSION_DENIED');
    assert.equal(await docs[1].client.evaluate(`corona.window.closeTab(${closedTab.tabId}).catch(e=>e.code)`), 'PERMISSION_DENIED');
    for (const args of ['null', '-1', '1.5', '"3"', 'Infinity', '1,2']) {
      assert.equal(await main.evaluate(`corona.window.closeTab(${args}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    assert.equal(await main.evaluate(`corona.window.closeTab(${closedTab.tabId})`), true);
    stage = 'receiving actual native window removal event';
    console.log(`CEF smoke: ${stage}`);
    const completion = await waitFor(async () => {
      const events = await main.evaluate('({closed:window.__nativeClosed,unsubscribed:window.__unsubscribedEvents})');
      return events.closed.length ? events : null;
    }, 10_000);
    assert.deepEqual(completion.closed, [{tabId:closedTab.tabId, panelId:'smoke-detached'}]);
    assert.deepEqual(completion.unsubscribed, []);
    assert.equal(await main.evaluate(`corona.window.closeTab(${closedTab.tabId})`), false);
    stage = 'self close settles its V8 promise before retiring the context';
    await main.evaluate(`window.__selfCloseSettled = []; window.__smoke.on('self-close-settled', value => window.__selfCloseSettled.push(value)); true`);
    await docs[1].client.evaluate(`(() => { ${source}
      JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); };
      window.__smoke = createWebMessages({scope:${JSON.stringify(scope)},documentId:'closing-dock'});
      corona.window.close().then(value => window.__smoke.emit('self-close-settled', value));
      return true;
    })()`);
    // The peer's close promise publishes through the Web module before teardown.
    await waitFor(async () => (await main.evaluate('window.__nativeClosed.length')) === 2, 10_000);
    assert.deepEqual(await main.evaluate('window.__selfCloseSettled'), [true]);
    evidence[1].selfCloseBeforeContextRelease = 'passed with JSON disabled';
    evidence[2].nativeClosedEvent = 'passed (actual tab identity, throwing callback isolated, unsubscribe idempotent)';
    stage = 'native camera window with a real scene identity';
    console.log(`CEF smoke: ${stage}`);
    const cameraWorld = path.join(output, `camera-world-${Date.now()}`);
    await mkdir(path.join(cameraWorld, 'Assets'), { recursive: true });
    await writeFile(path.join(cameraWorld, 'scene.ini'), `[format]\ntype=corona_scene_folder\nversion=1\n[scene]\nname=CEF camera smoke\n[world]\ntype=creative\n[actors]\n[scripts]\npath=\n[terrain]\ntype=\npath=\n[camera]\ncount=2\ncamera0.id=main-camera\ncamera1.id=smoke-camera\ncamera1.width=320\ncamera1.height=240\n`);
    await writeFile(path.join(cameraWorld, 'assets.manifest.json'), '{"format":"corona_scene_assets","version":1,"bundles":[]}');
    const fixtureApi = (api, args) => main.evaluate(`window.__fixtureApi(${JSON.stringify({ api, args })})`);
    const listCameras = async () => ({ data: await main.evaluate(`(async()=>{
      const state=await corona.scene.readActive();
      return corona.scene.readCameras(state.scene,{archive_session:state.archive_session});})()`) });
    stage = 'orphan mesh movement event removed from the native contract';
    const eventNames = (await fixtureApi('EditorApi.list_events', [])).data.events.map(spec => spec.event);
    assert.equal(eventNames.includes('SceneTools.meshMoveResult'), false);
    assert.equal(eventNames.includes('SceneTools.actorPickResult'), false);
    assert.equal(eventNames.includes('SceneTools.viewportGizmoPointerResult'), false);
    assert.match(await main.evaluate("corona.editor.on('SceneTools.viewportGizmoPointerResult', () => {})")
      .then(() => 'unexpected subscription', error => error.message), /event is unavailable/);
    assert.match(await main.evaluate("corona.editor.on('SceneTools.meshMoveResult', () => {})")
      .then(() => 'unexpected subscription', error => error.message), /event is unavailable/);
    evidence[0].orphanMeshMovement = 'legacy bridge absent in every window; retired mesh and Gizmo events absent and subscriptions refused';
    const opened = await fixtureApi('ProjectLauncher.open_project', [cameraWorld]);
    assert.equal(opened.data?.ok, true, JSON.stringify(opened));
    const listed = await listCameras();
    const camera = listed.data.cameras.find(item => item.camera_id === 'smoke-camera');
    assert.ok(camera?.archive_session);
    assert.equal(await main.evaluate("corona.editor.invoke('MainView.on_init',[]).then(()=>false,()=>true)"), true);
    const sceneRead = await main.evaluate('corona.scene.readActive()');
    assert.equal(await main.evaluate('typeof corona.scene.focusActor'), 'function');
    assert.equal(await main.evaluate('typeof corona.scene.setActorTransform'), 'function');
    assert.equal(await main.evaluate('typeof window.coronaBridge?.actorTransform'), 'undefined');
    assert.equal(sceneRead.archive_session, camera.archive_session);
    stage = 'native project documents with structured identity and JSON disabled';
    console.log(`CEF smoke: ${stage}`);
    const fileScope = { archive_session: sceneRead.archive_session, project_path: cameraWorld,
      identity: ['node_graph', 'actor', 'scene.ini', '角色:"A"'] };
    const legacyFolder = path.join(cameraWorld, 'Scripts', 'files', createHash('sha256').update(JSON.stringify(fileScope.identity)).digest('hex'));
    await mkdir(legacyFolder, { recursive: true });
    const oldCode = '# old revision\n';
    const oldRevision = createHash('sha256').update(oldCode).digest('hex');
    const legacyDocument = { target: { id: 'existing-target' }, workspace: { nodes: [{ id: 'existing-node' }] } };
    await writeFile(path.join(legacyFolder, `${oldRevision}.py`), oldCode);
    await writeFile(path.join(legacyFolder, 'document.json'), JSON.stringify({ document: legacyDocument, revision: oldRevision,
      code_path: path.relative(cameraWorld, path.join(legacyFolder, `${oldRevision}.py`)).replaceAll('\\', '/'),
      document_path: path.relative(cameraWorld, path.join(legacyFolder, 'document.json')).replaceAll('\\', '/') }));
    const fileCall = (method, request) => main.evaluate(`(async()=>{
      const stringify=JSON.stringify,parse=JSON.parse;
      JSON.stringify=JSON.parse=()=>{throw Error('JSON transport forbidden');};
      try{return await corona.projectFiles.${method}(${JSON.stringify(request)});}
      finally{JSON.stringify=stringify;JSON.parse=parse;}})()`);
    const oldDocument = await fileCall('load', fileScope);
    assert.equal(oldDocument.status, 'loaded');
    assert.deepEqual(oldDocument.document, legacyDocument, 'existing key encoding must remain readable');
    const fileDocument = { target: { id: 'existing-target' }, workspace: {
      nodes: [{ id: '节点', extra: [null, false, 2 ** 40 + .5, '文本\0tail'] }], empty: {}, list: [] } };
    const fileCode = '# 新代码\nprint("世界")\n';
    const savedDocument = await fileCall('save', { ...fileScope, code: fileCode, document: fileDocument });
    assert.equal(savedDocument.status, 'saved');
    assert.deepEqual(savedDocument.document, fileDocument);
    assert.equal(savedDocument.document_path, oldDocument.document_path);
    assert.equal(savedDocument.revision, createHash('sha256').update(fileCode).digest('hex'));
    assert.equal(await readFile(path.join(cameraWorld, savedDocument.code_path), 'utf8'), fileCode);
    assert.deepEqual(await fileCall('load', fileScope), { ...savedDocument, status: 'loaded' });
    assert.equal((await fileCall('load', { ...fileScope, identity: ['node_graph', 'project', '', ''] })).status, 'missing');
    for (const changed of [{ archive_session: 'retired' }, { project_path: path.dirname(cameraWorld) }]) {
      assert.equal((await fileCall('save', { ...fileScope, ...changed, code: 'must not publish', document: {} })).code, 'PROJECT_CONTEXT_CHANGED');
    }
    assert.deepEqual((await fileCall('load', fileScope)).document, fileDocument);
    for (const method of ['save', 'load']) {
      assert.match(await fixtureApi(`ProjectFiles.${method}`, [{}]).then(() => 'unexpected retired route', error => error.message), /not defined/);
    }
    for (const invalid of [{ ...fileScope, key: 'encoded DTO' }, { ...fileScope, identity: [] },
      { ...fileScope, identity: ['node_graph', 'actor', '', ''] }, { ...fileScope, identity: ['node_graph', 'project', 'wrong', ''] }]) {
      assert.equal(await main.evaluate(`corona.projectFiles.load(${JSON.stringify(invalid)}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    for (const expression of ['(()=>{const value={};value.self=value;return value;})()',
      '{value:()=>true}', '{value:Infinity}', '"encoded document"',
      '(()=>{let value={};for(let i=0;i<140;i++)value={child:value};return value;})()']) {
      assert.equal(await main.evaluate(`corona.projectFiles.save({...${JSON.stringify(fileScope)},code:'',document:${expression}}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    evidence[0].projectFiles = 'named V8 save/load with JSON disabled; structured identity preserves legacy folders, opaque workspace values and exact script revision; stale archive/path, malformed values and retired generic routes rejected';
    assert.equal(listed.data.active_camera_id, 'main-camera');
    for (const value of listed.data.cameras) {
      assert.equal(value.archive_session, sceneRead.archive_session);
      assert.equal(typeof value.opaque_handle, 'string');
      for (const field of ['handle', 'vision_pt_settings', 'vision_restir_settings', 'vision_reblur_settings', 'vision_relax_settings'])
        assert.equal(field in value, false);
      for (const field of ['vision_denoise', 'vision_accumulation', 'vision_stable_planes', 'view_open', 'deletable'])
        assert.equal(typeof value[field], 'boolean');
    }
    const cameraReadScope = { archive_session: sceneRead.archive_session };
    stage = 'typed resource import validation without opening a dialog';
    assert.equal(await main.evaluate('typeof corona.scene.importResourceFile'), 'function');
    const unsupportedImport = await main.evaluate(`corona.scene.importResourceFile('scene.ini',${JSON.stringify({ ...cameraReadScope, file_type: 'unsupported' })})`);
    assert.equal(unsupportedImport.status, 'error');
    assert.equal(unsupportedImport.code, 'unsupported_file_type');
    assert.equal(unsupportedImport.archive_session, sceneRead.archive_session);
    assert.equal(unsupportedImport.scene, 'scene.ini');
    for (const args of [[], ['scene.ini'], ['', { ...cameraReadScope, file_type: 'model' }],
      ['scene.ini', {}], ['scene.ini', { ...cameraReadScope, file_type: 1 }],
      ['scene.ini', { ...cameraReadScope, file_type: 'model', extra: true }]])
      assert.equal(await main.evaluate(`corona.scene.importResourceFile(...${JSON.stringify(args)}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    for (const [scene, session] of [['other.ini', sceneRead.archive_session], ['scene.ini', 'retired']])
      assert.equal(await main.evaluate(`corona.scene.importResourceFile(${JSON.stringify(scene)},${JSON.stringify({ archive_session: session, file_type: 'model' })}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate("corona.editor.invoke('MainView.import_resource_file',[]).then(()=>false,()=>true)"), true);
    evidence[0].resourceImportValidation = 'JSON disabled: typed import rejects malformed arguments and inactive scope before prompting; unsupported type result retains scope; old Web route denied';
    stage = 'typed audio validation and retired manifest routes';
    for (const name of ['playResource', 'stopResource', 'playActor', 'stopActor'])
      assert.equal(await main.evaluate(`typeof corona.audio.${name}`), 'function');
    for (const id of [0, 1, 1.5, null, '', '0', '-1', '18446744073709551616', '12suffix']) {
      assert.equal(await main.evaluate(`corona.audio.playResource(${JSON.stringify(id)},false).catch(e=>e.code)`), 'INVALID_ARGUMENT');
      assert.equal(await main.evaluate(`corona.audio.stopResource(${JSON.stringify(id)}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    for (const name of ['playActor', 'stopActor']) {
      for (const [scene, session] of [['other.ini', sceneRead.archive_session], ['scene.ini', 'retired']])
        assert.equal(await main.evaluate(`corona.audio.${name}(${JSON.stringify(scene)},'missing-actor',${JSON.stringify({ archive_session: session, ...(name === 'playActor' ? { loop: false } : {}) })}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    }
    for (const method of ['play_audio', 'stop_audio', 'actor_play_audio', 'actor_stop_audio'])
      assert.equal(await main.evaluate(`corona.editor.invoke('SceneTools.${method}',[]).then(()=>false,()=>true)`), true);
    evidence[0].audioValidation = 'typed audio methods exist; invalid resource IDs and inactive scene/archive rejected; old manifest routes refused';
    assert.equal(await main.evaluate('typeof corona.scene.readTree'), 'function');
    const readTree = () => main.evaluate(`corona.scene.readTree('scene.ini',${JSON.stringify(cameraReadScope)})`);
    const initialTree = await readTree();
    assert.equal(initialTree.archive_session, sceneRead.archive_session);
    assert.equal(initialTree.scene, 'scene.ini');
    assert.deepEqual(initialTree.cameras, listed.data.cameras);
    assert.deepEqual(initialTree.actors, []);
    for (const args of [[], ['scene.ini'], ['', cameraReadScope], ['scene.ini', {}],
      ['scene.ini', { ...cameraReadScope, extra: true }]])
      assert.equal(await main.evaluate(`corona.scene.readTree(...${JSON.stringify(args)}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    for (const args of [['other.ini', cameraReadScope], ['scene.ini', { archive_session: 'retired' }]])
      assert.equal(await main.evaluate(`corona.scene.readTree(...${JSON.stringify(args)}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    for (const args of [[], ['scene.ini'], ['', cameraReadScope], ['scene.ini', {}], ['scene.ini', null],
      ['scene.ini', { ...cameraReadScope, extra: true }], ['scene.ini', { archive_session: 1 }]])
      assert.equal(await main.evaluate(`corona.scene.readCameras(...${JSON.stringify(args)}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    for (const args of [['other.ini', cameraReadScope], ['scene.ini', { archive_session: 'retired' }]])
      assert.equal(await main.evaluate(`corona.scene.readCameras(...${JSON.stringify(args)}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    evidence[0].cameraRead = 'one scoped typed V8 camera list with active ID, opaque handles, pose/window/render controls; no advanced settings JSON, no Actor geometry, no scene creation';
    stage = 'legacy numeric results retain the JavaScript safe integer range';
    const integerValues = { signedMin: -2147483648, signedMax: 2147483647,
      unsignedMax: 4294967295, widePositive: 2 ** 40, wideNegative: -(2 ** 40),
      safeMax: Number.MAX_SAFE_INTEGER, safeMin: Number.MIN_SAFE_INTEGER };
    // JSON here is a test-owned file format. The CEF application still has its
    // JSON functions disabled and receives the document content as native values.
    await writeFile(path.join(cameraWorld, 'integer-boundaries.json'), JSON.stringify(integerValues));
    const documentScope = { project_path: cameraWorld, archive_session: camera.archive_session, document: 'integer-boundaries.json' };
    const settingsCall = (method, request) => main.evaluate(`corona.projectSettings.${method}(${request === undefined ? '' : JSON.stringify(request)})`);
    const numericDocument = await settingsCall('readProjectDocument', documentScope);
    assert.equal(numericDocument.success, true);
    assert.deepEqual(numericDocument.content, integerValues);
    assert.equal('data' in numericDocument || 'version' in numericDocument, false);
    assert.equal(numericDocument.revision, createHash('sha256').update(await readFile(path.join(cameraWorld, documentScope.document))).digest('hex'));
    const newContent = { values: integerValues, nested: [null, {}, [], '中文\0'] };
    const committed = await settingsCall('commitProjectDocument', { ...documentScope, revision: numericDocument.revision, content: newContent });
    assert.equal(committed.success, true);
    assert.equal('version' in committed, false);
    assert.equal(committed.revision, createHash('sha256').update(await readFile(path.join(cameraWorld, documentScope.document))).digest('hex'));
    assert.deepEqual((await settingsCall('readProjectDocument', documentScope)).content, newContent);
    assert.equal((await settingsCall('commitProjectDocument', { ...documentScope, revision: numericDocument.revision, content: {} })).error, 'DOCUMENT_CONFLICT');
    for (const badScope of [{ archive_session: 'retired' }, { project_path: cameraWorld + '-other' }]) {
      assert.equal((await settingsCall('readProjectDocument', { ...documentScope, ...badScope })).error, 'PROJECT_CONTEXT_CHANGED');
    }
    for (const document of ['../escape.json', 'assets.manifest.json', 'Scripts/files/forbidden.json']) {
      assert.equal((await settingsCall('readProjectDocument', { ...documentScope, document })).error, 'INVALID_PROJECT_DOCUMENT');
    }
    for (const invalid of [{ ...documentScope, version: '{}' }, { ...documentScope, revision_only: false }, {}, []]) {
      assert.equal(await main.evaluate(`corona.projectSettings.readProjectDocument(${JSON.stringify(invalid)}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    for (const content of ['Infinity', '(()=>{const c={};c.self=c;return c})()', '(()=>{let c={};for(let i=0;i<130;i++)c={child:c};return c})()']) {
      assert.equal(await main.evaluate(`corona.projectSettings.commitProjectDocument({...${JSON.stringify(documentScope)},revision:${JSON.stringify(committed.revision)},content:${content}}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    const metadata = await settingsCall('getActiveProjectInfo');
    assert.equal(metadata.archive_session, camera.archive_session);
    assert.equal('data' in metadata, false);
    const updateInfo = { archive_session: metadata.archive_session, project_path: metadata.project_path, name: '设置测试', core_version: 'test-v8' };
    const updated = await settingsCall('saveActiveProjectInfo', updateInfo);
    assert.equal(updated.name, updateInfo.name);
    assert.equal(updated.core_version, updateInfo.core_version);
    assert.equal((await settingsCall('getActiveProjectInfo')).name, updateInfo.name);
    for (const badScope of [{ archive_session: 'retired' }, { project_path: cameraWorld + '-other' }]) {
      assert.equal(await main.evaluate(`corona.projectSettings.saveActiveProjectInfo(${JSON.stringify({ ...updateInfo, ...badScope })}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    }
    assert.equal(await main.evaluate(`corona.projectSettings.saveActiveProjectInfo(${JSON.stringify({ ...updateInfo, mode: 'story' })}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    await settingsCall('saveActiveProjectInfo', { ...updateInfo, name: metadata.name, core_version: metadata.core_version });
    for (const method of ['read_project_document', 'commit_project_document', 'get_active_project_info', 'save_active_project_info']) {
      assert.match(await fixtureApi(`ProjectSettings.${method}`, method === 'get_active_project_info' ? [] : [{}]).then(() => 'unexpected retired route', error => error.message), /not allowed/);
    }
    evidence[0].projectSettings = 'named V8 metadata and document operations with JSON disabled; digest CAS, structured numeric/unicode values, strict arguments, stale scope, path fences and retired CEF routes';
    stage = 'typed project file CRUD with captured archive and JSON disabled';
    console.log(`CEF smoke: ${stage}`);
    const filesScope = { project_path: metadata.project_path, archive_session: metadata.archive_session };
    const filesCall = (method, request) => main.evaluate(`corona.files.${method}(${JSON.stringify({ ...filesScope, ...request })})`);
    const rootTree = await filesCall('getFileTree', { path: '' });
    assert.equal(rootTree.archive_session, filesScope.archive_session);
    assert.equal(rootTree.isDirectory, true);
    assert.equal(rootTree.path, '');
    assert.equal('data' in rootTree, false);
    assert.ok(rootTree.children.some(entry => entry.name === 'scene.ini' && !entry.isDirectory && entry.size > 0));
    assert.equal((await filesCall('createFolder', { path: '', name: '文件测试' })).ok, true);
    const template = await filesCall('createActorTemplate', { path: '文件测试', name: '单位' });
    assert.equal(template.ok, true);
    assert.equal(template.file_type, 'actor');
    assert.equal(template.path, '文件测试/单位.actor');
    assert.match(await readFile(path.join(cameraWorld, template.path), 'utf8'), /\[base\]/);
    assert.equal((await filesCall('createActorTemplate', { path: '文件测试', name: '单位' })).path, '文件测试/单位_1.actor');
    assert.equal((await filesCall('getFileTree', { path: '文件测试' })).children.length, 2);
    assert.equal((await filesCall('renameItem', { path: template.path, name: '改名.actor' })).ok, true);
    assert.equal((await filesCall('openFile', { path: '文件测试/改名.actor', file_type: 'actor' })).ok, true);
    const sceneBytes = await readFile(path.join(cameraWorld, 'scene.ini'));
    for (const [method, request] of [
      ['renameItem', { path: 'scene.ini', name: 'moved.ini' }],
      ['deleteItem', { path: 'scene.ini' }], ['deleteItem', { path: '.' }],
      ['renameItem', { path: '文件测试/改名.actor', name: '单位_1.actor' }],
      ['createFolder', { path: '../escape', name: 'no' }],
      ['createActorTemplate', { path: '文件测试', name: '../escape' }],
    ]) {
      await assert.rejects(filesCall(method, request), /Cannot change|Target already exists|outside current project|Invalid file name/);
    }
    assert.deepEqual(await readFile(path.join(cameraWorld, 'scene.ini')), sceneBytes);
    for (const [method, extra] of [
      ['getFileTree', {}], ['createFolder', { name: 'no' }], ['createActorTemplate', { name: 'no' }],
      ['renameItem', { name: 'no' }], ['deleteItem', {}], ['openFile', { file_type: 'actor' }],
    ]) {
      for (const badScope of [{ archive_session: 'retired' }, { project_path: cameraWorld + '-other' }]) {
        assert.equal(await main.evaluate(`corona.files.${method}(${JSON.stringify({ ...filesScope, path: template.path, ...extra, ...badScope })}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
      }
      assert.equal(await main.evaluate(`corona.files.${method}(${JSON.stringify({ ...filesScope, path: template.path, ...extra, extra: true })}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    }
    assert.equal(await main.evaluate(`corona.files.openFile(${JSON.stringify({ ...filesScope, path: '../escape.actor', file_type: 'actor' })}).catch(e=>e.code)`), 'INVALID_PROJECT_PATH');
    assert.equal(await main.evaluate(`corona.files.openFile(${JSON.stringify({ ...filesScope, path: '文件测试/missing.actor', file_type: 'actor' })}).catch(e=>e.code)`), 'FILE_NOT_FOUND');
    assert.equal((await filesCall('deleteItem', { path: '文件测试/改名.actor' })).ok, true);
    assert.equal((await filesCall('deleteItem', { path: '文件测试' })).ok, true);
    assert.equal((await filesCall('getFileTree', { path: '' })).children.some(entry => entry.name === '文件测试'), false);
    for (const method of ['get_project_info', 'get_files', 'get_file_tree', 'create_file', 'create_folder', 'rename_item', 'delete_item', 'open_file']) {
      assert.match(await fixtureApi(`FileManager.${method}`, []).then(() => 'unexpected retired route', error => error.message), /not allowed/);
    }
    evidence[0].files = 'named V8 typed tree and CRUD with JSON disabled; Actor template collision, rename/open/delete, root/scene/path fences, stale archive/path and retired CEF routes';
    stage = 'one typed native environment read and patch with JSON disabled';
    console.log(`CEF smoke: ${stage}`);
    const environmentScope = { archive_session: camera.archive_session };
    const readEnvironment = () => main.evaluate(`corona.scene.getEnvironment('scene.ini',${JSON.stringify(environmentScope)})`);
    const originalEnvironment = await readEnvironment();
    assert.equal(originalEnvironment.archive_session, camera.archive_session);
    assert.equal(originalEnvironment.scene, 'scene.ini');
    assert.equal('data' in originalEnvironment, false);
    assert.equal('authored_lighting' in originalEnvironment, false, 'authored lighting remains archive/resource data, separate from live controls');
    const environmentPatch = { ...environmentScope, sun: { enabled: false, direction: [0, 0, 0] }, grid: { enabled: false },
      physics: { gravity: [1, -8, 2], floor_y: 4, floor_restitution: 0.3, fixed_dt: 0.02 } };
    const appliedEnvironment = await main.evaluate(`corona.scene.setEnvironment('scene.ini',${JSON.stringify(environmentPatch)})`);
    assert.deepEqual(appliedEnvironment.sun, { enabled: false, direction: [0, 0, 0] });
    assert.deepEqual(appliedEnvironment.grid, { enabled: false });
    assert.deepEqual(appliedEnvironment.physics.gravity, [1, -8, 2]);
    assert.equal(appliedEnvironment.physics.floor_y, 4);
    assert.ok(Math.abs(appliedEnvironment.physics.fixed_dt - 0.02) < 1e-6);
    for (const invalid of ['null', '[]', '{}', '{grid:{enabled:true}}',
      `{archive_session:${JSON.stringify(camera.archive_session)}}`,
      ...['null', '{}', '{enabled:1}', '{direction:[1,2]}', '{direction:[1,2,Infinity]}', '{direction:[1,2,1e100]}', '{enabled:true,persist:false}']
        .map(value => `{...${JSON.stringify(environmentScope)},grid:{enabled:true},sun:${value}}`),
      ...['null', '{}', '{enabled:"false"}', '{enabled:false,extra:1}']
        .map(value => `{...${JSON.stringify(environmentScope)},grid:${value}}`),
      ...['null', '{}', '{gravity:[1,NaN,3]}', '{gravity:[1,2,3,4]}', '{floor_y:"3"}', '{fixed_dt:Infinity}', '{mass:3}']
        .map(value => `{...${JSON.stringify(environmentScope)},grid:{enabled:true},physics:${value}}`),
      `{...${JSON.stringify(environmentScope)},floor_grid_enabled:true}`,
      `{...${JSON.stringify(environmentScope)},grid:{enabled:true},authored_lighting:{}}`]) {
      assert.equal(await main.evaluate(`corona.scene.setEnvironment('scene.ini',${invalid}).catch(error=>error.code)`), 'INVALID_ARGUMENT', invalid);
    }
    assert.equal(await main.evaluate("corona.scene.setEnvironment('scene.ini',{archive_session:'old',grid:{enabled:true}}).catch(error=>error.code)"), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.getEnvironment('other.ini',${JSON.stringify(environmentScope)}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.deepEqual(await readEnvironment(), appliedEnvironment, 'invalid mixed environment values cannot partly apply');
    const partialEnvironment = await main.evaluate(`corona.scene.setEnvironment('scene.ini',{...${JSON.stringify(environmentScope)},sun:{enabled:true}})`);
    assert.deepEqual(partialEnvironment.sun, { enabled: true, direction: [1, 1, 1] });
    assert.deepEqual(partialEnvironment.grid, appliedEnvironment.grid);
    assert.deepEqual(partialEnvironment.physics, appliedEnvironment.physics);
    assert.equal(await main.evaluate('corona.scene.readActive().then(value=>value.tree_revision)'), sceneRead.tree_revision);
    await main.evaluate(`corona.scene.setEnvironment('scene.ini',${JSON.stringify({ ...environmentScope,
      sun: originalEnvironment.sun, grid: originalEnvironment.grid, physics: originalEnvironment.physics })})`);
    evidence[0].environment = 'two direct V8 environment methods replace six Web routes: scoped live reads, mixed/partial patches, strict validation before any mutation, no JSON and no authored-lighting transport';
    assert.equal(sceneRead.scene, opened.data.scene.path);
    assert.equal(typeof sceneRead.tree_revision, 'string');
    assert.deepEqual(sceneRead.selection, {});
    assert.equal(typeof camera.opaque_handle, 'string');
    await main.evaluate(`corona.scene.selectActor('scene.ini','scene','',{
      sourceViewport:'cameraView',sourceCameraId:'smoke-camera',node_index:4294967294
    })`);
    const selection = await main.evaluate('corona.scene.readActive().then(value=>value.selection)');
    assert.deepEqual(selection, { scene: 'scene.ini', actor_type: 'scene', actor: '',
      source_viewport: 'cameraView', source_camera_id: 'smoke-camera', node_index: 4294967294 });
    // Selection source IDs are opaque metadata, matching the native camera/max
    // round-trip contract. They are not filesystem paths or window identities.
    await main.evaluate(`corona.scene.selectActor('scene.ini','scene','',{
      sourceViewport:'cameraView',sourceCameraId:'camera/max'
    })`);
    assert.equal(await main.evaluate('corona.scene.readActive().then(value=>value.selection.source_camera_id)'), 'camera/max');
    const beforeInvalid = await main.evaluate('corona.scene.readActive()');
    for (const context of ['null', '{sourceViewport:"unknown"}', '{sourceViewport:"cameraView",sourceCameraId:22}',
      '{sourceViewport:"cameraView",sourceCameraId:""}',
      '{node_index:-1}', '{node_index:4294967295}']) {
      assert.equal(await main.evaluate(`corona.scene.selectActor('scene.ini','scene','',${context}).catch(error=>error.code)`), 'INVALID_ARGUMENT', context);
    }
    assert.deepEqual(await main.evaluate('corona.scene.readActive()'), beforeInvalid);
    await writeFile(path.join(cameraWorld, 'triangle.ply'), 'ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n3 0 1 2\n');
    await writeFile(path.join(cameraWorld, 'replacement.ply'), 'ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nelement face 1\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n2 0 0\n0 2 0\n3 0 1 2\n');
    stage = 'creating native transform fixture Actor';
    console.log(`CEF smoke: ${stage}`);
    const createPath = path.join(cameraWorld, 'triangle.ply');
    const createOptions = { archive_session: camera.archive_session, actor_type: 'model', actor_name: 'Transform probe' };
    const actor = await main.evaluate(`corona.scene.createActor('scene.ini',${JSON.stringify(createPath)},${JSON.stringify(createOptions)})`);
    assert.equal(actor.name, 'Transform probe');
    assert.equal(actor.actor_type, 'model');
    assert.equal(actor.archive_session, camera.archive_session);
    assert.equal(typeof actor.handle, 'string');
    assert.equal(typeof actor.actor_guid, 'string');
    assert.match(await main.evaluate("corona.editor.invoke('SceneTools.create_actor',[]).catch(error=>error.message)"), /not allowed/);
    const beforeInvalidCreation = await readTree();
    for (const [scene, options, expected] of [
      ['scene.ini', {}, 'INVALID_ARGUMENT'],
      ['scene.ini', {...createOptions, archive_session: 'retired'}, 'PROJECT_CONTEXT_CHANGED'],
      ['wrong.ini', createOptions, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', {...createOptions, actor_type: 'story'}, 'INVALID_ARGUMENT'],
      ['scene.ini', {...createOptions, actor_type: 'audio', audio_resource_id: 42}, 'INVALID_ARGUMENT'],
      ['scene.ini', {...createOptions, actor_type: 'audio', audio_resource_id: '18446744073709551616'}, 'INVALID_ARGUMENT'],
      ['scene.ini', {...createOptions, extra: true}, 'INVALID_ARGUMENT'],
    ]) {
      assert.equal(await main.evaluate(`corona.scene.createActor(${JSON.stringify(scene)},${JSON.stringify(createPath)},${JSON.stringify(options)}).catch(error=>error.code)`), expected);
    }
    assert.equal((await readTree()).tree_revision, beforeInvalidCreation.tree_revision);
    evidence[0].actorCreation = 'typed V8 with JSON disabled, scoped Actor projection, retired CEF route denied, invalid requests leave scene unchanged';
    stage = 'renaming Actor through scoped typed V8';
    console.log(`CEF smoke: ${stage}`);
    await waitFor(async () => (await readTree()).actors.some(value =>
      value.actor_guid === actor.actor_guid && value.load_status === 'loaded'));
    const renameScope = { archive_session: camera.archive_session };
    const renameBefore = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(renameScope)})`);
    const renamed = await main.evaluate(`corona.scene.renameActor('scene.ini',${JSON.stringify(actor.actor_guid)},'Renamed transform probe',${JSON.stringify(renameScope)})`);
    assert.deepEqual(renamed, { status: 'success', archive_session: camera.archive_session, scene: 'scene.ini',
      actor_guid: actor.actor_guid, old_name: 'Transform probe', new_name: 'Renamed transform probe' });
    actor.name = renamed.new_name;
    const renameAfter = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(renameScope)})`);
    assert.equal(renameAfter.tree_revision, renameBefore.tree_revision + 1);
    assert.equal(renameAfter.actors.find(value => value.actor_guid === actor.actor_guid).name, actor.name);
    for (const [scene, id, name, scope, code] of [
      ['scene.ini', actor.actor_guid, '', renameScope, 'INVALID_ARGUMENT'],
      ['other.ini', actor.actor_guid, 'Wrong scene', renameScope, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', 'missing-actor', 'Missing actor', renameScope, 'ACTOR_NOT_FOUND'],
      ['scene.ini', actor.actor_guid, 'Retired archive', { archive_session: 'retired' }, 'PROJECT_CONTEXT_CHANGED'],
    ]) {
      assert.equal(await main.evaluate(`corona.scene.renameActor(${JSON.stringify(scene)},${JSON.stringify(id)},${JSON.stringify(name)},${JSON.stringify(scope)}).catch(error=>error.code)`), code);
    }
    assert.match(await main.evaluate(`window.__fixtureApi(${JSON.stringify({api:'SceneTools.rename_actor',args:['scene.ini',actor.name,'Legacy rename']})}).catch(error=>error.message)`), /not allowed/);
    const renameAfterRejected = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(renameScope)})`);
    assert.equal(renameAfterRejected.actors.find(value => value.actor_guid === actor.actor_guid).name, actor.name);
    evidence[0].actorRename = 'scoped typed V8, JSON disabled, exact archive/scene/GUID, tree revision update, invalid/stale requests atomic, legacy manifest rejected';
    stage = 'rebinding Actor resource through scoped typed V8';
    console.log(`CEF smoke: ${stage}`);
    const rebindScope = { archive_session: camera.archive_session };
    const rebindBefore = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(rebindScope)})`);
    const actorContextBeforeRebind = await main.evaluate(`corona.scene.readActorTree('scene.ini',${JSON.stringify(rebindScope)})`);
    const actorBeforeRebind = actorContextBeforeRebind.actors.find(value => value.actor_guid === actor.actor_guid);
    assert.ok(actorBeforeRebind);
    const rebound = await main.evaluate(`corona.scene.rebindActorResource('scene.ini',${JSON.stringify(actor.actor_guid)},'replacement.ply',${JSON.stringify(rebindScope)})`);
    assert.equal(rebound.ok, true);
    assert.equal(rebound.status, 'loaded');
    assert.equal(rebound.archive_session, camera.archive_session);
    assert.equal(rebound.scene, 'scene.ini');
    assert.equal(rebound.actor_guid, actor.actor_guid);
    assert.equal(rebound.actor.actor_guid, actor.actor_guid);
    assert.equal(rebound.actor.name, actor.name);
    assert.equal(rebound.actor.load_status, 'loaded');
    actor.handle = rebound.actor.handle;
    const rebindAfter = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(rebindScope)})`);
    assert.equal(rebindAfter.tree_revision, rebindBefore.tree_revision + 1);
    assert.deepEqual(rebindAfter.actors.find(value => value.actor_guid === actor.actor_guid),
      { actor_guid: actor.actor_guid, name: actor.name, type: 'ply' });
    const missingResource = await main.evaluate(`corona.scene.rebindActorResource('scene.ini',${JSON.stringify(actor.actor_guid)},'missing-resource.ply',${JSON.stringify(rebindScope)})`);
    assert.equal(missingResource.ok, false);
    assert.equal(missingResource.status, 'resource_error');
    assert.equal(missingResource.diagnostics[0].code, 'RESOURCE_NOT_FOUND');
    for (const [scene, guid, pathValue, scope, code] of [
      ['other.ini', actor.actor_guid, 'replacement.ply', rebindScope, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', actor.actor_guid, 'replacement.ply', { archive_session: 'retired' }, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', 'missing-actor', 'replacement.ply', rebindScope, 'ACTOR_NOT_FOUND'],
    ]) {
      assert.equal(await main.evaluate(`corona.scene.rebindActorResource(${JSON.stringify(scene)},${JSON.stringify(guid)},${JSON.stringify(pathValue)},${JSON.stringify(scope)}).catch(error=>error.code)`), code);
    }
    assert.equal(await main.evaluate(`corona.scene.rebindActorResource('scene.ini',${JSON.stringify(actor.actor_guid)},'',${JSON.stringify(rebindScope)}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    assert.match(await main.evaluate(`window.__fixtureApi(${JSON.stringify({api:'SceneTools.rebind_actor_resource',args:['scene.ini',actor.actor_guid,'replacement.ply',rebindScope]})}).catch(error=>error.message)`), /not allowed/);
    evidence[0].actorResourceRebind = 'scoped typed V8 with JSON disabled; resource replacement updates one tree revision and preserves GUID/name, missing resource returns diagnostics, stale/foreign identity and malformed path reject, legacy manifest denied';
    stage = 'owned actor index projection without JSON or full scene geometry';
    console.log(`CEF smoke: ${stage}`);
    const indexScope = { archive_session: camera.archive_session };
    const actorIndex = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(indexScope)})`);
    assert.equal(actorIndex.archive_session, camera.archive_session);
    assert.equal(actorIndex.scene, 'scene.ini');
    assert.ok(Number.isSafeInteger(actorIndex.tree_revision));
    const indexedActor = actorIndex.actors.find(value => value.actor_guid === actor.actor_guid);
    assert.deepEqual(indexedActor, { actor_guid: actor.actor_guid, name: actor.name, type: 'ply' });
    assert.equal('cameras' in actorIndex, false);
    for (const invalid of [[], ['scene.ini'], ['', indexScope], ['scene.ini', {}],
      ['scene.ini', { ...indexScope, actors: [] }], ['scene.ini', indexScope, 'extra']]) {
      assert.equal(await main.evaluate(`corona.scene.readActorIndex(...${JSON.stringify(invalid)}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    for (const [scene, scope] of [['other.ini', indexScope], ['scene.ini', { archive_session: 'retired' }]]) {
      assert.equal(await main.evaluate(`corona.scene.readActorIndex(${JSON.stringify(scene)},${JSON.stringify(scope)}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    }
    evidence[0].actorIndex = 'scoped owned DTO, JSON disabled, lightweight fields only, stable GUID, main/CameraView equality, old archive and malformed request rejection';
    const actorContext = await main.evaluate(`corona.scene.readActorTree('scene.ini',${JSON.stringify(indexScope)})`);
    assert.equal(actorContext.archive_session, camera.archive_session);
    assert.equal(actorContext.scene, 'scene.ini');
    const contextualActor = actorContext.actors.find(value => value.actor_guid === actor.actor_guid);
    assert.equal(contextualActor.name, actor.name);
    assert.deepEqual(contextualActor.geometry, actorBeforeRebind.geometry);
    assert.equal(typeof contextualActor.physics_enabled, 'boolean');
    for (const field of ['ik_chains', 'material', 'world_aabb', 'local_aabb', 'mechanics', 'optics']) {
      assert.equal(field in contextualActor, false, `actor context must exclude ${field}`);
    }
    evidence[0].actorContext = 'scoped typed assistant projection carries transform, size and physics without full Actor JSON';
    const richTree = await readTree();
    const treeActor = richTree.actors.find(value => value.actor_guid === actor.actor_guid);
    assert.equal(treeActor.name, actor.name);
    assert.equal(treeActor.opaque_handle, String(actor.handle));
    assert.equal('handle' in treeActor, false);
    assert.equal(treeActor.load_status, 'loaded');
    assert.equal(treeActor.load_error, null);
    assert.ok(Number.isSafeInteger(treeActor.model_node_count));
    for (const field of ['world_aabb', 'geometry', 'material', 'model_nodes']) assert.equal(field in treeActor, false);
    assert.deepEqual(richTree.cameras, (await listCameras()).data.cameras);
    evidence[0].sceneTree = 'scoped direct V8 Actor rows and shared camera projection with JSON disabled';
    const indexTiming = await main.evaluate(`(async()=>{
      const run=async read=>{const durations=[];let result;for(let i=0;i<20;i++){
        const start=performance.now();result=await read();durations.push(performance.now()-start);
      }return {durations,result};};
      return {baseline:await run(()=>corona.scene.readTree('scene.ini',${JSON.stringify(indexScope)})),
        index:await run(()=>corona.scene.readActorIndex('scene.ini',${JSON.stringify(indexScope)})),
        properties:await run(()=>corona.scene.readActorProperties('scene.ini',${JSON.stringify(actor.actor_guid)},${JSON.stringify(indexScope)})),
        cameras:await run(()=>corona.scene.readCameras('scene.ini',${JSON.stringify(indexScope)}))};
    })()`);
    const projectionTiming = sample => {
      const durations = [...sample.durations].sort((a, b) => a - b);
      return { requests: durations.length, p50Ms: durations[Math.floor(durations.length * .5)],
        p95Ms: durations[Math.ceil(durations.length * .95) - 1],
        resultBytes: Buffer.byteLength(JSON.stringify(sample.result.data ?? sample.result)) };
    };
    evidence[0].actorIndexPerformance = { actors: actorIndex.actors.length,
      cameras: indexTiming.baseline.result.cameras.length,
      sceneTree: projectionTiming(indexTiming.baseline), actorIndex: projectionTiming(indexTiming.index) };
    evidence[0].actorPropertyPerformance = { actors: actorIndex.actors.length,
      cameras: indexTiming.baseline.result.cameras.length,
      sceneTree: projectionTiming(indexTiming.baseline), actorProperties: projectionTiming(indexTiming.properties) };
    evidence[0].cameraReadPerformance = { actors: actorIndex.actors.length,
      cameras: indexTiming.cameras.result.cameras.length,
      sceneTree: projectionTiming(indexTiming.baseline), cameraList: projectionTiming(indexTiming.cameras) };
    stage = 'one scoped native focus application with no old focus queue or transport';
    console.log(`CEF smoke: ${stage}`);
    const camerasBeforeFocus = (await listCameras()).data.cameras;
    const focusArgs = ['scene.ini', actor.actor_guid, 'smoke-camera', { archive_session: camera.archive_session }];
    const focused = await main.evaluate(`corona.scene.focusActor(...${JSON.stringify(focusArgs)})`);
    assert.equal(focused.archive_session, camera.archive_session);
    assert.equal(focused.scene, 'scene.ini');
    assert.equal(focused.actor_guid, actor.actor_guid);
    assert.equal(focused.camera.camera_id, 'smoke-camera');
    assert.equal(focused.camera.opaque_handle, camera.opaque_handle);
    assert.equal('handle' in focused.camera, false);
    assert.ok(focused.distance >= 1);
    const camerasAfterFocus = (await listCameras()).data.cameras;
    assert.deepEqual(camerasAfterFocus.find(value => value.camera_id === 'main-camera'),
      camerasBeforeFocus.find(value => value.camera_id === 'main-camera'));
    assert.deepEqual(camerasAfterFocus.find(value => value.camera_id === 'smoke-camera').position, focused.camera.position);
    for (const invalid of [[], ['scene.ini', actor.actor_guid, 'smoke-camera'],
      ['scene.ini', actor.actor_guid, 'smoke-camera', {}], ['scene.ini', actor.actor_guid, 12, focusArgs[3]],
      ['scene.ini', actor.actor_guid, 'smoke-camera', { ...focusArgs[3], cameraHandle: 12 }]]) {
      assert.equal(await main.evaluate(`corona.scene.focusActor(...${JSON.stringify(invalid)}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    for (const [field, value, code] of [[0, 'other.ini', 'PROJECT_CONTEXT_CHANGED'],
      [1, 'missing-actor', 'ACTOR_NOT_FOUND'], [2, camera.name, 'CAMERA_NOT_FOUND'],
      [2, camera.opaque_handle, 'CAMERA_NOT_FOUND'], [3, { archive_session: 'old' }, 'PROJECT_CONTEXT_CHANGED']]) {
      const invalid = [...focusArgs]; invalid[field] = value;
      assert.equal(await main.evaluate(`corona.scene.focusActor(...${JSON.stringify(invalid)}).catch(error=>error.code)`), code);
    }
    assert.deepEqual((await listCameras()).data.cameras, camerasAfterFocus);
    assert.match(await main.evaluate(`window.__fixtureApi(${JSON.stringify({api:'ResourceSearch.focus_actor',args:[]})}).catch(error=>error.message)`), /not defined/);
    evidence[0].actorFocus = 'one scoped typed native camera application with JSON disabled; exact IDs, validation/no mutation, other camera unchanged, orphan Fast and result event removed';
    const patch = { archive_session: camera.archive_session, position: [0, 2, -3], rotation: [0, 0, 0], scale: [2, 1, 3] };
    stage = 'applying native transform patch';
    console.log(`CEF smoke: ${stage}`);
    const transformed = await main.evaluate(`corona.scene.setActorTransform('scene.ini',${JSON.stringify(actor.actor_guid)},${JSON.stringify(patch)})`);
    assert.equal(transformed.status, 'success');
    assert.equal(transformed.actor.actor_guid, actor.actor_guid);
    assert.equal(transformed.actor.actor_version, actor.actor_version + 1);
    assert.deepEqual(transformed.actor.geometry, { position: patch.position, rotation: patch.rotation, scale: patch.scale });
    const partial = await main.evaluate(`corona.scene.setActorTransform('scene.ini',${JSON.stringify(actor.actor_guid)},${JSON.stringify({ archive_session: camera.archive_session, rotation: [1, 2, 3] })})`);
    assert.deepEqual(partial.actor.geometry, { ...transformed.actor.geometry, rotation: [1, 2, 3] });
    assert.equal(partial.actor.actor_version, transformed.actor.actor_version + 1);
    const readActor = async () => ({ data: await main.evaluate(`corona.scene.readActorProperties('scene.ini',${JSON.stringify(actor.actor_guid)},${JSON.stringify(indexScope)})`) });
    stage = 'reading transformed native Actor';
    console.log(`CEF smoke: ${stage}`);
    const beforeBadTransform = (await readActor()).data;
    assert.deepEqual(beforeBadTransform.geometry.position, patch.position);
    assert.deepEqual(beforeBadTransform.geometry.rotation, [1, 2, 3]);
    assert.equal(beforeBadTransform.actor_version, partial.actor.actor_version);
    assert.equal(beforeBadTransform.archive_session, camera.archive_session);
    assert.equal(beforeBadTransform.scene, 'scene.ini');
    assert.equal(beforeBadTransform.actor_guid, actor.actor_guid);
    assert.equal(beforeBadTransform.handle, String(actor.handle));
    assert.equal(beforeBadTransform.load_status, 'loaded');
    for (const field of ['world_aabb', 'local_aabb', 'material', 'model_node_overrides', 'animation_sources'])
      assert.equal(field in beforeBadTransform, false);
    for (const invalid of [[], ['scene.ini'], ['scene.ini', '', indexScope], ['scene.ini', actor.actor_guid, {}],
      ['scene.ini', actor.actor_guid, { ...indexScope, extra: true }], ['scene.ini', 42, indexScope],
      ['', actor.actor_guid, indexScope], ['scene.ini', actor.actor_guid, null]])
      assert.equal(await main.evaluate(`corona.scene.readActorProperties(...${JSON.stringify(invalid)}).catch(e=>e.code)`), 'INVALID_ARGUMENT');
    for (const [scene, scope] of [['other.ini', indexScope], ['scene.ini', { archive_session: 'retired' }]])
      assert.equal(await main.evaluate(`corona.scene.readActorProperties(${JSON.stringify(scene)},${JSON.stringify(actor.actor_guid)},${JSON.stringify(scope)}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.readActorProperties('scene.ini','missing',${JSON.stringify(indexScope)}).catch(e=>e.code)`), 'ACTOR_NOT_FOUND');
    evidence[0].actorPropertyRead = 'scoped owned property projection with JSON disabled; opaque handle, load state, shared mutation snapshot, no bounds/material/animation read';
    for (const invalid of ['null', '[]', '{}', '{position:[1,2,3]}',
      `{archive_session:${JSON.stringify(camera.archive_session)}}`,
      ...['null', '[1,2]', '[1,2,3,4]', '[1,NaN,3]', '[1,Infinity,3]', '[1,1e100,3]', '[1,"2",3]']
        .map(value => `{archive_session:${JSON.stringify(camera.archive_session)},position:[9,9,9],rotation:${value}}`),
      `{archive_session:${JSON.stringify(camera.archive_session)},pos:[1,2,3]}`,
      `{archive_session:${JSON.stringify(camera.archive_session)},position:[1,2,3],persist:false}`]) {
      stage = `rejecting native transform ${invalid}`;
      assert.equal(await main.evaluate(`corona.scene.setActorTransform('scene.ini',${JSON.stringify(actor.actor_guid)},${invalid}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    assert.equal(await main.evaluate(`corona.scene.setActorTransform('scene.ini',${JSON.stringify(actor.actor_guid)},{archive_session:'old',position:[9,9,9]}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.setActorTransform('other.ini',${JSON.stringify(actor.actor_guid)},${JSON.stringify(patch)}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.setActorTransform('scene.ini','missing',${JSON.stringify(patch)}).catch(error=>error.code)`), 'ACTOR_NOT_FOUND');
    const afterBadTransform = (await readActor()).data;
    assert.equal(afterBadTransform.actor_guid, beforeBadTransform.actor_guid);
    assert.equal(afterBadTransform.actor_version, beforeBadTransform.actor_version);
    for (const field of ['position', 'rotation', 'scale']) {
      assert.deepEqual(afterBadTransform.geometry[field], beforeBadTransform.geometry[field]);
    }
    stage = 'checking removed transform entry points';
    console.log(`CEF smoke: ${stage}`);
    assert.equal(await main.evaluate('typeof window.coronaBridge?.actorTransform'), 'undefined');
    evidence[0].actorTransform = 'one direct V8 patch with JSON disabled; native geometry/version shared with readers, partial fields preserved, invalid/stale writes atomic, Fast entry removed';
    stage = 'applying consolidated native Actor properties';
    console.log(`CEF smoke: ${stage}`);
    const properties = { archive_session: camera.archive_session, visible: false, follow_camera: true,
      mechanics: { mass: 7, restitution: 0.25, damping: 0.5, body_type: 'kinematic',
        linear_lock: [true, false, true], angular_lock: [false, true, false] },
      camera_lock: { enabled: true, position_offset: [1, 2, 3], rotation_offset: [4, 5, 6] } };
    const writeProperties = (value, suffix = '') => main.evaluate(`corona.scene.setActorProperties('scene.ini',${JSON.stringify(actor.actor_guid)},${value})${suffix}`);
    const propertyResult = await writeProperties(JSON.stringify(properties));
    assert.equal(propertyResult.actor.actor_version, afterBadTransform.actor_version + 1);
    assert.equal(propertyResult.actor.visible, false);
    assert.equal(propertyResult.actor.follow_camera, true);
    assert.deepEqual(propertyResult.actor.mechanics, properties.mechanics);
    assert.deepEqual(propertyResult.actor.camera_lock, properties.camera_lock);
    const stateAfterProperties = await main.evaluate('corona.scene.readActive()');
    for (const invalid of ['null', '[]', '{}', '{visible:false}',
      `{archive_session:${JSON.stringify(camera.archive_session)}}`,
      ...['{mass:NaN}', '{mass:Infinity}', '{mass:1e100}', '{mass:"4"}', '{body_type:"invalid"}',
        '{physics_enabled:true}', '{}', 'null', '{linear_lock:[1,0,1]}', '{angular_lock:[true,false]}', '{mass:1,unknown:2}']
        .map(value => `{archive_session:${JSON.stringify(camera.archive_session)},visible:true,mechanics:${value}}`),
      ...['{enabled:true,position_offset:[1,Infinity,0]}', '{}', '{enabled:1}', '{enabled:false,position_offset:[1,2]}',
        '{enabled:false,lock_to_camera:false}']
        .map(value => `{archive_session:${JSON.stringify(camera.archive_session)},visible:true,camera_lock:${value}}`),
      ...['["same.glb","./same.glb"]', '[""]', '[1]', '{}']
        .map(value => `{archive_session:${JSON.stringify(camera.archive_session)},visible:true,animation_sources:${value}}`),
      `{archive_session:${JSON.stringify(camera.archive_session)},visible:"false"}`,
      `{archive_session:${JSON.stringify(camera.archive_session)},mass:2}`]) {
      assert.equal(await writeProperties(invalid, '.catch(error=>error.code)'), 'INVALID_ARGUMENT');
    }
    assert.equal(await writeProperties(`{archive_session:'old',visible:true}`, '.catch(error=>error.code)'), 'PROJECT_CONTEXT_CHANGED');
    const afterInvalidProperties = (await readActor()).data;
    assert.equal(afterInvalidProperties.actor_version, propertyResult.actor.actor_version);
    assert.equal(afterInvalidProperties.visible, false);
    assert.equal(afterInvalidProperties.mechanics.mass, 7);
    assert.equal((await main.evaluate('corona.scene.readActive()')).tree_revision, stateAfterProperties.tree_revision);
    const propertyPartial = await writeProperties(JSON.stringify({ archive_session: camera.archive_session,
      visible: true, follow_camera: false, camera_lock: { enabled: false }, animation_sources: [] }));
    assert.equal(propertyPartial.actor.actor_version, propertyResult.actor.actor_version + 1);
    assert.equal(propertyPartial.actor.visible, true);
    assert.equal(propertyPartial.actor.follow_camera, false);
    assert.deepEqual(propertyPartial.actor.mechanics, properties.mechanics);
    assert.deepEqual(propertyPartial.actor.camera_lock, { ...properties.camera_lock, enabled: false });
    assert.deepEqual(propertyPartial.actor.requested_animation_sources, []);
    evidence[0].actorProperties = 'one typed V8 patch with JSON disabled: state/physics/camera-lock, partial preservation, animation source acceptance, invalid mixed patches atomic and all old CEF routes denied';
    for (const api of ['SceneTools.read_active_scene', 'SceneTools.read_actor', 'SceneTools.read_archive_scene', 'SceneTools.list_scene_tree', 'SceneTools.list_camera_views', 'SceneTools.get_scene_snapshot', 'SceneTools.select_actor', 'SceneTools.set_actor_transform',
      'SceneTools.set_actor_state', 'SceneTools.set_actor_physics', 'SceneTools.set_actor_camera_lock',
      'SceneTools.get_actor_skeleton_leaves', 'SceneTools.get_actor_ik_chains', 'SceneTools.set_actor_ik_chains',
      'SceneTools.list_actor_tree',
      'SceneTools.get_environment', 'SceneTools.set_environment', 'SceneTools.sun_direction',
      'SceneTools.floor_grid', 'SceneTools.get_physics_params', 'SceneTools.set_physics_params', 'SceneTools.focus_actor',
      'SceneTools.save_screenshot', 'SceneTools.select_screenshot_path', 'SceneTools.capture_viewport',
      'SceneTools.set_camera_pose', 'SceneTools.update_camera_view', 'SceneTools.open_camera_view',
      'SceneTools.close_camera_view', 'SceneTools.create_camera_view']) {
      assert.match(await main.evaluate(`window.__fixtureApi(${JSON.stringify({api,args:api.endsWith('select_actor')?['scene.ini','scene','']:[]})}).catch(error=>error.message)`), /not allowed/);
    }
    evidence[0].sceneReadAndSelection = 'direct V8 with JSON disabled: empty/active state, uint64 identity, uint32 node index, validation/no mutation, old CEF query denied';
    const options = { archiveSession: camera.archive_session, sceneId: opened.data.scene.path,
      cameraId: camera.camera_id, width: 320, height: 240, x: 160, y: 160,
      routePath: '/CameraView?uiWindow=real-camera&scene=scene.ini&camera=smoke-camera' };
    const toolPanel = await main.evaluate(`corona.window.createPanel({panelId:'SceneTools',nativeDocked:true,
      routePath:'/SceneBar?uiWindow=camera-tools',width:320,height:240})`);
    const toolTarget = await waitFor(async () => (await targets()).find(item => item.url.includes('uiWindow=camera-tools')), 30_000);
    const toolClient = await connect(toolTarget);
    await waitFor(() => toolClient.evaluate('typeof corona?.window?.getCurrent === "function"'));
    await toolClient.evaluate(`JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); }; true`);
    const createCamera = value => main.evaluate(`corona.window.createCameraView(${JSON.stringify(value)})`);
    const created = await toolClient.evaluate(`corona.window.createCameraView(${JSON.stringify(options)})`);
    assert.equal(Number.isInteger(created.tab_id), true);
    assert.equal(created.existing, false);
    assert.deepEqual(await createCamera(options), { tab_id: created.tab_id, existing: true });
    const cameraTarget = await waitFor(async () => (await targets()).find(item => item.url.includes('uiWindow=real-camera')), 30_000);
    const cameraClient = await connect(cameraTarget);
    await waitFor(() => cameraClient.evaluate('typeof corona?.window?.getCurrent === "function"'));
    await cameraClient.evaluate(`JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); }; true`);
    const currentIndex = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(indexScope)})`);
    assert.deepEqual(await cameraClient.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(indexScope)})`), currentIndex);
    stage = 'shared native camera properties with JSON disabled';
    console.log(`CEF smoke: ${stage}`);
    const cameraScope = { archive_session: camera.archive_session };
    stage = 'unified viewport configuration with JSON disabled';
    console.log(`CEF smoke: ${stage}`);
    const viewportPatch = (client, value, suffix = '') => client.evaluate(
      `corona.viewport.configure('scene.ini','smoke-camera',${value})${suffix}`);
    const calibration = { pe: 18, angle: 0.2, offset: 4, parallaxScale: 2 };
    const bounds = { x: 10, y: 20, width: 320, height: 240, renderWidth: 320, renderHeight: 240 };
    const configuredViewport = await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'stereo3d', calibration, bounds }));
    assert.equal(configuredViewport.archive_session, camera.archive_session);
    assert.equal(configuredViewport.scene, opened.data.scene.path);
    assert.equal(configuredViewport.camera_id, 'smoke-camera');
    assert.equal(configuredViewport.mode, 'stereo3d');
    assert.deepEqual(configuredViewport.bounds, bounds);
    for (const [key, value] of Object.entries(calibration)) assert.ok(Math.abs(configuredViewport.calibration[key] - value) < 1e-5);
    const cameraBounds = { ...bounds, x: 0, y: 0 };
    const cameraViewport = await viewportPatch(cameraClient, JSON.stringify({ ...cameraScope, bounds: cameraBounds }));
    assert.equal(cameraViewport.mode, 'stereo3d');
    assert.deepEqual(cameraViewport.calibration, configuredViewport.calibration);
    assert.deepEqual(cameraViewport.bounds, cameraBounds);
    assert.deepEqual((await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'stereo3d' }))).bounds, bounds,
      'bounds belong to the calling BrowserTab rather than another camera surface');
    for (const invalid of ['null', '[]', '{}', '{archive_session:1}', JSON.stringify(cameraScope),
      `{...${JSON.stringify(cameraScope)},mode:'invalid'}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',bounds:{...${JSON.stringify(bounds)},width:0}}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',bounds:{...${JSON.stringify(bounds)},x:-1}}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',bounds:{...${JSON.stringify(bounds)},renderWidth:1.5}}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',bounds:{...${JSON.stringify(bounds)},height:2147483648}}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',calibration:{...${JSON.stringify(calibration)},pe:0}}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',calibration:{...${JSON.stringify(calibration)},angle:NaN}}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',calibration:{...${JSON.stringify(calibration)},offset:Number.MAX_VALUE}}`,
      `{...${JSON.stringify(cameraScope)},calibration:{pe:18}}`,
      `{...${JSON.stringify(cameraScope)},mode:'flat2d',unknown:true}`]) {
      assert.equal(await viewportPatch(cameraClient, invalid, '.catch(error=>error.code)'), 'INVALID_ARGUMENT', invalid);
    }
    assert.deepEqual(await viewportPatch(cameraClient, JSON.stringify({ ...cameraScope, mode: 'stereo3d' })), cameraViewport,
      'invalid combined patches cannot partially mutate any viewport property');
    for (const [scene, id, session, code] of [
      ['wrong.ini', 'smoke-camera', camera.archive_session, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', camera.name, camera.archive_session, 'CAMERA_NOT_FOUND'],
      ['scene.ini', camera.opaque_handle, camera.archive_session, 'CAMERA_NOT_FOUND'],
      ['scene.ini', 'smoke-camera', 'retired', 'PROJECT_CONTEXT_CHANGED'],
    ]) {
      assert.equal(await cameraClient.evaluate(`corona.viewport.configure(${JSON.stringify(scene)},${JSON.stringify(id)},
        {archive_session:${JSON.stringify(session)},mode:'flat2d'}).catch(error=>error.code)`), code);
    }
    assert.equal(await cameraClient.evaluate(`corona.viewport.configure('scene.ini','main-camera',
      {...${JSON.stringify(cameraScope)},bounds:${JSON.stringify(bounds)}}).catch(error=>error.code)`), 'PERMISSION_DENIED');
    assert.deepEqual(await viewportPatch(cameraClient, JSON.stringify({ ...cameraScope, mode: 'stereo3d' })), cameraViewport);
    stage = 'pointer and system cursor share one typed state and context ownership';
    console.log(`CEF smoke: ${stage}`);
    const pointerState = { x: 12.5, y: 34.5, cursor: 'grab' };
    const pointerResult = await viewportPatch(main, JSON.stringify({ ...cameraScope, pointer: pointerState }));
    assert.deepEqual(pointerResult.pointer, pointerState);
    assert.equal(pointerResult.systemCursorHidden, true);
    for (const pointer of ['null', '{}', '{cursor:"unknown",x:1,y:2}', '{cursor:"arrow"}',
      '{cursor:"grab",x:-1,y:2}', '{cursor:"grab",x:NaN,y:2}', '{cursor:"grab",x:1,y:Infinity}',
      '{cursor:"grab",x:1,y:Number.MAX_VALUE}', '{cursor:"grab",x:1,y:2,owner_context:1}',
      '{cursor:"hidden",x:1}']) {
      assert.equal(await viewportPatch(main, `{...${JSON.stringify(cameraScope)},mode:'flat2d',pointer:${pointer}}`, '.catch(error=>error.code)'), 'INVALID_ARGUMENT');
    }
    assert.deepEqual(await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'stereo3d' })), pointerResult,
      'invalid mixed pointer patches cannot change mode, pointer or the OS cursor');
    for (const client of [main, cameraClient]) {
      assert.equal(await client.evaluate(`corona.viewport.configure('scene.ini','main-camera',
        {...${JSON.stringify(cameraScope)},pointer:${JSON.stringify(pointerState)}}).catch(error=>error.code)`), 'PERMISSION_DENIED');
    }
    assert.equal(await viewportPatch(toolClient, JSON.stringify({ ...cameraScope, pointer: pointerState }), '.catch(error=>error.code)'), 'PERMISSION_DENIED');
    const peerPointer = { ...pointerState, cursor: 'crosshair' };
    assert.equal((await viewportPatch(cameraClient, JSON.stringify({ ...cameraScope, pointer: peerPointer }))).systemCursorHidden, true);
    const oldSurface = await viewportPatch(main, JSON.stringify({ ...cameraScope, pointer: { cursor: 'hidden' } }));
    assert.deepEqual(oldSurface.pointer, peerPointer, 'old surface cleanup must not erase the new surface pointer');
    assert.equal(oldSurface.systemCursorHidden, false);
    // A real renderer context release must clean native pointer ownership even
    // when page teardown cannot send another Web request.
    await cameraClient.call('Page.reload', {});
    await waitFor(() => cameraClient.evaluate('typeof corona?.viewport?.configure === "function"'));
    await cameraClient.evaluate(`JSON.stringify = JSON.parse = () => { throw Error('JSON transport is forbidden'); }; true`);
    await waitFor(async () => (await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'stereo3d' }))).pointer.cursor === 'hidden');
    const afterReload = await viewportPatch(cameraClient, JSON.stringify({ ...cameraScope, pointer: pointerState }));
    assert.equal(afterReload.systemCursorHidden, true);
    assert.deepEqual(afterReload.pointer, pointerState);
    assert.equal((await viewportPatch(cameraClient, JSON.stringify({ ...cameraScope, pointer: { cursor: 'hidden' } }))).systemCursorHidden, false);
    evidence[0].viewportPointer = 'one configuration state replaces pointer/system-cursor Fast routes and render queue/cache; JSON disabled, atomic validation, exact surface identity, cross-context ownership, real renderer reload cleanup';
    await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'flat2d' }));
    evidence[0].viewportConfiguration = 'one typed V8 operation replaces three Fast routes; JSON disabled, exact scoped identity, atomic validation, partial preservation and per-window bounds isolation';
    stage = 'shared native camera properties with JSON disabled';
    const cameraPatch = (client, value, suffix = '') => client.evaluate(
      `corona.scene.setCameraProperties('scene.ini','smoke-camera',${value})${suffix}`);
    const beforeCameraPatch = (await listCameras()).data.cameras;
    const cameraPatchResult = await cameraPatch(main, JSON.stringify({ ...cameraScope, position: [3, 4, -5], fov: 55 }));
    assert.deepEqual(cameraPatchResult.camera.position, [3, 4, -5]);
    assert.equal(cameraPatchResult.camera.fov, 55);
    assert.equal(typeof cameraPatchResult.camera.opaque_handle, 'string');
    assert.equal('handle' in cameraPatchResult.camera, false);
    const configuredCamera = await cameraPatch(cameraClient, JSON.stringify({ ...cameraScope, move_speed: 0.5,
      width: 320, height: 240 }));
    assert.equal(configuredCamera.camera.move_speed, 0.5);
    assert.deepEqual(configuredCamera.camera.position, [3, 4, -5]);
    const afterCameraPatch = (await listCameras()).data.cameras;
    const patched = afterCameraPatch.find(value => value.camera_id === 'smoke-camera');
    const original = beforeCameraPatch.find(value => value.camera_id === 'smoke-camera');
    assert.deepEqual(patched.forward, original.forward);
    assert.deepEqual(patched.world_up, original.world_up);
    assert.equal(patched.output_mode, original.output_mode);
    assert.deepEqual(afterCameraPatch.filter(value => value.camera_id !== 'smoke-camera'),
      beforeCameraPatch.filter(value => value.camera_id !== 'smoke-camera'));
    for (const invalid of ['null', '[]', '{}', '{archive_session:1}',
      `{...${JSON.stringify(cameraScope)},position:[1,NaN,3],width:100}`,
      `{...${JSON.stringify(cameraScope)},position:[1,2,3],width:0}`,
      `{...${JSON.stringify(cameraScope)},move_speed:Infinity}`,
      `{...${JSON.stringify(cameraScope)},height:1.5}`,
      `{...${JSON.stringify(cameraScope)},view_open:1}`,
      `{...${JSON.stringify(cameraScope)},view_open:true,width:123}`,
      `{...${JSON.stringify(cameraScope)},view_open:false,width:123}`,
      `{...${JSON.stringify(cameraScope)},forward:null}`,
      `{...${JSON.stringify(cameraScope)},fov:180}`,
      `{...${JSON.stringify(cameraScope)},near_plane:100,far_plane:1}`,
      `{...${JSON.stringify(cameraScope)},position:[1,2,3],unknown:true}`]) {
      assert.equal(await cameraPatch(cameraClient, invalid, '.catch(error=>error.code)'), 'INVALID_ARGUMENT');
    }
    for (const [scene, id, session, code] of [
      ['wrong.ini', 'smoke-camera', camera.archive_session, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', camera.name, camera.archive_session, 'CAMERA_NOT_FOUND'],
      ['scene.ini', camera.opaque_handle, camera.archive_session, 'CAMERA_NOT_FOUND'],
      ['scene.ini', 'smoke-camera', 'retired', 'PROJECT_CONTEXT_CHANGED'],
    ]) {
      assert.equal(await cameraClient.evaluate(`corona.scene.setCameraProperties(${JSON.stringify(scene)},${JSON.stringify(id)},
        {archive_session:${JSON.stringify(session)},position:[9,8,7]}).catch(error=>error.code)`), code);
    }
    assert.deepEqual((await listCameras()).data.cameras, afterCameraPatch);
    evidence[0].cameraProperties = 'shared typed pose/settings patch from main and CameraView with JSON disabled; exact identity, opaque handle, partial preservation, atomic rejection, old CEF/Fast routes removed';
    stage = 'one scoped V8 pick Promise with actual GPU readback';
    console.log(`CEF smoke: ${stage}`);
    const pickOptions = { ...cameraScope, x: 160, y: 120, width: 320, height: 240 };
    const pick = (client, options = pickOptions, id = 'smoke-camera', scene = 'scene.ini') => client.evaluate(
      `corona.viewport.pick(${JSON.stringify(scene)},${JSON.stringify(id)},${JSON.stringify(options)})`);
    for (const value of ['null', '[]', '{}', '{archive_session:"a",x:NaN,y:0,width:1,height:1}',
      '{archive_session:"a",x:0,y:0,width:Infinity,height:1}',
      '{archive_session:"a",x:0,y:0,width:0,height:1}',
      '{archive_session:"a",x:0,y:0,width:1,height:1,cameraHandle:1}']) {
      assert.equal(await main.evaluate(`corona.viewport.pick('scene.ini','smoke-camera',${value}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    assert.equal(await toolClient.evaluate(`corona.viewport.pick('scene.ini','smoke-camera',${JSON.stringify(pickOptions)}).catch(error=>error.code)`), 'PERMISSION_DENIED');
    for (const [scene, id, session, code] of [
      ['wrong.ini', 'smoke-camera', camera.archive_session, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', camera.name, camera.archive_session, 'CAMERA_NOT_FOUND'],
      ['scene.ini', camera.opaque_handle, camera.archive_session, 'CAMERA_NOT_FOUND'],
      ['scene.ini', 'smoke-camera', 'old', 'PROJECT_CONTEXT_CHANGED'],
    ]) {
      assert.equal(await main.evaluate(`corona.viewport.pick(${JSON.stringify(scene)},${JSON.stringify(id)},
        {...${JSON.stringify(pickOptions)},archive_session:${JSON.stringify(session)}}).catch(error=>error.code)`), code);
    }
    const outside = await pick(cameraClient, { ...pickOptions, x: -1 });
    assert.equal(outside.status, 'miss');
    assert.equal(outside.archive_session, camera.archive_session);
    assert.equal(outside.camera_id, 'smoke-camera');
    assert.equal(await cameraClient.evaluate(`corona.viewport.pick('scene.ini','main-camera',${JSON.stringify(pickOptions)}).catch(error=>error.code)`), 'PERMISSION_DENIED');
    await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'flat2d' }));
    await main.evaluate(`corona.scene.setActorTransform('scene.ini',${JSON.stringify(actor.actor_guid)},
      {...${JSON.stringify(cameraScope)},position:[0,0,0],rotation:[0,0,0],scale:[1,1,1]})`);
    await cameraClient.evaluate(`corona.scene.setCameraProperties('scene.ini','smoke-camera',
      {...${JSON.stringify(cameraScope)},position:[-0.25,-0.25,3],forward:[0,0,-1],world_up:[0,1,0],fov:60})`);
    let picked;
    try {
      await waitFor(async () => {
        picked = await pick(cameraClient);
        return picked.status === 'success';
      }, 20_000);
    } catch (error) {
      await cameraClient.evaluate(`corona.scene.captureScreenshot('scene.ini','smoke-camera',
        {...${JSON.stringify(cameraScope)},path:${JSON.stringify(path.join(output, 'pick-debug.png'))}})`);
      console.log('Pick diagnostics', JSON.stringify({ picked, actor: (await readActor()).data,
        cameras: (await listCameras()).data }));
      throw error;
    }
    assert.equal(picked.actor_guid, actor.actor_guid);
    assert.equal(picked.actor_name, actor.name);
    assert.equal(typeof picked.actor_handle, 'string');
    assert.equal(picked.actor_handle, actor.opaque_handle || String(actor.handle));
    assert.ok(picked.node_index === null || Number.isInteger(picked.node_index));
    const simultaneous = await Promise.all([pick(main), pick(cameraClient)]);
    assert.ok(simultaneous.every(value => value.actor_guid === actor.actor_guid));
    await cameraClient.evaluate(`(() => { window.__oldPickContext = true;
      window.__oldPick = corona.viewport.pick('scene.ini','smoke-camera',${JSON.stringify(pickOptions)}).catch(()=>null);
      return true; })()`);
    await cameraClient.call('Page.reload', {});
    await waitFor(() => cameraClient.evaluate('!window.__oldPickContext && typeof corona?.viewport?.pick === "function"'));
    await cameraClient.evaluate(`JSON.stringify = JSON.parse = () => { throw new Error('JSON transport is forbidden'); }; true`);
    assert.equal((await pick(cameraClient)).actor_guid, actor.actor_guid);
    assert.match(await fixtureApi('SceneTools.pick_actor_at_pixel', ['scene.ini',0,0,320,240])
      .then(() => 'unexpected legacy pick', error => error.message), /not allowed/);
    assert.match(await main.evaluate("corona.editor.on('SceneTools.actorPickResult', () => {})")
      .then(() => 'unexpected subscription', error => error.message), /event is unavailable/);
    evidence[0].viewportPick = 'direct V8 with JSON disabled, actual GPU Actor/node hit, opaque handle, simultaneous main/CameraView isolation, actual reload with pending readback, miss/validation/surface permissions and retired query/event denied';
    stage = 'one scoped V8 script input queue with real Python consumption';
    console.log(`CEF smoke: ${stage}`);
    for (const client of [main, cameraClient, toolClient]) {
      assert.equal(await client.evaluate('typeof window.coronaBridge?.injectInput'), 'undefined');
    }
    const keyInput = { ...cameraScope, type: 'keyDown', code: 'KeyJ', key: 'd', modifiers: 'Shift' };
    const mouseInput = { ...cameraScope, type: 'mouse', event: 'click', button: 'LeftButton',
      x: 90, y: 80, viewport_x: 5, viewport_y: 6, viewport_width: 320, viewport_height: 240, picked_actor: actor.name };
    const dispatchInput = (client, event, id = 'smoke-camera', scene = 'scene.ini') => client.evaluate(
      `corona.input.dispatch(${JSON.stringify(scene)},${JSON.stringify(id)},${JSON.stringify(event)})`);
    for (const input of ['null', '[]', '{}', '{type:"keyDown"}',
      `{...${JSON.stringify(keyInput)},extra:true}`, `{...${JSON.stringify(keyInput)},code:1}`,
      `{...${JSON.stringify(mouseInput)},x:NaN}`, `{...${JSON.stringify(mouseInput)},viewport_width:0}`,
      `{...${JSON.stringify(mouseInput)},event:'unknown'}`, `{...${JSON.stringify(mouseInput)},button:'unknown'}`]) {
      assert.equal(await main.evaluate(`corona.input.dispatch('scene.ini','smoke-camera',${input}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    for (const [client, scene, cameraId, event, code] of [
      [toolClient, 'scene.ini', 'smoke-camera', keyInput, 'PERMISSION_DENIED'],
      [cameraClient, 'scene.ini', 'main-camera', keyInput, 'PERMISSION_DENIED'],
      [main, 'wrong.ini', 'smoke-camera', keyInput, 'PROJECT_CONTEXT_CHANGED'],
      [main, 'scene.ini', 'smoke-camera', { ...keyInput, archive_session: 'retired' }, 'PROJECT_CONTEXT_CHANGED'],
      [main, 'scene.ini', camera.opaque_handle, keyInput, 'CAMERA_NOT_FOUND'],
    ]) {
      assert.equal(await client.evaluate(`corona.input.dispatch(${JSON.stringify(scene)},${JSON.stringify(cameraId)},${JSON.stringify(event)}).catch(error=>error.code)`), code);
    }
    for (const method of ['key_event', 'key_release', 'mouse_event']) {
      assert.match(await fixtureApi(`Scripts.${method}`, []).then(() => 'unexpected legacy input', error => error.message), /not defined|Unknown|not found/i);
    }
    const tracePath = path.join(output, 'script-input.jsonl');
    await rm(tracePath, { force: true });
    const script = `import json\nfrom pathlib import Path\nfrom script_runtime import file_api\ndef run():\n    ctx = file_api.current_run()\n    ctx.keep_alive = lambda: True\n    def input_event(kind, *args):\n        with Path(${JSON.stringify(tracePath)}).open('a', encoding='utf-8') as stream:\n            stream.write(json.dumps([kind, args]) + '\\n')\n    ctx.on_input = input_event\n    file_api.mark('input-ready')\n`;
    await writeFile(path.join(cameraWorld, 'input_probe.py'), script);
    const scriptRequest = { archive_session: camera.archive_session, project_path: cameraWorld,
      path: 'input_probe.py', revision: createHash('sha256').update(script).digest('hex'), requestId: `input-${Date.now()}`,
      target_type: 'actor', scene_name: 'scene.ini', actor_name: actor.name };
    const scriptCall = (method, args = []) => cameraClient.evaluate(`corona.scripts.${method}(...${JSON.stringify(args)})`);
    const started = await scriptCall('runFile', [scriptRequest]);
    assert.ok(started.runId, JSON.stringify(started));
    assert.equal((await scriptCall('runFile', [scriptRequest])).runId, started.runId, 'duplicate start must recover the same run');
    assert.equal((await scriptCall('getRun', [scriptRequest.requestId])).runId, started.runId);
    assert.equal((await scriptCall('runFile', [{ ...scriptRequest, actor_name: 'different-identity' }])).code, 'REQUEST_CONFLICT');
    for (const method of ['run_file', 'get_run', 'stop', 'get_status']) {
      const args = method === 'get_status' ? [] : [{}];
      assert.match(await fixtureApi(`Scripts.${method}`, args).then(() => 'unexpected old script route', error => error.message), /not allowed/);
    }
    const cancelled = { ...scriptRequest, requestId: `cancelled-${Date.now()}` };
    assert.equal((await scriptCall('getRun', [cancelled.requestId])).status, 'unknown');
    assert.equal((await scriptCall('stop', ['', true, cancelled.requestId])).status, 'stopped');
    assert.equal((await scriptCall('runFile', [cancelled])).status, 'cancelled', 'a late start cannot revive a closed panel');
    assert.equal((await scriptCall('stop', ['foreign-run', false])).code, 'STALE_RUN');
    assert.equal((await scriptCall('getRun', [scriptRequest.requestId])).runId, started.runId);
    for (const bad of [{ ...scriptRequest, requestId: '' }, { ...scriptRequest, revision: 'invalid' },
      { ...scriptRequest, unexpected: true }]) {
      assert.equal(await cameraClient.evaluate(`corona.scripts.runFile(${JSON.stringify(bad)}).catch(error => error.code)`), 'INVALID_ARGUMENT');
    }
    assert.equal(await cameraClient.evaluate(`corona.scripts.runFile(${JSON.stringify({ ...scriptRequest, archive_session: 'foreign' })}).catch(error => error.code)`), 'PROJECT_CONTEXT_CHANGED');

    try {
      await waitFor(async () => (await scriptCall('getStatus')).marker?.id === 'input-ready');
      assert.equal((await filesCall('getFileTree', { path: '' })).isDirectory, true);
      for (const [method, request] of [
        ['createFolder', { path: '', name: 'blocked-folder' }],
        ['createActorTemplate', { path: '', name: 'blocked-template' }],
        ['renameItem', { path: 'integer-boundaries.json', name: 'blocked.json' }],
        ['deleteItem', { path: 'integer-boundaries.json' }],
      ]) await assert.rejects(filesCall(method, request), /Stop and restore the file run/);
      assert.deepEqual(await dispatchInput(main, keyInput), { status: 'queued' });
      assert.deepEqual(await dispatchInput(cameraClient, { ...cameraScope, type: 'keyUp', code: 'KeyJ', key: 'd' }), { status: 'queued' });
      assert.deepEqual(await dispatchInput(main, mouseInput), { status: 'queued' });
      assert.deepEqual(await dispatchInput(cameraClient, { ...mouseInput, event: 'move', picked_actor: '' }), { status: 'queued' });
      const rows = await waitFor(async () => {
        try { const rows = (await readFile(tracePath, 'utf8')).trim().split('\n').map(row => JSON.parse(row)); return rows.length >= 4 && rows; }
        catch { return false; }
      });
      assert.deepEqual(rows, [['key_down', ['KeyJ', ['Shift'], 'd']], ['key_up', ['KeyJ', [], 'd']],
        ['mouse', ['click', 'LeftButton', 90, 80, 5, 6, 320, 240, actor.name]],
        ['mouse', ['move', 'LeftButton', 90, 80, 5, 6, 320, 240, '']]]);
    } finally {
      await scriptCall('stop', [started.runId, false]);
    }
    evidence[0].scriptInput = 'one scoped typed V8 dispatch, JSON disabled, real Python file consumes keyboard display key and complete mouse context exactly once; invalid/retired/foreign scope and legacy routes denied';
    stage = 'native screenshot with JSON disabled and exact archive/camera identity';
    console.log(`CEF smoke: ${stage}`);
    const screenshotPath = path.join(output, 'native-screenshot.png');
    await rm(screenshotPath, { force: true });
    const screenshotOptions = { archive_session: camera.archive_session, path: screenshotPath };
    const screenshot = args => cameraClient.evaluate(`corona.scene.captureScreenshot(...${JSON.stringify(args)})`);
    const beforeCapture = (await listCameras()).data.cameras;
    const captured = await screenshot(['scene.ini', 'smoke-camera', screenshotOptions]);
    assert.deepEqual(captured, { status: 'success', archive_session: camera.archive_session,
      scene: 'scene.ini', camera_id: 'smoke-camera', path: screenshotPath });
    const png = await readFile(screenshotPath);
    assert.deepEqual(Array.from(png.subarray(0, 8)), [137, 80, 78, 71, 13, 10, 26, 10]);
    assert.equal(png.readUInt32BE(16), 320);
    assert.equal(png.readUInt32BE(20), 240);
    for (const invalid of [[], ['scene.ini', 'smoke-camera'], ['scene.ini', 'smoke-camera', {}],
      ['scene.ini', 1, screenshotOptions], ['scene.ini', 'smoke-camera', { ...screenshotOptions, path: '' }],
      ['scene.ini', 'smoke-camera', { ...screenshotOptions, path: null }],
      ['scene.ini', 'smoke-camera', { ...screenshotOptions, path: 'bad\0.png' }],
      ['scene.ini', 'smoke-camera', { ...screenshotOptions, camera: {} }]]) {
      assert.equal(await cameraClient.evaluate(`corona.scene.captureScreenshot(...${JSON.stringify(invalid)}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    for (const [scene, cameraId, scope, code] of [
      ['wrong.ini', 'smoke-camera', screenshotOptions, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', camera.name, screenshotOptions, 'CAMERA_NOT_FOUND'],
      ['scene.ini', camera.opaque_handle, screenshotOptions, 'CAMERA_NOT_FOUND'],
      ['scene.ini', 'smoke-camera', { ...screenshotOptions, archive_session: 'old' }, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', 'smoke-camera', { ...screenshotOptions, path: path.join(output, 'missing-parent', 'failed.png') }, 'CAPTURE_FAILED'],
    ]) {
      assert.equal(await cameraClient.evaluate(`corona.scene.captureScreenshot(...${JSON.stringify([scene, cameraId, scope])}).catch(error=>error.code)`), code);
    }
    assert.deepEqual((await listCameras()).data.cameras, beforeCapture);
    evidence[0].screenshot = 'typed direct V8 with JSON disabled: actual PNG signature/320x240 dimensions, exact identity, invalid options, capture failure, no pose/output changes';
    stage = 'deleting Actor through scoped typed V8';
    console.log(`CEF smoke: ${stage}`);
    const deleteScope = { archive_session: camera.archive_session };
    await main.evaluate(`corona.scene.selectActor('scene.ini','model',${JSON.stringify(actor.name)},{sourceViewport:'main'})`);
    assert.equal((await main.evaluate('corona.scene.readActive()')).selection.actor_guid, actor.actor_guid);
    const deleteBefore = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(deleteScope)})`);
    const deleted = await main.evaluate(`corona.scene.deleteActor('scene.ini',${JSON.stringify(actor.actor_guid)},${JSON.stringify(deleteScope)})`);
    assert.deepEqual(deleted, { status: 'success', archive_session: camera.archive_session, scene: 'scene.ini',
      actor_guid: actor.actor_guid, old_name: actor.name, tree_revision: deleteBefore.tree_revision + 1,
      selection_cleared: true });
    const deleteAfter = await main.evaluate(`corona.scene.readActorIndex('scene.ini',${JSON.stringify(deleteScope)})`);
    assert.equal(deleteAfter.tree_revision, deleted.tree_revision);
    assert.equal(deleteAfter.actors.some(value => value.actor_guid === actor.actor_guid), false);
    assert.deepEqual((await main.evaluate('corona.scene.readActive()')).selection, {});
    for (const [scene, id, scope, code] of [
      ['scene.ini', actor.actor_guid, deleteScope, 'ACTOR_NOT_FOUND'],
      ['scene.ini', actor.name, deleteScope, 'ACTOR_NOT_FOUND'],
      ['other.ini', actor.actor_guid, deleteScope, 'PROJECT_CONTEXT_CHANGED'],
      ['scene.ini', actor.actor_guid, { archive_session: 'retired' }, 'PROJECT_CONTEXT_CHANGED'],
    ]) {
      assert.equal(await main.evaluate(`corona.scene.deleteActor(${JSON.stringify(scene)},${JSON.stringify(id)},${JSON.stringify(scope)}).catch(error=>error.code)`), code);
    }
    assert.match(await main.evaluate(`window.__fixtureApi(${JSON.stringify({api:'SceneTools.remove_actor',args:['scene.ini',actor.name]})}).catch(error=>error.message)`), /not allowed/);
    evidence[0].actorDelete = 'scoped typed V8, JSON disabled, stable GUID identity, selection cleanup, tree revision, stale/foreign requests atomic, legacy manifest rejected';
    await cameraClient.evaluate(`(() => { ${source}
      window.__cameraMessages = createWebMessages({scope:${JSON.stringify(scope)},documentId:'real-camera'});
      window.__cameraMessages.handle('echo', value=>value);
      return true;
    })()`);
    assert.deepEqual(await cameraClient.evaluate(`window.__cameraMessages.request('main','echo',{
      id:'18446744073709551615',bytes:new Uint8Array([7,255])
    }).then(value=>({id:value.id,bytes:Array.from(value.bytes),typed:value.bytes instanceof Uint8Array}))`),
    { id: '18446744073709551615', bytes: [7, 255], typed: true });
    assert.deepEqual(await main.evaluate(`window.__smoke.request('real-camera','echo',new Uint8Array([3,9])).then(value=>Array.from(value))`), [3, 9]);
    await main.evaluate('corona.window.suspendCameraViews("")');
    const cameraWindow = await cameraClient.evaluate('corona.window.getCurrent()');
    assert.equal(cameraWindow.tabId, created.tab_id);
    assert.equal(cameraWindow.dockState, 'detached');
    assert.deepEqual(await cameraClient.evaluate(`corona.window.createCameraView(${JSON.stringify(options)})`),
      { tab_id: created.tab_id, existing: true });
    for (const method of ['toggleCameraMaximized', 'cycleCameraWindowMode', 'toggleCameraBorderless']) {
      assert.equal(await main.evaluate(`corona.window.${method}("scene.ini","smoke-camera")`), true);
    }
    assert.equal(await main.evaluate('corona.window.resizeCameraView(400,300,"scene.ini","smoke-camera")'), true);
    assert.equal((await viewportPatch(cameraClient, JSON.stringify({ ...cameraScope, mode: 'stereo3d', pointer: pointerState }))).systemCursorHidden, true);
    assert.deepEqual(await main.evaluate('corona.window.closeCameraView("scene.ini","smoke-camera")'), { closed: true });
    assert.equal((await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'stereo3d' }))).pointer.cursor, 'hidden',
      'native window removal must release the pointer even before renderer cleanup');
    await main.evaluate('corona.window.suspendCameraViews("")');
    const recreated = await createCamera({ ...options, routePath: '/CameraView?uiWindow=retiring-camera' });
    assert.notEqual(recreated.tab_id, created.tab_id);
    assert.equal(recreated.existing, false);
    assert.deepEqual(await main.evaluate('corona.window.suspendCameraViews("scene.ini")'), { closed: 1 });
    assert.equal((await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'stereo3d', pointer: pointerState }))).systemCursorHidden, true);
    const reloaded = await fixtureApi('ProjectLauncher.open_project', [cameraWorld]);
    assert.equal(reloaded.data?.ok, true);
    assert.equal(await main.evaluate(`corona.window.createCameraView(${JSON.stringify(options)}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.selectActor('scene.ini','scene','').catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.setActorTransform('scene.ini',${JSON.stringify(actor.actor_guid)},${JSON.stringify(patch)}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    const activeAgain = await main.evaluate('corona.scene.readActive()');
    assert.equal(await main.evaluate(`corona.scene.readTree('scene.ini',${JSON.stringify(cameraReadScope)}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.readCameras('scene.ini',${JSON.stringify(cameraReadScope)}).catch(e=>e.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.notEqual(activeAgain.archive_session, camera.archive_session);
    const replacedPointer = await viewportPatch(main, JSON.stringify({ archive_session: activeAgain.archive_session, mode: 'flat2d' }));
    assert.equal(replacedPointer.pointer.cursor, 'hidden');
    assert.equal(replacedPointer.systemCursorHidden, false);
    assert.equal(await main.evaluate(`corona.scene.focusActor(...${JSON.stringify(focusArgs)}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.captureScreenshot('scene.ini','smoke-camera',${JSON.stringify(screenshotOptions)}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await cameraPatch(main, JSON.stringify({ ...cameraScope, position: [9, 8, 7] }), '.catch(error=>error.code)'), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await viewportPatch(main, JSON.stringify({ ...cameraScope, mode: 'flat2d' }), '.catch(error=>error.code)'), 'PROJECT_CONTEXT_CHANGED');
    assert.equal(await main.evaluate(`corona.scene.getEnvironment('scene.ini',${JSON.stringify(environmentScope)}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.deepEqual(activeAgain.selection, {});
    assert.deepEqual(await main.evaluate(`corona.scene.selectActor('scene.ini','scene','')`),
      { status: 'success', scene: 'scene.ini', actor_type: 'scene', actor: '' });
    stage = 'typed camera object creation without a window state mutation';
    const creationRevision = activeAgain.tree_revision;
    for (const invalid of ['{}', '{archive_session:1}',
      `{archive_session:${JSON.stringify(activeAgain.archive_session)},name:null}`,
      `{archive_session:${JSON.stringify(activeAgain.archive_session)},view_open:true}`]) {
      assert.equal(await main.evaluate(`corona.scene.createCamera('scene.ini',${invalid}).catch(error=>error.code)`), 'INVALID_ARGUMENT');
    }
    assert.equal(await main.evaluate(`corona.scene.createCamera('scene.ini',${JSON.stringify({ archive_session: camera.archive_session })}).catch(error=>error.code)`), 'PROJECT_CONTEXT_CHANGED');
    assert.equal((await main.evaluate('corona.scene.readActive()')).tree_revision, creationRevision);
    const object = await main.evaluate(`corona.scene.createCamera('scene.ini',${JSON.stringify({ archive_session: activeAgain.archive_session, name: 'Typed camera probe' })})`);
    assert.equal(object.camera.view_open, false);
    assert.equal(object.camera.archive_session, activeAgain.archive_session);
    assert.equal(typeof object.camera.opaque_handle, 'string');
    assert.equal('handle' in object.camera, false);
    const stateOfCreated = async () => (await listCameras()).data.cameras
      .find(value => value.camera_id === object.camera.camera_id);
    assert.equal((await stateOfCreated()).view_open, false);
    await createCamera({ ...options, archiveSession: activeAgain.archive_session, cameraId: object.camera.camera_id,
      routePath: '/CameraView?uiWindow=typed-camera' });
    assert.equal((await stateOfCreated()).view_open, true);
    assert.deepEqual(await main.evaluate(`corona.window.closeCameraView('scene.ini',${JSON.stringify(object.camera.camera_id)})`), { closed: true });
    assert.equal((await stateOfCreated()).view_open, false);
    assert.deepEqual(await main.evaluate(`corona.window.closeCameraView('scene.ini',${JSON.stringify(object.camera.camera_id)})`), { closed: false });
    evidence[0].cameraCreation = 'typed archive-scoped camera object creation with JSON disabled; no window until one window operation, owner-published open/close state, idempotent close, retired CEF toggles rejected';
    assert.equal(await main.evaluate(`corona.window.closeTab(${toolPanel.tab_id})`), true);
    evidence.push({ document: 'real-camera', roundtrip: 'bidirectional structured-clone typed arrays with JSON disabled', nativeCameraLifecycle: 'passed with JSON disabled: create/existing, identity, permissions, geometry, close, recreate, suspend, archive replacement',
      fixtureScene: cameraWorld });
  }
  const result = { date: new Date().toISOString(), passed: true, documents: evidence, nativeBindings: testNativeBindings,
    fixture: 'file-origin HTML in production BrowserManager windows; test-owned scene loaded through unmigrated scene adapter; no user world',
    limitations: ['Packaged Vue collaboration and CameraView UI remain untested', 'Minimized-window and separate-engine isolation remain untested'] };
  await writeFile(path.join(output, 'result.json'), JSON.stringify(result, null, 2));
  console.log(JSON.stringify(result, null, 2));
} finally {
  for (const client of clients) client.close();
  if (child.pid && child.exitCode === null) {
    await new Promise(resolve => {
      const killer = spawn('taskkill', ['/PID', String(child.pid), '/T', '/F'], { windowsHide: true, stdio: 'ignore' });
      killer.once('exit', resolve); killer.once('error', resolve);
    });
  }
}
