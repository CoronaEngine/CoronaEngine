/** Web-owned window sessions. Native Dock remains a mode-agnostic transport. */
export const EDITOR_UI_POLICY_KEY = 'corona.editorUi.policy.v1';
const WINDOW_PREFIX = 'corona.editorUi.window.v1:';
const uniqueId = () => `${Date.now()}_${Math.random().toString(36).slice(2)}`;
const unwrap = value => value?.data ?? value;

export function createEditorWindowSession({ storage, send, getWorld, getQuery, owner = uniqueId(),
  wait = ms => new Promise(resolve => setTimeout(resolve, ms)), timeoutMs = 30_000 }) {
  let queue = Promise.resolve();
  let retirement = null;
  let retirementRevision = null;
  let needsRetirement = true;
  let preparationSequence = 0;
  const read = key => JSON.parse(storage.getItem(key) || 'null');
  const write = (key, value) => storage.setItem(key, JSON.stringify(value));
  // Surface identity must survive route changes; a secondary page never becomes
  // the policy owner simply by navigating to a URL without standalone=1.
  const surfaceQuery = new URLSearchParams(getQuery());
  const query = () => surfaceQuery;
  const standalone = () => query().get('standalone') === '1';
  const sessionOwner = () => standalone() ? query().get('uiOwner') : owner;
  const records = () => {
    const result = [];
    for (let i = 0; i < storage.length; i++) {
      const key = storage.key(i);
      if (key?.startsWith(WINDOW_PREFIX + sessionOwner() + ':')) result.push({ key, ...read(key) });
    }
    return result;
  };
  const forget = record => {
    storage.removeItem(record.key);
    record.status = 'disposed';
  };
  const save = record => write(record.key, record);
  const worldAllowed = () => getWorld().status === 'ready' && getWorld().mode === 'creative';
  function policy() {
    const current = read(EDITOR_UI_POLICY_KEY);
    const record = standalone() ? ownRecord() : null;
    const valid = current?.enabled && (standalone()
      ? current.generation === query().get('uiSession') && current.owner === query().get('uiOwner')
        && record?.generation === current.generation && record.status !== 'closing'
      : current.owner === owner && current.revision === getWorld().revision);
    return { ...current, enabled: Boolean(valid) };
  }
  function requirePolicy() {
    const current = policy();
    if (!current.enabled || !worldAllowed()) throw new Error('当前世界不允许打开编辑器窗口');
    return current;
  }
  // The existing command acknowledges after TWO main-thread turns. With no scene
  // selected it closes nothing, but fences queued creates and SDL surface retirement.
  const fence = () => send({ cmd: 'suspendCameraViews', sceneId: '' });
  function taggedRoute(route, generation, id, sessionOwner) {
    return `${route || '/'}${String(route || '').includes('?') ? '&' : '?'}uiSession=${encodeURIComponent(generation)}&uiWindow=${encodeURIComponent(id)}&uiOwner=${encodeURIComponent(sessionOwner)}`;
  }
  const outstanding = record => ['pending', 'uncertain', 'settling'].includes(record.status);
  async function drain(predicate = () => true) {
    const started = Date.now();
    while (records().some(record => predicate(record) && outstanding(record))) {
      if (Date.now() - started >= timeoutMs) throw new Error('等待旧窗口请求结束超时，已取消切换世界，请重试。');
      await wait(20);
    }
  }
  async function dispose(record, notify = false) {
    if (record.kind === 'operation') {
      await fence();
    } else if (record.kind === 'camera') {
      await fence();
      await send({ cmd: 'suspendCameraViews', sceneId: record.sceneId });
    } else if (Number.isInteger(record.tabId)) {
      if (notify) {
        record.status = 'closing'; save(record);
        await drain(item => item.source === record.id);
      }
      await send({ cmd: 'closePanelTab', tabId: record.tabId, panelId: '' });
      if (notify) await send({ cmd: 'broadcast', event: 'panel-closed', payload: {
        panelId: record.panelId, tabId: record.tabId, uiGeneration: record.generation,
      } });
    }
    forget(record);
  }
  async function retire() {
    await drain();
    await fence();
    for (const record of records()) await dispose(record);
    await fence();
    needsRetirement = false;
  }
  function prepare(enabled) {
    if (standalone()) return Promise.reject(new Error('只有主窗口可以更改世界界面策略'));
    const preparation = ++preparationSequence;
    const revision = getWorld().revision;
    const previous = read(EDITOR_UI_POLICY_KEY);
    if (previous?.owner !== owner || previous?.revision !== revision) needsRetirement = true;
    if (!enabled || needsRetirement) {
      if (previous?.owner !== owner || previous.enabled || previous.revision !== revision) {
        write(EDITOR_UI_POLICY_KEY, { owner, revision, generation: uniqueId(), enabled: false });
      }
      needsRetirement = true;
      if (!enabled && retirement && retirementRevision === revision) return retirement;
    }
    const operation = queue.catch(() => {}).then(async () => {
      if (revision !== getWorld().revision) return policy();
      if (needsRetirement) await retire();
      if (enabled && preparation === preparationSequence && revision === getWorld().revision && worldAllowed()) {
        const current = policy();
        if (!current.enabled) write(EDITOR_UI_POLICY_KEY, {
          owner, revision, generation: uniqueId(), enabled: true,
        });
      }
      return policy();
    });
    queue = operation;
    if (!enabled) {
      retirementRevision = revision;
      retirement = operation;
      operation.then(() => { if (retirement === operation) retirement = null; },
        () => { if (retirement === operation) retirement = null; });
    }
    return operation;
  }
  async function open(command) {
    const current = requirePolicy();
    const id = uniqueId();
    const record = { key: `${WINDOW_PREFIX}${current.owner}:${id}`, id,
      generation: current.generation, source: query().get('uiWindow'), panelId: command.panelId || '', sceneId: command.sceneId,
      kind: command.cmd === 'createCameraView' ? 'camera'
        : command.cmd.startsWith('create') ? 'panel' : 'operation', status: 'pending' };
    save(record);
    const request = { ...command };
    if (record.kind !== 'operation') request.routePath = taggedRoute(command.routePath, current.generation, id, current.owner);
    const finish = async result => {
      record.tabId = unwrap(result)?.tab_id;
      record.status = 'settling';
      save(record);
      if (record.kind !== 'panel') await fence(); // queued=true is not completion
      if (record.kind === 'operation') {
        forget(record);
        if (!policy().enabled || policy().generation !== current.generation) throw new Error('世界已切换，已丢弃旧窗口请求');
        return;
      }
      if (record.kind === 'panel' && !Number.isInteger(record.tabId)) {
        record.status = 'uncertain'; save(record);
        throw new Error('窗口创建结果缺少 tab_id，已取消切换世界');
      }
      if (!policy().enabled || policy().generation !== current.generation || !worldAllowed()) {
        await dispose(record);
        throw new Error('世界已切换，已丢弃旧窗口请求');
      }
      record.status = 'ready';
      save(record);
    };
    try {
      if (requirePolicy().generation !== current.generation) throw new Error('世界已切换，已丢弃旧窗口请求');
      const result = await send(request, { onLateResult: async (error, result) => {
        if (error) { forget(record); return; }
        record.tabId = unwrap(result)?.tab_id;
        if (record.kind === 'panel' && !Number.isInteger(record.tabId)) return;
        record.status = 'settling'; save(record);
        try { await dispose(record); } // A timed-out request is never resurrected.
        catch (failure) { record.status = 'ready'; save(record); throw failure; }
      } });
      await finish(result);
      return result;
    } catch (error) {
      // Unknown native completion must keep the world switch blocked. The late
      // callback cleans it up; retries cannot silently bypass this outstanding work.
      if (error.code === 'DOCK_TIMEOUT' && record.status === 'pending') { record.status = 'uncertain'; save(record); }
      else if (record.status === 'pending') forget(record);
      else if (record.status === 'settling') { record.status = 'ready'; save(record); }
      throw error;
    }
  }
  function ownRecord() {
    const current = read(EDITOR_UI_POLICY_KEY);
    const id = query().get('uiWindow');
    const sourceOwner = query().get('uiOwner') || current?.owner;
    return id && sourceOwner ? read(`${WINDOW_PREFIX}${sourceOwner}:${id}`) : null;
  }
  async function closeThis(panelId) {
    if (!standalone()) throw new Error('主世界窗口不能作为面板关闭');
    let record = ownRecord();
    // Once retired, the owner closes this page only after its outstanding native
    // replies settle. Self-closing now would strand their registered callbacks.
    if (record && !policy().enabled && record.status !== 'closing') return;
    // Retired camera windows are already suspended by their owning main surface.
    if (query().get('camera') && !record) return;
    if (record) {
      await drain(item => item.id === record.id);
      record = ownRecord();
      if (!record) return; // The main surface already retired it.
      record.status = 'closing'; save(record);
      await drain(item => item.source === record.id);
      const current = read(EDITOR_UI_POLICY_KEY);
      if (!current?.enabled || current.generation !== record.generation) return;
    }
    // Broadcast before closing: the CEF context can disappear immediately after
    // closeThisTab, taking any later notification with it.
    if (record) await send({ cmd: 'broadcast', event: 'panel-closed', payload: {
      panelId, tabId: record.tabId, uiGeneration: record.generation,
    } });
    await send({ cmd: 'closeThisTab', panelId: '' });
    if (record) forget(record);
  }
  async function closeTab(tabId, panelId) {
    const record = records().find(item => item.tabId === tabId);
    if (record) return dispose({ ...record, panelId }, true);
    return send({ cmd: 'closePanelTab', tabId, panelId: '' });
  }
  async function broadcast(event, payload) {
    const current = requirePolicy();
    const record = ownRecord();
    return send({ cmd: 'broadcast', event, payload: {
      ...payload, uiGeneration: current.generation, ...(record?.tabId !== undefined ? { tabId: record.tabId } : {}),
    } });
  }
  function accepts(payload) {
    const current = policy();
    return worldAllowed() && current.enabled && payload?.uiGeneration === current.generation;
  }
  return { prepare, policy, open, closeThis, closeTab, broadcast, accepts };
}
