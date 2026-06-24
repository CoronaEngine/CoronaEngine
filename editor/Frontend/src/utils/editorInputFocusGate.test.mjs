import assert from 'node:assert/strict';

import {
  isEditableEventTarget,
  shouldDisableEditorCameraInput,
  shouldHandleCameraFollowKey,
} from './editorInputFocusGate.js';

const input = { tagName: 'INPUT', isContentEditable: false };
const textarea = { tagName: 'TEXTAREA', isContentEditable: false };
const button = { tagName: 'BUTTON', isContentEditable: false };
const nestedInTextbox = {
  tagName: 'SPAN',
  isContentEditable: false,
  closest(selector) {
    return selector.includes('[role="textbox"]') ? { role: 'textbox' } : null;
  },
};

assert.equal(isEditableEventTarget({ target: input }), true);
assert.equal(isEditableEventTarget({ target: textarea }), true);
assert.equal(isEditableEventTarget({ target: nestedInTextbox }), true);
assert.equal(isEditableEventTarget({ target: button }), false);

assert.equal(
  shouldHandleCameraFollowKey({ key: 'w', target: input }, { following: true, previewLocked: false }),
  false
);
assert.equal(
  shouldHandleCameraFollowKey({ key: 'w', target: button }, { following: true, previewLocked: false }),
  true
);
assert.equal(
  shouldHandleCameraFollowKey({ key: 'w', target: button }, { following: false, previewLocked: false }),
  false
);
assert.equal(
  shouldHandleCameraFollowKey({ key: 'w', target: button }, { following: true, previewLocked: true }),
  false
);

assert.equal(shouldDisableEditorCameraInput({ activeElement: input }), true);
assert.equal(shouldDisableEditorCameraInput({ activeElement: button }), false);

console.log('[OK] editor input focus gate protects editable controls from camera shortcuts');
