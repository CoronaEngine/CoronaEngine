import assert from 'node:assert/strict';
import fs from 'node:fs';
import test from 'node:test';
import * as Vue from 'vue';
import { babelParse, compileScript, parse } from 'vue/compiler-sfc';

// Compile and render the production component with Vue's real renderer. The host
// records DOM operations without adding a browser/test-framework dependency.
const { descriptor } = parse(fs.readFileSync(new URL('../../src/components/ui/IkBoneSelect.vue', import.meta.url), 'utf8'));
const compiled = compileScript(descriptor, { id: 'ik-bone-select-test', genDefaultAs: 'BoneSelect', inlineTemplate: true });
let code = compiled.content;
const imports = babelParse(code, { sourceType: 'module' }).program.body.filter((node) => node.type === 'ImportDeclaration');
for (const node of [...imports].reverse()) {
  const bindings = node.specifiers.map((specifier) => `${specifier.imported.name}: ${specifier.local.name}`).join(', ');
  code = code.slice(0, node.start) + `const { ${bindings} } = Vue;` + code.slice(node.end);
}
const BoneSelect = new Function('Vue', `${code}; return BoneSelect;`)(Vue);

function mount(t, names, selected = '') {
  const previous = { document: globalThis.document, Document: globalThis.Document, ShadowRoot: globalThis.ShadowRoot };
  const documentListeners = new Map();
  class TestDocument {}
  const document = new TestDocument();
  document.addEventListener = (name, listener) => documentListeners.set(name, listener);
  document.removeEventListener = (name, listener) => {
    if (documentListeners.get(name) === listener) documentListeners.delete(name);
  };
  Object.assign(globalThis, { document, Document: TestDocument, ShadowRoot: class {} });
  const element = (type) => ({
    type, tagName: type.toUpperCase(), children: [], parent: null, props: {}, listeners: new Map(), scrollTop: 0,
    addEventListener(name, callback) { this.listeners.set(name, callback); },
    removeEventListener(name) { this.listeners.delete(name); },
    getRootNode: () => document,
    focus() { document.activeElement = this; },
    contains(target) { for (let node = target; node; node = node.parent) if (node === this) return true; return false; },
  });
  const renderer = Vue.createRenderer({
    createElement: element,
    createText: (text) => ({ ...element('#text'), text }),
    createComment: (text) => ({ ...element('#comment'), text }),
    setText: (node, text) => { node.text = text; },
    setElementText: (node, text) => { node.text = text; node.children = []; },
    patchProp: (node, key, _old, value) => { node.props[key] = value; if (key === 'value' || key === 'type') node[key] = value; },
    parentNode: (node) => node.parent,
    nextSibling: (node) => node.parent?.children[node.parent.children.indexOf(node) + 1] ?? null,
    insert(node, parent, anchor = null) {
      if (node.parent) node.parent.children.splice(node.parent.children.indexOf(node), 1);
      const index = anchor ? parent.children.indexOf(anchor) : -1;
      if (index < 0) parent.children.push(node); else parent.children.splice(index, 0, node);
      node.parent = parent;
    },
    remove(node) { if (node.parent) node.parent.children.splice(node.parent.children.indexOf(node), 1); node.parent = null; },
  });
  const value = Vue.ref(selected);
  const options = Vue.shallowRef(names);
  const changes = [];
  const root = element('root');
  const app = renderer.createApp({
    setup: () => () => Vue.h(BoneSelect, {
      modelValue: value.value, options: options.value,
      'onUpdate:modelValue': (next) => { value.value = next; },
      onChange: (next) => changes.push(next),
    }),
  });
  app.mount(root);
  const all = (predicate, node = root) => [ ...(predicate(node) ? [node] : []), ...node.children.flatMap((child) => all(predicate, child)) ];
  const role = (name) => all((node) => node.props.role === name);
  const button = () => all((node) => node.tagName === 'BUTTON')[0];
  let unmounted = false;
  const unmount = () => { if (!unmounted) app.unmount(); unmounted = true; };
  t.after(() => { unmount(); assert.equal(documentListeners.size, 0); Object.assign(globalThis, previous); });
  return {
    value, options, changes, role, button, documentListeners, unmount,
    async open() { button().props.onClick(); await Vue.nextTick(); },
    async key(key) {
      root.children[0].props.onKeydown({ key, preventDefault() {}, stopPropagation() {} });
      await Vue.nextTick();
    },
    async search(query) {
      const input = role('combobox')[0]; input.value = query; input.listeners.get('input')({ target: input });
      await Vue.nextTick();
    },
  };
}

test('large lists only mount a bounded viewport and scrolling to the end can select the final node', async (t) => {
  const names = Array.from({ length: 2000 }, (_, index) => `Bone_${index}`);
  const host = mount(t, names, 'Bone_0');
  assert.equal(host.role('option').length, 0);
  await host.open();
  assert.ok(host.role('option').length <= 11);
  const list = host.role('listbox')[0];
  let stopped = 0, prevented = 0;
  list.props.onWheel({ stopPropagation() { stopped++; }, preventDefault() { prevented++; } });
  assert.equal(stopped, 1);
  assert.equal(prevented, 0);
  list.scrollTop = (names.length - 7) * 30;
  list.props.onScroll({ target: list });
  await Vue.nextTick();
  assert.equal(host.value.value, 'Bone_0');
  assert.deepEqual(host.changes, []);
  assert.ok(host.role('option').length <= 11);
  const last = host.role('option').find((node) => node.text === 'Bone_1999');
  assert.ok(last);
  last.props.onClick();
  await Vue.nextTick();
  assert.equal(host.value.value, 'Bone_1999');
  assert.deepEqual(host.changes, ['Bone_1999']);
  assert.equal(host.role('option').length, 0);
  assert.equal(host.documentListeners.size, 0);
});

test('search covers the entire collection; keyboard movement only commits on Enter', async (t) => {
  const host = mount(t, Array.from({ length: 2000 }, (_, index) => `Bone_${index}`), 'Bone_0');
  await host.open();
  await host.search('BONE_199');
  assert.equal(host.value.value, 'Bone_0');
  assert.ok(host.role('option').some((node) => node.text === 'Bone_1999'));
  await host.key('ArrowDown');
  assert.deepEqual(host.changes, []);
  await host.key('Enter');
  assert.equal(host.value.value, 'Bone_1990');
  assert.deepEqual(host.changes, ['Bone_1990']);
});

test('invalid existing names are displayed without becoming options; mode changes and closing clean listeners', async (t) => {
  const host = mount(t, ['Toe_End'], 'NonLeafAnkle');
  await host.open();
  assert.deepEqual(host.role('option').map((node) => node.text), ['Toe_End']);
  await host.key('Escape');
  assert.equal(host.value.value, 'NonLeafAnkle');
  assert.equal(host.documentListeners.size, 0);
  await host.open();
  host.options.value = ['Ankle', 'Toe_End'];
  await Vue.nextTick();
  assert.equal(host.role('option').length, 0);
  assert.equal(host.value.value, 'NonLeafAnkle');
  await host.open();
  host.documentListeners.get('pointerdown')({ target: {} });
  await Vue.nextTick();
  assert.equal(host.documentListeners.size, 0);
  await host.open();
  host.unmount();
  assert.equal(host.documentListeners.size, 0);
  assert.deepEqual(host.changes, []);
});

test('empty search results and confirming the current node do not emit a change', async (t) => {
  const host = mount(t, ['Toe_End'], 'Toe_End');
  await host.open();
  await host.search('absent');
  assert.equal(host.role('option').length, 0);
  await host.key('Enter');
  assert.deepEqual(host.changes, []);
  await host.search('');
  await host.key('Enter');
  assert.equal(host.value.value, 'Toe_End');
  assert.deepEqual(host.changes, []);
});
