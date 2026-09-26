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


## 剧情子世界快捷键（O / P）

这是可选的编辑器运行时接入，不改变上面的纯 Python `GameSession`、小球或创作世界接口。
不需要小球，不增加屏幕按钮，也不需要新增 C++ API。

| 所在世界 | O | P |
| --- | --- | --- |
| 主剧情世界 | 首次保存并复制完整场景及资源，进入子世界；以后复用同一个副本 | 不切换 |
| 子剧情世界 | 不切换，不创建嵌套世界 | 保存子世界并返回关联主世界 |

主、子世界始终使用剧情游戏页面，不进入创作模式或显示编辑工具。两边的场景、模型资源、
玩家位置和相机位置/朝向独立保存；返回主世界恢复离开时的视角，再次进入子世界恢复其自己的视角。
相机尚未绑定、页面失焦或隐藏时不接受快捷键。长按重复、输入框/可编辑区域、输入法组合事件，
以及 Ctrl / Alt / Meta 组合键均被忽略。切换期间停止移动和镜头输入，并清空旧按键与动画帧。

### 文件与调用边界

- `runtime/subworlds.py`：文件系统副本事务、完整场景校验和持久化主子关系，不导入引擎。
- `runtime/story_navigation.py`：通过已有 Python 编辑器 API 查询当前项目、保存当前场景、校验资源；
  只返回导航信息，不直接打开世界。
- `frontend/storyNavigation.mjs`：快捷键过滤、切换锁、请求失效控制、打开失败恢复。
- `tests/runtime/`、`tests/frontend/storyNavigation.test.mjs`、`tests/frontend/storyWorld.test.mjs`：
  文件事务、适配器、部署以及真实剧情页面的模拟原生接口回归。

编辑器仅在剧情页面、Scratch 按键适配器、相机快照及现有启动器处接入。
按键仍通过 `scratch.sendKeyEvent`，切换仍通过 `projectLauncherService.openProject`，
复用世界模式确认、旧世界任务清理及视口重新绑定。其他 Scratch 按键和松键行为不变。
相机写入确认后才调用 Python 保存和复制，不把旧世界的相机句柄带到新世界。

### 持久化布局

仅支持 `scene.ini` 中明确标记 `[format] type=corona_scene_folder`、`version=1`，
且 `[world] type=story` 的可移植世界，资源必须通过现有哈希校验。

```text
主世界/
├── scene.ini
├── assets.manifest.json
├── Assets/                       # 主世界资源
└── .game/
    ├── story-link.json            # role=main, target=.game/subworld
    └── subworld/
        ├── scene.ini             # 子世界自己的场景和相机
        ├── assets.manifest.json
        ├── Assets/               # 独立文件，不是硬链接或符号链接
        └── .game/story-link.json # role=child, target=../..
```

双向 JSON 关联包含相同的版本和 UUID，仅存储相对路径。重启或整体移动主世界目录后仍可往返；
不要单独移动子目录或手工改写关联。复制排除源世界根目录的 `.game`（Windows 下识别大小写变体），
防止把旧副本和临时目录递归复制进去。符号链接/目录联接会报错，避免共享资源或绕回源目录。

首次复制在 `.game/.subworld-staging-*/world` 完成，完整校验后才提交正式子世界；
失败清理临时副本和本次新建的关联，不覆盖已有子世界、冲突目录或外部关联文件。
各次导航使用 `.game/story-navigation.lock` 防止并发准备。

### 取消与错误

- 保存、复制、资源校验失败：停留当前世界，显示原因，可修复问题后重试。
- 目标打开失败：通过同一启动器恢复来源世界；恢复也失败则返回启动页并报告两个失败原因。
- Escape 或另行打开世界：旧请求失效，不再打开旧目标、恢复旧来源或显示迟到错误。
  已开始的保存/复制可能完成，但新世界的原生打开必须等待来源世界任务结束。
- 关联损坏、目标缺失、目录冲突：明确报错，不静默创建替代副本。
- 进程异常终止可能留下锁或未提交的暂存目录。程序不会自动解除锁、覆盖关联或猜测如何恢复；
  请先关闭相关引擎进程并备份世界，核对双向关联和副本完整性，再清理遗留项。

### 部署与验证

发布脚本会把 Python `game` 包和 `game/frontend` 模块放在 `CabbageEditor` 的同级目录，
因此安装后的程序不依赖源码工作区。沿用现有发布流程同步脚本和前端产物，不需要为本功能修改或重编 C++。

从仓库根目录运行：

```powershell
python -B -m unittest discover -s game/tests -t . -v
node --test "game/tests/frontend/*.test.mjs" "editor/Frontend/tests/js/*.test.mjs"
npm --prefix editor/Frontend run build
```

实机验收（自动化测试使用模拟原生接口，不能代替本检查）：

1. 打开包含模型的剧情世界，移动并旋转视角；按 O，确认首次生成副本，模型完整且仍是无编辑工具的游戏页面。
2. 在子世界移动、旋转并缩放视角；按 P，确认返回主世界离开时的位置和朝向。
3. 再按 O，确认进入原来的子世界并恢复其视角；资源及场景修改互不影响。
4. 重启后分别打开主/子世界重复往返，确认关联仍有效；子世界 O、主世界 P 不切换。
5. 快速连续按 O/P、长按、在复制期间按 Escape 或选择另一世界，确认无重复副本和迟到跳转。
6. 在测试副本中模拟资源缺失/关联损坏，确认提示错误且不会覆盖原文件；不要在唯一的正式世界上做破坏性验证。
