import { defineStore } from 'pinia';

import {
  STORY_INVENTORY_SLOT_COUNT,
  STORY_INVENTORY_VERSION,
  STORY_ITEM_CATALOG,
  addItemToInventory,
  getStoryItemDefinition,
  inventoryOccupiedSlotCount,
  inventoryTotalItemCount,
  normalizeInventoryDocument,
  removeInventorySlotQuantity,
  removeItemFromInventory,
  seedStoryInventory,
  storyInventoryStorageKey,
} from '@/utils/storyInventory.js';

let noticeSequence = 0;

function browserStorage() {
  try {
    return window.localStorage;
  } catch {
    return null;
  }
}

export const useStoryInventoryStore = defineStore('storyInventory', {
  state: () => ({
    projectPath: '',
    storageKey: '',
    initialized: false,
    loaded: false,
    slots: Array.from({ length: STORY_INVENTORY_SLOT_COUNT }, () => null),
    selectedIndex: 0,
    notice: null,
  }),

  getters: {
    selectedSlot(state) {
      return state.slots[state.selectedIndex] || null;
    },
    selectedItem() {
      return this.selectedSlot
        ? getStoryItemDefinition(this.selectedSlot.itemId, STORY_ITEM_CATALOG)
        : null;
    },
    occupiedSlots(state) {
      return inventoryOccupiedSlotCount(state.slots);
    },
    totalItems(state) {
      return inventoryTotalItemCount(state.slots);
    },
  },

  actions: {
    notify(message, kind = 'info') {
      this.notice = message
        ? { id: ++noticeSequence, message: String(message), kind: String(kind || 'info') }
        : null;
    },

    clearNotice() {
      this.notice = null;
    },

    persist(storage = browserStorage()) {
      if (!this.storageKey || !storage) return false;
      try {
        storage.setItem(
          this.storageKey,
          JSON.stringify({
            version: STORY_INVENTORY_VERSION,
            initialized: true,
            slots: this.slots,
            updatedAt: Date.now(),
          })
        );
        return true;
      } catch (error) {
        console.warn('[StoryMode] failed to persist inventory', error);
        this.notify('背包数据暂时无法保存。', 'warning');
        return false;
      }
    },

    resetForProject(projectPath, storage = browserStorage()) {
      this.projectPath = String(projectPath || '').trim();
      this.storageKey = storyInventoryStorageKey(this.projectPath);
      this.selectedIndex = 0;
      this.notice = null;

      let document = null;
      if (storage) {
        try {
          const serialized = storage.getItem(this.storageKey);
          if (serialized) document = JSON.parse(serialized);
        } catch (error) {
          console.warn('[StoryMode] failed to load inventory', error);
          this.notify('背包存档损坏，已恢复为初始状态。', 'warning');
        }
      }

      const normalized = normalizeInventoryDocument(document);
      if (normalized.initialized) {
        this.slots = normalized.slots;
        this.initialized = true;
      } else {
        this.slots = seedStoryInventory(STORY_ITEM_CATALOG);
        this.initialized = true;
        this.persist(storage);
      }
      this.loaded = true;
      return this.slots;
    },

    selectSlot(index) {
      const normalized = Math.trunc(Number(index));
      if (normalized < 0 || normalized >= this.slots.length) return false;
      this.selectedIndex = normalized;
      return true;
    },

    addItem(itemId, quantity = 1) {
      const result = addItemToInventory(this.slots, itemId, quantity, STORY_ITEM_CATALOG);
      this.slots = result.slots;
      if (result.added > 0) this.persist();
      if (result.remaining > 0) {
        this.notify(`背包空间不足，还有 ${result.remaining} 个道具无法放入。`, 'warning');
      } else if (result.added > 0) {
        const item = getStoryItemDefinition(itemId, STORY_ITEM_CATALOG);
        this.notify(`获得 ${item.name} ×${result.added}`, 'success');
      }
      return result;
    },

    enchantSelectedItem(componentType, componentId = `${componentType}-basic`) {
      const slot = this.selectedSlot;
      if (!slot || slot.itemId !== 'world_fragment') {
        this.notify('请选择一个普通世界碎片。', 'warning');
        return { success: false, reason: 'invalid-source' };
      }
      const allowed = new Set(['terrain', 'object', 'enemy', 'objective']);
      if (!allowed.has(String(componentType))) {
        this.notify('未知的附魔类型。', 'warning');
        return { success: false, reason: 'invalid-type' };
      }
      const result = removeInventorySlotQuantity(this.slots, this.selectedIndex, 1);
      this.slots = result.slots;
      const itemId = `enchanted_${componentType}_fragment`;
      const added = addItemToInventory(this.slots, itemId, 1, STORY_ITEM_CATALOG);
      this.slots = added.slots;
      if (added.remaining > 0) {
        // Keep the source fragment when the converted component cannot fit.
        const restored = addItemToInventory(this.slots, 'world_fragment', 1, STORY_ITEM_CATALOG);
        this.slots = restored.slots;
        this.notify('背包已满，无法保存附魔结果。', 'warning');
        return { success: false, reason: 'full' };
      }
      const componentSlot = this.slots.findIndex((candidate) => candidate?.itemId === itemId);
      if (componentSlot >= 0) {
        const current = this.slots[componentSlot];
        this.slots[componentSlot] = {
          ...current,
          metadata: {
            ...(current.metadata || {}),
            enchantment: { componentType, componentId, source: 'creator-npc', version: 1 },
          },
        };
      }
      this.persist();
      this.notify(`世界碎片已附魔为${componentType}组件。`, 'success');
      return { success: true, itemId, componentType, componentId };
    },

    removeItem(itemId, quantity = 1) {
      const result = removeItemFromInventory(this.slots, itemId, quantity);
      this.slots = result.slots;
      if (result.removed > 0) this.persist();
      return result;
    },

    useSelectedItem() {
      const slot = this.selectedSlot;
      if (!slot) {
        this.notify('请先选择一个道具。', 'warning');
        return { success: false, reason: 'empty' };
      }
      const item = getStoryItemDefinition(slot.itemId, STORY_ITEM_CATALOG);
      if (!item.usable) {
        this.notify(`${item.name} 当前不能直接使用。`, 'warning');
        return { success: false, reason: 'unusable' };
      }
      const result = removeInventorySlotQuantity(this.slots, this.selectedIndex, 1);
      this.slots = result.slots;
      this.persist();
      this.notify(item.useMessage || `已使用 ${item.name}。`, 'success');
      return { success: true, item, removed: result.removed };
    },

    dropSelectedItem() {
      const slot = this.selectedSlot;
      if (!slot) {
        this.notify('请先选择一个道具。', 'warning');
        return { success: false, reason: 'empty' };
      }
      const item = getStoryItemDefinition(slot.itemId, STORY_ITEM_CATALOG);
      const result = removeInventorySlotQuantity(this.slots, this.selectedIndex, 1);
      this.slots = result.slots;
      this.persist();
      this.notify(`已丢弃 ${item.name} ×1。`, 'info');
      return { success: true, item, removed: result.removed };
    },
  },
});
