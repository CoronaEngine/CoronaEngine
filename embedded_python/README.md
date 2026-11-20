# Corona Engine Editor

`embedded_python` 是嵌入引擎进程的编辑器 Python 运行时，不是独立 Web 后端。

Python 负责项目场景存档的解析、校验与显式保存，以及 Web 提交的文档和脚本字节的读写；
C++ 持有场景、Actor、资源、变换和 revision 的权威状态；Vue 负责页面、输入和结果展示。
跨层业务只能调用 C++ 的 typed V8 具名绑定，Vue 只做薄适配。Python 侧只有顶层宿主支持入口
（7 个 `m.def` + 载荷类型 `InputEvent`）与 `cef_py_bind.cpp` 的编辑器传输；编辑器语义
不经 Python 到达 C++。

## 入口文档

- [ARCHITECTURE.md](ARCHITECTURE.md)：层次、目录职责和依赖方向。
- [API_OWNERSHIP.md](API_OWNERSHIP.md)：公共业务语义的 owner 和调用规则。
- [config/BOUNDARY.md](config/BOUNDARY.md)：路径和运行时配置边界。
- [runtime/BOUNDARY.md](runtime/BOUNDARY.md)：Python host、registry 和生命周期。
- [plugins/BOUNDARY.md](plugins/BOUNDARY.md)：编辑器插件边界。

Vue 源码在 `CoronaEditor` checkout 里，与本目录一起部署到 `<exe>/CabbageEditor/`，
它自己的边界文档在那边。

## 目录职责

| 目录 | 职责 |
|---|---|
| `main.py`、`runtime/bootstrap.py` | C++ 的 import 入口和一次性注册 |
| `config` | 宿主启动路径和运行时配置值 |
| `runtime` | 嵌入式 Python host、服务注册、生命周期和运行时数据 |
| `runtime/archive` | 场景/项目的解析、校验与原子写盘 |
| `plugins` | ProjectArchive 存档门面，以及文件管理器使用的 Actor 模板 |

项目的 `Scripts/files` 保存 Web 提交的工作区文档和按摘要标识的 .py。旧 `api`、`backend`、
`CoronaCore`、`CoronaPlugin`、`utils`、`scripts`、`script_runtime` 目录已删除，新增代码
不得重新引入这些路径。

## API 规则

跨层契约按调用方分裂，互相不重叠：Web 使用 C++ 的 typed V8 具名契约
（`window.corona.*`，加一个 `corona.bridge` 进入 Python 服务）；Python 侧是顶层宿主支持
（7 个 `m.def` + `InputEvent`）与编辑器传输。两组的名单由
`tests/tools/python_surface.py` 钉死。JSON manifest、`api/editor_api.py` 与 18 条脚本契约
随脚本运行时一起删除；脚本对象模型（`CoronaEngine.script`）随后也删除。场景事实始终由
C++ 持有。
