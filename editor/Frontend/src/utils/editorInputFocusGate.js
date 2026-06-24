const EDITABLE_SELECTOR = [
  'input',
  'textarea',
  'select',
  '[contenteditable=""]',
  '[contenteditable="true"]',
  '[role="textbox"]',
].join(',');

export function isEditableElement(target) {
  if (!target) return false;
  const tagName = String(target.tagName || target.nodeName || '').toUpperCase();
  if (tagName === 'INPUT' || tagName === 'TEXTAREA' || tagName === 'SELECT') return true;
  if (target.isContentEditable) return true;
  if (typeof target.closest === 'function') {
    return Boolean(target.closest(EDITABLE_SELECTOR));
  }
  return false;
}

export function isEditableEventTarget(event) {
  return isEditableElement(event?.target);
}

export function shouldDisableEditorCameraInput(documentRef = globalThis.document) {
  return isEditableElement(documentRef?.activeElement);
}

export function shouldHandleCameraFollowKey(event, { following = false, previewLocked = false } = {}) {
  if (previewLocked || !following) return false;
  return !isEditableEventTarget(event);
}
