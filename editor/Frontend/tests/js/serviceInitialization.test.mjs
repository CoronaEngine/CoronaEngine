import assert from 'node:assert/strict';
import test from 'node:test';

import {
  createServiceResponseError,
  createServiceInitializationRetry,
} from '../../src/utils/serviceInitialization.js';

test('marks an initializing service response as retryable', () => {
  const error = createServiceResponseError(
    {
      success: false,
      status: 'initializing',
      message: 'AITool is initializing',
    },
    'AI service unavailable'
  );

  assert.equal(error.message, 'AITool is initializing');
  assert.equal(error.status, 'initializing');
  assert.equal(error.retryable, true);
});

test('initialization retry coalesces schedules and supports cancellation', () => {
  const scheduled = [];
  const cancelled = [];
  const retry = createServiceInitializationRetry({
    delayMs: 750,
    schedule: (callback, delay) => {
      const handle = { callback, delay };
      scheduled.push(handle);
      return handle;
    },
    cancel: (handle) => cancelled.push(handle),
  });

  retry.schedule(() => {});
  retry.schedule(() => {});

  assert.equal(scheduled.length, 2);
  assert.equal(scheduled[1].delay, 750);
  assert.deepEqual(cancelled, [scheduled[0]]);

  retry.cancel();
  assert.deepEqual(cancelled, [scheduled[0], scheduled[1]]);
});
