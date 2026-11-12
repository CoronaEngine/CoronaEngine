# 集成冒烟测试流程

`tests/integration/` 下的脚本启动真实引擎、真实 CEF 窗口和真实 Vue 页面，验证 Web 接口层与
打包后的编辑器。它们不进入 ctest（需要 GPU、窗口和数分钟的运行时间），由本流程手动执行。

## 1. 前置条件

| 项目 | 要求 |
|---|---|
| 构建 | `examples` target family 的 `corona_engine`（见下） |
| Python | 仓库使用的 conda 环境（`coronaengine-dev`），因为脚本 import `tools/vision_frontend_probe.py` |
| Node | **22+**（CEF 冒烟用原生 WebSocket）。PATH 上的 Node 可能是 20，此时用 `envs/coronaengine-dev/node.exe` |
| 图形 | 可用的 Vulkan 设备；引擎会创建真实窗口 |

## 2. 构建运行时

```powershell
$env:PATH = "C:\Users\<user>\miniconda3\envs\coronaengine-dev;C:\Users\<user>\miniconda3\envs\coronaengine-dev\Scripts;C:\Users\<user>\miniconda3\envs\coronaengine-dev\Library\bin;$env:PATH"
python build_tools/dev.py build --target-family examples --configuration RelWithDebInfo corona_engine
```

输出目录（下文的 `<runtime>`）：

```
build/conan/examples/relwithdebinfo/examples/engine/RelWithDebInfo
```

这个目标的 POST_BUILD 负责部署运行所需的全部载荷，**不要手工复制 DLL**：

- CEF：`libcef.dll`、`chrome_elf.dll`、GL/编译相关 DLL、`v8_context_snapshot.bin`、`Resources/*`（`locales/`、`*.pak`、`icudtl.dat`）
- 目标自身链接的共享库：TBB、FFmpeg、Vulkan 加载器等
- vision 插件、slang 运行时、`CabbageEditor/` 编辑器资源（含构建好的 `Frontend/dist`）

前端依赖用项目自己的锁文件安装：`CabbageEditor/Frontend/pnpm-lock.yaml` 随编辑器资源一起部署，
构建阶段执行 `pnpm install --frozen-lockfile` 再 `pnpm run build`（pnpm 通过 npm 缓存获取）。
不要退回 `npm install`：它按 semver 重新解析依赖，同一份源码两次构建可能得到不同的 Vue/Vite，
打包页面会因此改变行为——曾导致 `focus` 套件的 viewport 点击不再触发 pick。

`vulkan-1.dll` 取引擎构建时使用的那一份，不取 CEF 自带的副本。

## 3. 打包 UI 套件（12 个）

```powershell
python tests/integration/web_packaged_ui_smoke.py <runtime> --suite all
# 或单个：--suite focus|environment|graph|properties|panels|creation|picker|calibration|assistant|join|launcher|lifetime
```

脚本自己启动引擎（带 `CORONA_CEF_REMOTE_DEBUG_PORT`）、创建一个一次性 creative world、打开
Python 归档服务，然后按顺序跑套件。`--suite all` 会在第一个失败处停止；排查时逐个跑更快：

```powershell
python tests/integration/web_packaged_ui_smoke.py <runtime> --suite panels
```

## 4. CEF 冒烟（单文件、覆盖面最广）

```powershell
& "C:\Users\<user>\miniconda3\envs\coronaengine-dev\node.exe" tests/integration/web_messages_cef_smoke.mjs <runtime> --native-bindings
```

`--native-bindings` 才会跑窗口/面板/相机的生命周期部分。它验证：结构化克隆（JSON 被禁用）、
512 次原生调用的队列突发、两个主线程回合的退休栅栏、`close` 的身份与权限、面板/相机窗口创建、
项目文档与文件 CRUD、资源导入、环境、Actor/相机属性、viewport pick、脚本输入、截图、Actor 删除，
以及**模型节点写入**：过期 `expected_actor_version` 得到 `ACTOR_VERSION_CONFLICT`、未知节点得到
`NODE_NOT_FOUND`、写入回复与下一次读取都反映请求推导出的状态。

## 5. 读取结果

- 打包套件：`build/web-packaged-ui-smoke/run-*/result.json`（`passed`、`failed_stage`、`error`、各套件证据）、
  `stdout.log`（引擎日志）、失败时还有 `failure.png` 与 `last_document`（URL + 页面文本）
- CEF 冒烟：`build/web-messages-smoke/result.json`，以及控制台输出的 `CEF smoke: <stage>`

失败信息里的 `stage` 指明套件，`error` 是 Python traceback 或断言消息；`last_document` 通常足以
判断是页面没挂载、接口拒绝了请求，还是断言过时。

## 6. 需要自备环境的独立脚本

以下脚本不自己启动引擎，需要先手动运行一个带 `CORONA_CEF_REMOTE_DEBUG_PORT=<port>` 的引擎，
并准备 world/peer 目录：

| 脚本 | 参数 |
|---|---|
| `scene_pull_smoke.py` | `port project control`（两个窗口、一个 Python 脚本控制目录） |
| `python_archive_smoke.py` | `port project peer`（一个 peer 项目用于归档会话切换） |
| `blockly_actor_file_smoke.py` | 内部 `verify(port, root, switch_root)`，需要两个世界 |

## 7. 已知失败与隔离

截至 2026-11-12，12 个套件（逐个运行）与 CEF 冒烟在真机上全部通过。

`panels` 的 `model_node_retirement` 曾经超时（重挂载后可见性按钮标题不翻转）：原因是
`setModelNodeState` 把写入排给 Geometry 线程后**立刻回读**，回复里带的是编辑前快照，UI 于是
应用了旧状态。现在回复直接用本次请求推导出的状态，测试恢复稳定（连续多次通过）。

之前的另一个问题是**重挂载竞态**：面板重新挂载会重读模型节点页，而组件整页替换；若这次读在
编辑之前发出、在编辑之后返回，它带的是旧状态，会把刚应用的编辑覆盖掉（表现为点击丢失）。现在
组件按 **actor_version 围栏**：比当前版本旧的页回复直接丢弃，并且一次写入成功后作废所有在途
读。冒烟因此恢复成"点一次、等一次"，不再重试。

`--suite all` 现在为**每个套件启动独立的引擎进程和独立世界**：前面的套件会留下打开的窗口
（`focus`、`environment` 都留下 `SceneTools`），共享引擎时 `panels` 的"重开后仍是同一个文档"
断言会看到遗留目标。逐套件的 `result.json` 聚合在顶层 `result.json` 的 `results` 里，第一个失败
处停止。单套件运行的耗时不变，`all` 约等于 12 次启动。

窗口/呈现请求的形状规则（`close` 的身份组合、`setWindowPresentation` 的 mode/size 互斥与主窗口
限制）由 `RequestRulesTests` 单独覆盖（`tests/systems/ui/test_request_rules.cpp`），不需要 CEF
上下文；V8 适配器本身仍由 CEF 冒烟覆盖。

需要单独排查时，`web_panel_lifecycle_smoke.py` 的 model-node 段有一个 opt-in 诊断：设
`CORONA_MODEL_NODE_DEBUG=1` 会记录 `setModelNodeState` 的实际参数、回复字段与页面标题，
失败时打印。

## 8. 常见故障

| 现象 | 原因与处理 |
|---|---|
| 引擎立刻退出，返回码 `3221225781`（0xC0000135） | 运行时目录缺 DLL。用第 2 节的构建重新部署，而不是手工复制；若刚改过 CMake，确认 `corona_install_cef_runtime` 与 `corona_copy_target_runtime_dlls` 被调用 |
| CEF 冒烟报 "requires Node 22+ with native WebSocket" | 用了 Node 20；改用 conda 环境里的 `node.exe` |
| 套件卡在 `service_initializing` | Python 归档服务仍在启动；`open_initial_test_world` 会重试，若持续出现看 `stdout.log` 里的 Python 错误 |
| 页面已挂载但接口报 `INVALID_ARGUMENT` | 请求形状与契约不符（见 `docs/interface-contract.md`）；这是本流程最常抓到的真实回归 |
