import assert from 'node:assert/strict';
import { readFile, readdir } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const messageDir = path.join(root, 'src', 'i18n', 'messages');

function flattenKeys(value, prefix = '') {
  return Object.entries(value).flatMap(([key, child]) => {
    const next = prefix ? `${prefix}.${key}` : key;
    if (child && typeof child === 'object' && !Array.isArray(child)) {
      return flattenKeys(child, next);
    }
    return [next];
  });
}

const zh = (await import(pathToFileURL(path.join(messageDir, 'zh-CN.js')))).default;
const en = (await import(pathToFileURL(path.join(messageDir, 'en-US.js')))).default;
const { DOM_TEXT_TRANSLATIONS, setupDomTranslation, stopDomTranslation } = await import(
  pathToFileURL(path.join(root, 'src', 'i18n', 'domTranslator.js'))
);
assert.deepEqual(flattenKeys(en).sort(), flattenKeys(zh).sort(), 'Locale message keys differ');

async function assertDomTranslatorIsIdempotent() {
  const previousGlobals = {
    CustomEvent: globalThis.CustomEvent,
    MutationObserver: globalThis.MutationObserver,
    Node: globalThis.Node,
    document: globalThis.document,
    localStorage: globalThis.localStorage,
    window: globalThis.window,
  };

  const restoreGlobal = (key, value) => {
    if (value === undefined) {
      delete globalThis[key];
    } else {
      globalThis[key] = value;
    }
  };

  let activeObserver = null;
  let writeCount = 0;
  const listeners = new Map();
  const closeSource = Object.entries(DOM_TEXT_TRANSLATIONS).find(([, value]) => value === 'Close')?.[0];
  const cancelSource = Object.entries(DOM_TEXT_TRANSLATIONS).find(([, value]) => value === 'Cancel')?.[0];
  assert.ok(closeSource, 'DOM fallback must contain a Close translation');
  assert.ok(cancelSource, 'DOM fallback must contain a Cancel translation');

  class FakeText {
    constructor(value) {
      this.nodeType = 3;
      this._nodeValue = value;
    }

    get nodeValue() {
      return this._nodeValue;
    }

    set nodeValue(value) {
      if (this._nodeValue === value) return;
      this._nodeValue = value;
      writeCount += 1;
      activeObserver?.callback([{ type: 'characterData', target: this, addedNodes: [] }]);
    }
  }

  class FakeElement {
    constructor(tagName, childNodes = []) {
      this.nodeType = 1;
      this.tagName = tagName;
      this.childNodes = childNodes;
      this.attrs = new Map();
    }

    hasAttribute(name) {
      return this.attrs.has(name);
    }

    getAttribute(name) {
      return this.attrs.get(name) ?? null;
    }

    setAttribute(name, value) {
      if (this.getAttribute(name) === value) return;
      this.attrs.set(name, value);
      writeCount += 1;
      activeObserver?.callback([{ type: 'attributes', target: this, addedNodes: [] }]);
    }
  }

  class FakeMutationObserver {
    constructor(callback) {
      this.callback = callback;
    }

    observe() {
      activeObserver = this;
    }

    disconnect() {
      if (activeObserver === this) activeObserver = null;
    }
  }

  try {
    globalThis.Node = { TEXT_NODE: 3, ELEMENT_NODE: 1 };
    globalThis.MutationObserver = FakeMutationObserver;
    globalThis.localStorage = { getItem: () => 'en-US' };
    globalThis.document = {
      body: new FakeElement('BODY', [new FakeText(closeSource)]),
      documentElement: { lang: 'en-US', dataset: {} },
    };
    globalThis.window = {
      addEventListener: (event, handler) => listeners.set(event, handler),
      removeEventListener: (event, handler) => {
        if (listeners.get(event) === handler) listeners.delete(event);
      },
    };

    setupDomTranslation();

    const firstText = globalThis.document.body.childNodes[0];
    assert.equal(firstText.nodeValue, 'Close');
    const writesAfterSetup = writeCount;
    activeObserver.callback([{ type: 'characterData', target: firstText, addedNodes: [] }]);
    assert.equal(writeCount, writesAfterSetup, 'Translated text node was written repeatedly');

    const nextText = new FakeText(cancelSource);
    globalThis.document.body.childNodes.push(nextText);
    activeObserver.callback([{ type: 'childList', target: globalThis.document.body, addedNodes: [nextText] }]);
    assert.equal(nextText.nodeValue, 'Cancel');
    const writesAfterAdd = writeCount;
    activeObserver.callback([{ type: 'characterData', target: nextText, addedNodes: [] }]);
    assert.equal(writeCount, writesAfterAdd, 'Added translated text node was written repeatedly');

    listeners.get('corona-ui-locale-changed')?.({ detail: { locale: 'zh-CN' } });
    assert.equal(firstText.nodeValue, closeSource);
    assert.equal(nextText.nodeValue, cancelSource);
  } finally {
    stopDomTranslation();
    for (const [key, value] of Object.entries(previousGlobals)) {
      restoreGlobal(key, value);
    }
  }
}

await assertDomTranslatorIsIdempotent();

const ignoredFiles = new Set([
  path.join(root, 'src', 'i18n', 'messages', 'zh-CN.js'),
  path.join(root, 'src', 'i18n', 'messages', 'en-US.js'),
  path.join(root, 'src', 'i18n', 'domTranslator.js'),
]);

async function walk(dir) {
  const entries = await readdir(dir, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) files.push(...await walk(full));
    else if (/\.(vue|js|html)$/.test(entry.name)) files.push(full);
  }
  return files;
}

const sourceFiles = await walk(path.join(root, 'src'));
const offenders = [];
for (const file of sourceFiles) {
  if (ignoredFiles.has(file)) continue;
  const text = await readFile(file, 'utf8');
  const attrMatches = [];
  for (const match of text.matchAll(/(?:title|placeholder|aria-label)="([^"]*[\u4e00-\u9fff][^"]*)"/g)) {
    const previous = text[match.index - 1] || '';
    if (previous !== ':' && previous !== '@') attrMatches.push(match);
  }
  const matches = [
    ...attrMatches,
    ...text.matchAll(/>\s*([^<>{}]*[\u4e00-\u9fff][^<>{}]*)\s*</g),
  ];
  for (const match of matches) {
    const value = match[1].trim();
    if (value && !Object.hasOwn(DOM_TEXT_TRANSLATIONS, value)) {
      offenders.push(`${path.relative(root, file)} :: ${value}`);
    }
  }
}

assert.equal(offenders.length, 0, `Visible hardcoded Chinese remains:\n${offenders.join('\n')}`);
console.log('[OK] i18n message keys match and visible fallback strings are registered');
