# `editor/script_runtime` 边界

`editor/script_runtime` 是受限角色脚本运行时和 Blockly/Scratch 执行层。它负责
生成脚本 runner 加载、脚本生命周期、受限引擎原子能力、Blockly 编译/合同检查，以及面向
编辑器状态的显式 manifest adapter。它不是编辑器公共 API、Vue service、AITool
runtime 或 C++ 原生对象的直接导出层。

## 文件职责

| 路径 | 类型 | 职责 | 删除/迁移条件 |
|---|---|---|---|
| `runner.py` | active execution boundary | 从 `runtime/generated` 加载生成脚本，并保留旧 `backend` 输出只读回退 | 所有旧项目迁移且生成脚本回归通过后移除 fallback |
| `manifest_adapter.py` | active aggregate adapter | 以独立 ScriptRuntime caller 通道访问 scene 路由、快照、环境、变换、scene_tools 和 viewport 聚合能力 | 新 manifest adapter 完整替代并完成权限/生命周期回归 |
| `native_engine_adapter.py` | active restricted adapter | 将角色脚本需要的原子引擎能力限制在 Script Runtime 边界内 | 新受限 binding 完整替代并完成脚本回归 |
| `engine/host.py` | active host lifecycle | 由编辑器 host 调用 ScriptsManager 的 canonical 初始化编排；旧 Scene 查询仍通过登记的 scene adapter | 旧项目脚本迁移到 native scene/project 生命周期后移除 legacy scene fallback |
| `compat/legacy_scene_adapter.py` | compatibility adapter | 旧 Python Scene Store 的唯一 Script Runtime 兼容实现；仅提供既有查询、切换和 Actor 查找转发 | 旧生成脚本和宿主迁移到 native scene/project 生命周期后删除 |
| `compat/legacy_scene_datas_adapter.py` | compatibility adapter | 旧 SceneDatas/脚本场景语义的唯一兼容实现；根目录旧路径仅作 shim | 旧脚本和宿主迁移后删除 |
| `engine/` | active runtime core | `CoronaEngine` 运行时 facade、脚本实体、ScriptsManager 和脚本生命周期 | 新运行时完整替代并完成角色脚本回归 |
| `engine/contracts.py` | active internal contract | Script Runtime 脚本生命周期所需的 Scene/Actor 结构化类型契约；不定义场景事实或运行时 API | 被统一的 Script Runtime value-object contract 吸收后调整；不得重新导入 `runtime.legacy.entities` |
| `blockly/` | active compiler/contract | Blockly/Scratch 生成执行合同和 AI 节点图输入合同检查；项目 workspace、持久化脚本和 manifest 写入 `<project>/Scripts/blockly` | 新编译器/合同 owner 替代并完成旧 workspace 回归 |
| `tests/` | active boundary tests | runner、adapter、生命周期、合同和关闭行为测试 | 随运行时 owner 一起迁移，不承载业务实现 |

## 依赖方向

- 项目 Blockly workspace、持久化脚本和 manifest 只能写入活动项目的
  `Scripts/blockly`；预览执行所需的临时生成脚本由 `generated_script_dir` 写入
  `runtime/generated`；`backend` 只能作为旧输出只读 fallback；
- 角色脚本音频按项目资源优先解析；仓库随附音频 fallback 只能通过
  `config.paths_config.get_repository_assets_dir()` 读取，不得把生成资源写入仓库目录；
- `manifest_adapter.py` 是脚本访问编辑器聚合语义的唯一显式 adapter，保持
  `ScriptRuntime` caller 通道和权限边界；
- `scene.list_routes` 和 `scene.switch` 只返回/接收结构化 route 值，不暴露
  Python Scene、Actor 或 manager；`corona_engine.setScene` 优先使用 native route，旧
  Scene Store 仅保留迁移期 fallback；
- `scene.get_environment` 和 `scene.set_environment` 只传递 sun、grid、physics 值对象；
  Blockly 预览快照、环境恢复、相机姿态和 Actor transform 优先走 native contract，
  旧 Scene adapter 仅处理未覆盖的历史快照；
- `compat/legacy_scene_adapter.py` 是 Script Runtime 旧 Scene fallback 的实现 owner；
  历史的 `runtime/legacy_script_scene_adapter.py` 已删除，不得恢复第二个实现入口；
- `compat/legacy_scene_datas_adapter.py` 是 Script Runtime 旧 SceneDatas fallback 的实现
  owner；`legacy_scene_datas_adapter.py` 仅为历史导入路径的兼容 shim，不得新增实现；
- `engine/host.py` 是 ScriptsManager host 初始化的 active owner；历史的
  `legacy_script_runtime_adapter` 导入 shim 已删除，host 只能负责生命周期编排，不得复制
  初始化逻辑；
- `engine/` 中的 `CoronaEngine.*` 是角色脚本/Blockly 的受限命名空间，不是 Vue、
  AITool 或普通编辑器 Python 的公共 API；
- 不得从本目录导入 `editor.plugins`、`backend` 或 raw CEF transport；编辑器插件由
  `runtime.registry` 管理，Vue 通过 `src/api/editorApi.js` 调用公共契约；
- 不得把 `Camera`、`Actor`、`Geometry` 等底层对象重新暴露给编辑器公共层。需要编辑器
  语义时只能新增受限聚合 adapter，并同步 manifest caller mask；
- `compat/legacy_scene_datas_adapter.py` 只能作为登记过的 compatibility fallback，不得在其
  上继续新增业务能力。

## 删除条件与验证

边界由 `editor/script_runtime/tests` 的 runner、adapter、合同和生命周期测试验证。
删除或收紧任何旧能力前必须覆盖旧生成脚本、角色脚本、Blockly workspace、运行中止、
重载和关闭场景；不能仅凭仓内没有 import 就删除外部宿主可能依赖的兼容入口。
