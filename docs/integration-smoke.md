# 集成冒烟测试流程

`tests/integration/` 下唯一的手动冒烟是 `web_messages_cef_smoke.mjs`。它启动真实引擎、真实 CEF
窗口和真实 Vue 页面，从 Web 侧调用原生接口；它不在默认 ctest 路径里（需要 GPU、窗口和数分钟的
运行时间），由本流程手动执行。其余集成测试是 ctest，见第 5 节。

## 改动接口层时的必跑清单

任何改动 `window.corona.*` 的方法名、bridge 服务名、wire key 或事件负载的提交，合并前跑完这三条：

| 顺序 | 命令 | 覆盖 |
|---|---|---|
| 1 | `pytest tests/tools` | 静态守卫 + 前端精选子集（`test_frontend_regressions.py` 逐个起 `node --test`） |
| 2 | `npm run test:unit`（在 `CoronaEditor/editor/Frontend`） | 前端全量单测 |
| 3 | `node tests/integration/web_messages_cef_smoke.mjs` | Web → 原生：窗口创建/关闭/焦点、事件负载、输入派发、截图、bridge 往返；自带引擎实例，跑完自动关闭 |

第 3 条是唯一会真的把请求发到原生的测试。单元测试和静态守卫只检查 Web 内部的形状——
键名或方法名对不上时只有它会红（例如 `Invalid native window operation arguments`），
而且能定位到具体是哪个调用。

## 1. 前置条件

| 项目 | 要求 |
|---|---|
| 构建 | `examples` target family 的 `corona_engine`（见下），带部署好的打包 UI |
| Python | 仓库使用的 conda 环境（`coronaengine-dev`），用于 `build_tools/dev.py` |
| Node | **22+**（冒烟使用原生 WebSocket）。PATH 上的 Node 可能是 20，此时用 `envs/coronaengine-dev/node.exe` |
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
- 嵌入式 Python 运行时（归档服务依赖它）

前端依赖用项目自己的锁文件安装：`CabbageEditor/Frontend/pnpm-lock.yaml` 随编辑器资源一起部署，
构建阶段执行 `pnpm install --frozen-lockfile` 再 `pnpm run build`（pnpm 通过 npm 缓存获取）。
不要退回 `npm install`：它按 semver 重新解析依赖，同一份源码两次构建可能得到不同的 Vue/Vite，
打包页面会因此改变行为。

`vulkan-1.dll` 取引擎构建时使用的那一份，不取 CEF 自带的副本。

## 3. 运行 CEF 冒烟

```powershell
& "C:\Users\<user>\miniconda3\envs\coronaengine-dev\node.exe" tests/integration/web_messages_cef_smoke.mjs <runtime> --native-bindings
```

第一个非 `--` 参数是运行时目录，默认 `build/conan/examples/relwithdebinfo/examples/engine/RelWithDebInfo`。
`--native-bindings` 才会跑窗口/面板/相机的生命周期部分；不带它只跑消息与 bridge 往返。

它验证：结构化克隆（JSON 被禁用）、512 次原生调用的队列突发、两个主线程回合的退休栅栏、
`close` 的身份与权限、面板/相机窗口创建、项目文档与文件 CRUD、资源导入、环境、Actor/相机属性、
脚本输入、截图、Actor 删除，以及**模型节点写入**：过期 `expected_actor_version` 得到
`ACTOR_VERSION_CONFLICT`、未知节点得到 `NODE_NOT_FOUND`、写入回复与下一次读取都反映请求推导出的状态。

## 4. 读取结果

- 结果：`build/web-messages-smoke/result.json`（`passed`、`cleanup_ok`、`graceful_exit`、
  `documents` 各阶段证据）
- 控制台输出 `CEF smoke: <stage>` 标出当前阶段；失败时打印 `CEF smoke failed at <stage>` 并带断言消息。

## 5. 其余集成测试（ctest）

`tests/integration/` 下的 C++ 测试由 ctest 驱动（`corona_ui_multisurface_smoke`、启动配置、vision
系列），`archive_driver.py` 是 `tests/systems/ui/` 的 C++ 测试 fixture。窗口/呈现请求的形状规则
由 `RequestRulesTests` 单独覆盖（`tests/systems/ui/test_request_rules.cpp`），不需要 CEF 上下文；
V8 适配器本身仍由 CEF 冒烟覆盖。构建与 CTest 方式见 [tests/README.md](../tests/README.md)。

## 6. 常见故障

| 现象 | 原因与处理 |
|---|---|
| 引擎立刻退出，返回码 `3221225781`（0xC0000135） | 运行时目录缺 DLL。用第 2 节的构建重新部署，而不是手工复制；若刚改过 CMake，确认 `corona_install_cef_runtime` 与 `corona_copy_target_runtime_dlls` 被调用 |
| CEF 冒烟报 "requires Node 22+ with native WebSocket" | 用了 Node 20；改用 conda 环境里的 `node.exe` |
| 页面已挂载但接口报 `INVALID_ARGUMENT` | 请求形状与契约不符（见 `docs/interface-contract.md`）；本流程最常抓到的真实回归 |
