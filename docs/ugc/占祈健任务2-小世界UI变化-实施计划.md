# 占祈健任务 2 实施计划 · 进入小世界后的 UI 变化

> 任务：**小世界内部 — 占祈健 — 10月4日20时 — 第 2 项：进入小世界后的 UI 变化**
> 写入日期：2026-10-03
> 唯一改动文件：`editor/Frontend/src/views/layout/StoryWorld.vue`
> 是否改 C++：**否** ｜ 是否需要新后端：**否**

---

## 0. 实施状态：✅ 已完成（2026-10-03）

| 验证项 | 结果 |
| --- | --- |
| `node --test "tests/js/*.test.mjs"` | **121 / 121 通过**（新增 3 个 HUD 测试） |
| ESLint（改动文件） | 无新增问题（7 → 6 条，剩余均为**改动前已存在**） |
| `vite build --configLoader runner` | **成功**（`StoryWorld-*.js` 正常产出） |

**实施中与初版计划的 1 处偏差（已获确认）**：
初版建议小世界**保留生命条**、只移除怒气与技能。实际实施为**整块 `.player-vitals` 隐藏**，并新增 `.subworld-notice` 卡片补位。
理由：`playerHp` 是写死的配置常量、玩家没有受伤逻辑，小世界里显示一个永远满血的血条没有信息量。
（详见 §3、§5.2）

---

## 1. 结论先行

这项任务**技术上没有阻塞**：判断"我在哪个世界"所需的字段已经在手上了。

真正的工作量不是"造功能"，而是**把现有 HUD 按世界拆成两套显示规则**，并且处理好 4 个容易漏的细节（见 §4）。

**预估工时：4~6 小时**（含自测），是四个任务里最稳的一个。

---

## 2. 现状盘点：现在进入小世界会发生什么

### 2.1 已有的数据来源（不需要新增任何字段）

Python 每次响应都会带上 `role`：

- [story_gameplay.py:226-227](CoronaEngine/game/runtime/story_gameplay.py#L226-L227)
  ```python
  def response(status='ok', **extra):
      return {'status': status, 'role': role, 'state': state, 'config': dict(CONFIG), **extra}
  ```
- `role` 取值只有 `'main'` / `'child'`，由 [subworlds.py:91-120](CoronaEngine/game/runtime/subworlds.py#L91-L120) 的 `_relation()` 判定
- 前端 `accept()` 已把它存进状态 → [storyGameplay.mjs:64-70](CoronaEngine/game/frontend/storyGameplay.mjs#L64-L70)
  ```js
  data = { state: response.state, config: response.config, role: response.role };
  ```
- `StoryWorld.vue` 里已多处直接使用 `gameplayState.role`（背包按钮文案、提示 tooltip）→ [StoryWorld.vue:446-451](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L446-L451)

**所以"UI 变化"的驱动信号是现成的，只差把它系统性地用到整个 HUD 上。**

### 2.2 世界切换时组件会重建（这点决定了架构）

按 O/P 切换时走的是 `projectLauncherService.openProject()`，它**先停旧世界运行时**（[projectLauncherService.js:45-68](CoronaEngine/editor/Frontend/src/services/projectLauncherService.js#L45-L68)），源组件在加载态被卸载，新世界重新挂载。

含义（好消息）：
- `gameplayState` 在新世界是**全新对象**，不存在"上一个世界的 UI 残留"
- 不需要写跨世界的状态重置逻辑
- **但**：切换期间有一段时间 UI 处于"旧数据 + 正在跳转"，必须防闪烁（见 §4.3）

### 2.3 小世界里现在实际被禁用的功能（已有，但 UI 没体现）

| 功能 | 代码里的现状 | 位置 |
| --- | --- | --- |
| 技能 | 返回 `reason: '小世界禁用'` | [storyGameplay.mjs:132](CoronaEngine/game/frontend/storyGameplay.mjs#L132) |
| 攻击 | `data.role !== 'main'` 直接拒绝 | [storyGameplay.mjs:119](CoronaEngine/game/frontend/storyGameplay.mjs#L119) |
| 拾取碎片 | 仅主世界有 drop | [storyGameplay.mjs:157](CoronaEngine/game/frontend/storyGameplay.mjs#L157) |
| Boss | `combatOnly` 下小世界不创建 Boss | [storyActors.mjs:55-57](CoronaEngine/game/frontend/storyActors.mjs#L55-L57) |
| 先知 | **仅小世界创建** | [storyActors.mjs:55-57](CoronaEngine/game/frontend/storyActors.mjs#L55-L57) |

**问题**：这些禁用逻辑全在 JS 层，但 UI 照样把 Boss 血条、怒气条、技能格渲染出来（技能格只是变灰）。玩家看到一堆用不了的东西。

这就是本任务要解决的核心体验问题。

---

## 3. 目标 UI 对照表

| HUD 区块 | 模板位置 | 主世界 | 小世界 | 处理 |
| --- | --- | --- | --- | --- |
| 位置指示（**新增**） | — | 显示「主世界」 | 显示「小世界 · <名称>」 | 新增 |
| Boss 血条 | [L381-388](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L381-L388) | 靠近时显示 | **不显示** | `v-if` 加 `!inSubworld` |
| 操作提示（左侧） | [L389-399](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L389-L399) | 完整 9 条 | **过滤掉战斗项** | 逐条加条件 |
| 生命条 | [L407-408](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L407-L408) | 显示 | **随整块移除** | 见 §0 偏差说明 |
| 怒气条 | [L409-412](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L409-L412) | 显示 | **不显示** | `v-if` 加 `!inSubworld` |
| 技能格 | [L414-421](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L414-L421) | 显示 | **整块不显示** | `v-if` 加 `!inSubworld` |
| 背包标题 | [L428](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L428) | 「背包」 | 「背包 · <小世界名>」 | 动态文案 |
| 背包底部按钮 | [L445-453](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L445-L453) | 「小世界」 | 「返回主世界」 | **已有，不动** |
| 拾取提示 | [L396](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L396) | 靠近碎片时显示 | 不显示（条件天然为假） | 不动 |

### 新增：位置指示器

建议放在**右上角**（`top: 26px; right: 24px`），理由：
- 左上被 `story-controls` 占、顶部中央被 `boss-status` 占、底部中央被 `story-bottom-stack` 占
- 右上角是唯一空闲区域
- 用 `role="status" aria-live="polite"`，世界切换时能被读屏播报

视觉风格沿用现有 `#9b8049` 描边 + 深色半透明底，不要引入新配色。

---

## 4. 必须处理的 4 个细节（漏了会出 bug）

### 4.1 【必须】`inSubworld` 从 `gameplay` 读取，不要读 `gameplayState.value`

`gameplayState.value` 只在 `current()` 为真时更新 → [StoryWorld.vue:91](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L91)。切换过程中它可能是 `null` 或旧值，用它驱动 UI 会闪。

**正确做法**：派生自 `gameplay.data`（`onState` 之外也始终有效），并保留 `gameplayState` 做渲染守卫：

```js
// gameplayState 仍用于 v-if 渲染门控；inSubworld 用 gameplay.data 避免切换期间闪烁
const inSubworld = computed(() => gameplay?.data?.role === 'child');
```

> 注意：`gameplay` 是普通对象（非 reactive），`computed` 不会自动追踪它。
> 因此需要用一个轻量 tick 触发重算——最简单的是复用已有的 `cooldownTick` 机制，
> 或在 `onState` 回调里同时写一个 `roleRef`：
> ```js
> const roleRef = ref(null);
> // onState 里：roleRef.value = data.role;
> const inSubworld = computed(() => roleRef.value === 'child');
> ```
> **推荐后者**，依赖清晰、可测试。

### 4.2 【必须】小世界名称必须有回退值

任务 ①（马鹏程）的命名功能未必与你同时完成，**你不能依赖它**。

```js
const worldLabel = computed(() => inSubworld.value
  ? `小世界${subworldName.value ? ` · ${subworldName.value}` : ''}`
  : '主世界');
```

`subworldName` 先用 `null`，等马鹏程的接口就绪再接。**这样你和他可以并行，不互相阻塞。**

### 4.3 【必须】切换期间不要显示"小世界"还是"主世界"的确定态

`navigationPending` / `navigation.busy` 期间，UI 应保持中性（不切换区块），否则会看到"先变主世界、再变小世界"的跳变。

```js
const transitioning = computed(() => navigation.busy || navigationPending.value);
```

在位置指示器上体现为：`transitioning` 时显示「切换中…」，其余区块沿用当前 `inSubworld` 值（此时旧值就是对的，因为组件还没重建）。

### 4.4 【必须】键盘提示文案要跟着变

[L397-398](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L397-L398) 的 `Tab 背包` / `Esc 保存退出` 在小世界里语义不同（`Esc` 在背包打开时是关背包，否则才是退出）。顺手把提示改准确，成本极低但体验提升明显。

---

## 5. 具体改动清单（逐处）

### 5.1 `<script setup>` 增加

```js
import { computed } from 'vue';   // 补进现有 import（当前是 onMounted, onUnmounted, ref, nextTick）

const roleRef = ref(null);
const subworldName = ref(null);   // 预留：等小世界命名接口
const transitioning = computed(() => navigation.busy || navigationPending.value);
const inSubworld = computed(() => roleRef.value === 'child');
const worldLabel = computed(() => transitioning.value
  ? '切换中…'
  : (inSubworld.value ? `小世界${subworldName.value ? ` · ${subworldName.value}` : ''}` : '主世界'));
```

在已有 `createStoryGameplay({ onState })` 里补一行：

```js
onState: data => {
  roleRef.value = data.role;          // ← 新增
  if (current()) { gameplayState.value = data; updateProximity(); }
},
```
→ 改 [StoryWorld.vue:91](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L91)

### 5.2 `<template>` 修改

**a) 位置指示器（新增，放在 `.story-hud` 内、`boss-status` 之前）**

```html
<div class="world-indicator" role="status" aria-live="polite">{{ worldLabel }}</div>
```

**b) Boss 血条** — [L381](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L381)
```html
<section v-if="bossNearby && !inventoryOpen && !inSubworld" class="boss-status" ...>
```

**c) 操作提示逐条过滤** — [L389-399](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L389-L399)

| 提示 | 主世界 | 小世界 |
| --- | --- | --- |
| WASD / 鼠标 / Space / Shift | ✅ | ✅ |
| 左键 攻击 | ✅ | 移除 |
| E / R 技能 | ✅ | 移除 |
| F 拾取世界碎片 | ✅ | 移除（小世界没有碎片） |
| Tab 背包 / Esc 保存退出 | ✅ | ✅（文案微调） |

**d) 怒气条 + 技能格** — [L409-421](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L409-L421)
```html
<div v-if="!inSubworld" class="vital-label"><span>怒气</span>...</div>
<div v-if="!inSubworld" class="vital-bar rage" ...>...</div>
<div v-if="!inSubworld" class="skill-strip" ...>...</div>
```

**e) 背包标题** — [L428](CoronaEngine/editor/Frontend/src/views/layout/StoryWorld.vue#L428)
```html
<h1 id="inventory-title">{{ inSubworld ? '背包 · 小世界' : '背包' }}</h1>
```

### 5.3 `<style scoped>` 增加

```css
.world-indicator { position: absolute; top: 26px; right: 24px; padding: 6px 12px;
  border: 1px solid #806b3d; background: #13130dd4; font-size: 12px; letter-spacing: 2px;
  text-shadow: 0 1px 3px #000; }
@media (max-width: 800px) { .world-indicator { top: 18px; right: 12px; font-size: 11px; } }
```

沿用现有 `#806b3d` / `#13130d` 配色，与 `.story-controls` 保持一致。

---

## 6. 验收清单

**功能**
1. [ ] 主世界 HUD：Boss 血条、怒气、技能格、完整操作提示都在
2. [ ] 小世界 HUD：Boss 血条**不出现**、怒气**不出现**、技能格**不出现**
3. [ ] 小世界操作提示只剩移动/跳跃/背包/退出，**没有**攻击和技能项
4. [ ] 位置指示器：主世界显示「主世界」，小世界显示「小世界」（或带名称）
5. [ ] 背包标题随世界变化

**边界（重点）**
6. [ ] 切换过程中指示器显示「切换中…」，**没有先主后小的跳变**
7. [ ] 小世界里打开背包，底部仍是「返回主世界」且可点击
8. [ ] 主世界 Boss 未死时，按钮仍禁用且 tooltip 正常
9. [ ] 小世界里按 E/R **不会**触发技能请求（现有逻辑已保证，回归确认）
10. [ ] `Tab` 开背包 → `Esc` 关背包 → 再 `Esc` 才退出，文案与实际行为一致

**回归**
11. [ ] 窄屏（≤800px）位置指示器不与其他元素重叠
12. [ ] 读屏能播报世界切换（`aria-live`）
13. [ ] 进小世界再返回主世界，HUD **完全恢复**（Boss 血条、怒气、技能格重新出现）
14. [ ] `npm --prefix editor/Frontend run build` 通过

---

## 7. 不做的事（划清边界）

- ❌ 不改 `subworlds.py` / `story_navigation.py`（那是任务 ①）
- ❌ 不新增 Vue 路由或页面（子世界复用 `StoryWorld.vue`，见 [game/README.md:124](CoronaEngine/game/README.md#L124)）
- ❌ 不实现小世界命名（用 `subworldName` 预留位，等马鹏程）
- ❌ 不改 C++，不改 Python
- ❌ 不碰第一人称/俯视角（那是你的任务 3）

---

## 8. 与其它任务的接口

| 对方 | 你需要什么 | 约定 |
| --- | --- | --- |
| 马鹏程（任务 ①） | 小世界名称字符串 | 先用 `subworldName = ref(null)` 占位，他给就接，**不阻塞** |
| 你自己（任务 3 视角） | 无耦合 | 视角切换是相机逻辑，与 HUD 分支互不干扰 |
| 你自己（任务 1 交互） | F 键提示文案 | 小世界里 F 改为"与先知交互"，届时替换拾取那条提示 |

---

## 9. 风险

| 风险 | 等级 | 应对 |
| --- | --- | --- |
| `computed` 追踪不到非响应式 `gameplay` | 🟠 中 | 用 `roleRef` 显式写入（§4.1） |
| 与马鹏程同时改 `StoryWorld.vue` | 🟠 中 | 你是该文件主改人；他只提供名称接口 |
| 切换期间 UI 跳变 | 🟡 低 | `transitioning` 中性态（§4.3） |
| 小世界名称为空导致显示「小世界 · 」 | 🟡 低 | `worldLabel` 里做条件拼接（§4.2） |

---

## 10. 建议执行顺序

1. **0.5h** — 加 `roleRef` + `inSubworld` + `worldLabel`，位置指示器先跑起来（能看到就赢一半）
2. **1h** — 按 §5.2 加 `v-if`，把 Boss/怒气/技能格按世界拆开
3. **1h** — 操作提示逐条过滤 + 背包标题
4. **1h** — 处理 §4.3 切换中性态、§4.4 文案
5. **1.5h** — 走完 §6 的 14 条验收，跑一次 `npm run build`
