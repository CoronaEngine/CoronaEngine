import assert from 'node:assert/strict';
import test from 'node:test';

import { createStoryNavigationController } from '../../../game/frontend/storyNavigation.mjs';
import {
  SWITCH_ATTEMPTS,
  isTransientSwitchLock,
  runStorySwitchWithRetry,
  switchFailureMessage,
} from '../../src/utils/storyWorldSwitchRetry.js';

const MAIN = 'D:/main';
const CHILD = 'D:/main/.game/subworld';
// 用户看到的那一行：Python 侧 `story_navigation.py` 把 rename 的 OSError 拼进消息里。
const LOCK_FAILURE = {
  status: 'error',
  message:
    "剧情世界切换准备失败：[WinError 5] 拒绝访问。: 'D:\\main\\.game\\.subworld-staging-8n07rd4x\\world' -> 'D:\\main\\.game\\subworld'",
};
const prepared = (direction = 'enter') => ({
  status: 'ok',
  navigation: { source: MAIN, target: CHILD, direction, mode: 'story' },
});

/**
 * 与生产一致的装配：真实的导航控制器 + 真实的退避重试。`responds(attempt)` 决定第几次才成功，
 * 每次桥调用都记为一次「尝试」。`flushCamera` 代表准备之前必须先完成的那次来源世界写入。
 * `stop()` 模拟 `exitStory()`：把代数 +1 并中断导航，正在退避的重试必须据此停手。
 */
function fixture({ responds, wait } = {}) {
  let version = 0;
  let ready = true;
  let sourceCurrent = true;
  let switchGeneration = 0;
  let state = { status: 'ready', mode: 'story', projectPath: MAIN };
  const calls = [];
  const errors = [];
  const delays = [];
  let attempts = 0;
  let controller;

  const prepareOnce = async () => {
    attempts++;
    calls.push('prepare');
    return responds(attempts);
  };

  controller = createStoryNavigationController({
    projectPath: MAIN,
    isReady: () => ready,
    isSourceCurrent: () => sourceCurrent,
    getSelectionVersion: () => version,
    readSession: () => state,
    resetInput: () => calls.push('reset'),
    flushCamera: async () => { calls.push('flush'); },
    // 生产里就是这一层：`StoryWorld.vue` 把 `scratch.sendKeyEvent` 交给重试器。
    prepare: () => {
      const generation = switchGeneration;
      return runStorySwitchWithRetry(prepareOnce, {
        wait: async (ms) => { delays.push(ms); if (wait) await wait(ms); },
        aborted: () => !sourceCurrent || switchGeneration !== generation,
      });
    },
    trackPreparation: (promise) => promise,
    openProject: (path) => {
      version++;
      sourceCurrent = false;
      state = { status: 'loading', mode: null, projectPath: path };
      calls.push('open');
      controller.dispose();
      return (async () => {
        state = { status: 'ready', mode: 'story', projectPath: path };
        return { ok: true, path };
      })();
    },
    cancelProjectOpen: () => { version++; calls.push('cancel'); },
    leave: () => calls.push('leave'),
    notify: (error) => errors.push(error.message),
  });

  return {
    controller,
    calls,
    errors,
    delays,
    get attempts() { return attempts; },
    opens: () => calls.filter((call) => call === 'open').length,
    reset: () => { calls.length = 0; attempts = 0; },
    stop: () => { switchGeneration++; controller.interrupt(); },
  };
}

test('a lock failure on the first two attempts still switches on the third, in three attempts', async () => {
  const f = fixture({ responds: (attempt) => (attempt < 3 ? { ...LOCK_FAILURE } : prepared()) });

  await f.controller.navigate('enter');

  assert.equal(f.attempts, 3, '前两次被 WinError 5 挡住、第三次成功，因此恰好三次尝试');
  assert.deepEqual(f.delays, [150, 250], '两次退避都是短等待，不是立刻重打');
  assert.deepEqual(f.errors, [], '最终成功不得上报任何错误');
  assert.equal(f.opens(), 1, '只打开一次目标世界，不重复进入');
  assert.equal(f.controller.busy, false, '切换结束后必须解除忙碌态');
  assert.deepEqual(f.calls, ['reset', 'flush', 'prepare', 'prepare', 'prepare', 'open']);
});

test('a lock failure on every attempt surfaces the error once and leaves the view usable', async () => {
  let locked = true;
  const f = fixture({ responds: () => (locked ? { ...LOCK_FAILURE } : prepared()) });

  await f.controller.navigate('enter');

  assert.equal(f.attempts, SWITCH_ATTEMPTS, '用满重试次数后放弃');
  assert.deepEqual(f.delays, [150, 250, 400], '退避 3 次、尝试 4 次');
  assert.equal(f.errors.length, 1, '只在最后一次尝试之后上报一次');
  assert.match(f.errors[0], /剧情世界切换准备失败/, '上报的仍然是 Python 的原文');
  assert.match(f.errors[0], /WinError 5/, '原始系统错误必须保留，便于定位');
  assert.equal(f.opens(), 0, '准备失败不得进入目标世界');
  assert.equal(f.calls.includes('leave'), false, '准备失败不是切换失败，不该被踢回启动页');
  assert.equal(f.controller.busy, false, '失败后按钮必须恢复可用，不能卡在载入态');

  // 界面仍可用：同一个控制器再点一次，这次世界准备好，仍然只进入一次。
  locked = false;
  f.reset();
  await f.controller.navigate('enter');
  assert.equal(f.attempts, 1, '锁已消失，一次就够');
  assert.equal(f.opens(), 1, '重试失败不影响下一次正常切换');
  assert.equal(f.errors.length, 1, '后续成功不会追加错误');
});

test('the source-world write flush completes before the first attempt and is not repeated', async () => {
  const f = fixture({ responds: (attempt) => (attempt < 3 ? { ...LOCK_FAILURE } : prepared()) });

  await f.controller.navigate('enter');

  assert.equal(f.calls[0], 'reset', '先收输入');
  assert.equal(f.calls[1], 'flush', '等待 flushCamera（异步）落盘之后才发起准备');
  assert.equal(f.calls.filter((call) => call === 'flush').length, 1, '重试只重放准备，不重写相机位姿');
  assert.ok(f.calls.indexOf('flush') < f.calls.indexOf('prepare'), 'flush 严格早于第一次准备');
  assert.equal(f.calls.at(-1), 'open');
});

test('only file-lock failures are retried; a definitive refusal fails immediately', async () => {
  const f = fixture({ responds: () => ({ status: 'error', message: '击败 Boss 后才能开启小世界' }) });

  await f.controller.navigate('enter');

  assert.equal(f.attempts, 1, '业务性拒绝不重试，别让玩家白等');
  assert.deepEqual(f.errors, ['击败 Boss 后才能开启小世界']);
  assert.deepEqual(f.delays, []);
});

test('leaving the world during the backoff stops further attempts', async () => {
  let f;
  f = fixture({ responds: () => ({ ...LOCK_FAILURE }), wait: () => f.stop() });

  await f.controller.navigate('enter');

  assert.equal(f.attempts, 1, '退出世界之后不得再发起一次准备');
  assert.deepEqual(f.delays, [150], '只走了一次退避就停手');
  assert.equal(f.opens(), 0, '取消不会打开任何世界');
  assert.deepEqual(f.errors, [], '退出是用户的意图，不是错误');
  assert.equal(f.controller.busy, false);
});

test('the policy primitives keep their contract', () => {
  assert.equal(switchFailureMessage({ data: { data: { ...LOCK_FAILURE } } }), LOCK_FAILURE.message);
  assert.equal(switchFailureMessage(prepared()), null);
  assert.equal(switchFailureMessage({ status: 'noop' }), null);
  assert.equal(switchFailureMessage(null), null);
  assert.equal(isTransientSwitchLock(LOCK_FAILURE.message), true);
  assert.equal(isTransientSwitchLock('另一个程序正在使用此文件，进程无法访问。'), true);
  assert.equal(isTransientSwitchLock('仅支持可移植剧情世界，不能切换旧格式场景'), false);
  assert.equal(isTransientSwitchLock(''), false);
  assert.equal(SWITCH_ATTEMPTS, 4);
});

test('a thrown transient error is retried, a thrown definitive error is not', async () => {
  const transientSeen = [];
  let calls = 0;
  const result = await runStorySwitchWithRetry(
    () => { calls++; if (calls < 3) throw new Error('EPERM: operation not permitted, rename'); return prepared(); },
    { wait: async () => {}, onRetry: (info) => transientSeen.push(info.attempt) },
  );
  assert.equal(result.status, 'ok');
  assert.equal(calls, 3);
  assert.deepEqual(transientSeen, [1, 2]);

  let definitive = 0;
  await assert.rejects(
    runStorySwitchWithRetry(
      () => { definitive++; throw new Error('仅支持可移植剧情世界，不能切换旧格式场景'); },
      { wait: async () => {} },
    ),
    /可移植剧情世界/,
  );
  assert.equal(definitive, 1, '非锁类异常立即抛出');
});
