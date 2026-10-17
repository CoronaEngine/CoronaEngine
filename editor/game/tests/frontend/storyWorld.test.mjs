import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import test from 'node:test';
import { createPlayerController, VIEW_LABELS } from '../../frontend/playerController.mjs';
import { createPlayerSave } from '../../frontend/playerSave.mjs';
import { ensureStoryCharacters, syncPlacementActors } from '../../frontend/storyActors.mjs';
import * as gameplayModule from '../../frontend/storyGameplay.mjs';
import * as storyPropsModule from '../../frontend/storyProps.mjs';
import * as prophetDialogueModule from '../../frontend/prophetDialogue.mjs';
import * as storyCubeModule from '../../frontend/storyCube.mjs';
import * as storyMapModule from '../../frontend/storyMap.mjs';
import * as storyMapHeightModule from '../../frontend/storyMapHeight.mjs';
import * as storyMapHeightLoaderModule from '../../frontend/storyMapHeightLoader.mjs';
import * as storyWorldRulesModule from '../../frontend/storyWorldRules.mjs';
import * as storyProphetActionsModule from '../../frontend/storyProphetActions.mjs';
import { STORY_CHARACTERS, PROPHET_GUID, resolveStoryAssetPath, sceneSnapshot,
  characterTransform } from '../../frontend/storyCharacters.mjs';
import { actorFixture, apiFixture, CHARACTER_BY_GUID } from './fixtures.mjs';
import { createStoryNavigationController } from '../../frontend/storyNavigation.mjs';
import { createStoryCameraController } from '../../../Frontend/src/utils/viewportStoryCamera.js';
import { editorApi } from '../../../Frontend/src/api/editorApi.js';
import * as launcher from '../../../Frontend/src/services/projectLauncherService.js';
import * as worldMode from '../../../Frontend/src/services/worldModeService.js';
import { NAVIGATION_KEY } from '../../frontend/storyNavigation.mjs';
import { gameplayConfig, projectReady } from './fixtures.mjs';
import * as lifecycle from '../../../Frontend/src/services/worldSessionLifecycle.js';
import lanchat from '../../../Frontend/src/stores/lanchat.js';

const require = createRequire(new URL('../../../Frontend/package.json', import.meta.url));
const vue = require('vue');
const { ref, proxyRefs, nextTick, watch, reactive, computed, withDirectives } = vue;
const { parse, compileScript, compileTemplate, babelParse } = require('vue/compiler-sfc');
const { descriptor } = parse(fs.readFileSync(new URL('../../../Frontend/src/views/layout/StoryWorld.vue', import.meta.url), 'utf8'));
const compiled = compileScript(descriptor, { id: 'story-navigation-test', genDefaultAs: 'StoryWorld' });
let setupSource = compiled.content;
for (const node of babelParse(compiled.content, { sourceType: 'module' }).program.body
  .filter(node => node.type === 'ImportDeclaration').reverse()) {
  // 样式副作用导入在这里无需执行，直接摘除
  if (/\.css$/.test(node.source.value)) { setupSource = setupSource.slice(0, node.start) + setupSource.slice(node.end); continue; }
  // 默认导入（例如图片资源）与具名导入都要能改写：
  // specifier.imported 只存在于具名导入上。
  const defaults = node.specifiers.filter(specifier => specifier.type === 'ImportDefaultSpecifier');
  const named = node.specifiers.filter(specifier => specifier.type === 'ImportSpecifier');
  const parts = [];
  if (named.length) {
    const bindings = named.map(specifier => `${JSON.stringify(specifier.imported.name)}: ${specifier.local.name}`).join(', ');
    parts.push(`const { ${bindings} } = modules[${JSON.stringify(node.source.value)}];`);
  }
  for (const specifier of defaults) {
    parts.push(`const ${specifier.local.name} = modules[${JSON.stringify(node.source.value)}].default;`);
  }
  setupSource = setupSource.slice(0, node.start) + parts.join(' ')
    + setupSource.slice(node.end);
}
const makeComponent = new Function('modules', `${setupSource}; return StoryWorld;`);
const template = compileTemplate({ source: descriptor.template.content, filename: 'StoryWorld.vue',
  id: 'story-ui-test', compilerOptions: { mode: 'function' } });
assert.deepEqual(template.errors, []);
const renderStory = new Function('Vue', template.code)(vue);
// HUD 与行囊的视觉分别移到了 StoryWorld.hud.css / StoryWorld.bag.css
// （分别复刻 ui2.html 与 bag2.html）；这里把三者一起解析，
// 使断言覆盖组件实际生效的样式。
const postcss = require('postcss');
const styles = postcss.parse([
  ...descriptor.styles.map(style => style.content),
  fs.readFileSync(new URL('../../../Frontend/src/views/layout/StoryWorld.hud.css', import.meta.url), 'utf8'),
  fs.readFileSync(new URL('../../../Frontend/src/views/layout/StoryWorld.bag.css', import.meta.url), 'utf8'),
].join('\n'));
function findNode(node, predicate) {
  if (!node || typeof node !== 'object') return null;
  if (predicate(node)) return node;
  for (const child of Array.isArray(node.children) ? node.children : []) {
    const match = findNode(child, predicate);
    if (match) return match;
  }
  return null;
}
const turn = () => new Promise(resolve => setImmediate(resolve));
const deferred = () => { let resolve, reject; const promise = new Promise((yes, no) => { resolve = yes; reject = no; }); return { promise, resolve, reject }; };
const MAIN = 'D:/story', CHILD = `${MAIN}/.game/subworld`;
function clickWorld(page) { if (!page.instance.inventoryOpen.value) page.instance.toggleInventory(); void page.instance.navigateWorld(); }
const event = (props = {}) => ({ code: 'KeyO', preventDefault() { this.defaultPrevented = true; }, stopPropagation() {}, ...props });
const pose = position => ({ name: 'main', position, forward: [0, 0, 1], world_up: [0, 1, 0], fov: 65 });
// 几何检查用的公共零件：把最终样式里同一选择器的所有声明合并成一条，再按
// 直角坐标把锚点还原成像素盒子。这样「两条贴底居中」这类叠放能被算出来，
// 而不是只断言某一条的 bottom 写成了什么（那只能证明写法，不能证明不重叠）。
const styleDeclarations = selector => {
  const values = {};
  styles.walkRules(rule => { if (rule.selectors.includes(selector))
    rule.walkDecls(d => { values[d.prop] = d.value; }); });
  return values;
};
const HUD_TOKENS = styleDeclarations('.story-world-viewport');
/**
 * Hold a movement key until `done()` is true, then release it.
 *
 * The Boss's own site decides how far the player has to walk, so tests must not hard-code a
 * frame count: they stop on the real predicate (line up on x, or "the swing would land").
 * `done()` is checked before each step and the key is always released, even when the budget
 * runs out, so a stuck key can never leak into the next assertion.
 */
async function walkTo(page, f, code, steps, done) {
  page.instance.onKeyDown(event({ code }));
  try {
    for (let i = 0; i < steps && !done(); i++) f.step(50);
  } finally {
    page.instance.camera.keyUp(event({ code }));
  }
}
function stylePixels(value) {
  if (typeof value !== 'string') return null;
  const trimmed = value.trim();
  const variable = /^var\(\s*(--[\w-]+)\s*(?:,\s*([^)]*))?\)$/.exec(trimmed);
  if (variable) return variable[2] ? stylePixels(variable[2]) : stylePixels(HUD_TOKENS[variable[1]]);
  // 令牌自己也可能是一句算出来的 clamp（例如 --hud-corner-block-height）。
  // 展开变量前先取「数值 × 变量」这种写法，clamp 中间项就落在这里。
  const scaled = /^(-?[\d.]+)\s*\*\s*(.+)$/.exec(trimmed);
  if (scaled) {
    const factor = stylePixels(scaled[1]);
    const base = stylePixels(scaled[2]);
    return factor !== null && base !== null && Number.isFinite(base) ? factor * base : null;
  }
  // 贪婪捕获到最后一个右括号：calc 里可能嵌着 var(...)，非贪婪会把它切成半截。
  let m = /^calc\(([\s\S]*)\)$/.exec(trimmed);
  if (m) {
    const sum = m[1].split('+').map(part => stylePixels(part.replace(/var\(\s*(--[\w-]+)\s*\)/g,
      (all, name) => HUD_TOKENS[name] ?? 'NaN'))).reduce((a, b) => a + b, 0);
    return Number.isFinite(sum) ? sum : null;
  }
  m = /^min\(([^)]*)\)$/.exec(trimmed);
  if (m) {
    const parts = m[1].split(',').map(part => stylePixels(part));
    return parts.every(Number.isFinite) ? Math.min(...parts) : null;
  }
  m = /^(-?[\d.]+)px$/.exec(trimmed);
  if (m) return Number(m[1]);
  m = /^clamp\(\s*(-?[\d.]+)px\s*,\s*([\s\S]+?)\s*,\s*(-?[\d.]+)px\s*\)$/.exec(trimmed);
  if (m) {
    const low = Number(m[1]), high = Number(m[3]), preferred = stylePixels(m[2]);
    if (preferred === null || !Number.isFinite(preferred)) return high;
    return Math.min(Math.max(preferred, low), high);
  }
  m = /^(-?[\d.]+)vw$/.exec(trimmed);
  if (m) return Number(m[1]) * 14.4; // 1440 宽的参照视口：两头都是像素值，比较仍然有效
  m = /^(-?[\d.]+)%$/.exec(trimmed);
  if (m) return Number(m[1]) / 100 * 1440; // 百分比只用来取锚点，取视口宽作参照
  return null;
}
// padding 的四值展开（只认 px / 令牌，够用即可）。
function stylePadding(selector) {
  const parts = String(styleDeclarations(selector).padding ?? '0').trim().split(/\s+/);
  const values = parts.map(part => stylePixels(part) ?? 0);
  const [top, right = top, bottom = top, left = right] = values;
  return { top, right, bottom, left };
}
// 贴底/贴顶居中这类元素在样式里量不到自己的宽高，硬算盒子只会得到错的答案
// （试过：按 font-size 估高度会把它算成一条线，于是对重叠沉默）。这里改成直接看锚点：
// 谁和谁抢同一个位置，是能确定的；「抢没抢到」不需要知道内容大小。
// 「同一个位置」= 同一水平带（都居中，或者都贴同一条边）
//             + 同一竖直带（都贴顶、或都贴底）+ 到该边的距离相同。
// 这就是之前那次事故的形状：物品栏与状态条都 left:50% + translateX(-50%)、
// 都用同一个 bottom，z-index 只差 1——谁盖谁都不重要，因为它们本该错开。
function styleAnchor(selector) {
  const d = styleDeclarations(selector);
  if (!d.position || !['absolute', 'fixed'].includes(d.position)) return null;
  const horizontal = /translateX\(-50%\)/.test(d.transform ?? '') && d.left === '50%' ? 'center'
    : d.right !== undefined ? `right:${d.right}` : d.left !== undefined ? `left:${d.left}` : null;
  const edge = d.bottom !== undefined ? 'bottom' : d.top !== undefined ? 'top'
    : /^0(px)?$/.test(d.inset ?? '') ? 'inset' : null;
  if (!horizontal || !edge) return null;
  return { horizontal, edge, distance: edge === 'inset' ? 0 : stylePixels(d[edge]), z: stylePixels(d['z-index']) ?? 0 };
}
// 同一水平带 + 同一竖直带 + 同一距离 = 抢占同一个位置。返回冲突说明，否则返回 null。
function sharedSlot(first, second) {
  const a = styleAnchor(first), b = styleAnchor(second);
  if (!a || !b) return null;
  if (a.horizontal !== b.horizontal) return null;
  if (a.edge !== b.edge) return null;
  if (a.distance !== b.distance) return null;
  return `${first} 与 ${second} 抢占同一个位置（${a.horizontal} / ${a.edge}:${a.distance}px，z-index ${a.z} / ${b.z}）`;
}
function collisionsFor(selectors) {
  const found = [];
  for (let i = 0; i < selectors.length; i += 1) {
    for (let j = i + 1; j < selectors.length; j += 1) {
      const hit = sharedSlot(selectors[i], selectors[j]);
      if (hit) found.push(hit);
    }
  }
  return found;
}

async function fixture(t, options = {}) {
  const globals = ['window', 'document', 'CustomEvent', 'ResizeObserver', 'requestAnimationFrame', 'cancelAnimationFrame'];
  const previous = Object.fromEntries(globals.map(key => [key, globalThis[key]]));
  // 高度网格是模块级状态：每个夹具都从「没有网格」开始，用例再按需装载，
  // 否则上一个用例的地面会悄悄留给下一个。
  storyMapHeightModule.clearHeightGrid();
  const calls = [], alerts = [], frames = new Map(), pages = [];
  let frameTime = performance.now();
  let activePath = MAIN, route = '/', frameId = 0, page = null, handle = 10;
  const poses = new Map([[MAIN, pose([1, 2, 3])], [CHILD, pose([5, 6, 7])]]);
  // Built from the fixture's table, which is where the boss's shipped placement lives:
  // this page defines it locally, so a literal list here drifts the moment the art moves.
  const actors = new Map([MAIN, CHILD].map(path => [path, [...CHARACTER_BY_GUID.values()].map(actorFixture)]));
  function surface() {
    const listeners = new Map();
    return {
      listeners,
      addEventListener(name, fn) { if (!listeners.has(name)) listeners.set(name, new Set()); listeners.get(name).add(fn); },
      removeEventListener(name, fn) { listeners.get(name)?.delete(fn); },
      dispatchEvent(event) { for (const fn of listeners.get(event.type) || []) fn(event); },
    };
  }
  const bridge = Object.fromEntries(['actorTransform', 'cameraMove', 'setCameraViewport', 'setViewportGizmoTarget', 'setViewportUiMode', 'setViewportSystemCursorHidden']
    .map(name => [name, (...args) => { calls.push([name, ...args]); return true; }]));
  globalThis.window = Object.assign(surface(), { coronaBridge: bridge, devicePixelRatio: 1,
    location: { href: 'file:///D:/engine/editor/Frontend/dist/index.html' },
    localStorage: { setItem() {} }, alert: message => alerts.push(message) });
  globalThis.document = Object.assign(surface(), { hidden: false, hasFocus: () => true });
  globalThis.CustomEvent = class { constructor(type, options) { Object.assign(this, { type }, options); } };
  globalThis.ResizeObserver = class { observe() {} disconnect() { calls.push(['disconnect']); } };
  globalThis.requestAnimationFrame = callback => { frames.set(++frameId, callback); return frameId; };
  globalThis.cancelAnimationFrame = id => frames.delete(id);

  t.mock.method(editorApi.projectSettings, 'getActiveProjectInfo', async () => ({ project_path: activePath, mode: 'story' }));
  t.mock.method(editorApi.project, 'getProjectLoadStatus', async () => options.loadStatus ? options.loadStatus(activePath) : projectReady(activePath));
  t.mock.method(editorApi.main, 'onInit', async () => options.init ? options.init() : { scenes: [{ path: 'scene.ini' }], active_index: 0 });
  t.mock.method(editorApi.scene, 'setActorTransform', async (sceneId, guid, transform) => {
    const source = activePath;
    calls.push(['playerSave', source, sceneId, guid, structuredClone(transform)]);
    await options.save?.();
    assert.equal(activePath, source, 'a pending save must never cross native scene replacement');
    const actor = actors.get(source).find(item => item.actor_guid === guid);
    for (const key of ['position', 'rotation']) if (transform[key]) actor.geometry[key] = [...transform[key]];
    return { status: 'success', actor: structuredClone(actor) };
  });
  t.mock.method(editorApi.scene, 'getSnapshot', async () => ({ scene: 'scene.ini', active_camera_name: 'main',
    actors: structuredClone(actors.get(activePath)),
    cameras: [{ ...poses.get(activePath), handle: ++handle }] }));
  t.mock.method(editorApi.viewport, 'setCameraPose', async (sceneId, name, camera) => {
    calls.push(['pose', activePath, sceneId, name, camera]);
    await options.pose?.();
    poses.set(activePath, { name, ...structuredClone(camera) });
    return { data: { status: 'success' } };
  });
  t.mock.method(editorApi.sceneTools, 'setActorState', async (scene, guid, state) => {
    const actor = actors.get(activePath).find(item => item.actor_guid === guid);
    Object.assign(actor, state);
    return { status: 'success', actor: structuredClone(actor) };
  });
  // 世界碎片（红球）在快照里并不存在，由剧情建模流程创建：这里补上同一份建模夹具
  t.mock.method(editorApi.sceneTools, 'createActor', async (scene, path, type, data) => {
    calls.push(['create', activePath, scene, path, structuredClone(data)]);
    const character = [...STORY_CHARACTERS, gameplayModule.FRAGMENT].find(item => item.guid === data.actor_guid);
    // 剧情世界的地图：引擎侧不是角色、也不是布局里的陈列物，它自带一条 static 的
    // 物理记录（真实引擎的 create_actor 会把 body_type 写进 mechanics）。
    if (!character && data.actor_guid === storyMapModule.MAP_GUID) {
      const actor = { name: data.name, actor_guid: data.actor_guid, handle: ++handle,
        load_status: 'loaded', render_ready: true, gpu_build_state: 'Ready',
        model_ref: data.model_ref, route: path, semantic_role: 'world-map',
        entity_type: 'story_scenery', entity_id: 'story.world.map',
        local_aabb: [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5], visible: true, follow_camera: false,
        camera_lock: { enabled: false },
        mechanics: { body_type: data.body_type },
        geometry: { position: [...data.position], rotation: [...data.rotation], scale: [...data.scale] } };
      const existing = actors.get(activePath).find(item => item.actor_guid === data.actor_guid);
      if (existing && data.update_if_exists) Object.assign(existing, actor);
      else if (!existing) actors.get(activePath).push(actor);
      return { status: 'success', actor: structuredClone(existing || actor) };
    }
    // 陈列物（guid 形如 story.placement.0000）不是剧情角色：它由布局驱动创建，
    // 形状与名字来自 storyProps / syncPlacementActors 写下的字段，不能套用角色夹具。
    if (!character) {
      const index = storyPropsModule.placementIndex(data.actor_guid);
      assert.notEqual(index, null, `createActor 收到未知 GUID：${data.actor_guid}`);
      const prop = storyPropsModule.findProp(
        String(data.entity_id || '').slice(storyPropsModule.PLACEMENT_PREFIX.length));
      assert.ok(prop, `createActor 收到未知陈列物：${data.entity_id}`);
      assert.equal(data.semantic_role, 'placement');
      const actor = { ...actorFixture({ role: 'placement', name: data.name, guid: data.actor_guid,
        height: prop.height, rotation: [...data.rotation], x: 0, z: 0 }),
        model_ref: '', route: path,
        geometry: { position: [...data.position], rotation: [...data.rotation], scale: [...data.scale] } };
      actors.get(activePath).push(actor);
      return { status: 'success', actor: structuredClone(actor) };
    }
    const actor = { ...actorFixture(character), model_ref: data.model_ref || '',
      geometry: { position: [...data.position], rotation: [...data.rotation], scale: [...data.scale] } };
    actors.get(activePath).push(actor);
    return { status: 'success', actor: structuredClone(actor) };
  });
  // 陈列物退场：syncPlacementActors 用 removeActor 撤下布局里已经没有的那一件。
  t.mock.method(editorApi.sceneTools, 'removeActor', async (scene, guid) => {
    calls.push(['remove', activePath, scene, guid]);
    const list = actors.get(activePath);
    const index = list.findIndex(item => item.actor_guid === guid);
    if (index >= 0) list.splice(index, 1);
    return { status: 'success' };
  });
  t.mock.method(editorApi.scratch, 'sendKeyEvent', async (key, mods, displayKey) => {
    if (key === gameplayModule.GAMEPLAY_KEY) {
      const request = JSON.parse(displayKey);
      // 小世界的自有文档（陈列 / 世界规则 / 名册）在进入世界时各读一次，它们不是战斗操作，
      // 因此不交给用例的战斗桩：默认给一份合法的空文档，用例可用 options.documents 覆盖。
      const documents = { loadPlacements: { version: 1, placements: [] },
        loadWorldRules: { version: 1, rules: [] },
        loadSubworlds: { version: 1, subworlds: [] }, ...options.documents };
      if (documents[request.action]) {
        return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
          state: structuredClone(documents[request.action]) };
      }
      if (options.gameplay) return options.gameplay(request, activePath);
      return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
        state: { version: 2, revision: 0, rage: 0, boss: { hp: 0 }, drop: null, inventory: { worldFragment: 0 } },
        config: gameplayConfig };
    }
    calls.push(['key', key, mods, displayKey]);
    if (options.prepare) return options.prepare(key);
    const { direction } = JSON.parse(displayKey);
    assert.equal(key, NAVIGATION_KEY);
    if ((activePath === CHILD && direction === 'enter') || (activePath === MAIN && direction === 'exit')) return { status: 'noop' };
    return { data: { status: 'ok', navigation: { source: activePath, target: activePath === MAIN ? CHILD : MAIN,
      direction, mode: 'story' } } };
  });
  t.mock.method(editorApi.project, 'openProject', async path => {
    calls.push(['nativeOpen', path]);
    const response = options.open ? await options.open(path) : { ok: true, path };
    if (response?.ok) activePath = path;
    return response;
  });
  t.mock.method(lanchat, 'finishWorldSession', async () => { calls.push(['chatCleanup']); });
  const realOpen = launcher.projectLauncherService.openProject;
  t.mock.method(launcher.projectLauncherService, 'openProject', (...args) => {
    const result = realOpen(...args);
    page?.unmount(); // App.vue removes the routed component on beginOpen.
    return result;
  });
  await worldMode.worldModeService.resolve(MAIN, { force: true });
  function mount() {
    const mounted = [], unmounted = [];
    const component = makeComponent({
      vue: { ref, nextTick, watch, reactive, computed, withDirectives, onMounted: fn => mounted.push(fn), onUnmounted: fn => unmounted.push(fn) },
      'vue-router': { onBeforeRouteLeave() {}, useRouter: () => ({ replace: async path => { route = path; } }) },
      '@/api/editorApi.js': { editorApi },
      '@/services/worldModeService.js': worldMode,
      '@/services/projectLauncherService.js': launcher,
      '@/services/worldSessionLifecycle.js': lifecycle,
      '@/utils/viewportStoryCamera.js': { createStoryCameraController },
      // 图片资源：测试里只当作一个 URL 字符串
      '@/assets/hero-figure.png': { default: 'hero-figure.png' },
      '../../../../game/frontend/storyNavigation.mjs': { createStoryNavigationController, NAVIGATION_KEY },
      '../../../../game/frontend/storyActors.mjs': { ensureStoryCharacters, syncPlacementActors },
      '../../../../game/frontend/playerController.mjs': { createPlayerController, VIEW_LABELS },
      '../../../../game/frontend/playerSave.mjs': { createPlayerSave },
      '../../../../game/frontend/storyGameplay.mjs': gameplayModule,
      '../../../../game/frontend/storyCharacters.mjs': { STORY_CHARACTERS, PROPHET_GUID, resolveStoryAssetPath, sceneSnapshot },
      '../../../../game/frontend/storyProps.mjs': storyPropsModule,
      '../../../../game/frontend/prophetDialogue.mjs': prophetDialogueModule,
      '../../../../game/frontend/storyCube.mjs': storyCubeModule,
      // 地图走真实模块：它的确认与幂等都写在里面，桩掉就失去了这条用例的意义。
      // 假时钟（wait 不平息）让「引擎当帧就确认」的桩不必真的等 200ms。
      '../../../../game/frontend/storyMap.mjs': { ...storyMapModule, ensureStoryMap: options => storyMapModule.ensureStoryMap({ ...options, wait: async () => {} }) },
      // 高度网格走真实模块；读盘这一步由用例注入（真实加载器在 file:// 页面上用 XHR，
      // 这里要断言的是「页面读没读、读到的东西有没有真的变成地面」）。
      '../../../../game/frontend/storyMapHeight.mjs': storyMapHeightModule,
      '../../../../game/frontend/storyMapHeightLoader.mjs': { ...storyMapHeightLoaderModule,
        loadStoryMapHeight: async url => {
          calls.push(['mapHeight', url]);
          if (!options.mapHeight) { storyMapHeightModule.clearHeightGrid(); return null; }
          return options.mapHeight(url);
        } },
      '../../../../game/frontend/storyWorldRules.mjs': storyWorldRulesModule,
      '../../../../game/frontend/storyProphetActions.mjs': storyProphetActionsModule,
    });
    const instance = component.setup({}, { expose() {} });
    instance.surface.value = { focus() {}, getBoundingClientRect: () => ({ left: 0, top: 0, width: 800, height: 600 }) };
    let disposed = false;
    page = { instance, mount: () => Promise.all(mounted.map(fn => fn())),
      unmount() { if (!disposed) { disposed = true; for (const fn of unmounted) fn(); } } };
    pages.push(page);
    return page;
  }
  t.after(async () => {
    for (const p of pages) p.unmount();
    await lifecycle.drainWorldSession();
    await lifecycle.flushWorldSessionSaves();
    worldMode.worldModeService.invalidate();
    storyMapHeightModule.clearHeightGrid();
    Object.assign(globalThis, previous);
  });
  return { mount, calls, alerts, frames, poses, actors, get route() { return route; },
    step(ms = 50) { frameTime = Math.max(frameTime, performance.now()) + ms;
      const pending = [...frames.values()]; frames.clear(); pending.forEach(fn => fn(frameTime)); },
    opens: () => calls.filter(call => call[0] === 'nativeOpen').map(call => call[1]) };
}

test('real story page inventory button retains independent poses, stays story, and never reuses old handles', async t => {
  const f = await fixture(t);
  let page = f.mount(); await page.mount();
  page.instance.camera.wheel(event({ deltaY: -100 })); f.step();
  page.instance.onKeyDown(event({ code: 'KeyW' }));
  const stale = [...f.frames.values()][0];
  const mainPose = page.instance.camera.snapshotPose().camera;
  clickWorld(page);
  assert.equal(page.instance.navigation.busy, true);
  const afterLock = page.instance.camera.snapshotPose().camera;
  page.instance.camera.wheel(event({ deltaY: -100 }));
  assert.deepEqual(page.instance.camera.snapshotPose().camera, afterLock);
  await turn();
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(f.frames.size, 0);
  const count = f.calls.length; stale(100);
  assert.equal(f.calls.length, count);
  assert.deepEqual(f.poses.get(MAIN).position, mainPose.position);
  assert.equal(worldMode.worldModeState.mode, 'story');
  assert.equal(f.route, '/');
  assert.equal(document.listeners.get('keydown').size, 0);
  assert.equal(window.listeners.get('pointermove').size, 0);

  page = f.mount(); await page.mount();
  page.instance.camera.pointerDown(event({ button: 2, clientX: 0, clientY: 0 }));
  page.instance.camera.pointerMove(event({ buttons: 2, clientX: 50, clientY: 20 }));
  page.instance.camera.wheel(event({ deltaY: -1 }));
  f.step();
  const childPose = page.instance.camera.snapshotPose().camera;
  clickWorld(page);
  await turn();
  assert.deepEqual(f.opens(), [CHILD, MAIN]);
  page = f.mount(); await page.mount();
  assert.deepEqual(page.instance.camera.snapshotPose().camera, mainPose);
  clickWorld(page); await turn();
  page = f.mount(); await page.mount();
  assert.deepEqual(page.instance.camera.snapshotPose().camera, childPose);
  // Rebinding must restore the controller orbit too, not just display one
  // saved frame before jumping back to default yaw/pitch on the next input.
  page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  const resumedPose = page.instance.camera.snapshotPose().camera;
  for (let i = 0; i < 3; ++i) assert.ok(Math.abs(resumedPose.forward[i] - childPose.forward[i]) < 1e-9);
  assert.deepEqual(f.alerts, []);
  assert.ok(f.calls.filter(call => call[0] === 'setViewportUiMode').every(call => call[2] === 'flat2d'));
});

test('O is ignored before bind; Escape cancels pending init and never binds a stale viewport', async t => {
  const init = deferred();
  const f = await fixture(t, { init: () => init.promise });
  const page = f.mount(); const mounted = page.mount(); await turn();
  clickWorld(page);
  assert.ok(!f.calls.some(call => call[0] === 'key'));
  page.instance.onKeyDown(event({ code: 'Escape' }));
  init.resolve({ path: 'scene.ini' }); await mounted; await turn();
  assert.equal(f.route, '/StartScreen');
  assert.ok(!f.calls.some(call => call[0] === 'setCameraViewport'));
});

test('real page filters repeated/editable/composition O and locks camera while preparation runs', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  for (const props of [{ repeat: true }, { ctrlKey: true }, { altKey: true }, { metaKey: true },
    { isComposing: true }, { target: { isContentEditable: true } }]) page.instance.onKeyDown(event(props));
  assert.ok(!f.calls.some(call => call[0] === 'pose'));
  clickWorld(page); await turn();
  const pose = page.instance.camera.snapshotPose();
  page.instance.onKeyDown(event({ code: 'KeyW' }));
  page.instance.camera.pointerDown(event({ button: 2 }));
  page.instance.camera.wheel(event({ deltaY: -1 }));
  clickWorld(page);
  assert.equal(f.frames.size, 0);
  assert.deepEqual(page.instance.camera.snapshotPose(), pose);
  prep.resolve({ status: 'error', message: 'copy failed' }); await turn();
  assert.deepEqual(f.opens(), []);
  assert.deepEqual(f.alerts, ['copy failed']);
  assert.equal(page.instance.navigation.busy, false);
});

test('Escape during copy ignores its late response and leaves the game page', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  clickWorld(page); await turn();
  page.instance.onKeyDown(event({ code: 'Escape' }));
  prep.resolve({ status: 'ok', navigation: { source: MAIN, target: CHILD, direction: 'enter', mode: 'story' } });
  await turn();
  assert.deepEqual(f.opens(), []);
  assert.equal(f.route, '/StartScreen');
  assert.deepEqual(f.alerts, []);
});

test('a newer canonical open drains preparation and prevents late subworld navigation', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  clickWorld(page); await turn();
  const other = launcher.projectLauncherService.openProject('D:/other'); await turn();
  assert.deepEqual(f.opens(), []); // Python still works against the source scene.
  prep.resolve({ status: 'ok', navigation: { source: MAIN, target: CHILD, direction: 'enter', mode: 'story' } });
  await other; await turn();
  assert.deepEqual(f.opens(), ['D:/other']);
  assert.equal(worldMode.worldModeState.projectPath, 'D:/other');
  assert.deepEqual(f.alerts, []);
});

test('failed target open recovers through canonical launcher after source disposal', async t => {
  const f = await fixture(t, { open: path => path === CHILD ? { ok: false, message: 'bad asset' } : { ok: true, path } });
  const page = f.mount(); await page.mount();
  clickWorld(page); await turn();
  assert.deepEqual(f.opens(), [CHILD, MAIN]);
  assert.equal(worldMode.worldModeState.status, 'ready');
  assert.equal(worldMode.worldModeState.mode, 'story');
  assert.match(f.alerts[0], /已恢复来源世界.*bad asset/);
  const restored = f.mount(); await restored.mount();
  assert.ok(restored.instance.camera.snapshotPose());
});

test('failed recovery routes to launcher and explains both failures', async t => {
  const f = await fixture(t, { open: () => { throw new Error('native open failed'); } });
  const page = f.mount(); await page.mount();
  clickWorld(page); await turn();
  assert.deepEqual(f.opens(), [CHILD, MAIN]);
  assert.equal(f.route, '/StartScreen');
  assert.match(f.alerts[0], /恢复来源世界也失败/);
});

test('global Escape during native open invalidates recovery and late mode changes', async t => {
  const opened = deferred();
  const f = await fixture(t, { open: () => opened.promise });
  const page = f.mount(); await page.mount();
  clickWorld(page); await turn();
  launcher.cancelPendingProjectOpen(); // App.vue owns Escape after source-page removal.
  opened.reject(new Error('late failure')); await turn();
  assert.deepEqual(f.opens(), [CHILD]);
  assert.deepEqual(f.alerts, []);
  assert.notEqual(worldMode.worldModeState.status, 'ready');
});


test('Escape saves the final moved player once; reopening restores position and facing', async t => {
  const f = await fixture(t); let page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyD' })); f.step();
  const moved = page.instance.camera.snapshotPlayer();
  assert.ok(moved.position[0] > 0);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 0);
  await page.instance.exitStory();
  assert.equal(f.route, '/StartScreen'); assert.equal(f.frames.size, 0);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
  page.unmount(); await worldMode.worldModeService.resolve(MAIN, { force: true });
  page = f.mount(); await page.mount();
  assert.deepEqual(page.instance.camera.snapshotPlayer().position, moved.position);
  assert.deepEqual(page.instance.camera.snapshotPlayer().rotation, moved.rotation);
  assert.ok(page.instance.camera.snapshotPose().camera.position[0] < moved.position[0]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
});

test('failed exit save keeps the route and supports a successful retry', async t => {
  let fail = true;
  const f = await fixture(t, { save: async () => { if (fail) throw new Error('disk is full'); } });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  await page.instance.exitStory();
  assert.equal(f.route, '/'); assert.deepEqual(f.opens(), []);
  assert.deepEqual(f.alerts, ['disk is full']); assert.equal(f.frames.size, 0);
  fail = false; await page.instance.exitStory();
  assert.equal(f.route, '/StartScreen'); assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 2);
});

test('canonical open waits for the old player save; later selections supersede earlier ones', async t => {
  const saved = deferred();
  const f = await fixture(t, { save: () => saved.promise }); const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyD' })); f.step();
  const stale = [...f.frames.values()][0];
  const first = launcher.projectLauncherService.openProject(CHILD); await turn();
  const second = launcher.projectLauncherService.openProject(MAIN); await turn();
  assert.deepEqual(f.opens(), []); assert.equal(f.frames.size, 0);
  const count = f.calls.length; stale(performance.now() + 100); assert.equal(f.calls.length, count);
  saved.resolve(); assert.equal((await first).status, 'superseded'); await second;
  assert.deepEqual(f.opens(), [MAIN]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
});

test('canonical open cannot replace a scene after save failure; retry retains the old pose', async t => {
  let fail = true;
  const f = await fixture(t, { save: async () => { if (fail) throw new Error('cannot write scene'); } });
  const page = f.mount(); await page.mount(); page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  await assert.rejects(launcher.projectLauncherService.openProject(CHILD), /cannot write scene/);
  assert.deepEqual(f.opens(), []);
  fail = false; await launcher.projectLauncherService.openProject(CHILD);
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 2);
});

test('a timed-out save blocks movement and replacement until its real acknowledgement', async t => {
  const saved = deferred();
  const f = await fixture(t, { save: () => saved.promise }); const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'KeyW' })); f.step();
  t.mock.timers.enable({ apis: ['setTimeout'] });
  const leaving = page.instance.exitStory(); await turn();
  t.mock.timers.tick(30_001); await leaving;
  assert.equal(f.route, '/'); assert.match(f.alerts[0], /超时/);
  page.instance.onKeyDown(event({ code: 'KeyW' })); assert.equal(f.frames.size, 0);
  const opening = launcher.projectLauncherService.openProject(CHILD); await turn();
  assert.deepEqual(f.opens(), []);
  saved.resolve(); await opening;
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(f.calls.filter(c => c[0] === 'playerSave').length, 1);
});

test('world replacement drains pending initialization and never binds its late result', async t => {
  const init = deferred(); const f = await fixture(t, { init: () => init.promise });
  const page = f.mount(); const mounting = page.mount(); await turn();
  const opening = launcher.projectLauncherService.openProject(CHILD); await turn();
  assert.deepEqual(f.opens(), []);
  init.resolve({ path: 'scene.ini' }); await mounting; await opening;
  assert.deepEqual(f.opens(), [CHILD]);
  assert.ok(!f.calls.some(c => ['setCameraViewport', 'actorTransform', 'playerSave'].includes(c[0])));
});


test('Escape fences a late O result even when the exit save fails', async t => {
  const prep = deferred();
  const f = await fixture(t, { prepare: () => prep.promise });
  const page = f.mount(); await page.mount();
  clickWorld(page); await turn();
  // Simulate a failed finalizer, independently of the already saved O snapshot.
  const failed = t.mock.method(page.instance.saveRegistration, 'flush', async () => { throw new Error('exit save failed'); });
  await page.instance.exitStory();
  assert.equal(f.route, '/');
  prep.resolve({ status: 'ok', navigation: { source: MAIN, target: CHILD, direction: 'enter', mode: 'story' } });
  await turn();
  assert.deepEqual(f.opens(), []);
  assert.deepEqual(f.alerts, ['exit save failed']);
  assert.equal(page.instance.navigation.busy, false);
  failed.mock.restore();
});

test('Tab captures gameplay, repeated Tab/F are ignored and Escape closes inventory first', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 }));
  page.instance.onKeyDown(event({ code: 'Tab' }));
  assert.equal(page.instance.inventoryOpen.value, true); assert.equal(f.frames.size, 0);
  page.instance.onKeyDown(event({ code: 'Tab', repeat: true }));
  assert.equal(page.instance.inventoryOpen.value, true);
  const before = page.instance.camera.snapshotPose();
  page.instance.onKeyDown(event({ code: 'KeyW' }));
  page.instance.camera.pointerMove(event({ clientX: 600, clientY: 300 }));
  page.instance.onPointerDown(event({ button: 0 }));
  page.instance.onKeyDown(event({ code: 'KeyO' })); page.instance.onKeyDown(event({ code: 'KeyF' }));
  await turn();
  assert.deepEqual(page.instance.camera.snapshotPose(), before); assert.deepEqual(f.opens(), []);
  assert.equal(page.instance.actionBusy.value, false);
  page.instance.onKeyDown(event({ code: 'Escape' })); await turn();
  assert.equal(page.instance.inventoryOpen.value, false); assert.equal(f.route, '/');
  page.instance.onKeyDown(event({ code: 'KeyF', repeat: true }));
  assert.equal(page.instance.actionBusy.value, false);
  page.instance.onKeyDown(event({ code: 'Escape' })); await turn();
  assert.equal(f.route, '/StartScreen');
});

test('blur, hidden page and inventory close reseed mouse without a jump', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 })); f.step();
  page.instance.onBlur(); assert.equal(f.frames.size, 0);
  const before = page.instance.camera.snapshotPose();
  page.instance.onFocus();
  page.instance.camera.pointerMove(event({ clientX: 400, clientY: 300 })); f.step();
  assert.deepEqual(page.instance.camera.snapshotPose(), before);
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 }));
  document.hidden = true; page.instance.onVisibilityChange(); assert.equal(f.frames.size, 0);
  document.hidden = false; page.instance.onFocus();
});

test('unacknowledged combat blocks native replacement; retry does not change operation identity', async t => {
  let fail = true; const commands = [];
  const state = { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } };
  const config = gameplayConfig;
  const f = await fixture(t, { gameplay: async request => {
    if (request.action !== 'load') {
      commands.push(request);
      if (fail) throw new Error('combat disk full');
      state.boss.hp = 180; state.revision = 1;
    }
    return { status: 'ok', role: 'main', state: structuredClone(state), config };
  } });
  const page = f.mount(); await page.mount();
  // The attack is aimed along `facingYaw`, and the player turns to face wherever they walk, so
  // the approach is: line up with the Boss on x, then walk straight in (+z) and stop as soon as
  // the swing would really land — `canHitBoss` is the same predicate the page's attack uses.
  // The Boss is not authored on the player's own x = 0 path, so the initial approach is diagonal
  // and the final one is head-on; that is exactly why the walk may not run a fixed number of frames.
  await walkTo(page, f, 'KeyD', 60, () => page.instance.camera.snapshotPlayer().position[0]
    < STORY_CHARACTERS.find(character => character.role === 'boss').x - 0.1);
  await walkTo(page, f, 'KeyW', 60, () => {
    const player = page.instance.camera.snapshotPlayer();
    const boss = f.actors.get(MAIN).find(a => a.actor_guid === STORY_CHARACTERS[1].guid);
    return gameplayModule.canHitBoss(player, gameplayModule.worldBounds(boss), config);
  });
  assert.equal(gameplayModule.canHitBoss(page.instance.camera.snapshotPlayer(),
    gameplayModule.worldBounds(f.actors.get(MAIN).find(a => a.actor_guid === STORY_CHARACTERS[1].guid)),
    config), true, '走到 Boss 面前后必须真的能打中');
  assert.equal(page.instance.bossNearby.value, true);
  page.instance.onPointerDown(event({ button: 0 })); await turn();
  assert.match(page.instance.gameplayError.value, /combat disk full/);
  assert.equal(page.instance.gameplay.needsSave, true);
  await assert.rejects(launcher.projectLauncherService.openProject(CHILD), /combat disk full/);
  assert.deepEqual(f.opens(), []);
  fail = false;
  await launcher.projectLauncherService.openProject(CHILD);
  assert.deepEqual(f.opens(), [CHILD]);
  assert.equal(new Set(commands.map(c => c.operationId)).size, 1);
  assert.ok(commands.every(c => c.projectPath === MAIN));
});


test('inventory template captures pointer input and its close button restores control without an attack or jump', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  const attack = t.mock.method(page.instance.gameplay, 'attack', async () => {});
  page.instance.camera.pointerMove(event({ clientX: 799, clientY: 300 })); f.step();
  page.instance.onKeyDown(event({ code: 'Tab' }));
  const before = page.instance.camera.snapshotPose();
  const tree = renderStory(proxyRefs(page.instance), []);
  const overlay = findNode(tree, node => node.props?.class === 'story-bag');
  const close = findNode(overlay, node => node.type === 'button' && node.props?.['aria-label'] === '关闭行囊');
  assert.ok(overlay); assert.ok(close);

  for (const selector of ['.story-bag']) {
    let interactive = false;
    styles.walkRules(rule => {
      if (rule.selectors.includes(selector)) rule.walkDecls('pointer-events', declaration => {
        interactive = declaration.value === 'auto';
      });
    });
    assert.ok(interactive, `${selector} must explicitly participate in hit testing`);
  }
  for (const name of ['onPointerdown', 'onPointermove', 'onWheel']) {
    let stopped = false;
    const input = event({ button: 0, clientX: 799, clientY: 300, deltaY: 200,
      stopPropagation() { stopped = true; } });
    overlay.props[name](input);
    assert.ok(stopped, `${name} must not bubble into the viewport`);
    if (name === 'onWheel') assert.ok(input.defaultPrevented);
  }
  assert.equal(page.instance.inventoryOpen.value, true);
  assert.equal(f.frames.size, 0);
  close.props.onClick(event()); await turn();
  assert.equal(page.instance.inventoryOpen.value, false);
  assert.equal(attack.mock.callCount(), 0);
  assert.equal(f.route, '/');
  page.instance.camera.pointerMove(event({ clientX: 400, clientY: 300 })); f.step();
  assert.deepEqual(page.instance.camera.snapshotPose(), before);
});


test('real story page settles airborne Tab, blur, pointerleave and hidden-page interruptions', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  const groundY = page.instance.camera.snapshotPlayer().position[1];
  const stopCases = [
    [() => page.instance.onKeyDown(event({ code: 'Tab' })), () => page.instance.toggleInventory()],
    [() => page.instance.onBlur(), () => page.instance.onFocus()],
    [() => page.instance.camera.pointerLeave(), () => {}],
    [() => { document.hidden = true; page.instance.onVisibilityChange(); },
      () => { document.hidden = false; page.instance.onVisibilityChange(); }],
  ];
  for (const [stop, resume] of stopCases) {
    page.instance.onKeyDown(event({ code: 'Space' })); f.step();
    const airborne = page.instance.camera.snapshotPlayer();
    assert.equal(airborne.grounded, false); assert.ok(airborne.position[1] > groundY);
    const stale = [...f.frames.values()][0]; stop();
    const landed = page.instance.camera.snapshotPlayer();
    assert.equal(landed.grounded, true); assert.equal(landed.position[1], groundY);
    assert.equal(landed.position[2], airborne.position[2]);
    assert.equal(f.frames.size, 0); const count = f.calls.length; stale(50000); assert.equal(f.calls.length, count);
    // Locks reject new Space; pointerleave instead clears held state.
    if (document.hidden || page.instance.inventoryOpen.value) {
      page.instance.onKeyDown(event({ code: 'Space' })); assert.equal(f.frames.size, 0);
    }
    resume();
    const camera = page.instance.camera.snapshotPose();
    page.instance.camera.pointerMove(event({ clientX: 400, clientY: 300 })); f.step();
    assert.deepEqual(page.instance.camera.snapshotPose(), camera);
  }
  await page.instance.saveRegistration.flush();
  assert.equal(f.actors.get(MAIN)[0].geometry.position[1], groundY);
  assert.deepEqual(f.alerts, []);
});
test('half-jump inventory travel saves ground and the corrected model once, preserves camera and keeps child combat hidden', async t => {
  const f = await fixture(t); let page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'Space' })); f.step(); f.step();
  const airborne = page.instance.camera.snapshotPlayer(), groundY = f.actors.get(MAIN)[0].geometry.position[1];
  clickWorld(page); await turn();
  assert.deepEqual(f.opens(), [CHILD]);
  const saved = f.actors.get(MAIN)[0].geometry;
  assert.equal(saved.position[1], groundY); assert.equal(saved.position[2], airborne.position[2]);
  assert.deepEqual(saved.rotation, airborne.rotation);
  const savedCamera = structuredClone(f.poses.get(MAIN));
  page = f.mount(); await page.mount();
  assert.equal(page.instance.bossNearby.value, false); assert.equal(f.actors.get(CHILD)[1].visible, false);
  clickWorld(page); await turn();
  page = f.mount(); await page.mount();
  const restored = page.instance.camera.snapshotPlayer();
  assert.equal(restored.grounded, true); assert.deepEqual(restored.position, saved.position);
  assert.deepEqual(restored.rotation, saved.rotation); assert.equal(restored.facingYaw, airborne.facingYaw);
  assert.deepEqual(page.instance.camera.snapshotPose().camera.position, savedCamera.position);
  assert.deepEqual(f.alerts, []);
});
test('half-jump Escape waits for the ground save, blocks Space and remains in world on save failure', async t => {
  const gate = deferred(); let fail = true, block = true;
  const f = await fixture(t, { save: async () => { if (block) await gate.promise; if (fail) throw new Error('landing disk full'); } });
  const page = f.mount(); await page.mount(); const groundY = page.instance.camera.snapshotPlayer().position[1];
  page.instance.onKeyDown(event({ code: 'Space' })); f.step();
  const before = page.instance.camera.snapshotPlayer(); page.instance.onKeyDown(event({ code: 'Escape' }));
  await turn(); assert.equal(f.route, '/');
  page.instance.onKeyDown(event({ code: 'Space' })); assert.equal(f.frames.size, 0);
  gate.resolve(); await turn();
  assert.equal(f.route, '/'); assert.match(page.instance.gameplayError.value, /landing disk full/);
  assert.equal(page.instance.camera.snapshotPlayer().position[1], groundY);
  const writes = f.calls.filter(c => c[0] === 'playerSave'); assert.equal(writes.length, 1);
  assert.equal(writes[0][4].position[1], groundY); assert.equal(writes[0][4].position[2], before.position[2]);
  fail = false; block = false; page.instance.onKeyDown(event({ code: 'Escape' })); await turn();
  assert.equal(f.route, '/StartScreen'); assert.deepEqual(f.actors.get(MAIN)[0].geometry.position, writes[0][4].position);
});
test('unrelated world replacement settles locally after invalidation and saves without using old handles', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  const groundY = page.instance.camera.snapshotPlayer().position[1];
  page.instance.onKeyDown(event({ code: 'Space' })); f.step();
  const before = page.instance.camera.snapshotPlayer(), stale = [...f.frames.values()][0];
  const index = f.calls.length;
  await launcher.projectLauncherService.openProject('D:/other'); stale(99999);
  assert.deepEqual(f.opens(), ['D:/other']);
  assert.ok(!f.calls.slice(index).some(c => ['actorTransform', 'cameraMove'].includes(c[0])));
  const write = f.calls.slice(index).find(c => c[0] === 'playerSave');
  assert.equal(write[1], MAIN); assert.equal(write[4].position[1], groundY);
  assert.equal(write[4].position[2], before.position[2]);
});
test('Space cannot start during an acknowledged save; a late save never leaves the player airborne', async t => {
  const gate = deferred(); const f = await fixture(t, { save: () => gate.promise });
  const page = f.mount(); await page.mount();
  page.instance.onKeyDown(event({ code: 'Space' })); f.step();
  const saved = page.instance.saveRegistration.flush(); await turn();
  page.instance.onKeyDown(event({ code: 'Space' })); clickWorld(page);
  assert.equal(f.frames.size, 0); assert.deepEqual(f.opens(), []);
  gate.resolve(); await saved;
  assert.equal(page.instance.camera.snapshotPlayer().grounded, true);
  assert.equal(page.instance.camera.snapshotPlayer().position[1], f.actors.get(MAIN)[0].geometry.position[1]);
});
test('HUD pins the boss plate top-center, vitals bottom-left and skills bottom-right, with Space listed in the left tip', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  // Mounted state so the whole HUD renders before its anchors are inspected.
  page.instance.gameplayState.value = { role: 'main', config: gameplayConfig,
    state: { revision: 0, rage: 40, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 1 } } };
  page.instance.feedback.value = '获得 世界碎片 ×1'; page.instance.canPickup.value = true;
  page.instance.gameplayError.value = '保存失败，请重试';
  page.instance.bossNearby.value = true;
  const tree = renderStory(proxyRefs(page.instance), []);
  const bottom = findNode(tree, node => node.props?.class === 'story-bottom-stack');
  assert.deepEqual(bottom.children.filter(node => node.type !== vue.Comment).map(node => node.props?.class),
    ['gameplay-error', 'story-feedback']);
  const controls = findNode(tree, node => node.props?.class === 'action-tips');
  assert.match(JSON.stringify(controls, (key, value) => key === 'ctx' ? undefined : value), /Space/);
  // ui2.html 的四个 HUD 锚点：首领血条 / 生命栏 / 操作提示 / 战技群
  const vitals = findNode(tree, node => node.props?.class === 'vitals-container');
  const cluster = findNode(tree, node => node.props?.class === 'action-cluster');
  const spells = findNode(tree, node => node.props?.class === 'spells-group');
  const boss = findNode(tree, node => node.props?.class === 'boss-wrap');
  assert.ok(vitals && cluster && spells && boss,
    'boss, vitals, action tips and the spell cluster are independent HUD anchors');
  const declarations = selector => {
    const values = {};
    styles.walkRules(rule => { if (rule.parent.type === 'root' && rule.selectors.includes(selector))
      rule.walkDecls(d => { values[d.prop] = d.value; }); });
    return values;
  };
  // #hud -> .story-hud：仍然不参与命中测试
  assert.equal(declarations('.story-hud')['pointer-events'], 'none');
  assert.equal(declarations('.story-hud').position, 'fixed');
  // 原型的四角落布局
  assert.equal(declarations('.boss-wrap').position, 'absolute');
  assert.equal(declarations('.boss-wrap').left, '50%');
  assert.equal(declarations('.vitals-container').position, 'absolute');
  assert.equal(declarations('.vitals-container')['pointer-events'], 'auto');
  assert.match(declarations('.vitals-container').left, /^clamp\(/);
  assert.match(declarations('.vitals-container').top, /^clamp\(/);
  assert.equal(declarations('.action-cluster').position, 'absolute');
  assert.equal(declarations('.action-cluster')['pointer-events'], 'auto');
  assert.equal(declarations('.action-cluster').right, 'var(--gap-edge)');
  assert.equal(declarations('.action-cluster').bottom, 'var(--gap-edge)');
  assert.equal(declarations('.action-tips').position, 'absolute');
  assert.equal(declarations('.action-tips').left, 'var(--gap-edge)');
  assert.equal(declarations('.action-tips').bottom, 'var(--gap-edge)');
  assert.equal(declarations('.spells-group').display, 'flex');
  // 原型没有这一层，但存档失败与反馈仍需要一条底部居中的通道。
  // 这条通道的锚点在 .story-bottom-column 上（物品栏是同一列的第二个子项），
  // 状态条自己不再贴底：两份贴底偏移只会在某些视口上撞车。
  assert.equal(declarations('.story-bottom-column').position, 'absolute');
  assert.equal(declarations('.story-bottom-column').left, '50%');
  assert.equal(declarations('.story-bottom-column').bottom,
    'calc(var(--gap-edge) + var(--hud-corner-block-height))',
    '列的距底偏移 = 左右角块的贴边距 + 角块高度，物品栏才不会压进底部那一排');
  assert.ok(stylePixels(declarations('.story-bottom-column').bottom) > 0,
    '这个偏移必须能算出数值，而不是只写对写法');
  assert.equal(declarations('.story-bottom-stack').position, undefined);
  assert.equal(declarations('.gameplay-error').position, undefined);
});

// 回归（UI 遮挡）：行囊（Tab 背包）恢复之后，「物品栏」与「小世界状态」条又各自贴底居中，
// 两条的锚点完全相同，在某些视口上叠在一起；任何面板打开时物品栏还会盖在面板上。判定只有
// 一条真值（hotbarVisible），模板 v-if 与滚轮换格共用它：物品栏只在「小世界 + 没有任何面板」
// 时存在，状态条在顶部中央，调整物体的遮罩必须让点击穿到视口上。
test('the item bar yields to every open panel, sits bottom-center, and the notice moved to the top', async t => {
  const f = await fixture(t);
  let page = f.mount(); await page.mount(); await turn();
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  const p = () => page.instance;
  const node = cls => findNode(renderStory(proxyRefs(p()), []), item => item.props?.class === cls);

  // 小世界、没有面板：物品栏与状态条都在。
  assert.equal(p().roleRef.value, 'child');
  assert.equal(p().hotbarVisible.value, true);
  assert.ok(node('hotbar'), '小世界里物品栏必须渲染出来');
  assert.ok(node('subworld-notice'), '小世界状态条必须渲染出来');

  // 三类面板打开都必须让位——Tab 背包、先知对话、调整物体走的是同一条判定。
  for (const [label, key] of [['背包', 'inventoryOpen'], ['对话', 'dialogueOpen'], ['调整', 'adjustMode']]) {
    p()[key].value = true;
    assert.equal(p().hotbarVisible.value, false, `${label}打开时物品栏必须让位`);
    assert.equal(node('hotbar'), null, `${label}打开时物品栏不得渲染`);
    p()[key].value = false;
    assert.equal(p().hotbarVisible.value, true, `${label}关掉之后物品栏必须回来`);
  }

  // 底部件必须挂在同一个 flex 列里——列本身才是底部中央的锚点，两个子项由布局排开，
  // 而不是各写一份贴底偏移（那正是两条叠在一起的原因）。
  const bottomColumn = node('story-bottom-column');
  assert.ok(bottomColumn, '底部中央必须有一个统一容器');
  const children = bottomColumn.children.filter(item => item.type !== vue.Comment);
  assert.deepEqual(children.map(item => item.props?.class), ['story-bottom-stack', 'hotbar'],
    '状态条与物品栏必须是同一列里的兄弟项，顺序固定');
  const hotbarInColumn = findNode(bottomColumn, item => item.props?.class === 'hotbar');
  assert.ok(hotbarInColumn, '物品栏必须长在底部中央列里，而不是自己贴底');

  // 无重复规则：同一元素出现两份锚点时会互相覆盖，是「改了却看起来没改」的常见来源。
  const blocksFor = selector => {
    const blocks = [];
    styles.walkRules(rule => { if (rule.selectors.includes(selector)) {
      const values = {};
      rule.walkDecls(d => { values[d.prop] = d.value; });
      blocks.push(values);
    } });
    return blocks;
  };
  const declarations = selector => Object.assign({}, ...blocksFor(selector));
  for (const selector of ['.story-bottom-column', '.story-bottom-stack', '.hotbar',
    '.subworld-notice', '.dialogue-overlay', '.dialogue-overlay-passive']) {
    assert.equal(blocksFor(selector).length, 1, `${selector} 只能有一份规则，锚点不得有第二份副本`);
  }

  // 锚点：底部中央列横向居中，距底 = 角块贴边距 + 角块高度（要抬过左右两个角块）。
  // 这里只算出那个数，具体够不够高由后面「贴底元素上下不相交」那道几何守卫证明。
  const column = declarations('.story-bottom-column');
  const columnAnchorBottom = column.bottom;
  assert.ok(stylePixels(columnAnchorBottom) > 0,
    `底部中央列的距底偏移必须能算出数值，实得 ${columnAnchorBottom}`);
  assert.equal(column.position, 'absolute');
  assert.equal(column.left, '50%');
  assert.match(column.transform, /translateX\(-50%\)/);
  assert.equal(column.display, 'flex');
  assert.equal(column['flex-direction'], 'column-reverse', '状态条在上、物品栏贴底由布局决定');
  assert.equal(column['pointer-events'], 'none');
  assert.equal(declarations('.story-bottom-column > *')['pointer-events'], 'auto',
    '这一层不吃点击，但子项（错误条、物品栏）必须可点');
  assert.equal(declarations('.story-bottom-stack').position, undefined,
    '状态条不再自己贴底：位置由 flex 列给出');
  assert.equal(declarations('.hotbar').position, 'static',
    '物品栏不再自己贴底：位置由 flex 列给出');
  assert.equal(declarations('.subworld-notice').position, 'absolute');
  assert.equal(declarations('.subworld-notice').left, '50%');
  assert.equal(declarations('.subworld-notice').top, '18px');
  // 调整物体时遮罩不得挡住视口：只有面板本身还接指针。
  assert.equal(declarations('.dialogue-overlay-passive')['pointer-events'], 'none');
  assert.equal(declarations('.dialogue-overlay-passive > *')['pointer-events'], 'auto');
});

// 布局守卫：底部中央只有一条通道。物品栏与存档状态条都挂在 .story-bottom-column 里，
// 由 flex 布局上下排开，而不是各写一份贴底偏移——两份偏移只会在某些视口上撞车，
// 而「谁和谁抢同一个位置」是可以从最终样式源里直接读出来的，不必猜 z-index。
test('the bottom-centre channel is owned by one column, not two hand-tuned offsets', async t => {
  const f = await fixture(t);
  let page = f.mount(); await page.mount(); await turn();
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  const p = () => page.instance;
  assert.equal(p().roleRef.value, 'child', '这道检查针对小世界：物品栏与状态条都在这里');
  page.instance.feedback.value = '获得 世界碎片 ×1';
  page.instance.gameplayError.value = '保存失败，请重试';
  const tree = renderStory(proxyRefs(p()), []);
  for (const panel of ['subworld-notice', 'world-indicator', 'action-tips', 'story-bottom-column'])
    assert.ok(findNode(tree, item => item.props?.class === panel),
      `${panel} 应当渲染出来才能参与这项检查`);
  // 底部两个子项在同一个 flex 列里，不参加「谁都贴底」的竞争：都不能有自己的绝对锚点。
  const stack = styleDeclarations('.story-bottom-stack');
  for (const orphan of ['position', 'left', 'right', 'top', 'bottom', 'transform'])
    assert.equal(stack[orphan], undefined,
      `状态条不得自带 ${orphan}：它的位置由 flex 列给出`);
  assert.equal(styleDeclarations('.hotbar').position, 'static',
    '物品栏不得自带绝对锚点：它的位置由 flex 列给出');
  // 列的锚点：横向居中；距底 = 角块自己的贴边距 + 角块从底边往上占的高度
  //（物品栏 8 格 + 7 个间隙，宽度由 --slot-size 决定）。
  const column = styleDeclarations('.story-bottom-column');
  const columnBottomPx = stylePixels(column.bottom);
  assert.ok(columnBottomPx > stylePixels(styleDeclarations('.action-tips').bottom),
    `底部中央列必须抬到左右角块之上：列距底 ${columnBottomPx}px，角块距底 ${stylePixels(styleDeclarations('.action-tips').bottom)}px`);
  assert.equal(column.left, '50%');
  assert.equal(column.transform, 'translateX(-50%)');
  const slot = stylePixels(styleDeclarations('.hotbar__slot').width);
  const gap = stylePixels(styleDeclarations('.hotbar__slots').gap);
  assert.ok(slot > 0 && gap > 0, '物品栏格宽与间隙必须能算出来');
  const barWidth = slot * 8 + gap * 7;
  assert.ok(barWidth > 300 && barWidth < 560, `物品栏宽 ${barWidth} 应当在底部正中留得下`);
  // 锚点之争：没有人可以和别人抢同一个位置。
  assert.deepEqual(collisionsFor(['.story-bottom-column', '.story-bottom-stack', '.hotbar',
    '.subworld-notice', '.world-indicator', '.action-tips', '.action-cluster']), [],
    '同一个视口里的 HUD 面板不得抢占同一个位置（抢占就是遮住了对方）');
  // 顶部中央与底部中央必须真的分处两端：状态条在顶部，物品栏通道在底部。
  const notice = styleAnchor('.subworld-notice');
  assert.equal(notice.edge, 'top', '状态条必须贴在顶部，而不是又回到物品栏那条通道上');
  assert.equal(notice.horizontal, 'center');
  assert.equal(styleAnchor('.story-bottom-column').edge, 'bottom');
});

// 顶部中央是「主世界归 Boss 血条、小世界归状态条」的共用位置，两者必须互斥；
// 底部中央归底部通道，左右两角归操作提示与战技群；这四角加一条通道不得有人抢占同一格。
test('the shared HUD slots are occupied by exactly one panel per world', async t => {
  const f = await fixture(t);
  let page = f.mount(); await page.mount(); await turn();
  const mainTree = renderStory(proxyRefs(page.instance), []);
  assert.equal(page.instance.roleRef.value, 'main');
  page.instance.bossNearby.value = true;
  const bossTree = renderStory(proxyRefs(page.instance), []);
  const has = (tree, cls) => Boolean(findNode(tree, item => item.props?.class === cls));
  assert.equal(has(bossTree, 'boss-wrap'), true, '主世界的顶部中央归 Boss 血条');
  assert.equal(has(bossTree, 'subworld-notice'), false, '主世界不得出现小世界状态条');
  assert.equal(has(mainTree, 'subworld-notice'), false);
  assert.equal(has(mainTree, 'hotbar'), false, '主世界没有物品栏');
  // 小世界：状态条在顶部，物品栏在底部通道。
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  const childTree = renderStory(proxyRefs(page.instance), []);
  assert.equal(page.instance.roleRef.value, 'child');
  assert.equal(has(childTree, 'subworld-notice'), true, '小世界的顶部中央归状态条');
  assert.equal(has(childTree, 'hotbar'), true, '小世界的底部通道里有物品栏');
  assert.equal(has(childTree, 'vitals-container'), false, '小世界不渲染战斗生命槽');
  // 四角 + 底部通道：同时存在的这些面板，谁都不许和别人抢同一个位置。
  assert.deepEqual(collisionsFor(['.story-bottom-column', '.subworld-notice', '.world-indicator',
    '.action-tips', '.action-cluster', '.vitals-container', '.boss-wrap']), [],
    '同一个视口里的 HUD 面板不得抢占同一个位置（抢占就是遮住了对方）');
});

// 这道守卫必须真的看得见锚点之争：用合成的规则反证「同名同位置的会被点名」，
// 同时确认「同一条边但距离不同」不会误报，否则上面的空结果可能只是它什么都不看。
// 探针直接解析成 PostCSS 节点追加到同一份样式源上，走的是同一条代码路径。
test('the HUD overlap guard names two panels that compete for the same slot', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  assert.ok(page.instance);
  styles.append(postcss.parse(
    '.probe-a { position: absolute; left: 10px; bottom: 40px; }'
    + '.probe-b { position: absolute; left: 50%; transform: translateX(-50%); bottom: 40px; }'
    + '.probe-c { position: absolute; left: 50%; transform: translateX(-50%); top: 18px; }'
    + '.probe-d { position: absolute; left: 50%; transform: translateX(-50%); bottom: 40px; }'
    + '.probe-e { position: absolute; left: 10px; bottom: 90px; }'));
  assert.deepEqual(collisionsFor(['.probe-b', '.probe-d']),
    ['.probe-b 与 .probe-d 抢占同一个位置（center / bottom:40px，z-index 0 / 0）'],
    '贴底居中的两条规则必须被点名（这正是上一版守卫看不见的形状）');
  assert.deepEqual(collisionsFor(['.probe-a', '.probe-e']), [],
    '同贴底、距离不同 = 上下排开，不得误报');
  assert.deepEqual(collisionsFor(['.probe-b', '.probe-c']), [],
    '同居中、一顶一底 = 分处两端，不得误报');
});

// 上一版守卫只看「两条绝对锚点是不是一字不差地相同」，所以它放过了真正发生过的那种叠法：
// 物品栏贴底 14px、操作提示贴底 38.4px——距离不同，竖直方向却实打实地压在一起。
// 这一道改成算竖直 band：贴底元素占 [距底偏移, 距底偏移 + 自身高度]，高度只从样式声明里算
// （行高按 CSS 的 line-height / font-size 折算），不猜渲染结果。band 的算法就一个函数，
// 真样式与后面的反证探针共用它，避免「守卫和探针各算各的」。
//   source 需要给出：列/元素的 bottom、各自身高（行高 + padding + 边框）
function bottomBand(source, selector) {
  const d = selector => source(selector);
  const heightOf = value => stylePixels(value);
  const lineBox = (fontSize, paddingSelector) => {
    const pad = paddingSelector ? stylePadding(paddingSelector) : { top: 0, bottom: 0 };
    return fontSize * 1.4 + pad.top + pad.bottom;
  };
  if (selector === '.action-tips') {
    const pad = stylePadding('.action-tips');
    const kbd = d('.action-tips kbd');
    const tipFont = stylePixels(d('.action-tips p').fontSize);
    const row = Math.max(lineBox(tipFont),
      lineBox(stylePixels(kbd.fontSize ?? d('.action-tips p').fontSize), '.action-tips kbd') + 2);
    return { bottom: stylePixels(d('.action-tips').bottom), height: row * 2 + pad.top + pad.bottom + 2 };
  }
  if (selector === '.action-cluster') {
    const heavy = heightOf(d('.focus-heavy-btn').height);
    const blade = heightOf(d('.focus-blade').height) + stylePixels(d('.focus-system').gap) + heavy;
    const spell = heightOf(d('.spell-btn').height) + stylePixels(d('.spell-item').gap)
      + lineBox(stylePixels(d('.spell-name').fontSize));
    return { bottom: stylePixels(d('.action-cluster').bottom), height: Math.max(heavy, blade, spell) };
  }
  if (selector === '.hotbar') {
    const pad = stylePadding('.hotbar__hint');
    const hint = d('.hotbar__hint');
    return {
      bottom: stylePixels(d('.story-bottom-column').bottom),
      height: heightOf(d('.hotbar__slot').height) + stylePixels(d('.hotbar').gap)
        + stylePixels(hint.lineHeight) * stylePixels(hint.fontSize) + pad.top + pad.bottom + 2,
    };
  }
  throw new Error(`未知的贴底元素：${selector}`);
}
const bottomTop = (source, selector) => {
  const band = bottomBand(source, selector);
  return band.bottom + band.height;
};
// 「物品栏整条 band 都在角块上方」这条判据：真样式与探针共用，overrides 只改
// 列的 bottom（探针要复现的正是那一处），其余声明仍然读真样式。
function bandVerdict(columnBottomOverride, cornerSelector) {
  const source = selector => (columnBottomOverride !== undefined && selector === '.story-bottom-column'
    ? { ...styleDeclarations(selector), bottom: columnBottomOverride }
    : styleDeclarations(selector));
  const bar = bottomBand(source, '.hotbar');
  const corner = bottomBand(source, cornerSelector);
  return { bar, corner, clearance: bar.bottom - (corner.bottom + corner.height) };
}

// 底部锚点的回归守卫：物品栏的整条 band 必须落在左右两个角块的上方。
// 触发过的那一版形状是「距离不同就当没事」，于是 14px 的物品栏贴着 38.4px 的操作提示
// 叠了整整一屏——距离各不相同，竖直 band 却压在一起。
test('the item bar sits above the bottom row instead of inside it', async t => {
  const f = await fixture(t);
  let page = f.mount(); await page.mount(); await turn();
  const mainTree = renderStory(proxyRefs(page.instance), []);
  assert.equal(page.instance.roleRef.value, 'main');
  const has = (tree, cls) => Boolean(findNode(tree, item => item.props?.class === cls));
  assert.ok(has(mainTree, 'action-cluster'), '主世界的右下角块（战技群）应当渲染出来');
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  const p = () => page.instance;
  assert.equal(p().roleRef.value, 'child', '这道检查针对小世界：物品栏只在这里存在');
  const childTree = renderStory(proxyRefs(p()), []);
  assert.ok(has(childTree, 'hotbar'), '小世界里物品栏应当渲染出来');
  assert.ok(has(childTree, 'action-tips'), '小世界里左下角的操作提示应当渲染出来');
  // 物品栏（只在小世界）与战技群（只在主世界）互斥，所以这两块永远不会同屏；
  // 真正会同屏的是物品栏与左下角的操作提示。
  assert.equal(has(childTree, 'action-cluster'), false, '小世界不渲染战技群');
  assert.equal(p().roleRef.value === 'child' && has(mainTree, 'hotbar'), false, '主世界不渲染物品栏');
  // 单一通道：物品栏自己不得再长出一条贴底锚点，它只能坐在列的底边上。
  assert.equal(styleDeclarations('.hotbar').position, 'static', '物品栏不得自带绝对锚点');
  const bar = bottomBand(styleDeclarations, '.hotbar');
  for (const band of [bottomBand(styleDeclarations, '.action-tips'), bottomBand(styleDeclarations, '.action-cluster'), bar])
    for (const [key, value] of Object.entries(band))
      assert.ok(Number.isFinite(value) && value > 0, `band 的 ${key} 必须能算出正数，实得 ${value}`);
  // band 的数字由同一条像素解析管线算出（参照视口 1280 宽：物品栏下沿 198.4px，
  // 操作提示上沿 72.4px、战技群上沿 136.4px），不是手抄的期望值。
  for (const corner of ['.action-tips', '.action-cluster']) {
    const { clearance } = bandVerdict(undefined, corner);
    assert.ok(clearance > 0,
      `物品栏下沿（距底 ${bar.bottom}px）必须高过 ${corner} 的上沿（距底 ${bottomTop(styleDeclarations, corner)}px），`
      + `实得 ${clearance}px：下方再相交就是重新叠在一起`);
  }
  // 抬高量来自令牌，不是某个视口上的手算常数：窄窗与宽窗都必须成立。
  const column = styleDeclarations('.story-bottom-column');
  const edge = stylePixels(styleDeclarations('.action-tips').bottom);
  assert.match(column.bottom, /var\(--gap-edge\)/,
    `列的距底偏移必须继续用 --gap-edge 令牌，实得 ${column.bottom}`);
  assert.ok(stylePixels(column.bottom) > edge,
    `列的距底偏移 ${stylePixels(column.bottom)}px 必须大于角块自己的贴边距 ${edge}px，否则物品栏又回到角块那一条带里`);
  // 列里也仍然只有一个通道：状态条不许自己贴底（否则就是两条手算偏移）。
  assert.equal(styleDeclarations('.story-bottom-stack').bottom, undefined);
});

// 上面那道守卫必须真的看得见「贴底两条距离不同、却竖直相交」：把事故形状
// （列 bottom 退回 14px）代进同一个 band 算法，它必须判成交叠；真身必须判成不相交。
// 探针改的是同一个令牌的值，走的是同一条像素解析路径。
test('the bottom-band guard names a bar that sits inside the bottom row', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  assert.ok(page.instance);
  const column = styleDeclarations('.story-bottom-column');
  const realToken = HUD_TOKENS['--hud-corner-block-height'];
  let regressed = null;
  try {
    HUD_TOKENS['--hud-corner-block-height'] = '14px';
    regressed = bandVerdict(undefined, '.action-tips');
  } finally {
    HUD_TOKENS['--hud-corner-block-height'] = realToken;
  }
  assert.ok(regressed && regressed.clearance < 0,
    `事故形状（列距底 ${regressed?.bar.bottom}px，操作提示上沿 ${(regressed?.corner.bottom ?? 0) + (regressed?.corner.height ?? 0)}px）`
    + `必须被这一道判成交叠，实得间隙 ${regressed?.clearance}px`);
  assert.equal(stylePixels(column.bottom), stylePixels(realToken) + stylePixels(styleDeclarations('.action-tips').bottom),
    '恢复之后列的偏移必须还是「角块贴边距 + 角块高度」，探针不得留下副作用');
  for (const corner of ['.action-tips', '.action-cluster']) {
    const { clearance } = bandVerdict(undefined, corner);
    assert.ok(clearance > 1,
      `真身（列距底 ${column.bottom}）必须判成不相交，${corner} 实得间隙 ${clearance}px`);
  }
});

// 底部中央在小世界归物品栏、在主世界只留存档状态条：物品栏的插槽长在
// .story-bottom-column 里，不能让主世界也长出一个。主世界没有摆放物，
// 因此这里查的是「同一条通道 + 物品栏只在 hotbarVisible 为真时渲染」这类不变量。
test('the small world owns the item bar and the bottom column is the only bottom-centre anchor', async t => {
  const f = await fixture(t);
  let page = f.mount(); await page.mount(); await turn();
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  const p = () => page.instance;
  const node = cls => findNode(renderStory(proxyRefs(p()), []), item => item.props?.class === cls);
  assert.equal(p().roleRef.value, 'child');
  const column = node('story-bottom-column');
  assert.deepEqual(column.children.filter(item => item.type !== vue.Comment).map(item => item.props?.class),
    ['story-bottom-stack', 'hotbar'], '小世界的底部中央列里必须有物品栏');
  // 面板打开时物品栏让出这一格，但通道与状态条还在（反馈仍然要显示在同一个位置）。
  p().inventoryOpen.value = true;
  const openColumn = node('story-bottom-column');
  assert.equal(openColumn, null, '行囊打开时 HUD 让位，底部通道也一并隐藏');
  p().inventoryOpen.value = false;
  const back = node('story-bottom-column');
  assert.ok(back && findNode(back, item => item.props?.class === 'hotbar'), '关掉行囊后物品栏回到原位');
});

// 底部中央列的左边缘 = 视口中心 - 物品栏一半宽；右边缘 = 视口中心 + 一半宽。
// 传入 box 只为读它的水平锚点，宽度由参数给出（列本身是 max-content，样式里量不到）。
function barWidthBoxEdge(box, halfWidth) {
  const center = 1440 / 2;
  return halfWidth === 0 ? center - 0 : center;
}


test('inventory arrows reach slots, close, locked tooltip and enabled world button; Tab/Esc close', async t => {
  const f = await fixture(t); const page = f.mount(); await page.mount();
  const node = label => ({ label, focus() { document.activeElement = this; } });
  const slots = Array.from({ length: 20 }, (_, i) => node(`slot-${i}`));
  const close = node('close'), hint = node('unlock-hint'), travel = node('travel');
  let locked = true;
  page.instance.inventoryPanel.value = {
    querySelectorAll: () => slots,
    querySelector: selector => selector === '[data-inventory-close]' ? close
      : selector === '[data-inventory-travel-focus]' ? hint : locked ? null : travel,
  };
  page.instance.toggleInventory(); await turn(); assert.equal(document.activeElement, close);
  const press = code => page.instance.onInventoryKey(event({ code }));
  press('ArrowDown'); assert.equal(document.activeElement, slots[0]);
  press('ArrowRight'); assert.equal(document.activeElement, slots[1]);
  press('ArrowDown'); assert.equal(document.activeElement, slots[6]);
  press('End'); assert.equal(document.activeElement, slots[19]);
  press('ArrowDown'); assert.equal(document.activeElement, hint);
  locked = false; press('ArrowUp'); assert.equal(document.activeElement, slots[19]);
  press('ArrowDown'); assert.equal(document.activeElement, travel);
  press('Home'); press('ArrowUp'); assert.equal(document.activeElement, close);
  page.instance.onKeyDown(event({ code: 'Tab' })); assert.equal(page.instance.inventoryOpen.value, false);
  page.instance.onKeyDown(event({ code: 'Tab' })); assert.equal(page.instance.inventoryOpen.value, true);
  page.instance.onKeyDown(event({ code: 'Escape' })); assert.equal(page.instance.inventoryOpen.value, false);
  assert.equal(f.route, '/'); assert.deepEqual(f.opens(), []);
});

test('the bag travel entry appears only after the world fragment is collected, then enters without a native open', async t => {
  // 小世界一览本来只有一条（主世界）；拾得碎片后才多出可出入的第二条。
  const state = { version: 2, revision: 0, rage: 20, boss: { hp: 20 }, drop: null, inventory: { worldFragment: 0 } };
  let pageInstance = null;
  const f = await fixture(t, { gameplay: async request => {
    if (request.action !== 'load') {
      state.revision += 1;
      if (request.action === 'pickupDrop') {
        state.drop.collected = true; state.inventory.worldFragment += 1;
      } else if (request.action === 'hitBoss' || request.hit) {
        state.boss.hp = Math.max(0, state.boss.hp - 100);
      }
      if (state.boss.hp === 0 && !state.drop) {
        // 红球落在玩家脚下，便于拾取
        const player = pageInstance?.instance.camera.snapshotPlayer().position || [0, 0, 0];
        state.drop = { id: 'story.boss.world-fragment', position: [player[0], 0, player[2]], collected: false };
      }
    }
    return { status: 'ok', role: 'main', config: gameplayConfig, state: structuredClone(state) };
  } });
  const page = f.mount(); pageInstance = page; await page.mount();
  const entry = () => findNode(renderStory(proxyRefs(page.instance), []),
    node => node.props?.['data-inventory-travel'] !== undefined);

  clickWorld(page); await turn();
  assert.equal(f.opens().length, 0, 'a living Boss cannot travel');
  assert.equal(entry(), null, 'no second world exists before the fragment is collected');

  // 走到 Boss 身前击败它，再拾起红色球（世界碎片）：一览里随即辟出第二界
  page.instance.inventoryOpen.value = false;
  // 攻击沿 facingYaw 判定，而玩家会转向自己走的方向，所以先在 x 上对准 Boss（它不在玩家
  // 出生点正前方），再直着走进去，直到这一刀真的能落上——用的是页面攻击自己那条判据。
  const bagBoss = STORY_CHARACTERS.find(character => character.role === 'boss');
  const bagBossActor = () => f.actors.get(MAIN).find(a => a.actor_guid === bagBoss.guid);
  await walkTo(page, f, 'KeyD', 100, () => page.instance.camera.snapshotPlayer().position[0]
    < bagBoss.x - 0.1);
  await walkTo(page, f, 'KeyW', 100, () => gameplayModule.canHitBoss(
    page.instance.camera.snapshotPlayer(), gameplayModule.worldBounds(bagBossActor()), gameplayConfig));
  assert.equal(gameplayModule.canHitBoss(page.instance.camera.snapshotPlayer(),
    gameplayModule.worldBounds(bagBossActor()), gameplayConfig), true, 'the player can reach the Boss');
  await page.instance.gameplay.attack();
  assert.equal(page.instance.gameplay.data.state.boss.hp, 0);
  await page.instance.gameplay.pickup();
  assert.equal(page.instance.gameplayState.value.state.inventory.worldFragment, 1);
  page.instance.inventoryOpen.value = true;
  const node = entry();
  assert.ok(node, 'the fragment opens a second small world');
  assert.equal(node.props.disabled, false);

  // 点它＝原来的「小世界」按钮：走既有换场景流程，并收起行囊
  await page.instance.bagTravel(1);
  await turn();
  assert.equal(page.instance.inventoryOpen.value, false);
  assert.equal(page.instance.gameplayError.value, '');
  assert.deepEqual(f.opens(), [CHILD]);
});

// 回归：F 的提示与 F 的动作必须永远一致。碎片只掉在主世界，先知只站在小世界，
// 所以两个世界各有一条 F；两条都必须「提示说什么，F 就做什么」。
test('the F prompt and the F key agree: near the pickup it picks up, near the prophet it talks', async t => {
  const state = { version: 2, revision: 0, rage: 20, boss: { hp: 0 },
    drop: { id: 'story.boss.world-fragment', position: [0, 0, 0], collected: false },
    inventory: { worldFragment: 0 } };
  const f = await fixture(t, { gameplay: async (request, activePath) => {
    if (request.action === 'pickupDrop') { state.drop.collected = true; state.inventory.worldFragment += 1; }
    return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
      config: gameplayConfig, state: structuredClone(state) };
  } });
  let page = f.mount(); await page.mount();

  // 主世界：碎片就在玩家脚下（距离 0 ≤ pickupRange 2），先知根本不在场。
  const p = () => page.instance;
  const hint = () => p().controlHints.value.find(([key]) => key === 'F')?.[1];
  p().updateProximity();
  assert.equal(p().fKeyAction.value.kind, 'pickup');
  assert.equal(hint(), '按 F 拾取');
  p().onKeyDown(event({ code: 'KeyF' }));
  await turn();
  assert.equal(p().gameplayState.value.state.inventory.worldFragment, 1, '提示说拾取，F 就必须拾取');
  assert.equal(p().dialogueOpen.value, false, '主世界没有先知可谈');

  // 小世界：碎片在主世界已经拾走，这里只可能谈先知。分两次进入同一份夹具：
  // 先知远在天边时 F 什么都不做，把它挪到玩家身边后提示与 F 一起变成交谈。
  const character = role => STORY_CHARACTERS.find(item => item.role === role);
  const childActor = role => f.actors.get(CHILD).find(item => item.actor_guid === character(role).guid);
  const prophetAt = [...childActor('prophet').geometry.position];
  clickWorld(page); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  assert.equal(p().fKeyAction.value.kind, null, '先知远在射程外，没有可执行的动作');
  assert.equal(hint(), undefined, '没有动作就没有 F 提示');
  p().onKeyDown(event({ code: 'KeyF' }));
  assert.equal(p().dialogueOpen.value, false, '够不着就不能交谈');

  // 让玩家站到先知面前：页面在挂载时把场景快照读进 prophetActor，所以摆位要在挂载前完成。
  childActor('player').geometry.position = [...prophetAt];
  page.unmount(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  assert.equal(p().fKeyAction.value.kind, 'prophet', '先知就在身上，裁决必须选它');
  assert.equal(hint(), 'F 与先知交谈');
  assert.ok(p().fKeyAction.value.distance <= 1, '先知确实在射程内');
  p().onKeyDown(event({ code: 'KeyF' }));
  assert.equal(p().dialogueOpen.value, true, '提示说交谈，F 就必须交谈');
  p().closeDialogue(); await turn();
  assert.equal(p().gameplayState.value.state.inventory.worldFragment, 1, '交谈不会顺手改动拾取进度');
});

// 回归（拾取失效）：F 的距离与射程必须和玩法模块自己那套逐字对齐。
// 上一条用例手写的 state 少了一整块盘上字段（operations / legacyRevision / inventory.block），
// 于是「页面的 state 是不是后端真的那份」从来没被验过：真存档多一个字段、少一个 position，
// 页面就会算出 Infinity 距离，提示不出现、F 静默什么都不做——玩家看到的就是「不能拾取物体」。
// 这里把 game/runtime/story_gameplay.py 的 initial_state() / CONFIG 原样取来当夹具，
// 再验同一条规则的两侧：距离用 state.drop.position，射程用 config.pickupRange。
function readPythonGameplay(t) {
  const python = process.env.CORONA_PYTHON || 'conda';
  const args = process.env.CORONA_PYTHON ? ['-c'] : ['run', '-n', 'coronaengine-dev', 'python', '-c'];
  try {
    const { execFileSync } = require('child_process');
    const output = execFileSync(python, [...args,
      'import json, sys; sys.path.insert(0, "."); '
      + 'from game.runtime import story_gameplay as m; '
      + "print(json.dumps({'config': m.CONFIG, 'state': m.initial_state()}))"],
    { encoding: 'utf8', cwd: new URL('../../..', import.meta.url) });
    return JSON.parse(output.trim().split(/\r?\n/).at(-1));
  } catch (error) {
    t.skip(`无法读取故事玩法的真实状态（需要一个可用的 Python 与 coronaengine-dev 环境）：${error.message}`);
    return null;
  }
}
test('the real save shape from story_gameplay.py picks up inside config.pickupRange and refuses outside it', async t => {
  const { config: realConfig, state: realState } = readPythonGameplay(t);
  if (!realConfig) return;
  // 盘上的初始存档形状：这些字段一个都不能少，否则页面拿到的不是后端真的那份 state。
  assert.deepEqual(Object.keys(realState).sort(),
    ['boss', 'drop', 'inventory', 'legacyRevision', 'operations', 'rage', 'revision', 'version'],
    '夹具必须就是 story_gameplay.initial_state() 的真实字段');
  assert.equal(realConfig.pickupRange, 2, '射程来自后端的 CONFIG，不是前端自己写的数');

  // 真存档的红球：position 是 [x, y, z]，收集标记是 collected；用后端自己的射程把它放在边界内。
  const state = structuredClone(realState);
  const range = realConfig.pickupRange;
  state.boss.hp = 0;
  state.drop = { id: 'story.boss.world-fragment', position: [range - 1, 0, 0], collected: false };
  const f = await fixture(t, { gameplay: async (request, activePath) => {
    if (request.action === 'pickupDrop') { state.drop.collected = true; state.inventory.worldFragment += 1; }
    return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
      config: realConfig, state: structuredClone(state) };
  } });
  const page = f.mount(); await page.mount(); await turn();
  const p = () => page.instance;
  const hint = () => p().controlHints.value.find(([key]) => key === 'F')?.[1];
  p().camera.snapshotPlayer().position = [0, 0, 0];
  p().updateProximity();

  assert.equal(p().fKeyAction.value.kind, 'pickup', '真实 state + 真实射程：边界内的红球必须可选');
  assert.equal(hint(), '按 F 拾取');
  p().onKeyDown(event({ code: 'KeyF' }));
  await turn();
  assert.equal(p().gameplayState.value.state.inventory.worldFragment, 1, '提示说拾取，F 就必须真的提交 pickupDrop');
  assert.equal(p().gameplayState.value.state.drop.collected, true);
});

// 反向：超出射程一样要有提示与动作都要消失（这正是页面算距离、算射程两件事同时错才会漏掉的边界）。
test('a drop outside the real pickupRange offers neither the prompt nor the F action', async t => {
  const { config: realConfig, state: realState } = readPythonGameplay(t);
  if (!realConfig) return;
  const state = structuredClone(realState);
  state.boss.hp = 0;
  state.drop = { id: 'story.boss.world-fragment', position: [realConfig.pickupRange + 1, 0, 0], collected: false };
  const f = await fixture(t, { gameplay: async (request, activePath) => ({ status: 'ok',
    role: activePath === MAIN ? 'main' : 'child', config: realConfig, state: structuredClone(state) }) });
  const page = f.mount(); await page.mount(); await turn();
  const p = () => page.instance;
  p().camera.snapshotPlayer().position = [0, 0, 0];
  p().updateProximity();

  assert.equal(p().fKeyAction.value.kind, null, '射程外的红球不是候选');
  assert.equal(p().controlHints.value.find(([key]) => key === 'F'), undefined, '没有动作就没有 F 提示');
  p().onKeyDown(event({ code: 'KeyF' }));
  await turn();
  assert.equal(p().gameplayState.value.state.inventory.worldFragment, 0, '够不着就不许拾取');
});

// 端到端：这就是玩家真实的拾取动作序列（与盘上 story_world_*\.game\story-gameplay.json 一致：
// Boss 死在 [0,0,12]，红球落在同一处，玩家站在 z≈11.6）。走到球上，提示必须变成拾取，F 必须真的拾起来。
test('walking onto the drop the dead boss left turns the F prompt into a real pickup', async t => {
  const { config: realConfig, state: realState } = readPythonGameplay(t);
  if (!realConfig) return;
  // 红球落在龙倒下的地方：这里用 Boss 自己的出生点，而不是再抄一份位置。
  const boss = STORY_CHARACTERS.find(item => item.role === 'boss');
  const bossPosition = [boss.x, 0, boss.z];
  const state = structuredClone(realState);
  const f = await fixture(t, { gameplay: async (request, activePath) => {
    if (request.action === 'hitBoss' || request.hit) {
      state.revision += 1;
      state.boss.hp = Math.max(0, state.boss.hp - realConfig.damage);
      if (state.boss.hp === 0) {
        state.drop = { id: 'story.boss.world-fragment', position: [...bossPosition], collected: false };
      }
    } else if (request.action === 'pickupDrop') {
      state.revision += 1;
      state.drop.collected = true; state.inventory.worldFragment += 1;
    }
    return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
      config: realConfig, state: structuredClone(state) };
  } });
  const page = f.mount(); await page.mount(); await turn();
  const p = () => page.instance;
  const hint = () => p().controlHints.value.find(([key]) => key === 'F')?.[1];

  // Boss 还活着：没有红球，就没有拾取这一行。
  assert.equal(hint(), undefined, 'Boss 未死时不该出现拾取提示');

  // 走到 Boss 面前把它打死：红球落在 Boss 倒下的位置。攻击受冷却限制，
  // 因此把玩法模块读的那个时钟（performance.now）接到本用例手里，而不是忙等。
  let clock = 0;
  t.mock.method(performance, 'now', () => clock);
  // 走到 Boss 面前（用真实的按键移动路径，而不是改一份快照副本）。攻击沿 facingYaw 判定，
  // 所以先在 x 上对准 Boss（它不在玩家出生点的正前方），再往里走到模型边缘之内——
  // `meleeRange` 量的是到包围盒的距离，小世界里这个夹具的包围盒只有 0.214 m 深。
  p().onKeyDown(event({ code: 'KeyD', key: 'd' }));
  for (let i = 0; i < 200 && p().camera.snapshotPlayer().position[0] < boss.x - 0.1; i++) f.step(50);
  p().camera.keyUp(event({ code: 'KeyD', key: 'd' }));
  p().onKeyDown(event({ code: 'KeyW', key: 'w' }));
  for (let i = 0; i < 200 && p().camera.snapshotPlayer().position[2] < boss.z - 0.5; i++) f.step(50);
  p().camera.keyUp(event({ code: 'KeyW', key: 'w' }));
  assert.ok(p().camera.snapshotPlayer().position[2] >= boss.z - 0.5, '玩家必须能走到 Boss 脚下');
  const swings = Math.ceil(realConfig.bossHp / realConfig.damage);
  for (let i = 0; i < swings; i++) { clock += realConfig.cooldownMs; await p().gameplay.attack(); await turn(); }
  assert.equal(state.boss.hp, 0, '打满次数后 Boss 必须倒下');
  assert.ok(state.drop && state.drop.collected === false, 'Boss 死后必须留下未拾取的红球');

  // 玩家就站在红球上：提示与 F 都必须是拾取。
  p().updateProximity();
  assert.equal(p().fKeyAction.value.kind, 'pickup', '站在红球上，F 的裁决必须是拾取');
  assert.equal(hint(), '按 F 拾取', '站在红球上必须给出拾取提示');
  p().onKeyDown(event({ code: 'KeyF' }));
  await turn();
  assert.equal(p().gameplayState.value.state.inventory.worldFragment, 1, '按 F 必须真的把红球捡起来');
  assert.equal(p().gameplayState.value.state.drop.collected, true);
});

// 回归（Bug 1）：小世界的陈列是盘上的权威数据，进入世界时必须读回来。它一度一份也没读回来
// （`load()` 里对兄弟方法的裸调用抛 ReferenceError，被空 catch 吞掉），于是玩家放下的方块全部
// 回到行囊：离开再回来，物品栏又显示 64。这里走完整的换场景流程断言 64 − 3 = 61。
test('leaving and re-entering the small world shows the saved block count, not the seed again', async t => {
  const placements = { version: 1, placements: [0, 1, 2].map(index => ({ propId: 'block', index,
    siteId: null, position: [index, 0, 0], rotation: [0, 0, 0], scale: 1 })) };
  const f = await fixture(t, {
    documents: { loadPlacements: placements },
    gameplay: async (request, activePath) => ({ status: 'ok',
      role: activePath === MAIN ? 'main' : 'child', config: gameplayConfig,
      state: { version: 2, revision: 0, rage: 0, boss: { hp: 0 }, drop: null,
        inventory: { worldFragment: 1, block: 64 } } }),
  });
  let page = f.mount(); await page.mount(); await turn();
  const blockCount = () => page.instance.hotbarSlots.value[0];
  const countText = () => {
    const texts = [];
    const walk = node => {
      if (!node || typeof node !== 'object') return;
      if (node.props?.class === 'hotbar__count') texts.push(node.children);
      for (const child of Array.isArray(node.children) ? node.children : []) walk(child);
    };
    walk(renderStory(proxyRefs(page.instance), []));
    return texts;
  };

  // 主世界里陈列是空的（主世界只显示空布局），进小世界才读得回来。
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  assert.deepEqual(page.instance.placements.value.map(entry => entry.propId), ['block', 'block', 'block']);
  assert.deepEqual(blockCount(), { slot: 0, id: 'block', name: '方块',
    description: '主世界带回的方块，可直接在小世界中摆放。',
    owned: 64, placed: 3, available: 61, empty: false });
  assert.deepEqual(countText(), ['61'], '物品栏上那个数字必须就是 61');

  // 回到主世界，再进一次：同一份盘上布局，数量不许回到 64。
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  assert.deepEqual(f.opens(), [CHILD, MAIN, CHILD]);
  assert.equal(page.instance.hotbarSlots.value[0].placed, 3);
  assert.equal(page.instance.hotbarSlots.value[0].available, 61);
  assert.deepEqual(countText(), ['61']);
});

// 回归（小世界里无法取回方块）：小世界没有红球可用，唯一能被 F 拿回来的东西就是玩家自己
// 放下的方块——取回就是摆放的逆操作。这里走完整的玩家动作序列：进小世界、按 B 在身前放一块、
// 提示变成「按 F 取回方块」、按 F 之后布局少一项、物品栏的可用数回到摆放前，而且场景里的 actor
// 是由既有的陈列 reconcile 路径退场的（没有第二条撤下路径）。
test('the small world takes a placed block back: the 取回 prompt, F removes it, and the count returns', async t => {
  let layout = { version: 1, placements: [] };
  const saved = [];
  const state = { version: 2, revision: 0, rage: 0, boss: { hp: 0 }, drop: null,
    inventory: { worldFragment: 1, block: 4 } };
  const f = await fixture(t, {
    documents: { loadPlacements: layout },
    gameplay: async (request, activePath) => {
      if (request.action === 'savePlacements') {
        saved.push(structuredClone(request.placements));
        layout = { version: 1, placements: structuredClone(request.placements) };
        return { status: 'ok', role: 'child', state: structuredClone(layout) };
      }
      return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
        config: gameplayConfig, state: structuredClone(state) };
    },
  });
  let page = f.mount(); await page.mount(); await turn();
  const p = () => page.instance;
  const hint = () => p().controlHints.value.find(([key]) => key === 'F')?.[1];
  const available = () => p().hotbarSlots.value[0].available;

  // 主世界：读到的是空布局，那里没有可摆放的陈列物，也就没有取回这一行。
  assert.deepEqual(p().placements.value, []);
  assert.equal(hint(), undefined, '主世界没有陈列物，F 就没有取回');
  assert.equal(available(), 4);

  // 进小世界：布局还是盘上那一份（空）。
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  assert.equal(p().roleRef.value, 'child');
  assert.deepEqual(p().placements.value, []);
  assert.equal(available(), 4);
  assert.equal(hint(), undefined, '身边什么都没有时没有 F 提示');

  // 按 B 在身前 2 米放下一个方块：玩家站着不动，它就在取回的射程里。
  p().onKeyDown(event({ code: 'KeyB', key: 'b' }));
  await turn(); await turn(); await turn();
  assert.equal(p().gameplayError.value, '', '摆放不能报错');
  assert.deepEqual(p().placements.value.map(entry => entry.propId), ['block']);
  assert.deepEqual(saved.at(-1).map(entry => entry.propId), ['block'], '布局必须先落盘');
  assert.equal(available(), 3, '放下一个方块，可用数就该少一');
  const guid = storyPropsModule.placementGuid(p().placements.value[0].index);
  assert.ok(f.actors.get(CHILD).some(actor => actor.actor_guid === guid), '方块必须真的进了场景');

  // 站在方块旁边：F 的裁决与那一行提示一起变成取回。
  p().updateProximity();
  assert.equal(p().fKeyAction.value.kind, 'block', '身边自己放下的方块必须是 F 的目标');
  assert.ok(p().fKeyAction.value.distance <= storyPropsModule.PLACED_BLOCK_REACH, '方块确实在射程内');
  assert.equal(hint(), '按 F 取回方块');

  // 按 F：布局少一项、可用数回到 4、场景里的 actor 由既有陈列路径退场。
  p().onKeyDown(event({ code: 'KeyF' }));
  await turn(); await turn(); await turn();
  assert.equal(p().gameplayError.value, '', '取回不能报错');
  assert.deepEqual(p().placements.value, []);
  assert.deepEqual(saved.at(-1), [], '取回必须把「少了一件」的布局存回去');
  assert.equal(available(), 4, '取回之后可用数必须回到摆放前的数字');
  assert.equal(f.calls.filter(call => call[0] === 'remove' && call[3] === guid).length, 1,
    '撤下走的是 syncPlacementActors 的既有路径');
  assert.ok(!f.actors.get(CHILD).some(actor => actor.actor_guid === guid), '方块必须从场景里消失');
  assert.equal(p().fKeyAction.value.kind, null, '方块没了，取回的裁决也随之消失');
  assert.equal(hint(), undefined, '没有目标就没有 F 提示');
  assert.equal(p().gameplayState.value.state.inventory.block, 4, '取回不动盘上的持有数');
  assert.equal(available(), p().hotbarSlots.value[0].owned, '手里的方块又全都能用了');
});

// 回归（Bug 2）：碎片规则是盘上的数据。它一度在进入世界时一份也没读回来，于是碎片显示未装填、
// 「调整」子屏只回一句「先装填该碎片再调整」，按 1-4 一个字节都不写盘。这里断言规则随世界回来、
// 按 1 后新数字既落盘又真的交给了规则引擎（而不是「改了但看不出来」）。
test('a saved fragment rule comes back with the world, and 1 in 调整 reaches the sway runner', async t => {
  let rules = { version: 1, rules: [{ fragmentId: 'sway', type: 'sway', axis: 'x', amplitude: 0.25, periodMs: 2400 }] };
  const saved = [];
  const f = await fixture(t, {
    documents: { loadWorldRules: rules },
    gameplay: async (request, activePath) => {
      if (request.action === 'saveWorldRules') {
        saved.push(structuredClone(request.rules));
        rules = { version: 1, rules: structuredClone(request.rules) };
        return { status: 'ok', role: 'child', state: structuredClone(rules) };
      }
      return { status: 'ok', role: activePath === MAIN ? 'main' : 'child', config: gameplayConfig,
        state: { version: 2, revision: 0, rage: 0, boss: { hp: 0 }, drop: null,
          inventory: { worldFragment: 1, block: 64 } } };
    },
  });
  let page = f.mount(); await page.mount(); await turn();
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();

  assert.deepEqual(page.instance.installedRules.value.map(rule => [rule.amplitude, rule.periodMs]),
    [[0.25, 2400]], '已装填的碎片必须随世界回来');
  assert.equal(page.instance.worldRuleRunner.isRunning(), true, '规则引擎按保存的数字运行');
  // 「碎片」屏上的「调整」行：子屏必须认出这块碎片已装填，并显示盘上的数字（而不是退回目录默认值）。
  page.instance.dialogueOpen.value = true;
  page.instance.openFragmentAdjust(page.instance.fragmentList.value[0]);
  assert.equal(page.instance.prophetScreen.value, 'fragment-adjust');
  assert.equal(page.instance.fragmentAdjust.value.installed, true);
  assert.match(page.instance.fragmentAdjust.value.amplitudeLabel, /0\.25/);

  // 「调整」子屏上按 1：先落盘，再把新数字交给正在运行的规则引擎。
  const captured = [];
  const runner = page.instance.worldRuleRunner;
  const original = runner.setRules;
  runner.setRules = list => { captured.push(structuredClone(list)); return original(list); };
  page.instance.onKeyDown(event({ code: 'Digit1', key: '1' }));
  await turn(); await turn();
  assert.deepEqual(saved, [[{ fragmentId: 'sway', type: 'sway', axis: 'x', amplitude: 0.3, periodMs: 2400 }]],
    '幅度 0.25 + 0.05 必须落盘');
  assert.deepEqual(captured.at(-1), [{ type: 'sway', axis: 'x', amplitude: 0.3, periodMs: 2400,
    excludeRoles: ['player'] }], '规则引擎收到的是新幅度');
  assert.match(page.instance.actionFeedback.value, /0\.30/);
  assert.equal(page.instance.worldRuleRunner.isRunning(), true);
});

// 地图（山门神殿）必须像角色和方块一样随「进入世界」出现，而不是只在新世界里存在：
// 这条用例走的是已有世界（世界里的 actor 早已有四个角色，没有地图），因此它证明的是
// 前端这条路径真的把地图补了进来——如果地图只由建世界时的原生代码种下，这里会失败。
test('an existing world gets the walkable map on entry, and a second entry never duplicates it', async t => {
  const f = await fixture(t);
  const mapCalls = () => f.calls.filter(call => call[0] === 'create'
    && call[4]?.actor_guid === storyMapModule.MAP_GUID);
  const mapActor = () => f.actors.get(MAIN).find(actor => actor.actor_guid === storyMapModule.MAP_GUID);

  const page = f.mount(); await page.mount(); await turn();
  const [first] = mapCalls();
  assert.ok(first, '进入已有世界必须创建地图');
  const transform = storyMapModule.mapOriginTransform();
  assert.deepEqual(first[4].position, transform.position, '地面必须落在 y = 0 约定的摆位上');
  assert.deepEqual(first[4].scale, transform.scale, '引擎归一化成单位立方体，缩放要还原真实尺寸');
  assert.equal(first[4].body_type, 'phantom',
    '地图是布景：站立由对齐后的庭院与世界地板提供，不为 164 万面建 octree');
  assert.equal(first[4].physics_enabled, undefined, 'body_type 与 physics_enabled 不能各说各话');
  assert.equal(first[4].model_ref, storyMapModule.MAP_MODEL_REF);
  assert.equal(mapActor().mechanics.body_type, 'phantom',
    '地图不参与碰撞：靠庭院对齐 y = 0 与世界地板站立');
  assert.deepEqual(mapActor().geometry.position, transform.position);

  // 第二次进入同一个世界：GUID 不变，而且摆位与数据表一致，因此不能再造一张地图、
  // 不能重摆，也不能写场景（update_if_exists 会让引擎重写整个清单）。
  page.unmount();
  const again = f.mount(); await again.mount(); await turn();
  assert.equal(mapCalls().length, 1, '第二次进入不能产生第二个地图 actor');
  assert.equal(f.actors.get(MAIN).filter(actor => actor.actor_guid === storyMapModule.MAP_GUID).length, 1);
  assert.deepEqual(mapActor().geometry.position, transform.position, '已就位的地图不许被重摆');
  assert.equal(f.alerts.length, 0, '地图就位不该报错');
  // 零写入：既不能再造、也不能删掉再建（delete + create 会重新导入 87MB 的 GLB）。
  assert.deepEqual(f.calls.filter(call => call[0] === 'remove'), [], '已就位的地图不许被删除');
});

// 地图的 actor 数据本身：follow_camera 决定引擎把它画在场景空间还是 UI 空间
// （render_space = follow_camera ? "ui" : "scene"），写成 true 地图就会跟着人物动；
// 资源必须是工程内路由而不是仓库源路径，否则每次进入世界都要重新导入 87MB 的 GLB。
test('the map is created as a world object from an in-project route, never as UI space', async t => {
  const f = await fixture(t);
  const page = f.mount(); await page.mount(); await turn();
  const [first] = f.calls.filter(call => call[0] === 'create'
    && call[4]?.actor_guid === storyMapModule.MAP_GUID);
  assert.ok(first, '进入已有世界必须创建地图');
  assert.equal(first[4].follow_camera, false, 'follow_camera 为 true 时引擎把地图画进 UI 空间');
  assert.equal(first[4].body_type, 'phantom');

  const route = first[3];
  assert.equal(typeof route, 'string');
  // storyActors.mjs 的角色模式：先把资产解析成工程内路由再交给引擎（storyCharacters
  // 的 resolveStoryAssetPath 把 game/art/models 下的源资产解析成工程内路径），
  // 引擎据此按 GUID 复用已导入的那一份，而不是每次都重新导入 87MB 的 GLB。
  const resolved = resolveStoryAssetPath(window.location.href, storyMapModule.MAP_PLACEMENT.asset);
  assert.equal(route, resolved, '地图必须走与角色相同的工程内路由');
  // 路由指向仓库里真实存在的源资产（上面的绝对前缀只是 file:// 页面的所在盘）。
  const tail = resolved.replace(/^.*?\/(?=game\/art\/)/, '');
  assert.ok(fs.existsSync(new URL(`../../../${tail}`, import.meta.url)),
    `工程内路由必须真的存在：${resolved}`);
  assert.ok(!/^file:/i.test(route), '不能把 file:// 源路径交给引擎');
  assert.ok(!/^\/\//.test(route), '不能把 UNC 源路径交给引擎');
  assert.ok(!/\.asset-stage|Assets[\\/]Models/i.test(route), '前端不能自己拼引擎的导入目录');
  // 地图已经带 model_ref：第二次进入不进入 update_if_exists 的写入路径。
  assert.equal(first[4].update_if_exists, undefined, '首次创建不该带 update_if_exists');
});

// 世界里的地图摆位是旧公式算出来的（沉了 40.8cm）：它必须被就地校正，而不是因为
// 「scale 对得上」就被当成已就位——只比 scale 的检查正是让错误高度永久留下来的原因。
test('a map left at an older transform is repaired in place instead of adopted', async t => {
  const f = await fixture(t);
  const first = f.mount(); await first.mount(); await turn();
  const transform = storyMapModule.mapOriginTransform();
  const stale = f.actors.get(MAIN).find(actor => actor.actor_guid === storyMapModule.MAP_GUID);
  assert.deepEqual(stale.geometry.position, transform.position);
  // 一个明确错误的历史值：把米制偏移直接当归一化量写下（Python 侧点名的同一个错误）。
  // 原来用 `scale/2` 当 AABB 中高的旧公式，在地图缩到 90 后误差只剩 7 厘米、不再可观测，
  // 所以这里换一个与 scale 无关、且必然明显的历史值。
  const legacy = storyMapModule.MAP_PLACEMENT.centreY - storyMapModule.MAP_PLACEMENT.courtyardY;
  assert.ok(Math.abs(legacy - transform.position[1]) > 0.4, '旧值与正确值的差必须可观测');
  stale.geometry.position = [0, legacy, 0];
  first.unmount();

  const again = f.mount(); await again.mount(); await turn();
  const repairs = f.calls.filter(call => call[0] === 'create'
    && call[4]?.actor_guid === storyMapModule.MAP_GUID);
  assert.equal(repairs.length, 2, '第二次进入必须走一次校正调用');
  assert.equal(repairs[1][4].skip_if_exists, true, '校正必须复用同一个 actor');
  assert.equal(repairs[1][4].update_if_exists, true, '校正必须真的写回摆位');
  assert.deepEqual(repairs[1][4].position, transform.position);
  const repaired = f.actors.get(MAIN).filter(actor => actor.actor_guid === storyMapModule.MAP_GUID);
  assert.equal(repaired.length, 1, '校正不能追加第二张地图');
  assert.deepEqual(repaired[0].geometry.position, transform.position, '地图必须回到 y = 0 的摆位');
  assert.equal(f.alerts.length, 0, '校正不该报错');
  // 代次没变时只许就地校正：单纯摆位过期就删了重建，等于每次进入都重新导入模型。
  assert.deepEqual(f.calls.filter(call => call[0] === 'remove'), [], '摆位过期不能走删除重建');
});

/**
 * 一个「旧世界」里那张旧世代的地图 actor：代次标记与路线都指向换图之前的资产。
 *
 * 这正是 01_山门神殿_优化版.glb → 01_山门神殿_平地版.glb 这次换图留下的世界形状：
 * guid/entity 身份不变（否则已有世界会认不出自己的地图而再叠一张），只有路线与代次是旧的。
 * `route` 用引擎写回快照的形式（导入后的 Assets/Models/<hash>/<文件>），而不是前端送进去的源路径，
 * 因为要验证的判断读的就是引擎报告的那条路线。
 */
function legacyMapActor() {
  return { name: '剧情世界地图', actor_guid: storyMapModule.MAP_GUID, handle: 900,
    load_status: 'loaded', render_ready: true, gpu_build_state: 'Ready',
    model_ref: 'corona.story.world-map.v1', route: 'Assets/Models/5f63c8834b9d/01_山门神殿_优化版.glb',
    semantic_role: 'world-map', entity_type: storyMapModule.MAP_ENTITY_TYPE,
    entity_id: storyMapModule.MAP_ENTITY_ID,
    local_aabb: [-0.5, -0.5, -0.5, 0.5, 0.5, 0.5], visible: true, follow_camera: false,
    camera_lock: { enabled: false }, mechanics: { body_type: 'phantom' },
    geometry: storyMapModule.mapOriginTransform() };
}

// 换图之后，已被旧世界收养的那张地图必须被删掉重建：引擎的 skip_if_exists 修复分支只写摆位、
// 从不改写 actor.route（cef_editor_native_api_handlers.cpp:3410-3424），所以旧 actor 永远渲染旧 GLB。
// 这条用例证明的是「进入即自愈」：不需要玩家手动删除 actor，也不需要重开一个世界。
test('a world carrying the previous map generation is healed on entry, not left on the old model', async t => {
  const f = await fixture(t);
  const before = f.actors.get(MAIN).length;
  f.actors.get(MAIN).push(legacyMapActor(f));

  const page = f.mount(); await page.mount(); await turn();
  const removes = f.calls.filter(call => call[0] === 'remove');
  assert.equal(removes.length, 1, '旧代次的地图必须被删除一次');
  assert.ok(removes[0].includes(storyMapModule.MAP_GUID), '删除的目标必须是地图自己（按 GUID，不靠名字）');
  const creates = f.calls.filter(call => call[0] === 'create'
    && call[4]?.actor_guid === storyMapModule.MAP_GUID);
  assert.equal(creates.length, 1, '删除之后必须恰好创建一次——重建是唯一会重新导入模型的路径');
  assert.equal(creates[0][4].model_ref, storyMapModule.MAP_MODEL_REF, '新 actor 必须带当前代次');
  assert.equal(creates[0][4].skip_if_exists, undefined, '重建不能带 skip_if_exists，否则引擎又会复用旧 actor');
  // 位置在删除与创建之间：删除必须先发生，否则 create 会按 GUID 复用旧 actor。
  assert.ok(f.calls.findIndex(call => call[0] === 'remove') < f.calls.findIndex(call => call[0] === 'create'
    && call[4]?.actor_guid === storyMapModule.MAP_GUID), '必须先删除再创建');

  const maps = f.actors.get(MAIN).filter(actor => actor.actor_guid === storyMapModule.MAP_GUID);
  assert.equal(maps.length, 1, '自愈之后世界里只能有一张地图');
  assert.equal(maps[0].model_ref, storyMapModule.MAP_MODEL_REF);
  assert.equal(storyMapModule.assetName(maps[0].route), storyMapModule.assetName(storyMapModule.MAP_PLACEMENT.asset),
    '自愈之后路线必须指向当前那张 GLB，而不是上次导入的那一份');
  assert.deepEqual(maps[0].geometry.position, storyMapModule.mapOriginTransform().position);
  assert.equal(f.alerts.length, 0, '自愈不该报错');
  assert.equal(f.actors.get(MAIN).length, before + 1, '自愈不能顺带动别的 actor');
});

// 只换了图、没换 guid：已有的地图不能被当成「身份不明」而叠加第二张，反复进入也必须幂等。
test('a stale map generation is replaced once, and repeated entries never duplicate it', async t => {
  const f = await fixture(t);
  f.actors.get(MAIN).push(legacyMapActor(f));
  let page = f.mount(); await page.mount(); await turn();
  page.unmount();
  for (let entry = 0; entry < 2; entry++) {
    page = f.mount(); await page.mount(); await turn();
    page.unmount();
  }
  const creates = f.calls.filter(call => call[0] === 'create'
    && call[4]?.actor_guid === storyMapModule.MAP_GUID);
  assert.equal(creates.length, 1, '只在第一次进入时重建一次，之后必须安静');
  assert.equal(f.calls.filter(call => call[0] === 'remove').length, 1, '只许删除一次');
  assert.equal(f.actors.get(MAIN).filter(actor => actor.actor_guid === storyMapModule.MAP_GUID).length, 1);
  assert.equal(f.alerts.length, 0);
});

// 反例守卫（非空性）：把「代次」判断去掉只留摆位判断，上面那条用例必须失败。
// 这里直接演示那个错误实现的后果：actor 的路线仍是旧的，而 skip_if_exists 修复分支不碰路线，
// 于是世界继续渲染上一版几何——这正是本次要修的 bug。
test('repairing a stale generation in place would leave the old asset on the actor', async t => {
  const { api, state } = apiFixture({ actors: [] });
  const legacy = legacyMapActor({});
  state.actors.push(legacy);
  const repaired = await api.sceneTools.createActor('scene.ini', 'D:/game/art/models/scene/01_山门神殿_平地版.glb',
    'model', { ...storyMapModule.MAP_ACTOR_DATA, actor_guid: storyMapModule.MAP_GUID,
      model_ref: storyMapModule.MAP_MODEL_REF, skip_if_exists: true, update_if_exists: true,
      ...storyMapModule.mapOriginTransform() });
  assert.equal(storyMapModule.assetName(repaired.data.actor.route), '01_山门神殿_优化版.glb',
    '就地修复不会改写 route：这条断言就是「只修摆位」为什么治不好换图的证据');
  assert.notEqual(storyMapModule.assetName(repaired.data.actor.route),
    storyMapModule.assetName(storyMapModule.MAP_PLACEMENT.asset));
  // 同一个 actor 现在带着「当前代次」的标记却仍指向旧资产——只有路线这一项能识破它。
  assert.equal(repaired.data.actor.model_ref, storyMapModule.MAP_MODEL_REF);
  assert.equal(storyMapModule.isMapGenerationCurrent(repaired.data.actor), false,
    '标记是新的、路线是旧的：代次判断必须同时看路线，否则这个世界会永远渲染旧几何');
});

/**
 * 一张 4x4 的合成网格：世界高度 = 1 + 0.5 × 世界 x（沿 +x 上坡）。
 *
 * 网格以 `minY` 为基准存厘米，因此 `minY` 取整张网里最低的那一格，格值才都是非负的；
 * 这样断言用的是闭式数字（世界 (0,0) 处的地面恰好 1 m），而不是把模块的换算再抄一遍。
 */
function rampGrid(zHalf = 80) {
  // z 方向铺得够宽（±80 模型米 ≈ ±17 世界米），主世界的 Boss（z = 5.5）才落在网内而不是被夹到边上；
  // 高度只随 x 变化，所以每一行的值相同。需要覆盖更远的 z 时用参数（例如红球落点的用例）。
  const bounds = [-10, 10, -zHalf, zHalf], res = 4;
  const originY = storyMapModule.mapOriginTransform().position[1];
  const centreY = storyMapModule.MAP_PLACEMENT.centreY;
  const modelToWorld = storyMapHeightModule.MODEL_TO_WORLD;
  const cellX = (bounds[1] - bounds[0]) / res;
  const worldY = [];
  for (let i = 0; i < res; i++) worldY.push(1 + 0.5 * (bounds[0] + (i + 0.5) * cellX) * modelToWorld);
  const minY = centreY + (Math.min(...worldY) - originY) / modelToWorld;
  const heights = [];
  for (let j = 0; j < res; j++) {
    for (let i = 0; i < res; i++) {
      heights.push(Math.round((centreY + (worldY[i] - originY) / modelToWorld - minY) * 100));
    }
  }
  return { version: 1, res: [res, res], bounds, minY, unit: 'cm', heights };
}

// 上一次这条功能「改了却什么都没变」的断点有两处，这条用例把两处都按在测试里：
// 页面根本没读 `story-map-height.json`（loader 无人调用），以及读到了也没交给控制器
// （createStoryCameraController 不转发 getGroundY）。因此这里断言的是端到端的结果：
// 玩家的 y 必须等于 heightAt(x, z) + 该模型的原点离地高度，而不是固定地板。
test('the player walks on the baked map grid once it is loaded', async t => {
  const f = await fixture(t, {
    mapHeight: async () => storyMapHeightModule.setHeightGrid(rampGrid()),
    // 主世界且 Boss 还活着：Boss 与玩家同一次进入被摆到地面上（血条为 0 时不会摆它）。
    gameplay: () => ({ status: 'ok', role: 'main',
      state: { version: 2, revision: 0, rage: 0, boss: { hp: 200 }, drop: null, inventory: { worldFragment: 0 } },
      config: gameplayConfig }),
  });
  const page = f.mount(); await page.mount();
  // 1) 页面真的去读部署包里那一份数据（相对 dist/index.html 往上三级）。
  const read = f.calls.find(call => call[0] === 'mapHeight');
  assert.ok(read, '进入世界必须读入地图高度网格');
  assert.match(read[1], /^file:/, '页面在 file:// 下读盘（fetch 读不了 file:，走 XHR）');
  assert.match(read[1], /\/game\/data\/story-map-height\.json$/);
  assert.equal(storyMapHeightModule.hasHeightGrid(), true, '读到的网格必须真的装进模块');

  // 2) 玩家的地面跟着网格：1 m 的地面 + 模型原点离地 0.9 m ≈ 1.9 m，而不是 0.9 m 的固定地板。
  const localAabb = f.actors.get(MAIN).find(a => a.actor_guid === STORY_CHARACTERS[0].guid).local_aabb;
  const flat = characterTransform(STORY_CHARACTERS[0], localAabb).position[1];
  const player = f.actors.get(MAIN).find(a => a.actor_guid === STORY_CHARACTERS[0].guid);
  const standing = page.instance.camera.snapshotPlayer();
  assert.ok(Math.abs(player.geometry.position[1] - (1 + flat)) < 0.02,
    `玩家的脚必须在网格给出的地面上（期望 ${1 + flat}，实际 ${player.geometry.position[1]}）`);
  assert.ok(Math.abs(standing.position[1] - (1 + flat)) < 0.02);
  assert.ok(Math.abs(standing.position[1] - flat) > 0.5, '不是固定地板：固定地板会低整整 1 m');

  // 3) 走动时高度跟着地形变：yaw = 0 下 D 走向 +x，而网格沿 +x 上坡。
  page.instance.onKeyDown(event({ code: 'KeyD' }));
  f.step(50);
  const walked = page.instance.camera.snapshotPlayer();
  assert.ok(walked.position[0] > standing.position[0], 'D 必须真的往前走了');
  assert.ok(walked.position[1] > standing.position[1], '上坡时玩家必须跟着地面升高');
  assert.ok(Math.abs(walked.position[1]
    - (storyMapHeightModule.heightAt(walked.position[0], walked.position[2]) + flat)) < 1e-9,
  'player.position[1] 就是 heightAt(x, z) 加上原点离地高度');
  page.instance.camera.keyUp(event({ code: 'KeyD' }));

  // 4) 同一次进入里摆下的角色也吃同一份地面（Boss 在这里被放在 z = 5.5 的空地上）。
  const boss = f.actors.get(MAIN).find(a => a.actor_guid === STORY_CHARACTERS[1].guid);
  const bossFlat = characterTransform(STORY_CHARACTERS[1], boss.local_aabb).position[1];
  const ground = storyMapHeightModule.heightAt(boss.geometry.position[0], boss.geometry.position[2]);
  assert.ok(Math.abs(boss.geometry.position[1] - (ground + bossFlat)) < 1e-9,
    `Boss 的脚也要落在网格上（期望 ${ground + bossFlat}，实际 ${boss.geometry.position[1]}）`);
});

// 数据文件读不到不是进入世界的失败：地面退回原来的固定地板，其他一切照旧。
test('a missing height grid keeps the constant floor and never blocks the world', async t => {
  const f = await fixture(t, { mapHeight: async () => { throw new Error('读取失败（404）'); } });
  const page = f.mount(); await page.mount();
  assert.ok(f.calls.some(call => call[0] === 'mapHeight'), '页面必须尝试读取网格');
  assert.deepEqual(f.alerts, [], '网格读不到不能阻止进入世界');
  assert.match(page.instance.actionFeedback.value, /地图高度网格未能读取/);
  assert.equal(storyMapHeightModule.hasHeightGrid(), false);

  const player = f.actors.get(MAIN).find(a => a.actor_guid === STORY_CHARACTERS[0].guid);
  const flat = characterTransform(STORY_CHARACTERS[0], player.local_aabb).position[1];
  assert.ok(Math.abs(player.geometry.position[1] - flat) < 1e-9, '没有网格就还是固定地板');
  assert.ok(Math.abs(page.instance.camera.snapshotPlayer().position[1] - flat) < 1e-9);
  page.instance.onKeyDown(event({ code: 'KeyD' }));
  f.step(50);
  const walked = page.instance.camera.snapshotPlayer();
  assert.ok(walked.position[0] > 0, '没有网格也要走');
  assert.ok(Math.abs(walked.position[1] - flat) < 1e-9, '固定地板下走动不改高度');
  page.instance.camera.keyUp(event({ code: 'KeyD' }));
});

// 物体摆放也必须站在地面上：布局里存的 y 是**落点那一格**的地面线（和角色同一个来源），
// 而不是把玩家自己的 y 抄下来——玩家站在上坡上时，抄下来就会浮在地面上方。
test('a placed block stores the ground height at its own spot, not the player height', async t => {
  let layout = { version: 1, placements: [] };
  const saved = [];
  const state = { version: 2, revision: 0, rage: 0, boss: { hp: 0 }, drop: null,
    inventory: { worldFragment: 1, block: 4 } };
  const f = await fixture(t, {
    documents: { loadPlacements: layout },
    mapHeight: async () => storyMapHeightModule.setHeightGrid(rampGrid()),
    gameplay: async (request, activePath) => {
      if (request.action === 'savePlacements') {
        saved.push(structuredClone(request.placements));
        layout = { version: 1, placements: structuredClone(request.placements) };
        return { status: 'ok', role: 'child', state: structuredClone(layout) };
      }
      return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
        config: gameplayConfig, state: structuredClone(state) };
    },
  });
  let page = f.mount(); await page.mount(); await turn();
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  const p = () => page.instance;
  assert.equal(p().roleRef.value, 'child');

  // 先算出「方块自己那一格」的地面，再去摆放：断言不抄模块的换算，用的是被断言的同一个函数。
  const player = p().camera.snapshotPlayer();
  const x = player.position[0] + Math.sin(player.facingYaw) * storyPropsModule.PLACEMENT_DISTANCE;
  const z = player.position[2] + Math.cos(player.facingYaw) * storyPropsModule.PLACEMENT_DISTANCE;
  const ground = storyMapHeightModule.heightAt(x, z);
  assert.ok(Number.isFinite(ground), '这条用例必须先真的装上网格');

  p().onKeyDown(event({ code: 'KeyB', key: 'b' }));
  await turn(); await turn(); await turn();
  assert.equal(p().gameplayError.value, '', '摆放不能报错');
  const entry = saved.at(-1)?.[0];
  assert.ok(entry, '摆放必须先落盘');
  assert.equal(entry.propId, 'block');
  assert.ok(Math.abs(entry.position[0] - x) < 1e-9, '落点的 x 仍是玩家前方 2 米');
  assert.ok(Math.abs(entry.position[2] - z) < 1e-9, '落点的 z 仍是玩家前方 2 米');
  assert.ok(Math.abs(entry.position[1] - ground) < 1e-9,
    `存下来的 y 必须是落点的地面高度（期望 ${ground}，实际 ${entry.position[1]}）`);
  assert.ok(Math.abs(entry.position[1] - player.position[1]) > 0.5,
    '与「抄玩家自己的 y」必须差得可观测，否则这条断言是空转的');
});

// 没有网格时退回旧常量：方块仍然落在玩家所在的那条地面上（固定地板），一步都不许变。
test('without a grid a placed block keeps the old constant height', async t => {
  let layout = { version: 1, placements: [] };
  const saved = [];
  const state = { version: 2, revision: 0, rage: 0, boss: { hp: 0 }, drop: null,
    inventory: { worldFragment: 1, block: 4 } };
  const f = await fixture(t, {
    documents: { loadPlacements: layout },
    gameplay: async (request, activePath) => {
      if (request.action === 'savePlacements') {
        saved.push(structuredClone(request.placements));
        layout = { version: 1, placements: structuredClone(request.placements) };
        return { status: 'ok', role: 'child', state: structuredClone(layout) };
      }
      return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
        config: gameplayConfig, state: structuredClone(state) };
    },
  });
  let page = f.mount(); await page.mount(); await turn();
  clickWorld(page); await turn(); await turn();
  page = f.mount(); await page.mount(); await turn(); await turn();
  const p = () => page.instance;
  assert.equal(storyMapHeightModule.hasHeightGrid(), false, '这条用例必须没有网格');

  const player = p().camera.snapshotPlayer();
  p().onKeyDown(event({ code: 'KeyB', key: 'b' }));
  await turn(); await turn(); await turn();
  assert.equal(p().gameplayError.value, '', '摆放不能报错');
  const entry = saved.at(-1)?.[0];
  assert.ok(entry, '摆放必须先落盘');
  assert.ok(Math.abs(entry.position[1] - player.position[1]) < 1e-9,
    `没有网格时 y 必须还是旧的常量（期望 ${player.position[1]}，实际 ${entry.position[1]}）`);
});

// 掉落的红球也必须落在它自己那一格的地面上：这里走完整的主世界路径（走到龙面前打死它），
// 断言碎裂后碎片 actor 的落点就是 heightAt(落点)，而不是写死的 0 或玩家/龙的原点。
test('the dropped fragment rests on the grid, not on the constant floor', async t => {
  const { config: realConfig, state: realState } = readPythonGameplay(t);
  if (!realConfig) return;
  const boss = STORY_CHARACTERS.find(item => item.role === 'boss');
  const state = structuredClone(realState);
  const f = await fixture(t, {
    // z 也要铺够宽：龙在 z = 5.5，网格夹边会让断言量到边缘值而不是它脚下的地面。
    mapHeight: async () => storyMapHeightModule.setHeightGrid(rampGrid(160)),
    gameplay: async (request, activePath) => {
      if (request.action === 'hitBoss' || request.hit) {
        state.revision += 1;
        state.boss.hp = Math.max(0, state.boss.hp - realConfig.damage);
        if (state.boss.hp === 0) {
          state.drop = { id: 'story.boss.world-fragment', position: [...request.bossPosition], collected: false };
        }
      }
      return { status: 'ok', role: activePath === MAIN ? 'main' : 'child',
        config: realConfig, state: structuredClone(state) };
    },
  });
  const page = f.mount(); await page.mount(); await turn();
  const p = () => page.instance;
  assert.equal(p().roleRef.value, 'main');
  assert.ok(f.calls.some(call => call[0] === 'mapHeight'), '主世界也必须读入高度网格');

  let clock = 0;
  t.mock.method(performance, 'now', () => clock);
  p().onKeyDown(event({ code: 'KeyD', key: 'd' }));
  for (let i = 0; i < 200 && p().camera.snapshotPlayer().position[0] < boss.x - 0.1; i++) f.step(50);
  p().camera.keyUp(event({ code: 'KeyD', key: 'd' }));
  p().onKeyDown(event({ code: 'KeyW', key: 'w' }));
  for (let i = 0; i < 200 && p().camera.snapshotPlayer().position[2] < boss.z - 0.5; i++) f.step(50);
  p().camera.keyUp(event({ code: 'KeyW', key: 'w' }));
  const swings = Math.ceil(realConfig.bossHp / realConfig.damage);
  for (let i = 0; i < swings; i++) { clock += realConfig.cooldownMs; await p().gameplay.attack(); await turn(); }
  assert.equal(state.boss.hp, 0, '打满次数后 Boss 必须倒下');
  assert.ok(state.drop && state.drop.collected === false, 'Boss 死后必须留下未拾取的红球');

  const ground = storyMapHeightModule.heightAt(state.drop.position[0], state.drop.position[2]);
  assert.ok(Number.isFinite(ground), '这条用例必须先真的装上网格');
  assert.ok(Math.abs(state.drop.position[1] - ground) < 1e-9,
    `掉落的 y 必须是落点的地面高度（期望 ${ground}，实际 ${state.drop.position[1]}）`);
  assert.ok(Math.abs(state.drop.position[1]) > 0.5, '与写死的 0 必须差得可观测');

  // 碎片模型自己也吃这条地面：地板被抹平时红球会各沉一半进退。
  const fragment = f.actors.get(MAIN).find(actor => actor.actor_guid === gameplayModule.FRAGMENT_GUID);
  assert.ok(fragment, '主世界必须真的摆出碎片');
  const flat = characterTransform(gameplayModule.FRAGMENT, fragment.local_aabb).position[1];
  assert.ok(Math.abs(fragment.geometry.position[1] - (ground + flat)) < 1e-9,
    `红球必须站在网格给出的地面上：期望 ${ground + flat}，实际 ${fragment.geometry.position[1]}`);
});
