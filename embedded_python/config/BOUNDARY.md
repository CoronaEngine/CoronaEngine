# `editor/config` 边界

`editor/config` 是编辑器 Python 的配置 owner：提供宿主启动需要的路径值和运行时配置值，
不负责插件注册、活动项目状态、Scene/Actor 事实、AI secret 或 C++ transport。

## 文件职责

| 文件 | canonical 职责 | 当前状态 | 删除/合并条件 |
|---|---|---|---|
| `paths_config.py` | 路径 owner：只提供宿主持有的一个路径——编辑器（部署）根目录 | active | — |
| ~~`project_state.py`~~ | 已删除：没有被任何模块引用，且与真正在用的 ini 通道重复 | — | — |
| `app_config.py` | 应用级 `AppConfig` 单例与 `CORONA_*` 环境覆盖（默认值 + 环境变量，不读配置文件） | active | 应用 host 配置 owner 完整替代并通过启动/环境变量回归 |
| `runtime_config.py` | 不可变运行时配置值对象和默认值 | active support | 被统一 app config schema 吸收后删除 |

## 依赖方向

- `paths_config.py` 只解析路径，**不创建目录**：它没有 `mkdir`/`makedirs`，需要目录的
  写入点自己建父目录。启动期 import 不得产生文件系统写入，装在只读目录里也必须能启动
  （`tests/tools/test_paths_have_no_write_side_effects.py` 会检查）；
- `PathsConfig` 只保留 `repo_root`（`bootstrap` 用它扩展 `sys.path`）。项目内的
  media/models/screenshots 与识别数据库路径、以及 Frontend 入口（`frontend_dist`）
  在仓内都没有消费方，已删除，不要再加回来：前端由 CEF 在 `Utils::editor_directory()`
  下自己定位并加载，Python 不保存第二份；
- 活动项目路径的唯一 owner 是 `runtime.project_context`；配置模块不再提供第二份
  `_get_active_project_path()`；
- `CoronaEditor.ini` 只有**一条**访问通道：C++ 的 `editor_ini_path()` 决定路径，通过 `ProjectArchive.editor_settings` 交给 Python 读写；
  Python 侧不得自己拼这个路径（`tests/tools/test_editor_settings_owner.py` 会检查）；
- 项目复制的 runtime data 目录由 C++ `project.*` handler 统一解析和管理；配置模块
  不再提供 Python legacy data 目录 helper；
- `get_repository_assets_dir()` 只解析仓库随附的只读资源；运行时生成物和项目资源不得
  写回仓库 `assets/`；
- `app_config.py` 组合 `RuntimeConfig` 和 `PathsConfig`，不拥有活动项目持久化状态；
- 插件和 runtime host 可以读取 canonical config owner；
- AI 与密钥不属于本目录：AI 业务已迁往 Web 会话运行时，密钥由前端持有；宿主 Python
  不加载 `.env`，也不注册 AITool；
- 路径实例只能从 `config.paths_config.get_default_paths()` 获取。

## 删除条件与验证

边界由 runtime host 启动回归、`tests/tools/test_paths_have_no_write_side_effects.py`、
`tests/tools/test_editor_settings_owner.py` 和应用配置的环境变量回归共同验证，覆盖首次
启动、已有 `CoronaEditor.ini`、只读安装目录、环境变量覆盖和运行期路径解析。
