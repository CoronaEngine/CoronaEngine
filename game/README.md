# 剧情玩法

纯 Python 的内存玩法逻辑，使用 Python 3.12+，不依赖引擎、系统时钟或第三方库。
由调用方创建 `GameSession` 并串行传入时间增量；暂停或进入创作世界时冻结剧情。

## 目录

```text
game/
├── __init__.py          # 稳定的公共入口
├── core/
│   ├── config.py        # 玩法配置和校验
│   ├── session.py       # 会话编排、奖励和事件队列
│   ├── types.py         # ID、位置、枚举、结果及数值工具
│   ├── state.py         # 不可变状态快照
│   └── events.py        # 事件及其载荷
├── systems/
│   ├── boss.py          # Boss 生命周期和追击
│   ├── merchant.py      # 商人和赠品
│   └── inventory.py     # 背包和物品身份
├── world/
│   ├── clock.py         # 纳秒时钟和昼夜边界
│   ├── safe_zone.py     # 安全区和移动约束
│   ├── spawn.py         # 生成位置采样
│   └── orbs.py          # 小球与创作世界往返
└── tests/
    ├── support.py       # 共用断言和操作辅助
    ├── core/            # 配置、会话、公共接口和导入
    ├── systems/         # Boss、商人、背包、掉落
    └── world/           # 时钟、安全区、生成、小球
```

目录表达职责，文件名不再重复 `game_`、`world_`、`_system`、`_utils`。
保留 `safe_zone.py` 这类有明确含义的名称，不用难懂的缩写。
类名和行为保持不变。调用方优先使用 `from game import ...`，公共导出不变。

底层的 `types` → `state` → `events` 只依赖数据定义；`systems` 和 `world`
实现具体规则，`core/session.py` 负责组合这些规则。子包的 `__init__.py`
不集中导入实现，避免循环依赖和导入副作用。

## 导入路径迁移

旧的内部模块路径已移除，不保留重复的转发文件：

| 原模块 | 新模块 |
| --- | --- |
| `game.game_config` | `game.core.config` |
| `game.game_session` | `game.core.session` |
| `game.game_types` | 基础类型在 `game.core.types`，状态在 `game.core.state`，事件在 `game.core.events` |
| `game.boss_system` | `game.systems.boss` |
| `game.merchant_system` | `game.systems.merchant` |
| `game.inventory` | `game.systems.inventory` |
| `game.world_clock` | `game.world.clock` |
| `game.safe_zone` | `game.world.safe_zone` |
| `game.spawn_utils` | `game.world.spawn` |
| `game.world_orb_system` | `game.world.orbs` |

## 使用示例

从仓库根目录运行。以下示例推进到第一晚、拾取 Boss 奖励，再通过小球进入和返回创作世界：

```python
from game import BossPhase, GameSession

session = GameSession(seed=42)
assert session.advance_seconds(180).ok
assert session.boss.phase is BossPhase.IDLE
assert session.notify_boss_defeated().ok
assert session.update_player_position(session.story_world, session.boss.position).ok
for drop in session.drops:
    assert session.collect_drop(drop.id).ok

orb = session.inventory.orbs[0]
assert session.use_world_orb(orb.id).ok
assert session.active_world == orb.world_id
assert not session.story_running
assert session.return_to_story().ok
assert session.story_running

events = session.take_events()
assert events
assert session.events == ()
```

需要独立使用某个系统时，直接从所属模块导入：

```python
from game import DayPhase, GameConfig
from game.core.types import NS_PER_SECOND
from game.world.clock import WorldClock

clock = WorldClock(GameConfig(day_duration_ns=3 * NS_PER_SECOND))
assert clock.advance_ns(3 * NS_PER_SECOND).ok
assert clock.state.phase is DayPhase.NIGHT
```

## 测试

从仓库根目录运行全部测试，无需安装第三方测试框架：

```sh
python -m unittest discover -s game/tests -t . -v
```

只运行某个职责目录或模块：

```sh
python -m unittest discover -s game/tests/world -t . -v
python -m unittest game.tests.systems.test_boss -v
```
