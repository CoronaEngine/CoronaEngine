/**
 * 世界切换（主世界 ↔ 小世界）准备阶段的瞬时锁重试。
 *
 * 「准备」这一步会把 `.game/.subworld-staging-*` 里的副本 rename 成正式的子世界目录。
 * Windows 上只要源树里还有任何一个文件被别的进程（包括编辑器自己打开的场景、资源、
 * 缩略图）持有，rename 就会以 `WinError 5 拒绝访问` 失败。失败时运行时已经按 fail-closed
 * 回滚（删掉父侧 `.game/story-link.json` 与整个暂存目录），所以磁盘上不会留下残留，
 * 重放同一次准备是安全的——唯一要避开的是「锁只持续几十毫秒」这种情况被当成致命错误。
 *
 * 本模块只描述策略，不碰 UI，也不改桥协议：`prepare` 的入参、返回值形状原样透传，
 * 调用方（StoryWorld.vue）在最后一次尝试之后才把它变成用户可见的错误。
 */

/** 最多尝试 4 次：首次 + 3 次重试。 */
export const SWITCH_ATTEMPTS = 4;
/** 两次尝试之间的退避（毫秒）；第 4 次起沿用最后一个值。 */
export const SWITCH_DELAYS_MS = [150, 250, 400];

// 只认「文件被占用」这一类可重试失败。其它错误（未击败 Boss、世界不匹配、格式不支持）
// 重试没有意义，必须原样立刻上报。
const TRANSIENT_LOCK = /WinError\s*5\b|拒绝访问|Access is denied|Access denied|being used by another process|另一个程序正在使用此文件|正在使用此文件|resource busy|EBUSY|EPERM/i;

const unwrap = (value) => {
  let result = value;
  for (let depth = 0; depth < 3; depth++) {
    if (!result || typeof result !== 'object' || !('data' in result)) break;
    result = result.data;
  }
  return result;
};

/** 把桥返回的失败结果翻译成消息；不是 `status: 'error'` 时返回 null（交给调用方裁决）。 */
export function switchFailureMessage(result) {
  const data = unwrap(result);
  if (!data || typeof data !== 'object' || data.status !== 'error') return null;
  return String(data.message || '准备世界切换失败');
}

/** 这条消息是不是「文件被占用」这类瞬时锁失败。 */
export function isTransientSwitchLock(message) {
  return typeof message === 'string' && message !== '' && TRANSIENT_LOCK.test(message);
}

/**
 * 依次尝试 `attempt(index)`（`index` 从 0 开始，异步）。
 *
 * - 结果/异常是瞬时锁失败且还有次数：退避后重试，不惊动 UI；
 * - 其它失败、或已经用尽次数：原样返回最后一次结果 / 抛出最后一次异常，由调用方上报；
 * - `aborted()` 在两次尝试之间为真（组件已卸载、世界已被替换、用户已退出）：立即停下并
 *   返回上一次结果，绝不在取消之后又发起一次文件系统操作。
 *
 * `wait` 与 `delays` 可注入，测试里不用真的等。
 */
export async function runStorySwitchWithRetry(attempt, {
  attempts = SWITCH_ATTEMPTS,
  delays = SWITCH_DELAYS_MS,
  wait = (ms) => new Promise((resolve) => { globalThis.setTimeout(resolve, ms); }),
  aborted = () => false,
  onRetry = null,
} = {}) {
  const limit = Math.max(1, Math.floor(Number(attempts)) || 1);
  const backoff = Array.isArray(delays) && delays.length ? delays : [0];
  let result;

  for (let index = 0; index < limit; index++) {
    if (index > 0) {
      if (aborted()) return result;
      await wait(backoff[Math.min(index - 1, backoff.length - 1)] ?? 0);
      if (aborted()) return result;
    }
    try {
      result = await attempt(index);
    } catch (error) {
      if (index === limit - 1 || !isTransientSwitchLock(error?.message)) throw error;
      if (onRetry) onRetry({ attempt: index + 1, attempts: limit, message: error.message });
      continue;
    }
    const failure = switchFailureMessage(result);
    if (!failure || index === limit - 1 || !isTransientSwitchLock(failure)) return result;
    if (onRetry) onRetry({ attempt: index + 1, attempts: limit, message: failure });
  }

  return result;
}
