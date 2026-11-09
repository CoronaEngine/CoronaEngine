# ProjectArchive facade 边界

`main.py` 是归档加载服务的 canonical owner，格式解析由 `runtime.archive` 负责。
`project.openProject` 只接受场景目录或其中的 `scene.ini`，不读取、迁移 `.scene` 或 `project.ini`。
Vision JSON 通过独立的 `project.importVisionScene` 原生入口导入，生成场景目录后再打开。

facade 处理 `load_policy`（prompt/degraded）及 parser 的结构化诊断：缺失资源可由用户
选择占位加载，格式错误和不支持的版本必须拒绝。服务按需初始化，通过
`archive_service_ready` / `service_initializing` 报告就绪状态。

Python parser 只返回值快照，不创建 Engine/Scene/Actor 对象。C++ 扫描模型依赖并维护内存场景。Python 的 `runtime/archive` 创建和保存场景、发布资源包与清单、读写项目文档和启动器设置。脚本中实例化和恢复 Actor 只使用内存绑定。

回归需覆盖目录与 scene.ini 的等价读取、旧格式拒绝、格式错误、缺失资源、load_policy
以及项目打开失败时保留当前运行态。
