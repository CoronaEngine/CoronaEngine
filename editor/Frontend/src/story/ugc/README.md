# 小世界（UGC World）模块交接说明

本目录只包含剧情模式的小世界运行时，不复用创造模式的 Dock、SceneBar、NodeGraph 或其他编辑器面板。小世界首版采用“建造 / 试玩”双模式，数据先在会话内隔离运行，显式保存成功后才提交主世界资源。

## 模块职责

- `ugcWorldState.js`：定义可序列化的小世界数据结构、版本、对象白名单、资源规范化和存档格式校验。
- `ugcWorldSession.js`：保存主世界快照，创建小世界会话，并隔离小世界状态和资源事务。
- `ugcWorldResourceService.js`：管理材料、世界碎片和对象成本。主世界借入资源只在保存成功后扣除。
- `ugcWorldBuilder.js`：建造模式下按 1 米网格放置 / 删除方块、出生点和目标，并负责碎片绑定。
- `ugcWorldLogicRunner.js`：解释受控触发器、条件和动作白名单，禁止 `eval`、`Function` 和任意脚本。
- `ugcWorldMode.js`：只管理 `build` 和 `play` 的状态切换。
- `ugcWorldScene.js`：创建独立 Three.js 场景并将纯数据同步成灰盒对象。
- `ugcWorldPersistence.js`：前端持久化边界，只调用 `editorApi.ugc`，不拼接绝对路径。
- `ugcWorldController.js`：组合会话、资源、建造、试玩、场景和存档，是 `StoryMode.vue` 的唯一业务入口。

## 主世界进入小世界

1. 剧情模式背包中的世界小球触发入口。
2. 控制器检查世界小球，并复制主世界玩家、摄像机、背包、Boss 和剧情状态快照。
3. 材料和世界碎片复制到小世界内部资源表，主世界数组不会被直接修改。
4. 创建独立 Three.js 场景，默认进入 `build` 建造模式。
5. `StoryMode.vue` 将当前渲染场景切换为小世界场景，并显示小世界专用 Overlay。

## 建造和试玩

### 建造模式（`build`）

允许放置 / 删除 `block`、`spawn`、`target`，选择材料和绑定世界碎片。所有消耗记录在资源服务的事务账本中，主世界背包仍保持不变。每个小世界最多 128 个对象。

### 试玩模式（`play`）

控制器从建造数据创建试玩副本。试玩中的目标生命、逻辑变量、完成状态和奖励不会写回建造数据；切回建造模式时直接丢弃试玩副本。WASD、鼠标视角、重力、跳跃、攻击和 F 键交互由剧情模式现有运行时接管。

## 资源事务和保存时机

- 放置对象时先从小世界内部库存扣除，并记录 `pendingQuantity`。
- 删除对象时只返还该对象成本一次，返还仍发生在小世界内部。
- 保存前调用 `prepareCommit()` 计算主世界背包的新副本。
- 文件写入成功后，才把新背包提交到主世界，并调用 `markCommitted()` 清空待提交账本。
- 文件写入失败时，小世界保持当前状态，主世界资源不变化，可以重新保存。
- 重复保存不会重复扣除已经提交的资源。
- 放弃退出时丢弃未保存的小世界变化，不写入存档，也不污染主世界。

## 正式存档位置

前端只发送 `worldId` 和 `worldData`：

```text
ugc.listWorlds
ugc.loadWorld
ugc.saveWorld
ugc.deleteWorld
```

引擎侧从当前活动创建世界项目目录取得根路径，最终文件固定为：

```text
<activeProjectPath>/ugc/worlds/<worldId>.json
```

例如：

```text
D:/CoronaEngine/CoronaEngine/projects/MyStoryWorld/ugc/worlds/ugc-world-001.json
```

路径创建、ID 安全校验、路径穿越防护、临时文件原子替换、损坏存档和版本错误均由 C++ `ugc_world_store` 负责。前端不使用 `localStorage` 作为正式存档，也不能指定保存根目录。

## 世界碎片逻辑

碎片只保存结构化数据，逻辑必须通过白名单校验。首版触发器：

```text
onStart
onInteract
onAttack
onTargetDefeated
onEnterArea
```

首版动作：

```text
setVariable
addItem
removeItem
showMessage
completeGoal
changeTargetState
```

非法逻辑会被标记为无效，运行器跳过该碎片并返回错误；不会执行任意 JavaScript。后续 NPC 对话、附魔效果、Blockly、节点图和脚本沙箱应继续扩展该数据协议，而不是绕过运行器。

## 后续扩展位置

- NPC 对话和附魔效果：扩展 `worldFragment.js` 数据模型及 `ugcWorldLogicRunner.js` 白名单。
- Blockly / 节点图导出：将编辑结果转换为结构化 `logic` 后交给 `validateUgcLogic()`。
- 更复杂的建造工具：扩展 `ugcWorldBuilder.js`，不要直接修改场景节点。
- 原生物理和摄像机：在 `ugcWorldScene.js` 与剧情模式运行时之间增加适配层，不改变状态和控制器接口。
- 真正的编辑器 UI：继续使用剧情模式专用组件，不把创造模式 Dock 引入小世界。

## 主要 UI 入口

- `UgcWorldOverlay.vue`：小世界总覆盖层。
- `UgcWorldToolbar.vue`：建造 / 试玩、保存、退出和状态。
- `UgcWorldResourcePanel.vue`：材料、碎片和对象操作。
- `UgcWorldPicker.vue`：新建或打开当前项目的小世界列表。

修改小世界功能时，优先从 `ugcWorldController.js` 进入，保证场景对象、UI 和主世界背包之间仍然保持边界。
