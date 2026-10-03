# 李淳珺任务 · 「先知」NPC · R2 — 物体与碎片世界规则

> 任务：**UGC 游戏化程序 — 「先知」NPC — 李淳珺 — 10月4日20时**
> 原任务文字：① 进行美术的交流，调整物体缩放位置等（有好方案可以换）② 进行程序（视角碎片）的交流，提前写好脚本碎片供使用
> 写入日期：2026-10-03
> **本文档取代 R1《李淳珺任务-先知NPC-实施计划.md》§4.1 的「碎片=台词分段」决策。**

---

## 0. 实施状态：✅ 已完成（2026-10-03）

| 验证项 | 结果 |
| --- | --- |
| `python -B -m unittest discover -s game/tests -t .` | **121 通过 / 0 失败**（改动前 108，新增 13：`test_world_rules.py` 12 条 + 美术/数据同步 1 条） |
| `node --test game/tests/frontend/*.test.mjs` | **129 通过 / 0 失败**（改动前 113，新增 16：`storyWorldRules` 7 条 + `storyCube`/动作菜单 9 条） |
| `node --test editor/Frontend/tests/js/*.test.mjs` | **118 通过 / 0 失败** |
| `npm run lint`（editor/Frontend） | 18 条（2 error + 16 warning）。**2 个 error 是既有的** `setImmediate` no-undef（`git grep` 确认在 HEAD 内，位于 `viewportCameraHost.test.mjs:261,270`）；warning 比改动前 +1，来自新增按钮的多行属性写法，与本文件既有风格问题同类 |
| `npm run build`（Vite）+ 完整 `build-fast` | **均成功** |
| 部署产物 | `game/data/fragments.json`、三个新 `.mjs`、`game/art/models/cube/*` **均已部署** |
| 端到端 | 新建 bundle `StoryWorld-*.js` 内含 `装入碎片` / `浮动碎片` / `先知方块` |
| Python 派发 | `WORLD_RULE_ACTIONS = ('loadWorldRules','saveWorldRules','clearWorldRules')`，`handle_world_rule_request` 可达 |

**过程中的一次事故（已完全修复，如实记录）**：给两个测试文件补桩表时，我用了 PowerShell 的
`Get-Content -Raw` + `WriteAllText`，而 PS 5.1 默认按 ANSI/GBK 读取 UTF-8 文件，导致
`storyWorld.test.mjs` 的中文被写成乱码（一个正则因此语法错误、该文件无法加载）。
处置：`git checkout --` 还原两个文件（HEAD 内本就是正确 UTF-8），改用 `edit` 工具重新应用。
复核：两文件 `瀹|鐨|涓|锛` 乱码行数 **0**，全部测试恢复全绿。**教训：这些 UTF-8 源文件只能用
`edit`/`read` 工具改，不能用 PowerShell 文本读写。**

---

## 0. 需求澄清（甲方原话 → 可执行条目）

| # | 原话 | 拆成可执行条目 |
| --- | --- | --- |
| R1 | 在小世界的先知旁边加一个正方体物体 | 小世界进入时，在先知的**旁边**创建一个**正方体** actor（游戏自有 GUID，主世界隐藏） |
| R2 | 与先知交互后选择物体缩放或调整位置 | 先知对话面板变成**动作菜单**：可**放大/缩小**该正方体，也可**前后左右移动**它；选择即生效并持久化 |
| R3 | 每个碎片对应一个世界规则 | 碎片 = **世界规则载体**：`{ id, name, rule }`，规则是可施加到小世界的持续效果 |
| R4 | 可以在先知处将碎片装给小世界 | 在先知处执行「装入碎片」→ 该规则被**安装到当前小世界**并持久化 |
| R5 | 初始获得的碎片为让所有物体持续左右小范围浮动 | 初始碎片自带 `sway` 规则：小世界内**所有物体**沿左右方向做小幅正弦浮动 |

**与 R1 的差异**：R1 把"脚本碎片"定义为**先知台词的分段单元**（并因此排除了"可拾取物件"路线）。
R2 明确碎片是**世界规则**，与台词无关。台词部分保持 R1 的成果不变（`game/data/prophet.json` 的 `script`），
但"碎片"一词在 R2 中一律指**规则碎片**。为避免歧义，数据文件命名为 `game/data/fragments.json`
（规则目录），与 `prophet.json` 的 `script` 互不重叠。

---

## 1. 现状与挂载点（都已实测确认）

| 事项 | 事实 | 证据 |
| --- | --- | --- |
| 唯一的每帧循环 | `playerController.mjs` 的 `tick()`，`requestAnimationFrame` 调度，**静止时主动停帧** | `playerController.mjs:155,201,205-211` |
| 不能每帧读场景 | `api.scene.getSnapshot` / `setActorTransform` 都是异步 CEF 往返 | `editorApi.js:248-288` |
| 正确的每帧写变换方式 | **同步 fire-and-forget** `window.coronaBridge.actorTransform(handle, op, vec)`，op `0=position,1=rotation,2=scale`，返回 `true` | `cef_renderer_bridge.cpp:24-47`、`cef_realtime_bridge.cpp:676-724` |
| 现成先例（照抄） | `movePlayer`/`facePlayer`：`bridge?.actorTransform?.(handle, op, v) !== true` 就抛错，**不 await** | `playerController.mjs:75,92,98` |
| 若走异步退路 | 必须 `persist: false`，否则每帧整场景落盘 | `cef_editor_native_api_handlers.cpp` 注释 |
| 碎片计数器 | 主世界存档 `inventory.worldFragment`，拾取时 +1 | `story_gameplay.py:37,126` |
| 陈列持久化范式 | 世界内 `.game/story-placements.json`，`version` + 校验 + 原子写 | `placements.py` 全文 |
| 陈列动作派发范式 | `PLACEMENT_ACTIONS` + `handle_placement_request` + 在 `handle_gameplay_request` 内路由 | `story_gameplay.py:19,195-236,245,256` |
| 正方体模型 | **仓库内不存在任何 cube/box 图元**，必须自制 | 全仓扫描 |

---

## 2. 关键设计决策

### 2.1 规则引擎跑**自己的** rAF 循环，不改 `playerController.mjs`

`playerController` 的 `hasWork()` 为 `jump || dodge || shifts.size || keys.size`（`:43`）——
玩家站住不动时会**停帧**。要让浮动持续，有两条路：

- ❌ 改 `hasWork` / 给 `createPlayerController` 加 `extraWork` 参数 —— 要动占祈健任务 3 的文件；
- ✅ **`storyWorldRules.mjs` 自带 rAF 循环**，仅在**已安装规则**时运行，卸载/退出世界即停。

选后者：零改动 `playerController.mjs`，且规则引擎与玩家输入解耦、调度器可注入（可单元测试）。

### 2.2 浮动是**相对基准位**的偏移，基准只取一次

不能每帧读场景。做法：
1. 安装规则（或进入小世界且已有规则）时，**读一次** `api.scene.getSnapshot`，得到每个目标的
   `{ handle, position }` 作为**基准**；
2. 每帧只算 `基准 + 正弦偏移`，用同步桥写 position；
3. 目标集合、基准在「安装规则 / 进入世界 / 陈列变化」时重建。

### 2.3 「所有物体」的边界（需甲方确认，已给单点开关）

R5 说"所有物体"。实现按 `excludeRoles: ["player"]` —— **只排除玩家**，因为：
玩家不是被陈列的"物体"，且用正弦偏移去写玩家位置会与玩家控制器（WASD/跳跃/落地）直接打架。

先知、正方体、陈列物**都会浮动**（字面满足"所有物体"）。
若希望先知也不浮动，只需把 `excludeRoles` 改成 `["player", "prophet"]`，**不改代码**。这条开关写在数据文件里并在下方标注。

### 2.4 正方体的创建不改 `storyActors.mjs`

`storyActors.mjs` 是四个人共抢的文件（任务表 §5）。正方体走**独立模块** `storyCube.mjs`，
自己完成「创建 → 等包围盒 → 归一化 → 落地 → 关物理」，不向 `storyActors.mjs` 增加分支。
也不进 `STORY_CHARACTERS`——`test_story_assets.py:31` 断言该数组恰好是
`["player","boss","merchant","prophet"]`，加进去会把测试打红。

### 2.5 正方体美术纳入"逐字节复制"不变量

`test_story_assets.py:47-64` 要求 `game/art/models/<role>/` 每个文件都与 `assets/` 下原件
**SHA-256 一致**。但它的 `sources` 字典硬编码了 4 个角色，**漏了 `fragment`**（既有盲区）。
本次：新增 `cube`，并把 `fragment` 一并补进该字典，同时把
"每个角色恰好 1 个 `.dae`" 拆成**仅角色目录**的断言（正方体/碎片是图元，没有骨骼动画）。

---

## 3. 交付物

| 文件 | 作用 |
| --- | --- |
| `assets/model/cube.obj` + `cube.mtl` | 正方体图元（1×1×1，8 顶点 / 4 UV / 6 法线 / 6 四边形面） |
| `game/art/models/cube/cube.obj` + `cube.mtl` | 与上者逐字节一致的副本 |
| `game/data/fragments.json` | **碎片目录**：`sway` 规则 + 初始碎片标记 |
| `game/data/prophet.json` | 追加 `cube` 段：模型、GUID、摆放与调节参数 |
| `game/frontend/storyCube.mjs` | 正方体：GUID/摆放数学/缩放与位移步进（纯函数）+ 创建流程 |
| `game/frontend/storyWorldRules.mjs` | 规则模型 + 正弦偏移（纯函数）+ rAF 规则引擎（调度器可注入） |
| `game/frontend/storyProphetActions.mjs` | 先知动作菜单的数据与执行器 |
| `game/runtime/world_rules.py` | 当前小世界已安装规则的持久化（照抄 `placements.py` 范式） |
| `game/runtime/story_gameplay.py` | 追加 3 个动作：`loadWorldRules` / `saveWorldRules` / `clearWorldRules` |
| `editor/Frontend/src/views/layout/StoryWorld.vue` | **接线点**：面板内渲染动作菜单 + 数字键选择 + 每帧规则引擎的启停 |
| 测试 | `game/tests/frontend/storyCube.test.mjs`、`storyWorldRules.test.mjs`、`storyProphetActions.test.mjs`；`game/tests/runtime/test_world_rules.py`；`test_story_assets.py` 扩展 |

---

## 4. 数据契约

### 4.1 `game/data/fragments.json`

```jsonc
{
  "version": 1,
  "fragments": [{
    "id": "sway", "name": "浮动碎片", "initial": true,
    "description": "让小世界里的物体持续左右小范围浮动。",
    "rule": { "type": "sway", "axis": "x", "amplitude": 0.25,
              "periodMs": 2400, "excludeRoles": ["player"] }
  }]
}
```

- `initial: true` = 开局即拥有（对应 R5「初始获得的碎片」）。
- `amplitude` 单位米，`periodMs` 一次完整左右往返的毫秒数。二者都必须是有限正数。
- `excludeRoles` 是 §2.3 的单点开关。

### 4.2 `game/data/prophet.json` 的 `cube` 段

```jsonc
"cube": {
  "asset": "cube/cube.obj", "guid": "story.prophet.cube",
  "modelRef": "story.prophet.cube.v1",
  "sideDistance": 1.4, "forwardDistance": 0.6, "scale": 0.8,
  "scaleStep": 0.2, "scaleRange": [0.3, 3.0],
  "moveStep": 0.5, "moveRange": 6.0
}
```

- 摆放：相对先知 `right * sideDistance + forward * forwardDistance`（先知右侧偏前）。
- 调节：缩放按 `scaleStep` 增减并夹在 `scaleRange`；位移按 `moveStep` 沿先知朝向的前/后/左/右，
  且**始终夹在距先知 `moveRange` 米以内**，防止把物体甩出视野。

### 4.3 规则存档（小世界内）

`.game/story-world-rules.json`：`{ "version": 1, "rules": [ { "fragmentId", "type", "axis", "amplitude", "periodMs" } ] }`
——与 `placements.py` 同样只允许 `version == 1`、原子写、校验失败拒绝覆盖、失败不产生半写文件。

---

## 5. 先知动作菜单

| 序号 | 动作 | 效果 |
| --- | --- | --- |
| 1 | 放大物体 | 正方体 scale += `scaleStep`（夹在上限） |
| 2 | 缩小物体 | 正方体 scale −= `scaleStep`（夹在下限） |
| 3 | 物体前移 | 沿先知朝向前移 `moveStep` |
| 4 | 物体后移 | 反之 |
| 5 | 物体左移 | 沿先知朝向左侧移 `moveStep` |
| 6 | 物体右移 | 反之 |
| 7 | 装入碎片 | 把当前拥有的碎片规则安装到本小世界并持久化 |

交互约定：
- 面板打开时**数字键 1–7** 直接执行；鼠标点击同一行也可执行；
- 保留 R1 的线性剧本：`Enter/Space` 推进台词，`Esc` 关闭；
- 面板打开时仍然吞掉其它按键（既有行为不变）；
- 每次执行后显示一行反馈（例如「物体已放大到 1.00」/「已装入：浮动碎片」），不关闭面板。

---

## 6. 验收

**R1 正方体**
1. [ ] 小世界内先知旁边出现一个正方体，主世界看不到它
2. [ ] 退出再进入小世界，正方体位置/大小保持（不被重置回默认）

**R2 动作菜单**
3. [ ] 先知旁按 `F` 打开面板，能看到 7 个动作
4. [ ] 按 `1`/`2` 正方体明显变大/变小，且有下限上限
5. [ ] 按 `3`–`6` 正方体沿四个方向移动，且不会离先知过远
6. [ ] `Enter` 仍能推进台词、`Esc` 仍能关闭面板（R1 行为零回归）

**R3/R4/R5 碎片与世界规则**
7. [ ] 按 `7` 装入碎片后，小世界里的物体开始**持续左右小幅浮动**
8. [ ] 浮动是连续的（玩家站住不动也不停）
9. [ ] 退出再进入小世界，浮动仍在（规则已持久化）
10. [ ] 主世界不受影响、无浮动
11. [ ] 玩家自身不被浮动（否则与移动/跳跃打架）

**回归**
12. [ ] 主世界按 `F` 仍是拾取世界碎片
13. [ ] `python -B -m unittest discover -s game/tests -t .` 全绿
14. [ ] `node --test game/tests/frontend/*.test.mjs` 全绿
15. [ ] `npm --prefix editor/Frontend run build` 通过

---

## 7. 明确不做

- ❌ 碎片做成可拾取/可组合物品（仍需郭子豪的物品表 G3；本版碎片来源=初始授予 + 动作菜单装入）
- ❌ 除 `sway` 之外的规则类型（数据模型已可扩展：只需在 `fragments.json` 加条目 + 在规则引擎加一个纯函数）
- ❌ 多小世界下的规则隔离（当前结构上只有 1 个小世界）
- ❌ 骨骼挂点 C++、装备穿在身上（与 R1 一致，非本任务）
- ❌ 正方体的旋转调节（甲方只要求缩放与位置）

---

## 8. 风险

| # | 风险 | 处置 |
| --- | --- | --- |
| 1 | 每帧写 transform 若退化成异步 CEF 往返会卡死 | 强制走同步桥 `coronaBridge.actorTransform`；同步桥不可用时报错而非静默降级 |
| 2 | 浮动与玩家控制器同时写玩家位置会抖动 | `excludeRoles` 默认排除玩家 |
| 3 | 规则引擎自开 rAF 循环可能与玩家循环叠加耗电 | 仅在有已安装规则时运行；退出小世界/卸载规则即停 |
| 4 | `StoryWorld.vue` 是四人共抢文件 | 只加 1 个动作菜单区块 + 1 处引擎启停；全部逻辑在独立 `.mjs` |
| 5 | `story_gameplay.py` 是三方共抢文件 | 只追加 3 个动作常量与 1 个分支，照抄既有陈列范式 |
| 6 | 正方体是自制图元，与"美术=引擎资产副本"惯例不同 | 同时放入 `assets/model/` 并纳入逐字节复制测试，保持不变量 |
