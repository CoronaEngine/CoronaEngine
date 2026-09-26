import { editorUiAllowed } from '../services/worldModeService.js';
import { defineStore } from 'pinia';
import { PLUGIN_MANIFEST } from '@/config/pluginManifest.js';

function buildDefaultPanels() {
  const panels = {};
  let i = 0;
  for (const p of PLUGIN_MANIFEST) {
    panels[p.id] = {
      open: false,
      mode: 'docked', // 'docked' | 'external'
      dockZone: p.defaultDock,
      // Order within a zone. Drag-reorder/cross-zone moves rewrite this. Seeded from manifest
      // order (×100) so there is room to insert between two panels without renumbering.
      order: i++ * 100,
      width: p.defaultWidth,
      height: p.defaultHeight,
      externalTabId: null,
    };
  }
  return panels;
}

export const useDockStore = defineStore('dock', {
  state: () => ({
    panels: buildDefaultPanels(),
    leftWidth: 360,
    rightWidth: 400,
    bottomHeight: 320,
    // Transient: the panel id currently being dragged in the DOM dock (null when idle).
    // Drives drop-target highlighting in DockLayout; not persisted.
    draggingId: null,
    // Transient: the zone the pointer is currently over during a drag ('left'|'right'|
    // 'bottom'|null). Drives zone highlight in DockLayout; not persisted.
    dragOverZone: null,
  }),

  getters: {
    panelsByZone: (state) => (zone) => {
      return Object.entries(state.panels)
        .filter(([, p]) => p.open && p.mode === 'docked' && p.dockZone === zone)
        .map(([id, p]) => ({ id, ...p }))
        .sort((a, b) => a.order - b.order);
    },
    openViewPanels(state) {
      return Object.entries(state.panels)
        .filter(
          ([id, p]) =>
            p.open &&
            PLUGIN_MANIFEST.find((m) => m.id === id)?.pageType === 'view'
        )
        .map(([id]) => id);
    },
    openPluginPanels(state) {
      return Object.entries(state.panels)
        .filter(
          ([id, p]) =>
            p.open &&
            PLUGIN_MANIFEST.find((m) => m.id === id)?.pageType === 'plugin'
        )
        .map(([id]) => id);
    },
  },

  actions: {
    clearSession() {
      this.draggingId = null;
      this.dragOverZone = null;
      for (const panel of Object.values(this.panels)) {
        panel.open = false;
        panel.mode = 'docked';
        panel.externalTabId = null;
      }
    },
    openPanel(id) {
      if (!editorUiAllowed()) return;
      if (this.panels[id]) {
        this.panels[id].open = true;
        this.panels[id].mode = 'docked';
      }
    },

    closePanel(id) {
      if (this.panels[id]) {
        this.panels[id].open = false;
      }
    },

    togglePanel(id) {
      if (!editorUiAllowed()) return;
      if (!this.panels[id]) return;
      if (this.panels[id].open && this.panels[id].mode === 'docked') {
        this.closePanel(id);
      } else {
        this.openPanel(id);
      }
    },

    setDockZone(id, zone) {
      if (this.panels[id]) {
        this.panels[id].dockZone = zone;
      }
    },

    // Move a docked panel into `zone`, inserting it before `beforeId` (or at the end of the
    // zone when beforeId is null/not found). Reassigns contiguous order values within the
    // target zone so the visual order matches panelsByZone's sort. Used by drag-reorder /
    // cross-zone drag in DockLayout.
    movePanel(id, zone, beforeId = null) {
      if (!editorUiAllowed()) return;
      const moving = this.panels[id];
      if (!moving) return;

      moving.dockZone = zone;
      moving.mode = 'docked';
      moving.open = true;

      // Current docked panels in the target zone, excluding the moving one, in visual order.
      const ordered = Object.entries(this.panels)
        .filter(
          ([pid, p]) =>
            pid !== id && p.open && p.mode === 'docked' && p.dockZone === zone
        )
        .map(([pid, p]) => ({ pid, order: p.order ?? 0 }))
        .sort((a, b) => a.order - b.order)
        .map((e) => e.pid);

      // Insert the moving panel before beforeId, or append if not found.
      const insertAt = beforeId ? ordered.indexOf(beforeId) : -1;
      if (insertAt >= 0) {
        ordered.splice(insertAt, 0, id);
      } else {
        ordered.push(id);
      }

      // Reassign contiguous order values.
      ordered.forEach((pid, idx) => {
        if (this.panels[pid]) this.panels[pid].order = idx;
      });
    },

    resizePanel(id, width, height) {
      if (this.panels[id]) {
        if (width !== undefined) this.panels[id].width = width;
        if (height !== undefined) this.panels[id].height = height;
      }
    },

    setExternal(id, tabId) {
      if (!editorUiAllowed()) return;
      if (this.panels[id]) {
        this.panels[id].open = true;
        this.panels[id].mode = 'external';
        this.panels[id].externalTabId = tabId;
      }
    },

    markExternalClosed(id) {
      if (this.panels[id]) {
        this.panels[id].open = false;
        this.panels[id].mode = 'docked';
        this.panels[id].externalTabId = null;
      }
    },

    popIn(id) {
      if (!editorUiAllowed()) return;
      if (this.panels[id]) {
        this.panels[id].mode = 'docked';
        this.panels[id].externalTabId = null;
      }
    },

    // Transient drag state for the DOM dock (pointer-events drag in DockPanel/DockLayout).
    // Not persisted; drives drop-target highlighting. Set on pointerdown-drag-start, cleared
    // on drop/cancel.
    setDraggingId(id) {
      this.draggingId = id;
      if (!id) this.dragOverZone = null;
    },

    // Highlight the zone under the pointer during a drag (or null to clear).
    setDragOverZone(zone) {
      this.dragOverZone = zone;
    },

    initDefaultLayout() {
      if (!editorUiAllowed()) return;
      for (const p of PLUGIN_MANIFEST) {
        if (p.autoInit && this.panels[p.id]) {
          this.panels[p.id].open = true;
        }
      }
    },
  },
});
