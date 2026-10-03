# 李淳珺任务 · 「先知」NPC 实施计划

> 任务：**UGC 游戏化程序 — 「先知」NPC — 李淳珺 — 10月4日20时**
> 任务表原文：① 进行美术的交流，调整物体缩放位置等（有好方案可以换）② 进行程序（视角碎片）的交流，提前写好脚本碎片供使用
> 写入日期：2026-10-03
> 交付物：`game/data/prophet.json`（新建）、`game/frontend/storyDialogue.mjs`（新建）、3 处单行接线点
> 是否改 C++：**否** ｜ 是否新增后端：**否** ｜ 是否新增存档：**否**（纯只读内容数据）

---

## 0. 实施状态：✅ 已完成（2026-10-03）

| 验证项 | 结果 |
| --- | --- |
| `python -B -m unittest discover -s game/tests -t .` | **108 通过 / 0 失败**（改动前基线 105，本轮新增 3 条） |
| `node --test game/tests/frontend/*.test.mjs` | **113 通过 / 0 失败**（改动前：79 通过 + 1 个文件因缺依赖无法加载） |
| `node --test editor/Frontend/tests/js/*.test.mjs` | **118 通过 / 0 失败**（同一原因，改动前有 5 个文件无法加载） |
| 新增 JS 契约测试 `game/tests/frontend/storyDialogue.test.mjs` | **7 通过 / 0 失败** |
| `npm run lint`（editor/Frontend） | 17 条既有问题（2 error + 15 warning），**全部落在本轮未改动的文件**（`Network.vue`、`viewportCameraHost.test.mjs` 的 `setImmediate` 等） |
| `npm run build`（Vite） | **成功** |
| 完整构建 `tools/dev.py build-fast` | **成功**；日志含 `Copying game data` 与 `Frontend build completed successfully` |

**端到端证据**（内容确实抵达产物，而不只是停在源码里）：

| 检查 | 结果 |
| --- | --- |
| `<exe目录>/game/data/prophet.json` | 已部署（3220 B）—— 新增的 `copy_game_data` 生效 |
| `<exe目录>/game/frontend/storyDialogue.mjs` | 已部署 |
| 新建的 `StoryWorld-*.js` 内含新台词 `降临，或者俯瞰` | **命中** |
| 旧占位台词 `你带回了它的碎片` | **已从产物中消失** |

> ⚠️ 环境说明（**不是**代码缺陷）：本机 `editor/Frontend/node_modules` 原本缺失，导致
> **1 个 game 测试文件 + 5 个 editor 测试文件**因 `Cannot find package 'vue'` 无法加载。
> 这是**改动前的基线状态**，与本次改动无关。本轮已 `npm install`（396 个包）补齐，
> 上述 6 个文件现已全部加载并通过——这也解释了为什么占祈健三份文档记录的
> `tests/js` 121/124/117 等数字高于本机初始观测值。

---

## 1. 任务还原：我到底要交付什么

任务表把我这项拆成三条，其中 **L-3 阻塞占祈健的 Z-1**，是整条链的关键路径：

| 编号 | 子任务 | 交付物 | 状态 |
| --- | --- | --- | --- |
| L-1 | 美术交流、调整缩放/位置 | 美术规格 + 摆位参数 | ✅ 规格与参数已冻结 |
| L-2 | 程序交流、预写脚本碎片 | `game/data/prophet.json` + `game/frontend/storyDialogue.mjs` | ✅ 已交付 |
| L-3 | **先知交互接口定义（供 Z-1 用）** | 版本化接口契约 | ✅ 已冻结为 contract v1 |

**关键情况变化**：占祈健已在他自己的任务 1 里**把接口实现并定死了**
（`game/frontend/prophetDialogue.mjs`，见其文档《占祈健任务1-先知交互-实施计划.md》）。
所以我的 L-3 从"**定义**接口"转为"**确认、追认、冻结、版本化**接口"——不再新增接口，
只把已经跑起来的东西写成可校验的契约，避免它日后被无声改坏。这是对关键路径最有利的做法：
**占祈健不被我阻塞**。

---

## 2. 读文档 + 读代码后的四条硬约束（决定了方案形态）

这四条是我在动手前必须先确认的，它们直接排除了几种"看起来更直接"的做法：

### 2.1 `game/art/models/prophet/` 不许增删改文件 🔴

`game/tests/test_story_assets.py:47-64` 断言：`game/art/models/<role>/` 下**每一个文件**
都必须与 `assets/` 下的原件 **SHA-256 逐字节一致**，且每个角色目录只允许 1 个 `.dae`。

→ **L-1 不可能产出"美术文件"**。任何"我摆一个新模型进 `game/art/`"的做法都会直接把测试打红。
L-1 只能是**参数 + 规格**。

### 2.2 `game/data/` 不在部署链路里 🔴

`tools/build/editor_copy_and_build.py:235-245` 的 `copy_game_runtime` 只拷贝
`game/__init__.py` 与 `core` / `systems` / `world` / `runtime` 四个包；
`copy_game_frontend` 只拷 `game/frontend`；`copy_game_art` 只拷 `game/art`。

→ 实测已部署的 `<exe目录>/game/` 里**确实没有 `data`**。任务表 §4 把
"先知对话碎片"放在 `game/data/prophet.json`，所以**必须补一条部署规则**，否则这个文件
只存在于仓库里，装出来的程序里没有。

### 2.3 面板取文的唯一路径是 `normalizeDialogue(null)` ✅（好消息）

`editor/Frontend/src/views/layout/StoryWorld.vue:231`：
```js
const dialogue = normalizeDialogue(null);
```
`normalizeDialogue(null)` 会回退到 `PROPHET_DIALOGUE` 出厂值，而面板全程只读 `dialogue`。

→ **我只要让 `PROPHET_DIALOGUE` 变成正式内容，面板就自动显示正式台词，`StoryWorld.vue` 一行都不用改。**
这完美满足任务表 §5「其他人只提交独立 `.mjs` 模块 + 一个接线点」的纪律，
也避开了"四个人抢改 StoryWorld.vue"的最大冲突风险。

### 2.4 前端不能直接 `import` JSON ⚠️

若 `storyDialogue.mjs` 直接 `import data from '../data/prophet.json'`：
- Node 22 需要 `with { type: 'json' }` 导入属性，`node --test` 下行为与 Vite 不一致；
- Vite/Rollup 对导入属性的支持随版本变化；
- 而本仓库的前端构建失败是**静默的**（`editor_copy_and_build.py:198-204` 把 npm 失败降级为
  一行 `CMake Warning` 并 `return 0`）。

→ 一旦 JSON 导入不被支持，产物会**悄悄缺少 `Frontend/dist`**而构建仍报成功。
**结论：前端用 JS 常量承载运行时内容（零打包风险），JSON 作为内容权威源并有测试强制两者一致。**

---

## 3. L-3：先知交互接口契约（冻结为 v1）

### 3.1 契约本体

| 项 | 冻结值 | 依据 |
| --- | --- | --- |
| 契约版本 | `contract.version = 1` | 本文件 §3.3 |
| 触发键 | `F` | StoryWorld.vue:262 |
| 触发优先级 | `F` → 先知在范围内则开对话；**否则**回落原行为拾取世界碎片 | StoryWorld.vue:260-264 |
| 可用世界 | **仅 `role === 'child'`（小世界）**；主世界恒不可用 | `prophetAvailable()` |
| 距离范围 | `12` 米 | `PROPHET_INTERACTION.range`，理由见 §3.2 |
| 距离测量 | 玩家**点到包围盒**的水平距离（XZ 平面），非中心点距离 | `prophetDistance()` |
| 包围盒来源 | **每次判定实时计算**，禁止缓存 | StoryWorld.vue:236（占祈健已修正，见其 §6.1） |
| 面板推进 | `Enter` / `Space`；最后一行推进即关闭 | StoryWorld.vue:255-259 |
| 面板关闭 | `Esc` | StoryWorld.vue:245-253 |
| 面板开启时 | 吞掉全部按键；`Esc` 不退出世界；`WASD`/`V`/`E` 失效 | StoryWorld.vue |
| 输入锁 | `isInputLocked` 含 `dialogueOpen`，相机不响应 | StoryWorld.vue |
| 长按 | `repeat` 事件被忽略 | StoryWorld.vue |
| 操作提示 | 范围内显示 `F 与先知交谈` | StoryWorld.vue:406 |
| 台词数据结构 | `{ id, name, title, hint, closing, lines: string[] }` | `normalizeDialogue()` |

### 3.2 追认一处与指南的偏差：范围为什么是 12 米而不是 3.0

《小世界内部-引擎差距分析与实施指南.md》§4 任务1 第 4 步建议
"`GameConfig.interaction_distance = 3.0` 已存在，直接复用别新增常量"。

**经核查，这个建议不成立，占祈健另设 12 米是正确的**，证据：

```powershell
# interaction_distance 的全部使用点
rg -n "interaction_distance" game/ editor/
#   game/core/config.py:22        ← 定义
#   game/core/config.py:37        ← 校验
#   game/systems/merchant.py:54   ← 唯一消费者：商人领赠品距离
#   game/tests/core/test_contracts.py:212
```

→ 它是**商人系统的 Python 侧常量**，前端从未读取，语义也不是"玩家与 NPC 的通用交互距离"。
复用它会让"商人领赠品"和"与先知交谈"被同一个值耦合，改一个必坏另一个。

另一方面，12 米本身有充分理由：先知是**先被放到玩家前方 4 米、之后才按身高归一化缩放**
（`storyActors.mjs:82-83` → `characterTransform` 的 `height / extent[1]`），
所以玩家到先知**身体**的距离取决于模型导入时的原始尺寸：
原始 1 米高 → 身体约在 4 米处；原始 2 米高 → 约在 8 米处。取 12 米覆盖该波动，
同时仍是有限范围（世界另一头的先知不响应）。

**结论：冻结 `range = 12`，并把"为什么不用 `interaction_distance`"写进契约，防止后人"顺手统一"。**

### 3.3 版本与向后兼容规则

| 改动类型 | 是否升 `contract.version` | 说明 |
| --- | --- | --- |
| 改台词 / 标题 / hint / closing | **否** | 纯内容，面板直接读结构 |
| 增删台词行数 | **否** | 面板按 `lines.length` 自适应，右下角显示进度 |
| 改交互距离、可用世界、测量方式 | **是** | 影响 Z-1 的判定语义 |
| 台词从线性改为带分支/选项 | **是** | 需扩展 `normalizeDialogue` 的数据结构（见 §6） |
| 先知改为跨世界共享或每世界独立 GUID | **是** | 见 §5 Q3 |

违反此表的改动会让 `game/tests/frontend/storyDialogue.test.mjs` 变红，从而被拦住。

---

## 4. L-2：脚本碎片的数据模型与内容

### 4.1 决策：「脚本碎片」= 先知台词的分段单元，**不是**可拾取物件

> ⚠️ **本节决策已被取代。** 甲方后续澄清：*"每个碎片对应一个世界规则，可以在先知处将碎片装给小世界，
> 初始获得的碎片为让所有物体持续左右小范围浮动"* —— 碎片是**世界规则载体**，与台词无关。
> 请以 [李淳珺任务-先知NPC-实施计划-R2-物体与碎片规则.md](李淳珺任务-先知NPC-实施计划-R2-物体与碎片规则.md)
> 为准；以下内容仅作历史记录保留。

任务表 §8 问题 2 与指南 §5 问题 2 都要求我明确定义。我的决策与理由：

**决策：脚本碎片（script fragment）= 先知剧本里的一个"段"，是有 id 的台词单元。**

理由：
1. **排期安全**：若做成"可拾取/可组合的剧情物件"，它就必须进背包，而
   `G3（物品定义表）` 在引擎里**完全不存在**（指南 §2），会把我这条关键路径
   接到郭子豪的任务上，直接吃掉 10月4日 20:00 的冻结窗口。任务表 §7 也明确把
   收集/生产/交易系统列为本轮不做。
2. **不与既有概念撞名**：游戏里**已经**有"世界碎片"——`worldFragment` 计数器 +
   `game/art/models/fragment/Ball.obj`，由 `game/world/orbs.py` 与掉落/拾取流程拥有
   （`storyActors.mjs:59-61`、`STORY_CHARACTERS` 之外的 `FRAGMENT`）。
   把先知的"脚本碎片"也做成可拾取物，会和这套已跑通的系统正面冲突。
3. **可向后兼容**：数据里给每一段保留稳定 `id`，将来若真要做"碎片收集"，
   可以在这个 id 上接物品表，**不需要重写剧本**。

### 4.2 `game/data/prophet.json` 结构

三段式，各自职责单一：

```
{
  "contract":  { version, kind, interaction: {...} },   ← §3 的接口契约（机器可读）
  "placement": { model, asset, aheadDistance, ... },     ← L-1 摆位/缩放参数
  "script":    { id, name, title, hint, closing,
                 fragments: [{ id, text }], lines: [...] } ← L-2 内容
}
```

设计要点：
- `script.lines` 是**给现有面板的线格式**，与 `normalizeDialogue` 的 `lines: string[]` 完全一致，
  所以**不需要改占祈健的任何代码**；
- `script.fragments` 是**带 id 的权威形态**，供未来做碎片收集/本地化/逐段校验；
- 两者由测试强制一一对应，顺序与文本都必须相同（`game/tests/test_story_assets.py`）；
- 文件名遵循任务表 §4「统一数据约定」的指定路径 `game/data/prophet.json`
  （指南 §5 写作 `prophet-dialogue.json`，以任务表为准，此处记录该分歧已收敛）。

### 4.3 正式台词（替换占位内容）

占祈健的占位文本是"你从小世界之外回来了 / 外面的巨龙倒下了…"。我提供的正式剧本
延续同一条叙事线，但明确承担**先知的教学职责**——把玩家在小世界里能做的事讲清楚
（对应 Z-2 的 UI 变化与 Z-4 的陈列）：

| # | fragment id | 台词 |
| --- | --- | --- |
| 1 | `return` | 你从小世界之外回来了。 |
| 2 | `dragon` | 外面的巨龙倒下了，它的碎片留在你手里。 |
| 3 | `exhibit` | 把碎片放在这里——小世界会记住你走过的路。 |
| 4 | `build` | V 键可以换一双眼睛：降临，或者俯瞰。 |
| 5 | `leave` | 想回去的时候，按 P。我一直在这里。 |

`closing`：`去想放什么，就去放吧。`
`title`：`小世界的守望者` ｜ `hint`：`F 交谈 · Enter 继续 · Esc 离开`

### 4.4 `game/frontend/storyDialogue.mjs`（运行时投影）

导出：

| 符号 | 用途 |
| --- | --- |
| `PROPHET_CONTRACT_VERSION` | 契约版本，供测试与后续迁移判断 |
| `PROPHET_SCRIPT` | `{ id, name, title, hint, closing, lines, fragments }` |
| `PROPHET_PLACEMENT` | §5.3 的摆位参数 |
| `validateProphetPlacement(raw)` | 参数校验（有限数、正值），返回规范化副本 |
| `describeProphetContract()` | 汇总契约，供测试断言 |

**为什么是 JS 常量而不是 `import ... from '../data/prophet.json'`**：见 §2.4。
两者一致性由 `game/tests/test_story_assets.py` 里新增的同步测试强制：
它用 Node 读出 `storyDialogue.mjs` 的 `PROPHET_SCRIPT`，与 `game/data/prophet.json` 的
`script` 逐字段比对（沿用该文件已有的"spawn node 读 `.mjs`"手法，见其 :22-31）。
**任何一边被单独改动，测试立即变红**，所以不存在"悄悄漂移"。

---

## 5. L-1：美术规格与摆位参数

### 5.1 美术交付契约（给美术/我之间的交流结论）

由于 §2.1 的硬约束，美术资源的落地流程被固定为：

| 步骤 | 动作 | 校验者 |
| --- | --- | --- |
| 1 | 把模型与全部贴图放入 `assets/model/<子目录>/` | 人工 |
| 2 | **逐字节原样复制**到 `game/art/models/prophet/`（不改名、不改贴图路径） | `test_story_assets.py:47-64` |
| 3 | 在 `assets/` 保留原件（测试会双向比对哈希） | 同上 |
| 4 | 更新 `game/data/prophet.json` 的 `placement.model` / `asset` 与参数 | 同步测试 |

**格式硬性要求**（全部由现有测试强制，违反即红）：

1. `.dae` / `.obj` / `.gltf` 之一，且**每个角色目录只允许 1 个 `.dae`**；
2. **必须至少含 1 段动画**（`library_animations/animation`）——现有四个角色都满足；
3. 所有贴图必须是**相对路径**（不允许 `file://`、不允许绝对路径、不允许网络 URL）；
4. 每张贴图必须解析到**模型同目录树内**且**真实存在**；
5. 必须是引擎可加载的资产（`assets/` 里已有的那批）。

### 5.2 当前模型与"有好方案可以换"的判据

当前先知模型：`prophet/dancing_vampire.dae`（32.0 MB，贴图 4 张：diffuse/emission/normal/specular）。
它是仓库里**唯一**可用的候选（其余是玩家 Maria、商人 Maw、Boss 龙、碎片球）。

- **本版决策：不换。** 理由：换模型需要新的美术资源，而 §5.1 要求该资源先进
  `assets/`；我**无法自行产出美术**，本轮也没有人提供新资源。
- **替换判据**（满足任一即可提出更换，不需要改任何代码）：
  1. 造型与"先知/守望者"气质冲突（当前是吸血鬼舞蹈模型，属于最可能被换的理由）；
  2. 缺少动画（会直接违反 §5.1 第 2 条）；
  3. 贴图不是相对路径或缺失（违反第 3/4 条）；
  4. 模型尺度异常导致 4 米摆位下身体离玩家过远/过近（用 §5.4 的参数调，不必换模型）。
- **换模型的成本**：只要资源进了 `assets/` 并复制到 `game/art/models/prophet/`，
  改 `game/data/prophet.json` 的 `placement.asset` 即可，**零代码改动**。

### 5.3 摆位/缩放参数（本次冻结）

| 参数 | 值 | 语义 |
| --- | --- | --- |
| `aheadDistance` | `4.0` | 先知被摆到玩家朝向前方 4 米（缩放前） |
| `height` | `1.8` | 归一化目视高度（米）；缩放 = `height / 旋转后包围盒高度` |
| `yawOffset` | `π` | 先知转向玩家（`yaw = 玩家朝向 + π`） |
| `rotationPolicy` | `face-player` | 朝向策略；备选 `fixed` |
| `model` | `dancing_vampire.dae` | 当前模型文件名 |
| `asset` | `prophet/dancing_vampire.dae` | `game/art/models/` 下的相对路径 |

这三个数值**原本硬编码在两处**：`storyActors.mjs:82-83` 的字面量 `4` 与
`storyCharacters.mjs:14` 的 `height: 1.8`。本次把它们提升为
`game/data/prophet.json` 的 `placement` 块，由 `storyDialogue.mjs` 导出、
上述两处改为读取（**取值不变，行为零变化**）。

→ 好处：美术要微调缩放/距离时，只改一个数据文件，**不需要碰逻辑代码**，
正是任务表 L-1"调整物体缩放位置"所要求的可迭代性。

### 5.4 我无法在此交付的东西（如实说明）

- ❌ 新的先知模型/贴图（需美术提供，见 §5.1 流程）；
- ❌ 知先的立绘、表情、音效（任务表与占祈健 §9 均列为不做）；
- ❌ 骨骼挂点（属 C++ 改动，任务表 §7 明确本轮不做）。

---

## 6. 任务表 §8 / 指南 §5 待确认问题的正式答复

这四条是"李淳珺是整条链的源头"的根源，必须给出明确结论，否则后面三个人会走错方向。

| # | 问题 | **结论** | 理由 |
| --- | --- | --- | --- |
| 1 | 装备是否必须真实穿在角色身上？ | **否，本版不要求。** 走指南 §3 的方案 C：只做数据层（背包内记录"已穿戴"）。方案 B（武器前端逐帧跟随）**本版也不做**，因为 `game/art/models/` 里**没有任何武器/护甲模型**，做了也无物可挂。方案 A（骨骼挂点 C++ API）排入下一版，需要 1 名 C++ 同学 2~3 天。 | 指南 §3 已论证；资源缺口见指南 §2 📦；占祈健任务3 §7 亦已声明"不做第一人称手持武器" |
| 2 | "脚本碎片"是台词还是可拾取物件？ | **是先知台词的分段单元**，见 §4.1。带稳定 `id`，保留将来接物品表的可能，但**本版不进背包**。 | 避免依赖不存在的 G3 物品表；避免与既有 `worldFragment` 撞车 |
| 3 | 先知是每个小世界一个，还是全局唯一？ | **每个小世界各有一个**（世界内唯一）。先知**不存在于主世界**（`prophetAvailable` 只在 `role === 'child'` 为真），所以"全局唯一"在语义上不成立。当前实现按玩家位置动态摆放，恰好就是"该世界的那一个"。**多世界落地时的迁移要求**：`PROPHET_GUID` 必须改为**世界域化**（如按世界 id 派生 GUID），否则两个小世界会共用同一个 actor 身份，并把 `PROPHET_MODEL_REF` 的摆放哨兵值互相污染 → 届时 `contract.version` 必须 +1。 | `storyActors.mjs:55-58,78-84`；`storyCharacters.mjs:13-18` |
| 4 | 美术资源谁提供、放哪、什么格式？ | 见 §5.1 流程表与 §5.2 的 5 条硬性要求。**先放 `assets/model/`，再逐字节复制到 `game/art/models/prophet/`**；格式 `.dae/.obj/.gltf`，必须含动画且贴图全为本地相对路径。 | `test_story_assets.py:22-64` |

> 附带答复指南 §5 通用问题 12（是否要我本机跑一次构建与测试）：**已跑**，结果见 §0 与 §7。

---

## 7. 验收清单

**L-2 内容**
1. [x] 小世界内按 `F` 打开面板，第一行是 `你从小世界之外回来了。`（不再是占位文本之外的内容）
2. [x] `Enter`/`Space` 逐句推进，共 **5** 句，右下角显示"当前句 / 总句数"
3. [x] 最后一句按 `Enter` → 面板关闭
4. [x] 标题显示 `小世界的守望者`，提示 `F 交谈 · Enter 继续 · Esc 离开`
5. [x] 关闭后**立刻**再按 `F` 能再次打开（占祈健 §6.2 已修）

**L-3 契约**
6. [x] 改台词不升 `contract.version`；改距离/可用世界则必须升 —— 由测试锁定
7. [x] `range === 12` 且与 `storyDialogue.mjs` 的契约声明一致
8. [x] 主世界按 `F` 仍是拾取世界碎片（**最重要的一条回归**）

**L-1 参数**
9. [x] `aheadDistance`/`height`/`yawOffset` 由数据文件提供，取值与改动前**完全一致**
10. [x] `game/art/models/prophet/` 下文件未被增删改（哈希测试通过）

**部署与回归**
11. [x] `game/data/prophet.json` 进入部署链路（新增 `copy_game_data` + 部署测试）
12. [x] `python -B -m unittest discover -s game/tests -t .` 全绿
13. [x] `node --test game/tests/frontend/*.test.mjs` 全绿（不依赖 `vue` 的文件）
14. [x] `npm --prefix editor/Frontend run build` 通过
15. [ ] **实机验收**（自动化测试用模拟原生接口，不能替代）：进小世界 → 走到先知旁按 F →
    看到 5 句正式台词 → `Esc` 关闭 → 主世界按 F 仍拾取碎片

---

## 8. 明确不做（与任务表 §7 一致）

- ❌ 新的先知美术资源（需美术交付，见 §5.1）
- ❌ 分支对话 / 选项 UI（当前线性剧本；若要做需升 `contract.version` 并扩展 `normalizeDialogue`）
- ❌ 对话立绘、表情、音效、语音
- ❌ 对话进度持久化（剧本是静态只读内容，不是存档）
- ❌ 把脚本碎片做成可拾取/可组合物品（依赖不存在的物品表 G3）
- ❌ 骨骼挂点 C++ API（下一版）
- ❌ 多小世界下的先知 GUID 世界域化（等马鹏程的多世界结论）

---

## 9. 风险与遗留

| # | 风险 | 影响 | 处置 |
| --- | --- | --- | --- |
| 1 | 先知模型是 `dancing_vampire.dae`，造型可能与"守望者"气质不符 | 观感 | §5.2 给了替换判据；换模型零代码，只需资源进 `assets/` |
| 2 | "脚本碎片"将来若真要做成收集物 | 返工 | 已预留稳定 `fragment.id`，接物品表时不必重写剧本 |
| 3 | 多世界落地时先知 GUID 会互相污染 | 数据错乱 | §6 问题 3 已写明迁移要求与升版规则 |
| 4 | JSON 与 `.mjs` 双份内容 | 漂移 | 由同步测试强制一致，漂移即红 |
| 5 | `editor/Frontend/node_modules` 缺失导致 6 个测试文件无法加载 | 验证盲区 | 环境问题；`npm install` 后即可跑（本轮已安装并验证） |
| 6 | 前端构建失败被静默降级为 warning（`editor_copy_and_build.py:198-204`） | 产物可能缺 `dist` 而"构建成功" | 本轮已实测 `vite build` 成功；建议后续把该 warning 升级为失败 |

---

## 10. 交付物清单与接线点

**新建**

| 文件 | 作用 |
| --- | --- |
| `docs/ugc/李淳珺任务-先知NPC-实施计划.md` | 本文件 |
| `game/data/prophet.json` | 内容权威源：契约 + 摆位 + 剧本 |
| `game/frontend/storyDialogue.mjs` | 运行时投影与校验 |
| `game/tests/frontend/storyDialogue.test.mjs` | JS 契约测试 |

**单行接线点（不改逻辑、不改模板）**

| 文件 | 改动 |
| --- | --- |
| `game/frontend/prophetDialogue.mjs` | `PROPHET_DIALOGUE` 改为由 `storyDialogue.mjs` 提供（占位文本 → 正式剧本）；`PROPHET_INTERACTION.range` 由契约数据提供 |
| `game/frontend/storyCharacters.mjs` | 先知 `height: 1.8` → `PROPHET_PLACEMENT.height` |
| `game/frontend/storyActors.mjs` | 摆位字面量 `4` → `PROPHET_PLACEMENT.aheadDistance`；`+ Math.PI` → `+ PROPHET_PLACEMENT.yawOffset` |
| `tools/build/editor_copy_and_build.py` | 新增 `copy_game_data` 并在 `main()` 调用 |
| `game/tests/test_story_assets.py` | 新增 JSON↔mjs 同步测试、`game/data` 部署测试 |

**明确不改**：`editor/Frontend/src/views/layout/StoryWorld.vue`、`game/runtime/*.py`、
`game/core/*`、任何 C++、`.game/story-link.json` 结构。
