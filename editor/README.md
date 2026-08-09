# Corona Engine Editor

`editor` 是嵌入引擎进程的编辑器运行时和 UI 工程，不是独立 Web 后端。

Python 负责 AI/Agent、角色脚本、Blockly/Scratch 和自动化；C++ 持有场景、Actor、资源、
变换和 revision 的权威状态；Vue 负责页面、输入和结果展示。跨层业务只能调用同一份
C++ manifest 聚合契约，Vue/Python 只做薄适配，不直接暴露 `camera()`、`geometry()` 等
引擎底层对象。

## 入口文档

- [ARCHITECTURE.md](ARCHITECTURE.md)：层次、目录职责和依赖方向。
- [API_OWNERSHIP.md](API_OWNERSHIP.md)：公共业务语义的 owner 和调用规则。
- [config/BOUNDARY.md](config/BOUNDARY.md)：路径、活动项目和配置边界。
- [runtime/BOUNDARY.md](runtime/BOUNDARY.md)：Python host、registry 和生命周期。
- [script_runtime/BOUNDARY.md](script_runtime/BOUNDARY.md)：受限角色脚本与 Blockly 运行时。
- [plugins/BOUNDARY.md](plugins/BOUNDARY.md)：编辑器插件边界。
- [Frontend/src/BOUNDARY.md](Frontend/src/BOUNDARY.md)：Vue 源码目录和依赖方向。

## 目录职责

| 目录 | 职责 |
|---|---|
| `Frontend` | Vue 页面、输入交互、JS manifest adapter |
| `api` | Python 对 C++ manifest 的唯一公共契约 adapter |
| `config` | 路径、活动项目和应用配置 |
| `runtime` | 嵌入式 Python host、服务注册和生命周期 |
| `script_runtime` | 受限角色脚本、Blockly 编排和生成脚本执行 |
| `plugins` | MainView、SceneTools、AITool、ProjectLauncher 等业务插件 |
| `data` | native 项目运行时数据，不是 Python 业务 owner |

`runtime/generated` 只保存 Blockly/Scratch 生成物；旧 backend、CoronaCore、CoronaPlugin、
utils 和 scripts 兼容目录已删除，新增代码不得重新引入这些路径。

## API 规则

C++ manifest 是契约唯一来源。Python 使用 `api.editor_api.CoronaEditorApi`，Vue 使用
Frontend manifest facade，Script Runtime 使用受限 `script_runtime.manifest_adapter`。
三者共享 schema，但不复制业务状态机；场景事实始终由 C++ 持有。
