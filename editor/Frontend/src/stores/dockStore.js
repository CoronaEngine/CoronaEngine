import { defineStore } from 'pinia';
import { PLUGIN_MANIFEST } from '@/config/pluginManifest.js';

function buildDefaultPanels() {
  const panels = {};
  for (const p of PLUGIN_MANIFEST) {
    panels[p.id] = {
      open: false,
      mode: 'docked', // 'docked' | 'external'
      dockZone: p.defaultDock,
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
  }),

  getters: {
    panelsByZone: (state) => (zone) => {
      return Object.entries(state.panels)
        .filter(([, p]) => p.open && p.mode === 'docked' && p.dockZone === zone)
        .map(([id, p]) => ({ id, ...p }));
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
    openPanel(id) {
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

    resizePanel(id, width, height) {
      if (this.panels[id]) {
        if (width !== undefined) this.panels[id].width = width;
        if (height !== undefined) this.panels[id].height = height;
      }
    },

    setExternal(id, tabId) {
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
      if (this.panels[id]) {
        this.panels[id].mode = 'docked';
        this.panels[id].externalTabId = null;
      }
    },

    initDefaultLayout() {
      for (const p of PLUGIN_MANIFEST) {
        if (p.autoInit && this.panels[p.id]) {
          this.panels[p.id].open = true;
        }
      }
    },
  },
});
