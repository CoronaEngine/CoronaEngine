/** Story-only hotkeys and navigation ownership, independent of Vue and the native bridge. */
const unwrap = value => value?.data ?? value;
const normalizePath = value => String(value || '').replace(/\\/g, '/').replace(/\/+$/, '').toLowerCase();

export function storyNavigationKey(event) {
  if (event.defaultPrevented || event.repeat || event.isComposing || event.keyCode === 229
    || event.ctrlKey || event.altKey || event.metaKey) return '';
  const targets = event.composedPath?.() || [event.target];
  if (targets.some(target => target?.isContentEditable
    || target?.closest?.('input, textarea, select, [contenteditable]:not([contenteditable="false"]), [role="textbox"]'))) return '';
  if (event.code === 'KeyO' || event.code === 'KeyP') return event.code;
  return { o: 'KeyO', p: 'KeyP' }[String(event.key || '').toLowerCase()] || '';
}

export function createStoryNavigationController({
  projectPath, isReady, isSourceCurrent, getSelectionVersion, readSession,
  resetInput, flushCamera, prepare, trackPreparation, openProject, cancelProjectOpen,
  leave, notify,
}) {
  let busy = false, canceled = false, disposed = false, handedOff = false;
  let selection = getSelectionVersion(), attempt = 0;
  const ownsSelection = () => !canceled && selection === getSelectionVersion();
  const sourceCurrent = () => !disposed && ownsSelection() && isSourceCurrent();

  const ownsRequest = request => request === attempt && ownsSelection();
  const requestCurrent = request => request === attempt && sourceCurrent();

  async function open(path, request) {
    // Ownership transfers from the source component to the serialized launcher.
    // Its loading state intentionally unmounts that component; this is not Escape.
    handedOff = true;
    const pending = openProject(path);
    selection = getSelectionVersion();
    const result = unwrap(await pending);
    if (!ownsRequest(request) || result?.status === 'superseded') return false;
    if (result?.ok !== true) throw new Error(result?.message || '打开世界失败');
    const session = readSession();
    if (session.status !== 'ready' || session.mode !== 'story'
      || normalizePath(session.projectPath) !== normalizePath(path)) {
      throw new Error('目标世界未确认为剧情模式，已停止切换');
    }
    return true;
  }

  async function navigate(key) {
    const request = ++attempt;
    busy = true;
    selection = getSelectionVersion();
    resetInput();
    try {
      // Track only source-world work. Tracking open() itself would deadlock the
      // launcher, which drains source-world work before replacing the scene.
      const work = trackPreparation((async () => {
        await flushCamera();
        if (!requestCurrent(request)) return null;
        return prepare(key);
      })());
      const result = unwrap(await work);
      if (!requestCurrent(request)) return;
      if (result?.status === 'noop') return;
      if (result?.status !== 'ok') throw new Error(result?.message || '准备世界切换失败');
      const navigation = result.navigation;
      if (!navigation || navigation.mode !== 'story' || !navigation.target
        || normalizePath(navigation.source) !== normalizePath(projectPath)
        || normalizePath(navigation.target) === normalizePath(projectPath)
        || navigation.direction !== (key === 'KeyO' ? 'enter' : 'exit')) {
        throw new Error('世界导航信息与当前世界不匹配');
      }
      try {
        await open(navigation.target, request);
      } catch (error) {
        if (!ownsRequest(request)) return;
        try {
          if (!await open(projectPath, request)) return;
          if (ownsRequest(request)) notify(new Error(`切换失败，已恢复来源世界：${error.message}`));
        } catch (recoveryError) {
          if (!ownsRequest(request)) return;
          notify(new Error(`切换失败：${error.message}；恢复来源世界也失败：${recoveryError.message}。已返回启动页。`));
          await leave();
        }
      }
    } catch (error) {
      if (requestCurrent(request)) notify(error);
    } finally {
      busy = false;
    }
  }

  return {
    get busy() { return busy; },
    keyDown(event) {
      const key = storyNavigationKey(event);
      if (!key || !sourceCurrent() || !isReady()) return false;
      event.preventDefault?.();
      event.stopPropagation?.();
      if (busy) return true;
      return navigate(key);
    },
    // Fence pending source work before an async exit save. If saving fails,
    // this page stays usable, but the abandoned O/P request can never revive.
    interrupt() {
      ++attempt;
      resetInput();
    },
    cancel() {
      ++attempt;
      canceled = true;
      resetInput();
      cancelProjectOpen();
    },
    dispose() {
      disposed = true;
      // An unrelated open already changes launcher ownership; do not cancel it.
      if (!handedOff) canceled = true;
    },
  };
}
