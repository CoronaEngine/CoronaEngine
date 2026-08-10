# API Ownership

manifest/schema 是跨层契约唯一来源；本文只说明 owner，不复制完整 schema。

| 语义 | C++ owner | Python adapter | Vue adapter |
|---|---|---|---|
| 项目与设置 | `project.*`、`projectSettings.*` | `CoronaEditorApi.project` / `project_settings` | `editorApi.project` |
| 场景与 Actor | `scene.*`、`sceneTools.*` | `CoronaEditorApi.scene` / `scene_tools` | `editorApi.scene` / `sceneTools` |
| 视口与相机 | `viewport.*`、`sceneTools.*` | `CoronaEditorApi.viewport` | viewport facade |
| 文件与资源 | `files.*`、`resource.*` | `CoronaEditorApi.files` / `resource_search` | files/resource facade |
| 网络与 LANChat | `network.*`、`lanChat.*` | `CoronaEditorApi.network` / `lan_chat` | network facade |
| 角色脚本能力 | Script Runtime binding | `script_runtime.native_engine_adapter` | 不直接使用 |

## 规则

1. C++ 持有场景、Actor、资源、变换和 revision 的权威事实。
2. Vue/Python 只能调用聚合契约，不直接暴露 `camera()`、`geometry()` 等底层对象。
3. Python 普通业务使用 `api.editor_api`；角色脚本和 Blockly 只能使用受限 Script Runtime
   adapter。
4. adapter 只负责参数/结果转换，不复制校验、状态机或错误码。
5. 新业务先登记 manifest，再实现各语言薄 adapter；禁止新增第二份 API schema。

仓内已放弃旧外部 import 兼容。`backend`、`CoronaCore`、`CoronaPlugin`、`utils`、
`scripts` 及 `runtime.legacy*` 均已删除；旧项目或插件必须迁移到 canonical owner。
