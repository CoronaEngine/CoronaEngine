import { createI18n } from 'vue-i18n';
import zhCN from './messages/zh-CN.js';
import enUS from './messages/en-US.js';

export const LOCALE_STORAGE_KEY = 'corona.ui.locale';
export const LOCALE_CHANGED_EVENT = 'corona-ui-locale-changed';
export const SUPPORTED_LOCALES = ['zh-CN', 'en-US'];
export const DEFAULT_LOCALE = 'zh-CN';

const messages = {
  'zh-CN': zhCN,
  'en-US': enUS,
};

function normalizeLocale(value) {
  const raw = String(value || '').trim();
  if (!raw) return '';
  const normalized = raw.replace('_', '-').toLowerCase();
  if (normalized === 'zh' || normalized === 'zh-cn' || normalized === 'zh-hans') return 'zh-CN';
  if (normalized === 'en' || normalized === 'en-us' || normalized === 'en-gb') return 'en-US';
  return '';
}

export function isSupportedLocale(locale) {
  return SUPPORTED_LOCALES.includes(locale);
}

export function resolveInitialLocale() {
  const stored = normalizeLocale(globalThis.localStorage?.getItem(LOCALE_STORAGE_KEY));
  if (stored) return stored;

  const preferred = [
    ...(Array.isArray(globalThis.navigator?.languages) ? globalThis.navigator.languages : []),
    globalThis.navigator?.language,
  ];
  for (const locale of preferred) {
    const normalized = normalizeLocale(locale);
    if (normalized) return normalized;
  }
  return DEFAULT_LOCALE;
}

export const i18n = createI18n({
  legacy: false,
  globalInjection: true,
  locale: resolveInitialLocale(),
  fallbackLocale: DEFAULT_LOCALE,
  missingWarn: true,
  fallbackWarn: true,
  messages,
});

function currentLocale() {
  const locale = i18n.global.locale;
  return typeof locale === 'string' ? locale : locale.value;
}

function applyLocaleToDocument(locale) {
  const root = globalThis.document?.documentElement;
  if (!root) return;
  root.lang = locale;
  root.dataset.locale = locale;
}

export function setLocale(locale, options = {}) {
  const nextLocale = normalizeLocale(locale) || DEFAULT_LOCALE;
  if (!isSupportedLocale(nextLocale)) return currentLocale();

  i18n.global.locale.value = nextLocale;
  applyLocaleToDocument(nextLocale);

  try {
    globalThis.localStorage?.setItem(LOCALE_STORAGE_KEY, nextLocale);
  } catch {}

  if (!options.silent) {
    globalThis.dispatchEvent?.(
      new CustomEvent(LOCALE_CHANGED_EVENT, { detail: { locale: nextLocale } })
    );
  }
  return nextLocale;
}

export function translate(key, params) {
  return i18n.global.t(key, params);
}

export function setupLocaleSync() {
  setLocale(currentLocale(), { silent: true });

  globalThis.addEventListener?.('storage', (event) => {
    if (event.key !== LOCALE_STORAGE_KEY) return;
    const nextLocale = normalizeLocale(event.newValue);
    if (nextLocale && nextLocale !== currentLocale()) {
      setLocale(nextLocale, { silent: true });
      globalThis.dispatchEvent?.(
        new CustomEvent(LOCALE_CHANGED_EVENT, { detail: { locale: nextLocale } })
      );
    }
  });
}

export function useLocale() {
  return {
    defaultLocale: DEFAULT_LOCALE,
    locales: SUPPORTED_LOCALES,
    setLocale,
    storageKey: LOCALE_STORAGE_KEY,
  };
}
