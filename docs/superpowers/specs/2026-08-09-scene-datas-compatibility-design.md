# SceneDatas 兼容层收敛设计

## 目标

将仓内 Object 面板的事实访问和写入统一到现有 `scene.*`、`sceneTools.*` 与
`projectSettings.*` manifest 聚合接口，同时保留 `SceneDatas` 旧服务名，避免外部
CEF 宿主和历史 Script Runtime 立即失效。

## 当前证据

- `editor/Frontend/src/views/sidebar/Object.vue` 已使用 `editorApi.scene` 和
  `editorApi.sceneTools`，不调用 `scene_datas.*`。
- `editor/plugins/SceneDatas/main.py` 只是旧入口 wrapper；实际类只注册历史
  `SceneDatas` 服务名，不持有场景状态。
- C++ 仍保留 `scene_datas.*` manifest 方法，调用者限制为 CEF 和 Script Runtime，
  用于外部旧宿主及受限角色脚本兼容。

## 设计决策

1. 不新增 `SceneDatas` 原生 API，也不复制 `scene.*` / `sceneTools.*` schema。
2. 将“仓内 Object 面板迁移”标记为完成；`SceneDatas` 目录保持 compatibility-only。
3. 保留 Python 注册壳、旧 manifest 方法和 Script Runtime adapter，直到外部宿主确认
   不再依赖历史服务名和 `scene_datas.*`。
4. 新增边界测试，禁止 Vue、普通编辑器 Python、AITool 和新插件生产代码导入或调用
   `scene_datas.*`；Script Runtime 兼容 adapter 是唯一例外。

## 删除条件

删除 `SceneDatas` 注册壳和旧 manifest 方法前，必须确认：

- 外部 CEF 宿主不再注册或查询 `SceneDatas`；
- 历史脚本已迁移到受限 Script Runtime 聚合 adapter；
- Object 面板和场景编辑回归通过；
- 删除提交包含兼容测试、发布说明和可回退路径。

## 验收

- Object 面板继续读取、修改、保存 Actor 属性；
- 仓内生产代码不新增 `scene_datas.*` 调用；
- 旧入口导入和 Script Runtime 兼容 adapter 行为保持不变；
- 相关边界测试、`git diff --check` 通过。
