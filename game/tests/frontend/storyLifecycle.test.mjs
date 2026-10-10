import assert from 'node:assert/strict';
import test from 'node:test';
import { registerWorldSessionSave, flushWorldSessionSaves, trackWorldSessionWork, drainWorldSession, withWorldTimeout }
  from '../../../editor/Frontend/src/services/worldSessionLifecycle.js';
import { deferred } from './fixtures.mjs';

test('retired old-world save survives unmount and blocks replacement until confirmed', async () => {
  const order=[], gate=deferred();
  const registration=registerWorldSessionSave(async()=>{order.push('save'); await gate.promise; order.push('saved');});
  registration.retire();
  const open=(async()=>{await flushWorldSessionSaves(); order.push('open');})();
  await new Promise(r=>setImmediate(r)); assert.deepEqual(order,['save']); gate.resolve(); await open;
  assert.deepEqual(order,['save','saved','open']); await flushWorldSessionSaves(); assert.equal(order.length,3);
});
test('failed retired save blocks open and is retained for retry', async () => {
  let fail=true, writes=0;
  const registration=registerWorldSessionSave(async()=>{writes++; if(fail)throw new Error('disk failure');});
  registration.retire(); await assert.rejects(flushWorldSessionSaves(),/disk failure/);
  fail=false; await flushWorldSessionSaves(); assert.equal(writes,2); await flushWorldSessionSaves(); assert.equal(writes,2);
});
test('exit and launcher flushes cannot duplicate a pending native save', async () => {
  const gate=deferred(); let writes=0;
  const registration=registerWorldSessionSave(async()=>{writes++; await gate.promise;});
  const a=registration.flush(), b=flushWorldSessionSaves();
  await new Promise(r=>setImmediate(r)); assert.equal(writes,1); gate.resolve(); await Promise.all([a,b]); registration.release();
});
test('initialization drains before saving/replacing the world; late writes stay fenced after timeout', async () => {
  const gate=deferred(), order=[];
  trackWorldSessionWork(gate.promise.then(()=>order.push('initialization')));
  const registration=registerWorldSessionSave(async()=>order.push('save')); registration.retire();
  const open=(async()=>{await drainWorldSession(); await flushWorldSessionSaves(); order.push('open');})();
  await assert.rejects(withWorldTimeout(open,'test',1),/超时/); assert.deepEqual(order,[]);
  gate.resolve(); await open; assert.deepEqual(order,['initialization','save','open']);
});
