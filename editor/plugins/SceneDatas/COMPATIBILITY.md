# `SceneDatas` 兼容插件壳

`plugins/SceneDatas` 只保留历史 `SceneDatas` 服务名和旧宿主注册点。Vue Object 面板使用
独立的 `Object` UI panel ID，不再依赖该服务名。它不是
新的 Scene API，也不拥有 Scene、Actor 或组件状态。

仓内 Object 面板已迁移到 manifest 聚合接口（`scene.*` / `sceneTools.*`）。
SceneDatas 注册壳仍仅用于外部兼容，不是仓内面板的数据或状态 owner；正常编辑器启动
不会自动注册该 Python 空壳，旧宿主必须显式请求 legacy service。

| 兼容入口 | canonical owner | 当前调用范围 | 删除条件 |
|---|---|---|---|
| `SceneDatas/main.py` 的 `SceneDatas` 注册壳 | `scene.get_snapshot`、`scene.set_actor_transform`、`sceneTools.*` manifest 聚合契约 | 旧 CEF 宿主和 Script Runtime 兼容注册；Vue Object panel 已使用独立 UI ID | native scene/project lifecycle 覆盖旧面板初始化、场景/Actor 绑定、读写、切换和关闭，并通过旧宿主回归 |

## 约束

- 壳只允许保留 `PluginBase.register_web("SceneDatas")` 和空服务类；
- 不在此目录新增 `scene_datas.*`、底层 `Actor`/`Scene` 对象或状态机；
- 新 Vue、普通 Python 和 AITool 代码必须使用 `scene.*`、`sceneTools.*` 或 `viewport.*`；
- Script Runtime 的旧 `scene_datas.*` 访问只能经过登记的受限 adapter；
- 删除前必须更新 `API_OWNERSHIP.md` 并通过 Frontend、runtime registry 和旧宿主回归。
