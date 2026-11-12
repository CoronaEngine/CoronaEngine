# Web 接口层的决定记录

契约本身是生成的，见 [interface-contract.md](interface-contract.md)：8 个命名空间、58 个方法，
以及三个边界和失败码表。这份文档只记**为什么**它长成这样——哪些看似能合并的东西合并了，哪些
被明确否决并保留。

## 1. 三条边界

| 边界 | 形态 | 规模 |
|---|---|---|
| Web → C++ | `window.corona.<ns>.<method>`，typed V8 | 8 命名空间 / 58 方法 |
| Web → Python | `corona.bridge.call(信封)`，JSON | 3 个服务 + bridge |
| C++ → Python | `python_service_bridge` | 18 个 `script_scene_*` |

所有名字都是具名函数；没有任何"按名字分发"的通用入口（见第 3 节）。

## 2. 演进记录

| 变化 | 理由 |
|---|---|
| 窗口动词 18 → 13：`createPanel({kind})` 收掉三种面板创建，加 `setWindowPresentation` | 同一个动作，只是宿主种类不同 |
| 删 `setPanelPlacement` / `dockingPos` / `nativeDocked` | ImGui 接管停靠，这些请求没有落地点 |
| 删 `setDragRegions` | 无边框拖动声明，SDL 命中测试早已没有安装者 |
| `close` + `closeTab` + `closeCameraView` → `close(identity?)` | 三种身份，一个动作；回复统一为 `{closed: n}` |
| `resizeCameraView` → `setWindowPresentation({width,height})` | 尺寸就是一种呈现状态 |
| `suspendCameraViews` → `close({sceneId})` | 复数关闭是 `close` 的第二种身份 |
| `focusTab(tabId)` → `focus({panelId})` | 和 `close` 用同一个稳定身份 |
| `files` / `gizmo` / `input` / `projectSettings` 折叠进 `project` / `viewport` / `scene` | 一个方法一个命名空间没有意义 |
| `audio.playResource|playActor|stopResource|stopActor` → `audio.play|stop` | 两个动作，按身份区分资源或 Actor |
| 删 `getCurrent` | 没有产品消费者；身份来自 `createPanel` 返回值和 URL 参数 |
| 删 `minimized` 等死字段与死函数 | 没有写入路径或没有读取方 |

结果：**65 → 58 个方法**，窗口面从 18 降到 6。

## 3. 明确否决并保留

| 提议 | 为什么不做 |
|---|---|
| 通用 `command({...})` 总入口 | 就是已经删除的 manifest 模式：名字变成数据，守卫和类型都失效 |
| `scene.*` 全部改成单对象请求 | 20 多个方法的迁移成本大于一致性收益；位置参数在热路径上更紧凑 |
| `SceneReply` 改 `std::variant` | 30 个字段每一个都被绑定读取；变体只改变形状，不减任何载荷 |
| `createActor` / `deleteActor` / `updateActor` 与相机同名方法合并 | 两者的载荷结构不同，`kind` 只会把类型清晰度换成字符串分支 |
| 保留 `getCurrent` | 新页面应当用 `createPanel` 的返回值和打开时带在 URL 上的 `panelId` |
| 面板接受 `x` / `y` 或相机身份 | 接受却静默丢弃是陷阱；现在是 `INVALID_ARGUMENT` |
| 资源音频接受 `archive_session` | 资源播放是全局的；现在是 `INVALID_ARGUMENT` |

## 4. 身份词汇

| 名字 | 稳定性 | 用途 |
|---|---|---|
| `panelId` | 稳定 | 打开、关闭、聚焦、广播都用它 |
| `windowToken` | 随这一次创建生灭 | 同一次创建的令牌：`createPanel` 返回值与 `onClosed` 负载对齐；重开后不同 |
| `sceneId` + `cameraId` | 稳定 | 相机窗口的身份 |

主工作区不是面板：它不能 `close` 自己，也不能被当作面板关闭；它可以关/聚焦任何面板。

原始的 CEF `tab_id` 不再出现在 Web 接口上：它是浏览器层的内部编号，暴露出去让调用方
把它当身份用。`windowToken` 是每次创建时分配的递增令牌，只服务"我开的那扇窗还在吗"这个问题；
需要"是哪块面板"时一律用 `panelId`。

## 5. 失败形状

拒绝码的**唯一声明处**是 `tests/tools/rejection_codes.py`（基础设施 5 个、窗口 2 个、场景/归档 29 个、
bridge 4 个，共 40 个），守卫 `tests/tools/test_rejection_vocabulary.py` 保证源码只发这些码，契约文档的失败码表
由它生成。扫描器覆盖 `error_code = "…"`、`fail("…")`、`reject(…, "…")`、`RejectPromise("…")` 等写法；
`fail("…")` 这条是补上的——它漏掉了 `ACTOR_VERSION_CONFLICT` 和另外 6 个场景码。

Python 服务的失败分成两类，分界是"谁的问题"：

- **业务拒绝**（会话过期、只读、路径越界、目标已存在）是**数据**，服务返回
  `{status:'error', code, message}`，不抛异常。这样 Web 能拿到码去分支，而不是去匹配译文。
  `ProjectArchive.file_manager` 曾经抛 `ValueError`，于是 bridge 把它压成 `SERVICE_FAILED`，
  原因只剩 message 里的字符串——2026-11 改成了和其它两个服务同形的返回。
- **传输/服务失败**（信封坏、服务没启动、Python 抛未预期异常）才拒绝，码是
  `SERVICE_UNAVAILABLE` / `SERVICE_FAILED`，原因在 message。

**两个层级**：`STABLE`（20 个）是调用方可以据此分支的码——重试、刷新、禁用控件；其余是
**细节码**，照常拒绝，但只应进日志，可能随实现改名或合并。`tests/tools/test_rejection_vocabulary.py`
还会扫描前端，禁止它按细节码分支。

**回复版本**：分页读（`readModelNodes`）与写（`setModelNodeState`）的回复都带 `actor_version`。
消费方以它为围栏：比已应用版本旧的回复与已确认的编辑合并（确认值优先），而不是整页替换掉编辑；
这是"整页替换 + 异步读"不丢编辑的顺序规则，也是 model-node 面板不再需要重试点击的原因。

页回复**按 node_index 合并**而不是整页替换：未变的节点保留原对象（Vue 不会重排整行），
offset 0 的回复仍是该范围的权威（少掉的节点会消失），后续页只覆盖它覆盖的那一段，因此重叠页不会
把节点重复插入。

写入被 `ACTOR_VERSION_CONFLICT`（或 `CONFLICT`）拒绝时，消费方重读该页拿到当前版本、**重试一次**；
再失败就重读展示权威状态。所以这两个码进了稳定码表：它们对应一个明确的动作（重读+重试）。

**上下文释放时的挂起调用**：`CoronaV8Endpoint::release()` 直接丢弃未完成的请求，不做
`RejectPromise`。尝试过在释放回调里拒绝，但那会在单进程 CEF 里调用已释放的 V8 上下文而崩溃
（冒烟在 pick 阶段丢掉 CDP 连接）。等待这些 Promise 的页面正是被销毁的那个，丢弃是安全的
settlement；C++ 侧的所有者另走 `cancel_context` 取消工作。

前端 facade 负责把话说到 Web 程序员的语言：`editorApi.files.*` 收到 `{status:'error'}` 就抛出
带 `error.code` 的 `Error`（`fileOp` 一处），调用方的 `try/catch` 统一处理，不必各自解析回复字段。

## 6. 改动一个接口的固定流程

1. 改 `src/systems/ui/cef/`：读者、注册、执行三处。
2. 更新 `tests/tools/test_typed_native_surface.py` 的 `EXPECTED_SURFACE`。
3. `python tests/tools/generate_interface_contract.py` 重新生成契约。
4. 改 `tests/integration/web_messages_cef_smoke.mjs`（调用与 `typeof` 断言都会被守卫检查）。
5. 前端：改 facade → 推送 CoronaEditor → `python build_tools/dev.py pin editor <sha>` →
   `python build_tools/dev.py editor-sync`。
6. `python -m pytest tests/tools` 与 `ctest`。
7. 真机验证：按 [integration-smoke.md](integration-smoke.md) 构建 examples 运行时并跑冒烟套件与 CEF 冒烟。

## 7. 现在的守卫

全部列在契约文档的守卫表里，并由 `tests/tools/test_interface_contract_doc.py` 保证表与
`tests/tools/*.py` 一致：新增一个守卫必须登记，否则失败。

覆盖重点：方法名表、文档一致性、拒绝码词表、集成脚本（调用与 `typeof` 断言）、前端 facade
（存在性、唯一入口、一一映射）、前端回归文件、行数 ratchet、pin 工具。
