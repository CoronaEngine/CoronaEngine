# AITool SceneComposer 聚合场景适配迁移设计

## 背景

`editor/plugins/AITool/cai_extensions/agent/scene_composer.py` 的场景后处理仍直接
解析旧 Scene，并通过旧 Actor/Mechanics 对象写入变换和物理状态。这样会让 AITool
业务层依赖 legacy Scene 实体，违背 C++ 场景权威和聚合 manifest adapter 边界。

## 目标

- SceneComposer 后处理优先使用 C++ 聚合场景快照和值对象；
- 位置、旋转、缩放通过 `scene.set_actor_transform` 写入；
- 物理状态通过 `sceneTools.set_actor_physics` 写入；
- 旧宿主仍可运行，但只能由 `native_scene_state` 的集中 fallback 负责兼容；
- 保持现有壁挂、边界钳制、整平和短暂物理沉降行为不变。

## 设计

SceneComposer 使用 `native_actor_views` 获取 Actor 视图，并按
名称建立本地查找表。native 视图提供位置、旋转、缩放和值对象 Mechanics 更新；当
宿主不具备聚合 scene snapshot 时，该函数内部才回退到
`plugins.AITool.compat.legacy_aitool_scene_adapter`；旧的
`runtime.legacy_aitool_scene_adapter` 仅作为历史导入 shim 保留。
SceneComposer 不再导入或调用 `get_legacy_scene`。

物理操作采用与当前行为等价的部分更新：先关闭物理并设置阻尼/恢复系数，完成位置
修正后短暂开启，再关闭。旧 Actor 对象的 Mechanics 方法仅在集中 fallback 返回的
兼容对象上使用；native 视图使用同一聚合物理方法。

## 测试与验收

- AITool 边界测试确认 SceneComposer 使用聚合 Actor 视图且不直接调用 `get_legacy_scene`；
- native Actor view 的变换和 Mechanics 更新保持现有返回语义；
- 旧宿主 fallback 测试继续通过；
- 运行 AITool SceneComposer/SceneTools 相关测试、`compileall` 和 `git diff --check`；
- 完成后使用独立提交：`refactor(aitool): route scene composer through aggregate actors`。

## 非目标

- 不修改 Quasar 子模块；
- 不修改 manifest schema、C++ 协议或底层引擎 API；
- 不删除 `runtime/legacy_*` 或其他兼容目录；
- 不改变 SceneComposer 的布局算法和场景生成策略。
