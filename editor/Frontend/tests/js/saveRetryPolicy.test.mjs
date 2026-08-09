import assert from 'node:assert/strict';
import test from 'node:test';
import { existsSync } from 'node:fs';

const policyUrl = new URL('../../src/blockly/utils/saveRetryPolicy.js', import.meta.url);

test('failed save remains dirty without scheduling an automatic retry', async () => {
  assert.equal(existsSync(policyUrl), true, 'save retry policy module must exist');
  const { nextSaveAction } = await import(policyUrl);
  assert.equal(nextSaveAction({ succeeded: false, saveQueued: true, graphDirty: true }), 'idle');
});

test('successful save drains edits queued while the request was in flight', async () => {
  assert.equal(existsSync(policyUrl), true, 'save retry policy module must exist');
  const { nextSaveAction } = await import(policyUrl);
  assert.equal(nextSaveAction({ succeeded: true, saveQueued: true, graphDirty: true }), 'resave');
  assert.equal(nextSaveAction({ succeeded: true, saveQueued: false, graphDirty: false }), 'idle');
});

test('project context event resumes one blocked dirty save for the same project', async () => {
  assert.equal(existsSync(policyUrl), true, 'save retry policy module must exist');
  const { shouldResumeBlockedSave } = await import(policyUrl);
  assert.equal(
    shouldResumeBlockedSave({
      dirty: true,
      blockedProjectPath: 'D:\\Projects\\World',
      eventProjectPath: 'd:/projects/world/',
    }),
    true
  );
  assert.equal(
    shouldResumeBlockedSave({
      dirty: true,
      blockedProjectPath: 'D:/Projects/Old',
      eventProjectPath: 'D:/Projects/New',
    }),
    false
  );
});
