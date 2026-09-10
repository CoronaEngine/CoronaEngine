/** 装备领域服务：未穿戴实例归背包所有，所有转移均为原子操作，不额外生成物品。 */
export const EQUIPMENT_SLOTS = Object.freeze({
  head: '头盔',
  chest: '胸甲',
  legs: '裤子',
  feet: '鞋子',
  mainHand: '主手',
});
export const EMPTY_ATTACK = Object.freeze({ damage: 10, cooldown: 350, maxDistance: 3 });
export function emptyEquipment() {
  return Object.fromEntries(Object.keys(EQUIPMENT_SLOTS).map((key) => [key, null]));
}
export function starterEquipment() {
  return [
    ['helmet', '旅行头盔', 'head', 2],
    ['cuirass', '旅行胸甲', 'chest', 6],
    ['trousers', '旅行裤子', 'legs', 5],
    ['boots', '旅行鞋子', 'feet', 2],
    ['blade', '旅行刀', 'mainHand', 0, 25, 350],
    ['axe', '旅行斧头', 'mainHand', 0, 40, 800],
  ].map(([key, name, slot, armor, damage, cooldown]) => ({
    id: `starter-${key}`,
    instanceId: `starter-${key}`,
    name,
    category: 'equipment',
    quantity: 1,
    slot,
    armor,
    ...(damage ? { attack: { damage, cooldown, maxDistance: 3 } } : {}),
    description:
      slot === 'mainHand'
        ? `主手武器 · 伤害 ${damage} · 间隔 ${cooldown / 1000} 秒`
        : `${EQUIPMENT_SLOTS[slot]} · 护甲 +${armor}`,
  }));
}
export function armorValue(equipment) {
  return ['head', 'chest', 'legs', 'feet'].reduce(
    (sum, slot) => sum + Math.max(0, Number(equipment?.[slot]?.armor) || 0),
    0
  );
}
export function attackProfile(equipment) {
  return equipment?.mainHand?.attack || EMPTY_ATTACK;
}
export function createEquipmentSystem(state, capacity = 21) {
  const fail = (error) => ({ ok: false, error });
  return {
    seed() {
      if (state.equipmentSeeded) return;
      const all = [...state.items, ...Object.values(state.equipment).filter(Boolean)];
      const missing = starterEquipment().filter(
        (item) => !all.some((value) => value.id === item.id || value.instanceId === item.instanceId)
      );
      // 初始化同样是原子操作：不超出可见的 21 格背包，也不只发放部分装备。
      if (state.items.length + missing.length > capacity)
        return fail('背包空间不足，无法领取初始装备。');
      state.items.push(...missing);
      state.equipmentSeeded = true;
    },
    equip(id, slot) {
      const index = state.items.findIndex((item) => item.id === id);
      const item = state.items[index];
      if (!item || item.category !== 'equipment' || item.quantity !== 1 || !item.instanceId)
        return fail('请选择背包中的装备。');
      if (!(slot in EQUIPMENT_SLOTS) || item.slot !== slot) return fail('这件装备不能放入此槽位。');
      if (Object.values(state.equipment).some((value) => value?.instanceId === item.instanceId))
        return fail('该装备实例已穿戴。');
      // 在原背包格替换物品，保证背包已满时仍能交换装备。
      const previous = state.equipment[slot];
      if (previous) state.items.splice(index, 1, previous);
      else state.items.splice(index, 1);
      state.equipment[slot] = item;
      return { ok: true };
    },
    unequip(slot) {
      if (!(slot in EQUIPMENT_SLOTS) || !state.equipment[slot]) return fail('此装备槽为空。');
      if (state.items.length >= capacity) return fail('背包已满，请先腾出一个格子。');
      state.items.push(state.equipment[slot]);
      state.equipment[slot] = null;
      return { ok: true };
    },
  };
}
