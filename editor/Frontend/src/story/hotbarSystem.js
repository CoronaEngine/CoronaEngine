/**
 * 剧情模式快捷栏系统：提供固定 7 格快捷栏的查询、选择和同步接口。
 */
import { clearHotbar, hotbarStore, selectHotbarSlot, syncHotbarSlots } from './hotbarStore.js';

export const hotbarSystem = {
  /**
   * 获取固定长度的快捷栏槽位。
   * @returns {Array<object|null>} 7 个快捷栏槽位。
   */
  getSlots() {
    return hotbarStore.slots;
  },

  /**
   * 获取当前选中的快捷栏槽位。
   * @returns {number} 当前槽位索引。
   */
  getSelectedIndex() {
    return hotbarStore.selectedIndex;
  },

  /**
   * 选择快捷栏槽位。
   * @param {number} index 槽位索引。
   * @returns {boolean} 是否选择成功。
   */
  select(index) {
    return selectHotbarSlot(index);
  },

  /**
   * 将背包物品同步到快捷栏前 7 格。
   * @param {Array<object>} items 背包物品列表。
   */
  syncFromInventory(items) {
    syncHotbarSlots(items);
  },

  /**
   * 清空快捷栏。
   */
  clear() {
    clearHotbar();
  },
};
