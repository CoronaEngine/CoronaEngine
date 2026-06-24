/**
 * MouseTracker — DOM context capture + action logging.
 *
 * Two roles:
 * 1. Track mouse position and current DOM context (snapshot)
 * 2. Accumulate user actions (clicks, keys, dwells) for stage-based AI analysis
 */

/* ── Landmark detectors (ordered: more specific first) ── */

function detectLandmark(el) {
  if (!el || el === document.body || el === document.documentElement) return null;

  const tag = el.tagName?.toLowerCase() || '';
  const cls = (typeof el.className === 'string' ? el.className : '').toLowerCase();
  const id = (el.id || '').toLowerCase();
  const text = (el.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 40);
  const aria = el.getAttribute?.('aria-label') || '';
  const title = el.title || '';
  const role = el.getAttribute?.('role') || '';
  const type = el.type || '';

  /* ── Blockly specific ── */
  if (cls.includes('blocklytoolbox')) return { label: '积木工具箱', area: 'blockly-toolbox' };
  if (cls.includes('blocklyflyout')) return { label: '积木选择面板', area: 'blockly-flyout' };
  if (cls.includes('blocklytree') && cls.includes('blocklytree-row')) {
    if (text) return { label: `积木分类「${text}」`, area: 'blockly-category' };
    return { label: '积木分类条目', area: 'blockly-category' };
  }
  if (cls.includes('blocklyblock')) {
    const blockText = text || '积木块';
    return { label: `积木「${blockText}」`, area: 'blockly-block' };
  }
  if (cls.includes('blocklyworkspace') || cls.includes('blocklymainbackground') || id === 'blockdiv') {
    return { label: '积木工作区', area: 'blockly-workspace' };
  }
  if (cls.includes('blocklytrash')) return { label: '积木回收站', area: 'blockly-trash' };
  if (cls.includes('blocklyzoom')) return { label: '积木缩放控件', area: 'blockly-zoom' };
  if (cls.includes('blocklyscroll')) return { label: '积木滚动条', area: 'blockly-scrollbar' };
  if (cls.includes('blocklytooltip')) return { label: '积木提示', area: 'blockly-tooltip' };
  if (id.includes('blockly')) return { label: '积木区域', area: 'blockly-area' };

  /* ── Menu bar ── */
  if (cls.includes('bg-[#2d2d2d]') && cls.includes('border-gray-700') && cls.includes('h-10')) {
    return { label: '顶部菜单栏', area: 'menubar' };
  }
  if (tag === 'button' && cls.includes('hover:bg-[#3d3d3d]') && cls.includes('rounded')) {
    if (text) return { label: `菜单「${text}」`, area: 'menu-button' };
    return { label: '菜单按钮', area: 'menu-button' };
  }
  if (tag === 'a' && cls.includes('hover:bg-[#3d3d3d]') && cls.includes('block')) {
    if (text) return { label: `菜单项「${text}」`, area: 'menu-item' };
    return { label: '菜单选项', area: 'menu-item' };
  }

  /* ── Camera speed slider ── */
  if (title === '摄像头移动速度（Shift+滚轮调节）' || (type === 'range' && cls.includes('accent-white'))) {
    return { label: '摄像头速度滑块', area: 'camera-speed' };
  }
  if (text === '速度' && tag === 'span') return { label: '速度标签', area: 'camera-speed-label' };

  /* ── Physics panel ── */
  if (text && (text.includes('重力') || text.includes('地面') || text.includes('弹性') || text.includes('步长'))) {
    return { label: `物理参数「${text}」`, area: 'physics-param' };
  }
  if (text === '应用物理参数') return { label: '应用物理参数按钮', area: 'physics-apply-btn' };

  /* ── Dialogs / modals ── */
  if (cls.includes('bg-black/50') && cls.includes('backdrop-blur')) {
    return { label: '弹窗遮罩', area: 'modal-overlay' };
  }
  if (cls.includes('bg-white') && cls.includes('rounded-lg') && cls.includes('shadow-xl')) {
    return { label: '弹窗面板', area: 'modal-panel' };
  }
  if (id === 'new-tab-name' || (type === 'text' && el.getAttribute('placeholder') === '输入场景名称')) {
    return { label: '场景名称输入框', area: 'modal-input' };
  }
  if (text === '创建场景' || text === '取消') {
    return { label: `弹窗按钮「${text}」`, area: 'modal-btn' };
  }

  /* ── Dock panels & sidebars ── */
  if (cls.includes('docktitlebar')) return { label: '面板标题栏', area: 'dock-titlebar' };
  if (id.includes('AITalkBar') || (cls.includes('aitalkbar'))) return { label: 'AI 对话面板', area: 'ai-talk' };
  if (id.includes('FileManager') || cls.includes('filemanager')) return { label: '文件管理器', area: 'file-manager' };
  if (id.includes('SceneBar') || cls.includes('scenebar')) return { label: '场景管理器', area: 'scene-manager' };
  if (id.includes('Object') || cls.includes('object-panel')) return { label: '对象属性面板', area: 'object-panel' };
  if (id.includes('LogView') || cls.includes('logview')) return { label: '日志查看器', area: 'log-view' };
  if (id.includes('ProjectSettings') || cls.includes('projectsettings')) return { label: '项目设置', area: 'project-settings' };
  if (id.includes('ProjectLauncher') || cls.includes('projectlauncher')) return { label: '项目启动器', area: 'project-launcher' };
  if (cls.includes('pet') || id.includes('Pet')) return { label: 'AI 助手白菜', area: 'cabbage' };

  /* ── Common UI elements (lower priority) ── */
  if (tag === 'button' || role === 'button') {
    if (text) return { label: `按钮「${text}」`, area: 'ui-button' };
    if (aria) return { label: `按钮「${aria}」`, area: 'ui-button' };
    return { label: '按钮', area: 'ui-button' };
  }
  if (tag === 'input' || tag === 'textarea') {
    const ph = el.getAttribute('placeholder') || '';
    if (ph) return { label: `输入框(${ph})`, area: 'ui-input' };
    if (type) return { label: `${type}输入框`, area: 'ui-input' };
    return { label: '输入框', area: 'ui-input' };
  }
  if (tag === 'select') return { label: '下拉选择器', area: 'ui-select' };
  if (type === 'range') return { label: '滑块', area: 'ui-slider' };
  if (type === 'checkbox') return { label: '复选框', area: 'ui-checkbox' };
  if (tag === 'a' && text) return { label: `链接「${text}」`, area: 'ui-link' };
  if (tag === 'img') return { label: '图片', area: 'ui-image' };
  if (tag === 'svg' || tag === 'path') return { label: '图标', area: 'ui-icon' };
  if (tag === 'label' && text) return { label: `标签「${text}」`, area: 'ui-label' };
  if (role === 'tab') return { label: '标签页', area: 'ui-tab' };
  if (role === 'menu') return { label: '菜单', area: 'ui-menu' };
  if (role === 'menuitem') return { label: '菜单项', area: 'ui-menuitem' };
  if (tag === 'hr') return { label: '分隔线', area: 'ui-separator' };

  if (text && tag !== 'div' && tag !== 'span' && tag !== 'section' && tag !== 'main') {
    return { label: `${tag}「${text}」`, area: `ui-${tag}` };
  }

  return null;
}

/* ── Build breadcrumb path ── */
function buildBreadcrumb(el) {
  const crumbs = [];
  let node = el;
  while (node && node !== document.body && node !== document.documentElement) {
    const lm = detectLandmark(node);
    if (lm) crumbs.push(lm);
    if (crumbs.length >= 8) break;
    node = node.parentElement;
  }
  return crumbs;
}

/* ── Context snapshot builder ── */
function buildSnapshot(el, dwellMs) {
  const breadcrumb = buildBreadcrumb(el);
  const primary = breadcrumb[0] || { label: '编辑器', area: 'general' };
  const path = breadcrumb.map(b => b.label);
  if (path.length === 0) path.push(el?.tagName?.toLowerCase() || '编辑器');

  const tag = el?.tagName?.toLowerCase() || '';
  const elCls = (typeof el?.className === 'string' ? el.className : '').split(' ').filter(Boolean).slice(0, 6).join(' ');
  const elText = (el?.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 60);
  const elAria = el?.getAttribute?.('aria-label') || '';
  const elTitle = el?.title || '';
  const elPlaceholder = el?.getAttribute?.('placeholder') || '';
  const elType = el?.type || '';

  const parent = el?.parentElement;
  const siblings = [];
  if (parent) {
    for (const sib of parent.children) {
      if (sib === el) continue;
      const t = (sib.textContent || '').replace(/\s+/g, ' ').trim().slice(0, 30);
      if (t) siblings.push(t);
      if (siblings.length >= 4) break;
    }
  }

  return {
    area: primary.area,
    areaLabel: primary.label,
    path: path.join(' → '),
    pathList: path,
    element: {
      tag,
      classSample: elCls,
      text: elText,
      ariaLabel: elAria,
      title: elTitle,
      placeholder: elPlaceholder,
      type: elType,
    },
    nearby: {
      siblings: siblings.filter(Boolean),
      parentTag: parent?.tagName?.toLowerCase() || '',
    },
    dwellMs,
  };
}

/* ── Quick landmark lookup (without full snapshot) ── */
function quickLandmark(el) {
  if (!el || el === document.body || el === document.documentElement) return null;
  const lm = detectLandmark(el);
  if (lm) return lm;
  // Walk up to find nearest landmark
  let node = el.parentElement;
  while (node && node !== document.body && node !== document.documentElement) {
    const lm2 = detectLandmark(node);
    if (lm2) return lm2;
    node = node.parentElement;
  }
  return null;
}

class MouseTracker {
  constructor() {
    this.position = { x: 0, y: 0 };
    this.snapshot = null;
    this.tracking = false;
    this.listeners = new Set();

    this.lastAreaKey = '';
    this.lastAreaLabel = '';
    this.areaSince = 0;

    // Visit history — areas the user moved through
    this.visitHistory = [];

    // Action log — what the user actually DID (clicks, keys)
    this.actionLog = [];

    // Bound handlers
    this._onMove = this._handleMove.bind(this);
    this._onOver = this._handleOver.bind(this);
    this._onClick = this._handleClick.bind(this);
    this._onKey = this._handleKey.bind(this);
    this._rafId = null;
    this._pendingPos = null;
    this._overDebounce = null;
  }

  start() {
    if (this.tracking) return;
    this.tracking = true;
    document.addEventListener('mousemove', this._onMove, { passive: true });
    document.addEventListener('mouseover', this._onOver, { passive: true });
    document.addEventListener('click', this._onClick, { passive: true });
    document.addEventListener('keydown', this._onKey, { passive: true });
  }

  stop() {
    if (!this.tracking) return;
    this.tracking = false;
    document.removeEventListener('mousemove', this._onMove);
    document.removeEventListener('mouseover', this._onOver);
    document.removeEventListener('click', this._onClick);
    document.removeEventListener('keydown', this._onKey);
    if (this._rafId) { cancelAnimationFrame(this._rafId); this._rafId = null; }
    if (this._overDebounce) { clearTimeout(this._overDebounce); this._overDebounce = null; }
  }

  /* ── Mouse ── */

  _handleMove(e) {
    this._pendingPos = { x: e.clientX, y: e.clientY };
    if (!this._rafId) {
      this._rafId = requestAnimationFrame(() => {
        this._rafId = null;
        if (this._pendingPos) { this.position = this._pendingPos; this._pendingPos = null; }
      });
    }
  }

  _handleOver(e) {
    if (this._overDebounce) return;
    const target = e.target;
    this._overDebounce = setTimeout(() => {
      this._overDebounce = null;
      this._updateSnapshot(target);
    }, 100);
  }

  _updateSnapshot(target) {
    if (!target) return;
    const snap = buildSnapshot(target, 0);
    const areaKey = snap.path;
    if (areaKey === this.lastAreaKey) return;

    // Record previous area's dwell
    if (this.lastAreaKey && this.areaSince) {
      const prevDwell = Date.now() - this.areaSince;
      if (prevDwell >= 3000) {
        this._logAction('dwell', this.lastAreaLabel, this.lastAreaKey, `${Math.round(prevDwell / 1000)}秒`);
      }
      this.visitHistory.push({
        areaKey: this.lastAreaKey,
        areaLabel: this.lastAreaLabel,
        dwellMs: prevDwell,
        timestamp: this.areaSince,
      });
      if (this.visitHistory.length > 50) this.visitHistory.shift();
    }

    this.lastAreaKey = areaKey;
    this.lastAreaLabel = snap.areaLabel;
    this.areaSince = Date.now();
    this.snapshot = snap;
    this._notify();
  }

  /* ── Click tracking ── */

  _handleClick(e) {
    const lm = quickLandmark(e.target);
    const label = lm ? lm.label : (e.target?.tagName?.toLowerCase() || '未知');
    const area = lm ? lm.area : 'general';
    this._logAction('click', label, area);
  }

  /* ── Keyboard tracking ── */

  _handleKey(e) {
    // Only log meaningful shortcuts, not every keystroke
    const key = e.key;
    const mods = [];
    if (e.ctrlKey || e.metaKey) mods.push('Ctrl');
    if (e.shiftKey) mods.push('Shift');
    if (e.altKey) mods.push('Alt');

    // Filter: only log shortcuts with modifiers, or special keys
    const specialKeys = ['Delete', 'Backspace', 'Escape', 'Enter', 'Tab', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight'];
    if (mods.length > 0 || specialKeys.includes(key)) {
      const combo = mods.length > 0 ? `${mods.join('+')}+${key}` : key;
      const area = this.snapshot?.area || 'general';
      const areaLabel = this.snapshot?.areaLabel || '编辑器';
      this._logAction('key', areaLabel, area, combo);
    }
  }

  /* ── Action log ── */

  _logAction(type, label, area, detail) {
    // Deduplicate: same type+label within 2 seconds → skip
    if (this.actionLog.length > 0) {
      const last = this.actionLog[this.actionLog.length - 1];
      if (last.type === type && last.label === label && (Date.now() - last.timestamp) < 2000) {
        return;
      }
    }
    this.actionLog.push({ type, label, area, detail: detail || '', timestamp: Date.now() });
    if (this.actionLog.length > 100) this.actionLog.shift();
  }

  getActionLog() {
    return [...this.actionLog];
  }

  resetActionLog() {
    this.actionLog = [];
  }

  /* ── Visit history ── */

  getVisitHistory() {
    const history = [...this.visitHistory];
    if (this.lastAreaKey && this.areaSince) {
      history.push({
        areaKey: this.lastAreaKey,
        areaLabel: this.lastAreaLabel,
        dwellMs: Date.now() - this.areaSince,
        timestamp: this.areaSince,
        current: true,
      });
    }
    return history;
  }

  /* ── Snapshot ── */

  currentSnapshot() {
    if (!this.snapshot) return null;
    return {
      position: { ...this.position },
      ...this.snapshot,
      visitHistory: this.getVisitHistory(),
      actionLog: this.getActionLog(),
    };
  }

  /* ── Listeners ── */
  on(cb) { this.listeners.add(cb); }
  off(cb) { this.listeners.delete(cb); }
  _notify() {
    const s = this.currentSnapshot();
    this.listeners.forEach(cb => { try { cb(s); } catch (e) { console.error('[MouseTracker]', e); } });
  }
}

export const mouseTracker = new MouseTracker();
