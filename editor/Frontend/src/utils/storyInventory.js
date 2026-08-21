export const STORY_INVENTORY_VERSION = 1;
export const STORY_INVENTORY_SLOT_COUNT = 24;
export const STORY_INVENTORY_STORAGE_PREFIX = 'corona.story.inventory.v1:';

export const STORY_ITEM_CATALOG = Object.freeze({
  bandage: Object.freeze({
    id: 'bandage',
    name: '绷带',
    description: '经过简单处理的医疗用品。当前版本使用后会消耗一个。',
    category: '消耗品',
    symbol: '✚',
    color: '#d98b78',
    stackLimit: 10,
    usable: true,
    useMessage: '已使用绷带。',
  }),
  old_key: Object.freeze({
    id: 'old_key',
    name: '旧钥匙',
    description: '一把有些生锈的钥匙，也许能打开某处的门。',
    category: '任务道具',
    symbol: '⚿',
    color: '#d8b86c',
    stackLimit: 1,
    usable: false,
  }),
  blue_crystal: Object.freeze({
    id: 'blue_crystal',
    name: '蓝晶矿',
    description: '散发微弱蓝光的矿石，可作为后续制作系统的材料。',
    category: '材料',
    symbol: '◆',
    color: '#71b9db',
    stackLimit: 99,
    usable: false,
  }),
});

export const STORY_INITIAL_ITEMS = Object.freeze([
  Object.freeze({ itemId: 'bandage', quantity: 3 }),
  Object.freeze({ itemId: 'old_key', quantity: 1 }),
  Object.freeze({ itemId: 'blue_crystal', quantity: 12 }),
]);

const cloneSlot = (slot) =>
  slot && typeof slot === 'object'
    ? { itemId: String(slot.itemId || ''), quantity: Math.max(Math.trunc(Number(slot.quantity) || 0), 0) }
    : null;

export function createEmptyInventorySlots(count = STORY_INVENTORY_SLOT_COUNT) {
  return Array.from({ length: Math.max(Math.trunc(Number(count) || 0), 0) }, () => null);
}

export function normalizeStoryProjectPath(projectPath) {
  const normalized = String(projectPath || '')
    .trim()
    .replace(/\\/g, '/')
    .replace(/\/+$/, '')
    .toLowerCase();
  return normalized || 'active-project';
}

export function storyInventoryStorageKey(projectPath) {
  return `${STORY_INVENTORY_STORAGE_PREFIX}${encodeURIComponent(normalizeStoryProjectPath(projectPath))}`;
}

export function getStoryItemDefinition(itemId, catalog = STORY_ITEM_CATALOG) {
  const normalizedId = String(itemId || '').trim();
  if (normalizedId && catalog?.[normalizedId]) return catalog[normalizedId];
  return {
    id: normalizedId || 'unknown',
    name: '未知道具',
    description: '当前版本无法识别这个道具，但它仍会保留在背包存档中。',
    category: '未知',
    symbol: '?',
    color: '#96928a',
    stackLimit: 99,
    usable: false,
    unknown: true,
  };
}

export function normalizeInventorySlots(slots, slotCount = STORY_INVENTORY_SLOT_COUNT) {
  const normalized = createEmptyInventorySlots(slotCount);
  if (!Array.isArray(slots)) return normalized;

  for (let index = 0; index < normalized.length; index += 1) {
    const slot = cloneSlot(slots[index]);
    if (!slot?.itemId || slot.quantity <= 0) continue;
    normalized[index] = slot;
  }
  return normalized;
}

export function normalizeInventoryDocument(document, slotCount = STORY_INVENTORY_SLOT_COUNT) {
  const source = document && typeof document === 'object' && !Array.isArray(document) ? document : {};
  return {
    version: STORY_INVENTORY_VERSION,
    initialized: Boolean(source.initialized),
    slots: normalizeInventorySlots(source.slots, slotCount),
    updatedAt: Number.isFinite(Number(source.updatedAt)) ? Number(source.updatedAt) : 0,
  };
}

export function addItemToInventory(
  sourceSlots,
  itemId,
  quantity,
  catalog = STORY_ITEM_CATALOG
) {
  const slots = normalizeInventorySlots(sourceSlots);
  const normalizedItemId = String(itemId || '').trim();
  let remaining = Math.max(Math.trunc(Number(quantity) || 0), 0);
  if (!normalizedItemId || remaining <= 0) return { slots, added: 0, remaining };

  const definition = getStoryItemDefinition(normalizedItemId, catalog);
  const stackLimit = Math.max(Math.trunc(Number(definition.stackLimit) || 1), 1);
  const requested = remaining;

  for (let index = 0; index < slots.length && remaining > 0; index += 1) {
    const slot = slots[index];
    if (!slot || slot.itemId !== normalizedItemId || slot.quantity >= stackLimit) continue;
    const amount = Math.min(stackLimit - slot.quantity, remaining);
    slots[index] = { ...slot, quantity: slot.quantity + amount };
    remaining -= amount;
  }

  for (let index = 0; index < slots.length && remaining > 0; index += 1) {
    if (slots[index]) continue;
    const amount = Math.min(stackLimit, remaining);
    slots[index] = { itemId: normalizedItemId, quantity: amount };
    remaining -= amount;
  }

  return { slots, added: requested - remaining, remaining };
}

export function removeItemFromInventory(sourceSlots, itemId, quantity) {
  const slots = normalizeInventorySlots(sourceSlots);
  const normalizedItemId = String(itemId || '').trim();
  let remaining = Math.max(Math.trunc(Number(quantity) || 0), 0);
  const requested = remaining;
  if (!normalizedItemId || remaining <= 0) return { slots, removed: 0, remaining };

  for (let index = slots.length - 1; index >= 0 && remaining > 0; index -= 1) {
    const slot = slots[index];
    if (!slot || slot.itemId !== normalizedItemId) continue;
    const amount = Math.min(slot.quantity, remaining);
    const nextQuantity = slot.quantity - amount;
    slots[index] = nextQuantity > 0 ? { ...slot, quantity: nextQuantity } : null;
    remaining -= amount;
  }

  return { slots, removed: requested - remaining, remaining };
}

export function removeInventorySlotQuantity(sourceSlots, slotIndex, quantity = 1) {
  const slots = normalizeInventorySlots(sourceSlots);
  const index = Math.trunc(Number(slotIndex));
  const amount = Math.max(Math.trunc(Number(quantity) || 0), 0);
  if (!Number.isInteger(index) || index < 0 || index >= slots.length || !slots[index] || amount <= 0) {
    return { slots, removed: 0, slot: null };
  }

  const current = slots[index];
  const removed = Math.min(current.quantity, amount);
  const nextQuantity = current.quantity - removed;
  slots[index] = nextQuantity > 0 ? { ...current, quantity: nextQuantity } : null;
  return { slots, removed, slot: current };
}

export function seedStoryInventory(catalog = STORY_ITEM_CATALOG) {
  let slots = createEmptyInventorySlots();
  for (const item of STORY_INITIAL_ITEMS) {
    slots = addItemToInventory(slots, item.itemId, item.quantity, catalog).slots;
  }
  return slots;
}

export function inventoryOccupiedSlotCount(slots) {
  return normalizeInventorySlots(slots).filter(Boolean).length;
}

export function inventoryTotalItemCount(slots) {
  return normalizeInventorySlots(slots).reduce((total, slot) => total + (slot?.quantity || 0), 0);
}
