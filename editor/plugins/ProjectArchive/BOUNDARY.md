# `ProjectArchive` facade 边界

`plugins/ProjectArchive` 是归档迁移服务的 Python compatibility facade，不是归档格式解析器
的 owner。实现位于 `compat/legacy_project_archive.py`，根目录 `main.py` 仅保留历史注册
入口。
它负责把请求转换为 `runtime.archive` parser 调用，并在跨层边界统一 load policy、诊断
和状态返回；归档 service 在 runtime 中按需 lazy 初始化，前端通过
`archive_service_ready` 和 `service_initializing` 等状态等待，不阻塞编辑器启动。
归档解析、快照规范化和格式校验由 `runtime/archive` 负责。

## Owner 映射

| 责任 | canonical owner | ProjectArchive 允许做什么 |
|---|---|---|
| 归档路径读取、格式校验、快照生成 | `runtime.archive` | 调用 `parse_archive`，不复制解析规则 |
| 迁移请求入口 | `project.migrateLegacyScene` | 作为 `ProjectArchive` 服务注册和 facade |
| `load_policy` | `compat/legacy_project_archive.py:ProjectArchive.parse` | 处理 `prompt`/`degraded` 选择和非法策略 |
| 诊断状态 | `compat/legacy_project_archive.py:ProjectArchive.parse` | 将 parser diagnostics 映射为 `decision_required`、`ready_degraded` 或错误结果 |
| Engine/Scene/Actor 状态 | C++ manifest/domain handler | ProjectArchive 不创建或修改引擎对象 |

## 约束

- 不在插件中新增归档格式解析、Scene/Actor 实体或项目权威状态；
- 不直接导入 `CoronaCore`、`scene_manager` 或底层 Engine API；
- parser 的结构化诊断必须完整保留，facade 只能增加跨层状态，不得静默丢弃错误；
- Vue/Python 新调用应使用 `project.migrateLegacyScene` 对应 adapter，不以插件文件路径
  作为第二份公共 API 定义。
- `main.py` 只能作为兼容导入和注册入口；新的归档 facade 逻辑必须放在 compat owner，
  不得重新写入根目录。

## 迁移条件

将归档迁移完全纳入 native `project.migrateLegacyScene` handler 前，必须通过：

1. 合法、损坏、可恢复缺失资源和不支持版本的归档回归；
2. `prompt` 与 `degraded` 两种 load policy 的返回语义回归；
3. parser diagnostics、快照 schema 和 C++ handler 的错误/事件映射回归；
4. MainView、ProjectLauncher 和旧宿主的项目打开流程回归。
