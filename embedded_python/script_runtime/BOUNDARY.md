# Script Runtime 边界

`script_runtime` 是受限角色脚本运行时。Web 为 Blockly/Scratch 生成自包含的 Python 文件，包括积木需要的变量、输入、广播、克隆和游戏逻辑。Web 将工作区作为文档提交给 Python 存档服务。Python 在 `Scripts/files` 原子发布文档及对应脚本。用户无需打开图面板即可运行已经保存的文件。

| 路径 | 职责 |
|---|---|
| `file_runner.py` | 接受 `scripts.runFile` 的项目会话、相对路径、SHA-256 和 `requestId`，校验文件，分配独立模块并调用 `run()`。 |
| `file_api.py` | 提供与本次执行绑定的输入、协作停止、运行标记和受限的 Actor 操作。 |
| `scene_state.py` | 在 Python 捕获、恢复并校验同一场景会话的运行前状态。 |
| `manifest_adapter.py` | 受限 manifest adapter，主动查询场景、更新 Actor 和视口。 |
| `native_engine_adapter.py` | 提供音频、鼠标和射线查询等底层脚本能力。 |

脚本不得绕过受限接口取得编辑器公共 API 的原生对象。存档与文件执行共享宿主解释器，每次运行拥有独立模块和输入处理器。C++ 与 Web 均可查询启动回执，并用 `runId` 停止指定运行。恢复失败时保留快照，重新加载或切换项目必须先完成恢复。