"""人物属性：装备词条的唯一聚合入口。

战斗扣血、技能冷却与技能消耗都从这里取数，任何系统都不再自己算一遍。
数值刻意保持朴素——攻击决定伤害，护体减伤，疾行压缩冷却——够用即可。
装备 id 必须与前端物品目录一致（`game/frontend/storyLoot.mjs` 与
`StoryWorld.vue` 的 BOSS_LOOT_ITEMS）。
"""

SLOTS = ('weapon', 'helm', 'chest', 'legs', 'boots')

# 空手时的基础属性。攻击 20 与旧版固定伤害一致，换装前的手感不变。
BASE_ATTRIBUTES = {'atk': 20, 'def': 0, 'hpMax': 100}

# 装备图鉴：槽位 + 词条。数值取自物品详情页的展示词条（取中间值）。
EQUIPMENT = {
    'hunter-blade': {'slot': 'weapon', 'mods': {'atk': 80}},
    'hunter-helm': {'slot': 'helm', 'mods': {'def': 66}},
    'hunter-chest': {'slot': 'chest', 'mods': {'def': 94}},
    'hunter-boots': {'slot': 'boots', 'mods': {'def': 58, 'cd': 0.07}},
    'boss-main': {'slot': 'weapon', 'mods': {'atk': 148}},
    'boss-helm': {'slot': 'helm', 'mods': {'def': 150}},
    'boss-chest': {'slot': 'chest', 'mods': {'def': 210}},
    'boss-legs': {'slot': 'legs', 'mods': {'def': 168}},
    'boss-boots': {'slot': 'boots', 'mods': {'def': 112, 'cd': 0.10}},
}
EQUIPMENT_IDS = tuple(EQUIPMENT)

# 消耗品：服用后回气。效果只在后端执行，前端只管显示。
CONSUMABLES = {'rage-herb': {'rage': 30}}

# 单件装备最多压缩 50% 冷却，避免叠满词条后技能变成无冷却。
MAX_COOLDOWN_CUT = 0.5


def aggregate(equipped: dict) -> dict:
    """把已披挂器物的词条累加到基础属性上。

    空槽位（None）与未知 id 都被忽略：存档里的陈旧 id 不该炸掉整场战斗。
    """
    total = dict(BASE_ATTRIBUTES)
    total['cd'] = 0.0
    for slot in SLOTS:
        item = EQUIPMENT.get(equipped.get(slot))
        if item is None:
            continue
        for key, value in item['mods'].items():
            total[key] = total.get(key, 0) + value
    return total


def damage(attributes: dict, coefficient: float, defense: float = 0) -> int:
    """一次结算的伤害：攻击 × 系数 − 护体的一半，最少 1。"""
    raw = attributes.get('atk', 0) * coefficient - defense * 0.5
    return max(1, int(raw))


def skill_numbers(skill: dict, attributes: dict) -> dict:
    """技能的伤害与冷却：配置值经属性修正后的结果。"""
    cut = min(MAX_COOLDOWN_CUT, max(0.0, attributes.get('cd', 0.0)))
    return {
        'damage': damage(attributes, skill.get('coefficient', 1)),
        'cooldownMs': round(skill['cooldownMs'] * (1 - cut)),
    }
