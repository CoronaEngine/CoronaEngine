/**
 * 剧情模式背包数据服务：负责物品增删、查询和数量校验。
 */
import { storyModeStore } from './storyModeStore.js';

export const inventorySystem = {
  getItems() {
    return storyModeStore.items;
  },

  addItem(item) {
    const quantity = item.quantity ?? 1;
    const existing = storyModeStore.items.find((value) => value.id === item.id);
    if (existing) {
      existing.quantity += quantity;
      return existing;
    }

    const nextItem = { quantity: 1, ...item };
    storyModeStore.items.push(nextItem);
    return nextItem;
  },

  removeItem(id, quantity = 1) {
    const item = storyModeStore.items.find((value) => value.id === id);
    if (!item || item.quantity < quantity) return false;

    item.quantity -= quantity;
    return true;
  },

  hasItem(id, quantity = 1) {
    return (storyModeStore.items.find((item) => item.id === id)?.quantity ?? 0) >= quantity;
  },
};
