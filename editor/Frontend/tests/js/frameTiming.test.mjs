import test from 'node:test';
import assert from 'node:assert/strict';
import { formatFrameTiming, startFrameTimingPolling } from '../../src/utils/frameTiming.js';

test('frame timing displays only fresh finite positive measurements in milliseconds', () => {
  assert.equal(formatFrameTiming({ render_ms: 24.9035, age_ms: 50 }), '24.90 ms');
  assert.equal(formatFrameTiming({status: 'success', data: {render_ms: 24.9035, age_ms: 50}}), '24.90 ms');
  for (const sample of [null, {}, {render_ms: 0}, {render_ms: -1}, {render_ms: NaN},
    {render_ms: Infinity}, {render_ms: '25'}, {render_ms: 25, age_ms: 2500}]) {
    assert.equal(formatFrameTiming(sample), '— ms');
  }
});

test('total frame interval is formatted independently from render time', () => {
  const sample = {data: {render_ms: 12.3, frame_ms: 33.3333, age_ms: 10}};
  assert.equal(formatFrameTiming(sample), '12.30 ms');
  assert.equal(formatFrameTiming(sample, 'frame_ms'), '33.33 ms');
  assert.equal(formatFrameTiming({render_ms: 12.3, frame_ms: 0, age_ms: 10}, 'frame_ms'), '— ms');
  assert.equal(formatFrameTiming({render_ms: 12.3, frame_ms: 33.3, age_ms: 2500}, 'frame_ms'), '— ms');
});

test('stopping or switching cameras discards an in-flight result', async () => {
  let resolve;
  const samples = [];
  let scheduled = 0;
  const stop = startFrameTimingPolling({
    read: () => new Promise((done) => { resolve = done; }),
    onSample: (sample) => samples.push(sample),
    schedule: () => { scheduled++; }, cancel: () => {},
  });
  stop();
  resolve({render_ms: 20, age_ms: 0});
  await new Promise(setImmediate);
  assert.deepEqual(samples, []);
  assert.equal(scheduled, 0);
});

test('polling never overlaps requests and recovers after errors', async () => {
  let resolve;
  let calls = 0;
  let next;
  const samples = [];
  const stop = startFrameTimingPolling({
    read: () => { calls++; return calls === 1 ? new Promise(done => {resolve = done;}) : Promise.reject(new Error('offline')); },
    onSample: sample => samples.push(sample),
    schedule: (callback, delay) => { assert.equal(delay, 250); next = callback; return 1; },
    cancel: () => {},
  });
  assert.equal(calls, 1);
  assert.equal(next, undefined);
  resolve({render_ms: 10, age_ms: 1});
  await new Promise(setImmediate);
  await next();
  assert.equal(calls, 2);
  assert.deepEqual(samples, [{render_ms: 10, age_ms: 1}, null]);
  stop();
});
