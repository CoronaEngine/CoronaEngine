# `editor/runtime` 边界

`editor/runtime` 是编辑器 Python host 层：负责嵌入式 Python 的启动、插件注册与
生命周期、公共 host support，以及迁移期 legacy adapter。它不是 Vue transport、
C++ 引擎对象 owner，也不是角色脚本执行环境。

## 文件职责

| 路径 | 类型 | 职责 | 删除/迁移条件 |
|---|---|---|---|
| `bootstrap.py`、`editor_host.py` | active host | Python host 启动、C++ dispatcher 和运行时生命周期 | 新 host 完整替代并完成启动/关闭回归 |
| `registry.py`、`plugin_loader.py`、`plugin_base.py` | active registry | 插件注册、按需加载、初始化/关闭顺序和兼容基础设施 | registry 生命周期被明确 host owner 吸收后删除 |
| `archive/` | active support | 项目归档解析及解析错误类型 | 所有调用方迁移到新的 archive owner 后删除 |
| `project_templates.py` | active project template support | ProjectLauncher 模板复制、模板路径规范化和项目 INI 初始化 | native project 创建完整覆盖模板初始化且旧宿主完成迁移后删除 |
| `compat/legacy_project_copy.py` | removed compatibility code | ProjectCopy 已集中到 `legacy_project_copy.py`，新调用传入显式 `data_root` | 如发现外部旧 import，按 canonical owner 迁移 |
| `project_copy.py` | removed compatibility code | 旧 runtime import 已删除 | 如发现外部旧 import，按 canonical owner 迁移 |
| `scene_support.py` | active scene support | 场景清单读写和 legacy 场景自动保存 | Script Runtime 与 legacy entity 完成新场景持久化迁移后删除 |
| `project_context.py` | active project context | 为 Runtime、Script Runtime 和插件解析活动项目路径；兼容同步仍只写入旧宿主状态 | 所有 legacy consumer 完成迁移且 native project context adapter 完整覆盖后删除 |
| `response_utils.py`、`native_engine.py` | active host support | 结构化响应和嵌入式原生模块解析 | 对应 host support owner 吸收后删除 |
| `network_sync_policy.py`、`logging.py` | active policy/support | 网络同步策略和 Python 日志配置 | 统一 host policy owner 吸收后删除 |
| `legacy_*.py` | compatibility adapter/shim | 通用 host legacy 适配和仍登记的插件/Script Runtime 兼容入口；专属实现归对应 owner 的 `compat` | 对应外部宿主/旧项目迁移且回归通过后删除 |
| `legacy/` | compatibility model | 历史 `Actor`、`Scene`、`Camera`、组件和 manager 的导入/运行兼容层 | 旧脚本与 legacy adapter 完成迁移后删除 |

## 依赖方向

- `runtime` 负责编辑器 Python host 和插件生命周期；业务插件的实现归
  `editor/plugins`，不应重新写入本目录；
- `registry.py` 是 Python service 注册与初始化的唯一 owner，`script_runtime` 仅作为
  受限脚本服务注册项，不应反向拥有编辑器插件 registry；
- `archive/`、host support 和 manifest adapter 只能输出结构化值/adapter 结果，不能
  把 `Camera`、`Actor`、`Geometry` 等原生对象暴露为 Vue 或普通编辑器 Python API；
- `legacy/` 和 `legacy_*.py` 是 compatibility，不得新增编辑器业务或新的公共 schema；
- 角色脚本能力归 `editor/script_runtime`，AITool/编辑器业务归对应 plugin，`runtime`
  只提供宿主、生命周期和明确登记的 adapter；
- 新增跨层能力必须先进入 C++ manifest/schema 和对应聚合 adapter，不能只在 legacy
  wrapper 中添加入口。

## 启动与关闭边界

- `editor/main.py` 只是 host-facing shim；实际一次性注册由
  `runtime/bootstrap.py` 负责；
- `runtime/bootstrap.py` 负责加载应用配置、配置日志、注册 core Python services、
  注册 dispatcher、安装退出处理，并调用 `runtime.plugin_loader.reimport()`；
- `runtime/plugin_loader.py` 只注册当前编辑器需要的 active services；旧 `SceneDatas`、
  `ProjectLauncher`、`FileManager`、`ProjectSettings`、`MainView` 和 Vision 兼容用的
  `SceneTools` Python services 必须通过
  `register_legacy_python_script_services()` 显式注册，不在 Vue/native 编辑器启动时自动创建；
- `runtime.registry` 通过注册集合保证 service 注册幂等，`CoronaEditor.initialize_runtime()`
  通过状态标记防止重复初始化；`show_log_on_js()` 只是已有 C++ 入口的幂等兼容调用；
- `runtime.editor_host` 负责每帧 Script Runtime 更新和统一关闭；插件不得自行创建
  第二套 registry、dispatcher 或 Python runtime；
- 关闭后 runtime 不自动重新注册。需要重新启动时必须由新的 Python host 进程/生命周期
  明确完成，不能在同一关闭状态中隐式复活 worker。

## 删除条件与验证

边界由 `editor/runtime/tests` 的 registry、host、legacy adapter 和本地目录测试验证。
删除 compatibility 代码前必须确认仓内及外部宿主调用方、旧项目脚本、启动/关闭和
回退路径；仅凭仓内搜索不到调用方不能直接删除。
