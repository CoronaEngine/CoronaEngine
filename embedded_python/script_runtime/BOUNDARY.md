# Script Runtime 边界

`script_runtime` 是受限角色脚本运行时。Web 为 Blockly/Scratch 生成自包含的 Python 文件，包括积木需要的变量、输入、广播、克隆和游戏逻辑。Web 将工作区作为文档提交给 Python 存档服务。Python 在 `Scripts/files` 原子发布文档及对应脚本。用户无需打开图面板即可运行已经保存的文件。

| 路径 | 职责 |
|---|---|
| `file_runner.py` | 接受 `scripts.runFile` 的项目会话、相对路径、SHA-256 和 `requestId`，校验文件，分配独立模块并调用 `run()`。 |
| `file_api.py` | 提供与本次执行绑定的输入、协作停止、运行标记和受限的 Actor 操作。 |
| `scene_state.py` | 在 Python 捕获、恢复并校验同一场景会话的运行前状态。 |
| `native_scene_adapter.py` | 受限原生场景适配器：把 16 条 `script_scene_*` 具名绑定映射成脚本可用的 API。 |

脚本不得绕过受限接口取得编辑器公共 API 的原生对象。存档与文件执行共享宿主解释器，每次运行拥有独立模块和输入处理器。C++ 与 Web 均可查询启动回执，并用 `runId` 停止指定运行。恢复失败时保留快照，重新加载或切换项目必须先完成恢复。

## 能力缺口（有意保留，不是遗留代码）

旧的 `native_engine_adapter.py` 探测 `set_mouse_locked` / `get_mouse_delta` / `ray_cast` /
`import_media` / `play_audio` / `stop_audio` 六个模块函数，C++ 侧一个都没绑定，所以它恒返回
`None`——已删除。相应地：

- **音频**：ScriptRuntime 契约里没有播放能力（Web 走 `corona.audio.*`）。`block_program.py`
  的音频积木是明确的空操作，每次运行只警告一次。
- **射线**：没有原生射线查询；`raycast_hit` 用运行时自己跟踪的 Actor AABB 求交。
- **鼠标锁定/位移**：没有原生指针捕获；保留脚本侧状态，位移来自输入事件累积。

要用上这些能力，需要先在 `script_runtime_api.h` 增加具名入口并在 C++ 实现，再回来接。
不要恢复"探测一个不存在的函数然后静默降级"的写法。