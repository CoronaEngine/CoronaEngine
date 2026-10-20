/**
 * Equipment a main-world monster may leave behind.
 *
 * The backend (`game/runtime/story_gameplay.py`) owns the drop roll and stores the chosen
 * id in the save; this module is the shared catalogue the page renders. The ids must match
 * `MONSTER_EQUIPMENT` there, and the shape is the same as the bag's item definitions so
 * dropped gear uses the existing icon/rarity/detail paths without a second definition.
 */
export const MONSTER_EQUIPMENT = Object.freeze([
  Object.freeze({
    u: 'hunter-blade', name: '猎人长刃', type: '刀剑', cat: 'equip', slot: 'weapon',
    rarity: 'rare', glyph: 'g-sword',
    stats: [['锋芒', '68 - 92'], ['身法', '+16']],
    flavor: '刀背留着一排细齿，据说原是用来剥兽皮的。',
  }),
  Object.freeze({
    u: 'hunter-helm', name: '兽骨轻盔', type: '轻甲', cat: 'equip', slot: 'helm',
    rarity: 'rare', glyph: 'g-helm-l',
    stats: [['护体', '+66'], ['定神', '+12']],
    flavor: '额前那枚骨片磨得很亮，猎人说它挡过一次角撞。',
  }),
  Object.freeze({
    u: 'hunter-chest', name: '兽皮护甲', type: '轻甲', cat: 'equip', slot: 'chest',
    rarity: 'rare', glyph: 'g-chest-l',
    stats: [['护体', '+94'], ['身法', '+18']],
    flavor: '硝过的厚皮还带着草木味，针脚粗，却极耐磨。',
  }),
  Object.freeze({
    u: 'hunter-boots', name: '猎风战靴', type: '靴履', cat: 'equip', slot: 'boots',
    rarity: 'rare', glyph: 'g-boots-l',
    stats: [['护体', '+58'], ['身法', '+15'], ['疾行', '+7%']],
    flavor: '靴筒里缝着一圈软草，走上一天也不磨脚。',
  }),
]);

export const findMonsterEquipment = id =>
  MONSTER_EQUIPMENT.find(item => item.u === id) || null;

export const monsterEquipmentName = id => findMonsterEquipment(id)?.name || String(id);
