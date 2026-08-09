/**
 * Compatibility-only camera-follow panel for the pre-Vue host.
 *
 * The old host can load this before Vue has a stable actor context. Keep the
 * legacy protocol here until that host is migrated to the Vue camera-lock UI.
 */

(function installLegacyCameraLockPanel() {
  if (window.__camPanelLoaded) return;

  function isMainPage() {
    const hash = window.location.hash || '';
    return hash === '#/' || hash.indexOf('#/MainPage') === 0;
  }

  function tryInit() {
    if (window.__camPanelLoaded) return;
    if (!isMainPage()) return;
    if (!document.body) {
      setTimeout(tryInit, 30);
      return;
    }
    if (!window.__coronaLegacyEditorAdapter) {
      setTimeout(tryInit, 30);
      return;
    }
    window.__camPanelLoaded = true;
    init();
  }

  window.addEventListener('hashchange', tryInit);
  setTimeout(tryInit, 150);
  setTimeout(tryInit, 500);

  function init() {
    if (!document.body) {
      setTimeout(init, 30);
      return;
    }

    function isEnglish() {
      return (localStorage.getItem('corona.ui.locale') || document.documentElement.lang) === 'en-US';
    }

    function tr(zh, en) {
      return isEnglish() ? en : zh;
    }

    const dot = document.createElement('div');
    dot.id = '__cam_toggle_dot';
    dot.title = tr('相机跟随 - 按住拖拽，点击展开', 'Camera Follow - hold to drag, click to expand');
    dot.textContent = '●';
    document.body.appendChild(dot);

    const panel = document.createElement('div');
    panel.id = '__camlock_ctrl';
    panel.innerHTML =
      '<div style="font-weight:bold;margin-bottom:8px;color:#ec4899;">' + tr('相机跟随', 'Camera Follow') + '</div>' +
      '<label style="display:flex;align-items:center;gap:8px;margin-bottom:6px;">' +
      '<span>' + tr('启用', 'Enable') + '</span><input id="__camlock_checkbox" type="checkbox" style="accent-color:#ec4899;"></label>' +
      '<div style="margin-top:4px;font-size:10px;color:#909090;">' + tr('偏移', 'Offset') + ' ' +
      'X<input id="__camlock_ox" type="number" value="0" step="0.1" style="width:50px;background:#1a1a1a;color:#e0e0e0;border:1px solid #3c3c3c;border-radius:3px;margin:0 2px;padding:1px 3px;">' +
      'Y<input id="__camlock_oy" type="number" value="0" step="0.1" style="width:50px;background:#1a1a1a;color:#e0e0e0;border:1px solid #3c3c3c;border-radius:3px;margin:0 2px;padding:1px 3px;">' +
      'Z<input id="__camlock_oz" type="number" value="2" step="0.1" style="width:50px;background:#1a1a1a;color:#e0e0e0;border:1px solid #3c3c3c;border-radius:3px;margin:0 2px;padding:1px 3px;">' +
      '</div>' +
      '<button id="__camlock_apply" style="margin-top:8px;width:100%;padding:4px;background:#ec4899;color:white;border:none;border-radius:4px;cursor:pointer;font-size:11px;">' + tr('应用', 'Apply') + '</button>' +
      '<div id="__camlock_status" style="margin-top:6px;font-size:10px;color:#f0a020;min-height:14px;"></div>';
    document.body.appendChild(panel);

    let dragging = false;
    let startX;
    let startY;
    let startLeft;
    let startTop;
    let wasDragged = false;
    dot.addEventListener('mousedown', (event) => {
      if (event.button !== 0) return;
      wasDragged = false;
      dragging = true;
      startX = event.clientX;
      startY = event.clientY;
      const rect = dot.getBoundingClientRect();
      startLeft = rect.left;
      startTop = rect.top;
      dot.style.right = 'auto';
      dot.style.left = `${startLeft}px`;
      dot.style.top = `${startTop}px`;
      dot.style.animation = 'none';
      event.preventDefault();
    });
    window.addEventListener('mousemove', (event) => {
      if (!dragging) return;
      if (Math.abs(event.clientX - startX) > 2 || Math.abs(event.clientY - startY) > 2) {
        wasDragged = true;
      }
      dot.style.left = `${startLeft + event.clientX - startX}px`;
      dot.style.top = `${startTop + event.clientY - startY}px`;
    });
    window.addEventListener('mouseup', () => {
      if (!dragging) return;
      dragging = false;
      dot.style.cursor = 'grab';
      dot.style.animation = '__camPulse 1.5s ease-in-out infinite';
      const rect = dot.getBoundingClientRect();
      panel.style.right = 'auto';
      panel.style.left = `${Math.max(0, rect.left - 220 + 24)}px`;
      panel.style.top = `${rect.bottom + 6}px`;
    });

    dot.addEventListener('mouseup', () => {
      if (wasDragged) return;
      const current = panel.style.display;
      if (current === 'none' || current === '') {
        const rect = dot.getBoundingClientRect();
        panel.style.right = 'auto';
        panel.style.left = `${Math.max(0, rect.left - 220 + 24)}px`;
        panel.style.top = `${rect.bottom + 6}px`;
        panel.style.display = 'block';
      } else {
        panel.style.display = 'none';
      }
    });

    let following = false;
    function isGamePreviewInputLocked() {
      return !!window.__coronaGamePreviewInputLocked;
    }

    function readOffsets() {
      return [
        parseFloat(document.getElementById('__camlock_ox').value) || 0,
        parseFloat(document.getElementById('__camlock_oy').value) || 0,
        parseFloat(document.getElementById('__camlock_oz').value) || 2,
      ];
    }

    function setFollowingUI(active) {
      const status = document.getElementById('__camlock_status');
      if (active) {
        dot.style.background = '#4caf50';
        status.style.color = '#4caf50';
        status.textContent = tr('跟随中', 'Following');
      } else {
        dot.style.background = '#ec4899';
        status.style.color = '';
        status.textContent = tr('已关闭', 'Closed');
      }
    }

    function applyLock() {
      const enabled = document.getElementById('__camlock_checkbox').checked;
      const offset = readOffsets();
      const status = document.getElementById('__camlock_status');
      status.style.color = '#f0a020';
      status.textContent = enabled ? tr('正在设置...', 'Setting...') : '';
      const adapter = window.__coronaLegacyEditorAdapter;
      if (!adapter || typeof adapter.query !== 'function') {
        status.textContent = tr('兼容桥不可用', 'Compatibility bridge unavailable');
        return;
      }
      adapter.query('CoronaEditor', 'camera_lock_set', [enabled, ...offset, 0, 0, 0], {
        onSuccess: (raw) => {
          try {
            const response = JSON.parse(raw);
            if (!response.success) return;
            const data = response.data;
            if (data && data.ok) {
              following = enabled;
              if (enabled && data.offset) {
                document.getElementById('__camlock_ox').value = data.offset[0].toFixed(1);
                document.getElementById('__camlock_oy').value = data.offset[1].toFixed(1);
                document.getElementById('__camlock_oz').value = data.offset[2].toFixed(1);
              }
              setFollowingUI(enabled);
            } else {
              document.getElementById('__camlock_checkbox').checked = false;
              following = false;
              setFollowingUI(false);
              status.style.color = '#f44336';
              status.textContent = (data && data.error) || tr('失败', 'Failed');
            }
          } catch {
            status.textContent = '';
          }
        },
        onFailure: () => {
          document.getElementById('__camlock_checkbox').checked = false;
          following = false;
          setFollowingUI(false);
          status.style.color = '#f44336';
          status.textContent = tr('通信失败', 'Communication failed');
        },
      });
    }

    function updateOffset() {
      if (!following) return;
      const offset = readOffsets();
      const status = document.getElementById('__camlock_status');
      status.style.color = '#f0a020';
      status.textContent = tr('更新偏移...', 'Updating offset...');
      const adapter = window.__coronaLegacyEditorAdapter;
      if (!adapter || typeof adapter.query !== 'function') {
        status.textContent = tr('兼容桥不可用', 'Compatibility bridge unavailable');
        return;
      }
      adapter.query('CoronaEditor', 'camera_lock_set', [true, ...offset, 0, 0, 0], {
        onSuccess: () => {
          status.style.color = '#4caf50';
          status.textContent = tr('跟随中', 'Following');
        },
        onFailure: () => {
          status.style.color = '#f44336';
          status.textContent = tr('更新失败', 'Update failed');
        },
      });
    }

    const wasdKeys = { w: 1, a: 1, s: 1, d: 1 };
    document.addEventListener('keydown', (event) => {
      if (isGamePreviewInputLocked()) return;
      const key = event.key.toLowerCase();
      if (key === 'escape' && following) {
        event.preventDefault();
        event.stopImmediatePropagation();
        document.getElementById('__camlock_checkbox').checked = false;
        applyLock();
        return;
      }
      if (wasdKeys[key] && following) {
        event.preventDefault();
        event.stopImmediatePropagation();
        const adapter = window.__coronaLegacyEditorAdapter;
        adapter?.query?.('CoronaEditor', 'object_key_down', [key], {
          onSuccess: () => {},
          onFailure: () => {},
        });
      }
    }, true);
    document.addEventListener('keyup', (event) => {
      if (isGamePreviewInputLocked()) return;
      const key = event.key.toLowerCase();
      if (wasdKeys[key] && following) {
        event.preventDefault();
        event.stopImmediatePropagation();
        const adapter = window.__coronaLegacyEditorAdapter;
        adapter?.query?.('CoronaEditor', 'object_key_up', [key], {
          onSuccess: () => {},
          onFailure: () => {},
        });
      }
    }, true);

    document.getElementById('__camlock_checkbox').onchange = applyLock;
    document.getElementById('__camlock_apply').onclick = updateOffset;
    window.__camPanelLoaded = true;
  }
})();
