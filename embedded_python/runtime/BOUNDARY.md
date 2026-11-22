# `editor/runtime` 边界

`editor/runtime` 是编辑器 Python host 层（embedded Python host），负责启动、插件注册（插件 registry）、生命周期、项目
上下文和 host support；不负责 Vue transport、C++ 场景事实或角色脚本能力。

| 路径 | 职责 |
|---|---|
| `bootstrap.py`、`editor_host.py` | Python host 启动、服务调用入口、更新和关闭 |
| `registry.py`、`plugin_loader.py` | service 注册、加载、初始化和关闭顺序 |
| `project_context.py` | native 活动项目会话（`project_path` / `archive_session` / `read_only`）解析 |
| `archive/` | 场景归档、项目文档和脚本文件 |
| `native_engine.py`、`logging.py` | host support |

`runtime.registry` 是唯一 service registry，`ACTIVE_PYTHON_SCRIPT_SERVICES` 是正常启动
清单；ProjectArchive 按 lazy 生命周期初始化。跨层业务只有一条通道：`WebBridgeService`
加上服务白名单（`project_archive` / `project_files`）；`api/editor_api.py` 的 manifest
adapter 已随 script runtime 删除。

## 生命周期

C++ 主线程只负责启动和最终 join；启动之后，帧循环由 Script 线程里的 Python 拥有：

```
C++ 主线程                       Script 线程（唯一 Python 消费者）
─────────                       ────────────────────────────
initializeInterpreter
  Py_InitializeFromConfig
  PyEval_SaveThread    ← GIL 交还
  import main → bootstrap 模块级副作用：
    paths → config → logging → 注册 dispatcher
    → plugin_loader → registry（project_files 同步 import）
                                  ScriptSystem::update() → main.loop()（只进入一次，阻塞到循环退出）
                                    loop 内：
                                      ├ poll_service_requests()：约 4 ms 轮询队列，空则 sleep
                                      ├ update_runtime()：60 Hz（排空输入并重挂 watchdog）
                                      └ should_stop() 为真 → 退出循环
                                  finally: shutdown_runtime → registry.shutdown（2s 预算）→ stopped
```

`main.loop()` 退出后 Script 线程不会回到 C++ 帧循环：`update()` 的后继调用直接返回。停机是
一个标志加一次 join：`request_stop()` 只置 `should_stop()`，C++ 在 `stop_for(3s)` 里等待线程
结束；没有 `LifecycleControl` 跨线程请求握手，也没有 2.5 秒的等待预算。循环抛异常，或在没有
stop 请求的情况下提前返回，是 policy ③ 的失败：记录 phase 与 error（`host_loop_failed` /
`host_loop_exited_unexpectedly`），只降级、不请求引擎关机。`ProjectArchive` 等 lazy 服务在注册时
（bootstrap 的模块级副作用里）就由 `LazyPythonScriptService` 起后台线程 import；首次调用只会复用
这次已启动（或已完成）的加载，与宿主循环不共享控制流。

| 状态机 | 状态 | 规则 |
|---|---|---|
| `CoronaEditor._runtime_state` | created → initializing → initialized → running → stop_requested → stopping → stopped | 初始化幂等；关闭是终态，`start/initialize` 返回 False，`shutdown_runtime` 复用同一快照 |
| `LazyPythonScriptService` | cold → initializing → ready / degraded → stop_requested → stopped / stop_timeout | 未就绪时调用抛 `SERVICE_UNAVAILABLE`（可重试）；降级信息进 `event=runtime.service.degraded` |
| 停机 | `request_stop()` 置 `should_stop()` → `main.loop()` 退出 → `finally` 执行 `shutdown_runtime()` | 只置标志；C++ 用 `stop_for(3s)` join。没有跨线程请求握手，也没有 2.5 秒等待预算 |
| 超时 | 脚本线程 3s 内未退出 → `python.lifecycle.shutdown_timeout` 诊断 → `TerminateProcess` | 诊断含 lifecycle phase/error 与 coordinator 快照；`python_snapshot`（含 Python 线程列表）只在循环退出后记录，硬超时时是 `unavailable`；registry 自己的关闭预算是 2s，不是第二个等待层级 |
| 循环失败（policy ③） | `main.loop()` 抛异常，或未收到 stop 请求就返回 | 记录 phase/error（`host_loop_failed` / `host_loop_exited_unexpectedly`）；只降级，不请求引擎关机 |

关闭后到达的调用被拒绝为 `SERVICE_UNAVAILABLE`，不会被误报成"未知服务"；解释器不做
`Py_Finalize`，清理必须发生在 `shutdown_runtime` 内。

## 日志与错误

- 模块日志统一用 `logging.getLogger(__name__)`；事件行统一为
  `event=<dotted.name>` + `key=value`（生命周期 `runtime.*`、帧超时
  `python.lifecycle.overrun`、调用失败 `bridge.call.failed`），不再写只有人读的句子。
- 失败只有一个出口：`WebBridgeService` → `dispatch_script_values_from_cpp`。它把任何
  异常归一成 `{success: false, code, error}`，并记录
  `event=bridge.call.failed service=… function=… code=…`；消息只给人看，调用方只认
  `code`，上下游都不解析消息。
- 服务拒绝抛 `BridgeError(code, message)`；`code` 必须在
  `tests/tools/rejection_codes.py` 的词表内（`test_python_refusal_codes.py` 会检查）。
- 失败一律抛码，包括 C++ 直连的服务：`save`（`SAVE_FAILED` / `INVALID_ARGUMENT`）、
  `project_info`（`INVALID_PROJECT_PATH`）、`update_metadata`、`editor_settings`
  （`FILE_OPERATION_FAILED`）。唯一保留数据级结果的是"读出来了但需要人决策"这一类：
  `parse` 的 `invalid_archive` / `decision_required` + `diagnostics`，以及
  `validate_project` / `asset_operation` 的校验报告；调用方按数据解析，不当作拒绝。

## 会话身份

`read_project_session()` 每次向 native 模块拉取 `project_path` / `archive_session` /
`read_only`，不缓存：`archive_session` 由 Python 的 `parse` 生成、随后归 C++ 场景所有，
同路径重开也会换新值。三个字段分别对应 `NO_ACTIVE_PROJECT`、`PROJECT_CONTEXT_CHANGED`、
`READ_ONLY_PROJECT` 三个拒绝；旧 native 模块只有 `active_project_path` 时 fail-closed
并打一次 `event=runtime.session.binding_missing`。

`editor/main.py` 只做入口转发，`runtime/bootstrap.py` 负责一次性注册；注册和初始化必须
幂等，关闭后不得重复初始化。

旧 `backend`、`CoronaCore`、`CoronaPlugin`、`utils`、`scripts` 和 `runtime.legacy*` 已
删除，不提供外部兼容入口。关闭后不得在同一 host 生命周期中重新注册或复活 worker。

AI 已迁往 Web，不在 PYTHON_SCRIPT_SERVICES 中注册；本目录只维护其余 Python 宿主服务生命周期。
