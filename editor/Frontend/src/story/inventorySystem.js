/**
 * 剧情模式背包数据服务：负责物品增删、查询和数量校验。
 */
import { storyModeStore } from './storyModeStore.js';

function isUniqueUgcItem(item) {
  return item?.category === 'ugc' && (item?.logic || item?.id === 'world-fragment-demo');
}

export const inventorySystem = {
  getItems() {
    return storyModeStore.items;
  },

  addItem(item) {
    const quantity = Math.max(1, item.quantity ?? 1);
    const existing = storyModeStore.items.find((value) => value.id === item.id);

    if (existing) {
      Object.assign(existing, item, { quantity: existing.quantity });
      if (!isUniqueUgcItem(item)) existing.quantity += quantity;
      return existing;
    }

    const nextItem = { quantity, ...item };
    storyModeStore.items.push(nextItem);
    return nextItem;
  },

  removeItem(id, quantity = 1) {
    if (!Number.isSafeInteger(quantity) || quantity < 1) return false;
    const item = storyModeStore.items.find((value) => value.id === id);
    if (!item || item.quantity < quantity) return false;

    item.quantity -= quantity;
    if (item.quantity === 0) storyModeStore.items.splice(storyModeStore.items.indexOf(item), 1);
    return true;
  },

  hasItem(id, quantity = 1) {
    return (storyModeStore.items.find((item) => item.id === id)?.quantity ?? 0) >= quantity;
  },
};
