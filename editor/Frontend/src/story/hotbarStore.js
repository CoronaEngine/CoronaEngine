/**
 * 剧情模式快捷栏状态：保存固定 7 格快捷栏的选中槽位和当前物品映射。
 */
import { reactive } from 'vue';

export const HOTBAR_SLOT_COUNT = 7;

export const hotbarStore = reactive({
  selectedIndex: 0,
  slots: Array.from({ length: HOTBAR_SLOT_COUNT }, () => null),
});

/**
 * 将选中槽位限制在固定快捷栏范围内。
 * @param {number} index 目标槽位索引。
 * @returns {number} 合法的槽位索引，非法输入返回 -1。
 */
export function normalizeHotbarIndex(index) {
  const value = Number(index);
  return Number.isInteger(value) && value >= 0 && value < HOTBAR_SLOT_COUNT ? value : -1;
}

/**
 * 选择快捷栏槽位。
 * @param {number} index 目标槽位索引。
 * @returns {boolean} 是否选择成功。
 */
export function selectHotbarSlot(index) {
  const normalizedIndex = normalizeHotbarIndex(index);
  if (normalizedIndex < 0) return false;

  hotbarStore.selectedIndex = normalizedIndex;
  return true;
}

/**
 * 使用背包物品重建快捷栏映射。
 * @param {Array<object>} items 背包物品列表。
 */
export function syncHotbarSlots(items = []) {
  hotbarStore.slots = Array.from({ length: HOTBAR_SLOT_COUNT }, (_, index) => {
    const item = Array.isArray(items) ? items[index] : null;
    return item && Number(item.quantity ?? 0) > 0 ? item : null;
  });

  if (!hotbarStore.slots[hotbarStore.selectedIndex]) {
    const firstAvailableIndex = hotbarStore.slots.findIndex(Boolean);
    hotbarStore.selectedIndex = firstAvailableIndex >= 0 ? firstAvailableIndex : 0;
  }
}

/**
 * 清空快捷栏并恢复默认选中槽位。
 */
export function clearHotbar() {
  hotbarStore.slots = Array.from({ length: HOTBAR_SLOT_COUNT }, () => null);
  hotbarStore.selectedIndex = 0;
}
