# Script Runtime 场景路由聚合契约设计

## 背景

`editor/script_runtime` 已通过 `scene.get_snapshot` 和
`scene.set_actor_transform` 使用受限的 Script Runtime manifest channel，但旧脚本的
场景列表、当前场景绑定和场景切换仍通过
`script_runtime.compat.legacy_scene_adapter` 访问 Python Scene Store。该 adapter 不能
在没有等价 native 契约前删除，也会使角色脚本运行时依赖旧 Scene 生命周期。

## 目标与非目标

目标是为 Script Runtime 提供场景路由和值对象级切换能力，使 host、生成脚本和 Blockly
可以逐步脱离 Python `Scene`/`Actor` 实体。新接口只返回结构化 JSON，不暴露引擎对象、
manager 或文件系统对象。

本设计不迁移旧 Scene 的实体兼容模型、Actor 代理方法或 SceneDatas 的物理操作；这些仍
由已有兼容边界承担，待独立 native contract 覆盖后处理。

Blockly 预览状态的第一阶段例外地覆盖环境值对象：`scene.get_environment` 和
`scene.set_environment` 提供 sun、grid、physics 的结构化读写。相机和 Actor 的预览
恢复分别使用已有 `viewport.set_camera_pose` 和 `scene.set_actor_transform`；旧 Scene
实体仍只用于不具备 native 快照的历史宿主回退。

## 契约

在已有 `scene.*` namespace 下新增两个仅允许 CEF 和 Script Runtime 调用的方法：

- `scene.list_routes()` 返回 `{scenes: [{path, name}], active_scene}`；结果来自 native
  project scene index，route 使用项目相对路径并保持现有 project.ini 规范化。
- `scene.switch(route)` 返回 `{status, scene, active_scene, actor_count, camera_count}`；
  成功后 native editor scene、active scene index 和后续 snapshot 使用同一路由。
- `scene.get_environment(scene_name)` 返回 `{scene, sun, grid, physics}` 值对象；
- `scene.set_environment(scene_name, state)` 接收同结构的部分状态并返回更新后的值对象。
  `state` 不包含引擎对象、Python Scene 或 Actor 引用。

请求参数和错误均由 C++ manifest schema 校验。不存在的 route、无法加载的场景和关闭中
请求返回结构化错误，不回退为 Python Scene 对象。

Python `ScriptRuntimeEditorApi.scene` 只封装这些 manifest 方法。host 和脚本运行时先
尝试 native route API；旧 Scene adapter 仅作为迁移期 fallback，并记录绑定模式，便于
回归确认 native 路径覆盖率。

## 数据流与生命周期

```text
Script Runtime host / generated script
        -> ScriptRuntimeEditorApi.scene
        -> ScriptRuntime caller channel
        -> C++ scene route manifest
        -> NativeEditorScene + project scene index
        -> structured result / snapshot
```

切换成功后，调用方更新自己的 scene route 和 target context；失败时保留原 context，
不创建 Python Scene，也不发布半成功状态。关闭期间 C++ handler 拒绝切换请求。

## 迁移顺序

1. 为 Python adapter、manifest 清单和 C++ handler 增加失败测试。
2. 增加 C++ route manifest 与 Script Runtime caller mask。
3. 迁移 `engine/host.py`、`corona_engine.py` 和 Blockly 的列表/切换调用。
4. 保留旧 adapter 作为显式 fallback，运行旧生成脚本、首场景绑定、切换、重载和关闭
   回归。
5. 当受支持流程不再进入 fallback 且外部旧脚本完成迁移后，删除
   `script_runtime/compat/legacy_scene_adapter.py` 及其历史 shim。

## 验证

- Python adapter 验证方法名、参数和 Script Runtime channel。
- C++ source/contract 测试验证 caller mask、route payload 和错误结果。
- Script Runtime 测试覆盖首次绑定、有效/无效切换、空项目、切换失败、重载和关闭。
- 完整 editor Python 回归必须保持通过；应用内验证生成脚本与 Blockly 的场景切换结果。
