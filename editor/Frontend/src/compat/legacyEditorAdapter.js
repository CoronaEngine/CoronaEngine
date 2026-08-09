/**
 * Compatibility-only adapter for the pre-Vue editor host.
 *
 * New editor code must use editorApi and the manifest contracts. This module
 * deliberately contains the remaining raw CEF request format so the legacy
 * camera-follow panel does not become a second protocol implementation.
 */

export function legacyEditorQuery(
  moduleName,
  functionName,
  args = [],
  { onSuccess, onFailure } = {}
) {
  if (typeof window === 'undefined' || typeof window.cefQuery !== 'function') {
    onFailure?.('CEF compatibility bridge is unavailable');
    return false;
  }

  window.cefQuery({
    request: JSON.stringify({ module: moduleName, function: functionName, args }),
    persistent: false,
    onSuccess: onSuccess || (() => {}),
    onFailure: onFailure || (() => {}),
  });
  return true;
}

export function installLegacyEditorAdapter() {
  if (typeof window === 'undefined') return;
  window.__coronaLegacyEditorAdapter = { query: legacyEditorQuery };
}
