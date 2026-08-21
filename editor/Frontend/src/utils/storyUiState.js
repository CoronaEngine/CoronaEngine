export function isStoryEditableTarget(target) {
  if (!target || typeof target !== 'object') return false;
  const tagName = String(target.tagName || '').toUpperCase();
  return (
    tagName === 'INPUT' ||
    tagName === 'TEXTAREA' ||
    tagName === 'SELECT' ||
    Boolean(target.isContentEditable)
  );
}

export function storyShortcutFromEvent(event = {}) {
  if (event.repeat || event.ctrlKey || event.altKey || event.metaKey || event.shiftKey) return '';
  if (isStoryEditableTarget(event.target)) return '';
  const code = String(event.code || '');
  const key = String(event.key || '').toLowerCase();
  if (code === 'Escape' || key === 'escape') return 'escape';
  if (code === 'KeyB' || key === 'b') return 'inventory';
  if (code === 'KeyM' || key === 'm') return 'map';
  return '';
}

export function reduceStoryUiState(state = {}, shortcut = '') {
  const current = {
    ready: Boolean(state.ready),
    menuOpen: Boolean(state.menuOpen),
    inventoryOpen: Boolean(state.inventoryOpen),
    mapOpen: Boolean(state.mapOpen),
  };
  if (!current.ready) return current;

  if (shortcut === 'escape') {
    if (current.inventoryOpen) return { ...current, inventoryOpen: false };
    if (current.mapOpen) return { ...current, mapOpen: false };
    return { ...current, menuOpen: !current.menuOpen };
  }
  if (current.menuOpen) return current;
  if (shortcut === 'inventory') {
    return { ...current, inventoryOpen: !current.inventoryOpen, mapOpen: false };
  }
  if (shortcut === 'map') {
    return { ...current, inventoryOpen: false, mapOpen: !current.mapOpen };
  }
  return current;
}
