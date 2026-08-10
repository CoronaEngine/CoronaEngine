import { appService } from '@/services/appService.js';
import { getPluginManifest, PLUGIN_MANIFEST } from '@/config/pluginManifest.js';

export const FLOATING_PANEL_IDS = [
  'SceneTools',
  'Object',
  'NodeGraphPanel',
  'CabbageChatPanel',
];

const POSITION_WHITELIST = new Set(['right_top', 'right_bottom', 'left_bottom', 'center']);
const panelOperationQueues = new Map();
const panelDesiredStates = new Map();
const expectedCloseEvents = new Map();
const EXPECTED_CLOSE_EVENT_TTL_MS = 5000;

export function isFloatingPanel(panelId) {
  return FLOATING_PANEL_IDS.includes(panelId);
}

function normalizeFloatPosition(position) {
  return POSITION_WHITELIST.has(position) ? position : 'right_top';
}

function enqueuePanelOperation(panelId, operation) {
  const previous = panelOperationQueues.get(panelId) || Promise.resolve();
  const current = previous
    .catch(() => {})
    .then(operation);
  panelOperationQueues.set(panelId, current);
  const cleanup = () => {
    if (panelOperationQueues.get(panelId) === current) {
      panelOperationQueues.delete(panelId);
    }
  };
  current.then(cleanup, cleanup);
  return current;
}

function forgetExpectedClose(panelId, token) {
  const entries = expectedCloseEvents.get(panelId);
  if (!entries) return;
  const index = entries.findIndex((entry) => entry.token === token);
  if (index >= 0) {
    const [entry] = entries.splice(index, 1);
    window.clearTimeout(entry.timerId);
  }
  if (entries.length === 0) expectedCloseEvents.delete(panelId);
}

function expectPanelClosed(panelId) {
  const token = Symbol(panelId);
  const entries = expectedCloseEvents.get(panelId) || [];
  const entry = {
    token,
    expiresAt: Date.now() + EXPECTED_CLOSE_EVENT_TTL_MS,
    timerId: 0,
  };
  entry.timerId = window.setTimeout(() => {
    forgetExpectedClose(panelId, token);
  }, EXPECTED_CLOSE_EVENT_TTL_MS);
  entries.push(entry);
  expectedCloseEvents.set(panelId, entries);
  return token;
}

/**
 * Consume the native panel-closed event produced by a close request initiated here.
 * Such an event can arrive after a new tab has already opened, so MainPage must not
 * use it to close the newly-created tab in the Pinia store.
 */
export function consumeExpectedPanelClosed(panelId) {
  const entries = expectedCloseEvents.get(panelId);
  if (!entries?.length) return false;
  const now = Date.now();
  while (entries.length && entries[0].expiresAt < now) {
    const expired = entries.shift();
    window.clearTimeout(expired.timerId);
  }
  if (!entries.length) {
    expectedCloseEvents.delete(panelId);
    return false;
  }
  const entry = entries.shift();
  window.clearTimeout(entry.timerId);
  if (!entries.length) expectedCloseEvents.delete(panelId);
  return true;
}

export function floatingPanelManifests() {
  return PLUGIN_MANIFEST.filter(
    (panel) => panel.defaultOpenMode === 'external' && isFloatingPanel(panel.id)
  );
}

async function closeFloatingPanelNow(dockStore, panelId) {
  const panelState = dockStore?.panels?.[panelId];
  if (!panelState) {
    console.error('[panelWindows] Unknown panel:', panelId);
    return false;
  }

  if (panelState.mode !== 'external') {
    dockStore.closePanel(panelId);
    return true;
  }

  const tabId = panelState.externalTabId;
  if (!Number.isInteger(tabId)) {
    dockStore.markExternalClosed(panelId);
    return true;
  }

  const closeToken = expectPanelClosed(panelId);
  try {
    await appService.closePanelTab(tabId, panelId);
    // Native broadcasts panel-closed as well. The expected-event token prevents a
    // delayed broadcast from closing a newer tab opened by the next queued click.
    dockStore.markExternalClosed(panelId);
    return true;
  } catch (error) {
    forgetExpectedClose(panelId, closeToken);
    console.error(`[panelWindows] Failed to close floating panel ${panelId}:`, error);
    return false;
  }
}

async function openFloatingPanelNow(dockStore, panelId) {
  const manifest = getPluginManifest(panelId);
  if (!manifest || !dockStore?.panels?.[panelId]) {
    console.error('[panelWindows] Unknown panel:', panelId);
    return false;
  }

  const panelState = dockStore.panels[panelId];
  if (panelState.open && panelState.mode === 'external' && Number.isInteger(panelState.externalTabId)) {
    return true;
  }

  // A reused main CEF page can retain a stale tab id after its visible state was reset.
  // Close that native tab before creating another one so shortcut clicks never duplicate panels.
  if (panelState.mode === 'external' && Number.isInteger(panelState.externalTabId)) {
    const closeToken = expectPanelClosed(panelId);
    try {
      await appService.closePanelTab(panelState.externalTabId, panelId);
    } catch (error) {
      forgetExpectedClose(panelId, closeToken);
      console.warn(`[panelWindows] Failed to clear stale floating panel ${panelId}:`, error);
    }
    dockStore.markExternalClosed(panelId);
  }

  const routePath = `#${manifest.routePath || ''}`;
  const width = manifest.defaultFloatWidth || manifest.defaultWidth || 400;
  const height = manifest.defaultFloatHeight || manifest.defaultHeight || 600;
  const dockingPos = normalizeFloatPosition(manifest.defaultFloatPosition);
  const zPriority = Number.isFinite(Number(manifest.floatingPriority)) ? Number(manifest.floatingPriority) : 0;

  try {
    // Default floating panels use the single-surface in-main-window path (createPanelTab).
    // The title-bar control can still detach that floating tab into its own OS window later.
    const result = await appService.createPanelTab(panelId, routePath, width, height, dockingPos, zPriority);
    const tabId = result?.tab_id ?? result?.data?.tab_id;
    if (!Number.isInteger(tabId)) {
      throw new Error(`Invalid external tab id for ${panelId}`);
    }
    dockStore.setExternal(panelId, tabId);
    return true;
  } catch (error) {
    console.error(`[panelWindows] Failed to open floating panel ${panelId}:`, error);
    return false;
  }
}

function panelMatchesDesiredState(panelState, desiredOpen) {
  if (!panelState) return false;
  if (desiredOpen) {
    return panelState.open
      && panelState.mode === 'external'
      && Number.isInteger(panelState.externalTabId);
  }
  const hasNativeTab = panelState.mode === 'external' && Number.isInteger(panelState.externalTabId);
  return !panelState.open && !hasNativeTab;
}

function requestFloatingPanelState(dockStore, panelId, desiredOpen) {
  const panelState = dockStore?.panels?.[panelId];
  if (!panelState) {
    console.error('[panelWindows] Unknown panel:', panelId);
    return Promise.resolve(false);
  }

  const existing = panelDesiredStates.get(panelId);
  if (existing) {
    // Keep only the user's latest intent. Repeated open/close clicks while native CEF is
    // creating or destroying the heavy node editor no longer enqueue every intermediate
    // state and therefore cannot leave a stale tab id behind.
    existing.desiredOpen = Boolean(desiredOpen);
    return existing.promise;
  }

  const state = {
    desiredOpen: Boolean(desiredOpen),
    promise: null,
  };
  panelDesiredStates.set(panelId, state);
  state.promise = enqueuePanelOperation(panelId, async () => {
    let result = true;
    while (result) {
      const requestedState = state.desiredOpen;
      const current = dockStore?.panels?.[panelId];
      if (!current) return false;

      if (!panelMatchesDesiredState(current, requestedState)) {
        if (requestedState) {
          result = await openFloatingPanelNow(dockStore, panelId);
        } else if (current.mode === 'external') {
          result = await closeFloatingPanelNow(dockStore, panelId);
        } else {
          dockStore.closePanel(panelId);
        }
      }

      // A click may have changed the desired state while the native operation was in
      // flight. Reconcile once more, but skip all obsolete intermediate toggles.
      if (state.desiredOpen === requestedState) break;
    }
    return result;
  }).finally(() => {
    if (panelDesiredStates.get(panelId) === state) panelDesiredStates.delete(panelId);
  });
  return state.promise;
}

export function closeFloatingPanel(dockStore, panelId) {
  return requestFloatingPanelState(dockStore, panelId, false);
}

export function openFloatingPanel(dockStore, panelId) {
  return requestFloatingPanelState(dockStore, panelId, true);
}

export function toggleFloatingPanel(dockStore, panelId) {
  const panelState = dockStore?.panels?.[panelId];
  if (!panelState) {
    console.error('[panelWindows] Unknown panel:', panelId);
    return Promise.resolve(false);
  }
  const pendingState = panelDesiredStates.get(panelId);
  const currentlyDesiredOpen = pendingState
    ? pendingState.desiredOpen
    : Boolean(panelState.open);
  return requestFloatingPanelState(dockStore, panelId, !currentlyDesiredOpen);
}
