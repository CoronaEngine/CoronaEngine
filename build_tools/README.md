# build_tools

CoronaEngine 的构建工具目录。**引擎侧参与配置、构建、依赖安装、打包的脚本都集中在这里**，
不再散落在 `tools/`、仓库根或各模块目录（例外见 1.2）。

入口约定：

| 场景 | 命令（在仓库根执行） |
| --- | --- |
| 日常开发构建 | `python build_tools/dev.py <command>` |
| IDE / 脚本直接配置 | 根目录 `CMakePresets.json` 的 `<family>-<config>` preset |
| Conan 缓存维护 | `python build_tools/conan_cache.py <command>` |
| 发布打包 | `python build_tools/pack.py` |

> 下文所有命令都以仓库根为工作目录，并且应在 `coronaengine-dev` Conda 环境里运行：
> `conda run -n coronaengine-dev python build_tools/dev.py ...`。CMake 配置时会自动创建并复用该环境。

---

## 1. 当前目录的职责

### 1.1 负责什么

`build_tools/` 是**构建事实的唯一脚本来源**，具体承担：

| 职责 | 落点 |
| --- | --- |
| 构建编排入口（configure / build / rebuild / clean / 工作区同步） | `dev.py`、`workflow.py` |
| CMake 模块：选项、工具链、依赖解析、编译配置、资源部署 | `cmake/*.cmake` |
| CMake 直接调用的 Python 辅助脚本 | `scripts/*.py` |
| Conan 依赖安装编排与本地 recipe 导出 | `workflow.py`、`dev.py` |
| Horizon 源码工作区（`.workspace/Horizon`）管理 | `horizon_workspace.py` |
| Conan 本地缓存维护 | `conan_cache.py` |
| 发布制品打包 | `pack.py` |
| 本目录工具自身的测试 | `tests/` |

文件清单：

| 路径 | 作用 | 调用方 |
| --- | --- | --- |
| `dev.py` | 开发工作流 CLI：`status` / `install` / `configure` / `build` / `build-fast` / `rebuild` / `update` / `horizon-sync` / `clean` / `_bootstrap` | 人手执行；`corona_dev_bootstrap.cmake` 调用 `_bootstrap` |
| `workflow.py` | 编排核心：Conan install、CMake configure/build、环境写入、安全删除。无 CLI | `dev.py`、`conan_cache.py`、`horizon_workspace.py` |
| `conan_cache.py` | Conan 本地缓存 `list` / `update` / `remove` / `clear` | 人手执行 |
| `horizon_workspace.py` | 克隆、校验、同步、检视 `.workspace/Horizon`（锁定提交见 `.workspace/horizon.lock.json`） | `dev.py` |
| `pack.py` | 把编辑器工程打成 `tar.gz`（默认输出到 `CoronaEditor/editor/`） | 人手执行 |
| `cmake/corona_options.cmake` | 项目级 option 与缓存变量（`BUILD_CORONA_*`、`VISION_*` 等） | 根 `CMakeLists.txt` |
| `cmake/corona_dev_bootstrap.cmake` | 创建/复用 Conda 环境，生成 Conan 工具链并注入 CMake | 根 `CMakeLists.txt`（最先） |
| `cmake/corona_python.cmake` | 定位 Conda Python，校验/安装 `requirements.txt` 依赖 | 根 `CMakeLists.txt` |
| `cmake/corona_nodejs.cmake` | 为编辑器准备 Node.js 22.19.0 与 npm | 根 `CMakeLists.txt` |
| `cmake/corona_msvc_dependencies.cmake` | 归一化 MSVC `showIncludes` 输出，稳定 Ninja 头文件依赖 | 根 `CMakeLists.txt` |
| `cmake/corona_compile_config.cmake` | 编译器选项、标准、全局宏 | 根 `CMakeLists.txt` |
| `cmake/corona_third_party.cmake` | 消费 Conan 生成的包配置，重建兼容 target | 根 `CMakeLists.txt` |
| `cmake/corona_horizon_workspace.cmake` | 把 `.workspace/Horizon` 以 `add_subdirectory` 接入 | `corona_third_party.cmake` |
| `cmake/corona_tbb.cmake`、`corona_tbb_package.cmake` | TBB 发现与运行时复制判定 | 根 `CMakeLists.txt` |
| `cmake/corona_cef.cmake` | 消费 Conan `cef-binary`，构建 `libcef_dll_wrapper` | 根 `CMakeLists.txt` |
| `cmake/corona_add_system.cmake` | 新增系统库的标准化 helper（`corona_add_system`） | `src/systems/*/CMakeLists.txt` |
| `cmake/corona_editor.cmake` | 收集并部署编辑器资源，触发前端构建 | 根 `CMakeLists.txt`、各可执行目标 |
| `cmake/corona_runtime_deps.cmake` | 收集并复制运行时 DLL/PDB | 根 `CMakeLists.txt`、`src/CMakeLists.txt` |
| `cmake/corona_seed_startup_config.cmake` | 以脚本模式（`cmake -P`）播种启动配置，不覆盖本地改动 | `examples/engine/CMakeLists.txt`、启动配置测试 |
| `cmake/corona_test_runtime.cmake` | 给 CTest 注入 PATH，脱离 shell 的 Conan/Python 激活 | 根 `CMakeLists.txt` |
| `scripts/check_pip_modules.py` | 解析 requirements、校验版本、可选安装并复检 | `corona_python.cmake` |
| `scripts/copy_files.py` | 内容相同则跳过的批量复制 | `corona_runtime_deps.cmake` |
| `scripts/editor_copy_and_build.py` | 拷贝编辑器资源并执行 `npm install` / `npm run build` | `corona_editor.cmake` |
| `scripts/ensure_nodejs.py` | 在 Conda 环境内安装/修复 Node.js，JSON 结果回传 CMake | `corona_nodejs.cmake` |
| `scripts/msvc_showincludes.py` | 把依赖行归一化为 ASCII 前缀 + UTF-8 路径 | `corona_msvc_dependencies.cmake` |
| `tests/` | 上述工具与脚本的单元测试 | `python -m pytest build_tools/tests` |

### 1.2 不负责什么

| 不属于本目录 | 实际位置 / 原因 |
| --- | --- |
| 引擎运行时代码与公开头文件 | `src/`、`include/`、`modules/`、`vision/`；这里只描述**怎么构建**，不放实现 |
| 编辑器前端/后端源码 | `CoronaEditor/`，由 CMake 配置时浅克隆 |
| 第三方源码与二进制 | 由 Conan 提供；`vision/src/ext/` 是 Vision 自带的 vendored 依赖，归 Vision 模块 |
| 业务测试用例 | 根 `tests/`、`vision/src/tests/`、`modules/corona_resource/tests/` |
| 性能剖析、渲染对比、QA 脚本 | 仍留在 `tools/`（`profile_process.py`、`reblur_compare_*.py`、`vision_*`、`switch_profile_report.py` 等），它们不参与构建 |
| Vision 模块内的历史批处理/PS 脚本 | `vision/tools/`，由 Vision 模块自行管理，暂未纳入本目录 |
| 构建产物 | `build/`、`cmake-build-*`、`install/`、`out/`、`dist/`，均被 `.gitignore` 忽略 |
| 项目定义入口 | 根 `CMakeLists.txt`、`CMakePresets.json`、`conanfile.py`：CMake / Conan / IDE 约定必须在仓库根，不迁移 |

---

## 2. 常用功能操作

### 2.1 新增一个引擎系统库（最常见的「加一个库」）

以新增 `foo` 系统为例：

1. 公开头文件：`include/corona/systems/foo/foo_system.h`
2. 实现目录：`src/systems/foo/`，放入 `foo_system.cpp`
3. 新增 `src/systems/foo/CMakeLists.txt`：

   ```cmake
   corona_add_system(foo
       SOURCES foo_system.cpp
       # DEPENDENCIES corona::physics::core
       # INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
   )
   ```

   `corona_add_system` 会自动：创建静态库 `corona_foo_system`、创建别名 `corona::system::foo`、
   链到 `corona::engine::kernel`、设置 `PUBLIC` 的 `include/`、启用 C++20。
   可选参数：`NAMESPACE`（别名命名空间，默认 `system`）、`DEPENDENCIES`、`INCLUDE_DIRS`、`QUIET`。

4. **无需**修改 `src/CMakeLists.txt` 的模块发现：它以 `file(GLOB)` 扫描 `src/systems/*`，
   只要目录内有 `CMakeLists.txt` 就会 `add_subdirectory`。
5. 若该库要成为引擎主库的一部分，在 `src/CMakeLists.txt` 的
   `target_link_libraries(CoronaEngine PUBLIC ...)` 中追加 `corona::system::foo`
   （不追加则只是独立 target，不会自动进入 `CoronaEngine`）。
6. 需要事件时同步新增 `include/corona/events/foo_system_events.h`。
7. 重新配置并构建：

   ```powershell
   python build_tools/dev.py configure --configuration Debug --target-family core
   python build_tools/dev.py build corona_engine --configuration Debug
   ```

> 注意：`src/CMakeLists.txt` 的 `file(GLOB)` **没有** `CONFIGURE_DEPENDS`，
> 新增或删除系统目录后必须重新 Configure，否则 CMake 看不到变化。

### 2.2 删除一个引擎系统库

1. 从 `src/CMakeLists.txt` 的 `target_link_libraries(CoronaEngine PUBLIC ...)` 移除 `corona::system::foo`。
2. 删除 `src/systems/foo/`、`include/corona/systems/foo/`、`include/corona/events/foo_system_events.h`。
3. 清理引用它的 `tests/`、`examples/`、`vision/` 目标（按编译错误逐个处理）。
4. 重新 Configure 并全量构建，确认 CMake 不再枚举该目录。
5. 如果曾把它的 target 加进 `build_tools/dev.py` 的 `TARGET_FAMILY_OPTIONS` 或
   `VISION_TEST_TARGETS` 之类的目标族映射，一并删除，否则 target family 推断会出错。

### 2.3 新增一个独立模块（带自己的 CMake 工程）

1. 新建 `modules/<name>/` 并提供 `CMakeLists.txt`（可参考 `modules/corona_resource`）。
2. 在根 `CMakeLists.txt` 的 `add_subdirectory(...)` 区域加一行 `add_subdirectory(modules/<name>)`。
3. 需要被他人 `include()` 的 CMake 模块放在该模块的 `cmake/` 下，并把该目录追加进
   `CMAKE_MODULE_PATH`（参考现有 `modules/corona_resource/cmake`）。

删除时反向操作：先在根 `CMakeLists.txt` 移除 `add_subdirectory`，再删目录与 `CMAKE_MODULE_PATH` 条目。

### 2.4 新增第三方库（Conan）

1. 在根 `conanfile.py` 的 `requirements()` 里追加依赖：

   ```python
   self.requires("pkg/1.2.3", transitive_headers=True, transitive_libs=True)
   ```

2. 如果这个包需要由仓库自己打包（本地 recipe）：新建 `conan/recipes/<pkg>/conanfile.py`，
   并把 `"conan/recipes/<pkg>"` 加入 `build_tools/dev.py` 的 `LOCAL_RECIPES` 元组。
3. 在 `build_tools/cmake/corona_third_party.cmake` 中消费它：

   ```cmake
   find_package(pkg CONFIG REQUIRED)
   corona_require_target(pkg::pkg "pkg")
   # 老代码使用无命名空间 target 名时，建兼容别名：
   # corona_make_interface_target(pkg pkg::pkg)
   ```

4. 在需要它的 `target_link_libraries(...)` 里写 `pkg::pkg`。
5. 重新安装依赖并配置：

   ```powershell
   python build_tools/dev.py configure --configuration Debug --target-family core
   ```

删除第三方库：反向执行上述 4 步，再清理 Conan 缓存
（`python build_tools/conan_cache.py remove pkg --version 1.2.3`）。
只增删**依赖**时不需要改 `conanfile.py` 的 `options`/`_target_families`——那些是构建族开关，与单个包无关。

### 2.5 常用构建命令

```powershell
# 查看 git / Horizon / conan / cmake preset 状态
python build_tools/dev.py status

# 只装依赖（Conan install），不配置
python build_tools/dev.py install --configuration RelWithDebInfo --target-family examples

# 依赖 + CMake configure
python build_tools/dev.py configure --configuration Debug --target-family core

# configure + build（增量安全，会先确保依赖就绪）
python build_tools/dev.py build corona_engine --configuration Debug

# 只 build，不刷新依赖（Horizon 源码改动后会自动提示）
python build_tools/dev.py build-fast all --configuration Debug --target-family examples

# 删除该配置的构建目录后完整重来
python build_tools/dev.py rebuild corona_engine --configuration Debug

# 更新 Conan 依赖后重新配置
python build_tools/dev.py update

# 同步 .workspace/Horizon 到 .workspace/horizon.lock.json
python build_tools/dev.py horizon-sync

# 删除 build / install / out / dist
python build_tools/dev.py clean
```

- **配置（configuration）**：`Debug`、`Release`、`RelWithDebInfo`（默认）、`MinSizeRel`
- **目标族（target family）**：`core`、`examples`（默认）、`tests`、`vision`、`vision-tests`
- 不传 `--target-family` 时按 target 名推断：`all`/`corona_engine` → `examples`，
  `test-*`/`vision-test*` → `vision-tests`，`vision-*` → `vision`，`*_tests` → `tests`，其余 → `core`。
  一次只能构建同一个族，跨族请分多次执行。
- 等价的 CMake preset 名为 `<family>-<configuration 小写>`，例如 `core-debug`、`tests-relwithdebinfo`；
  构建目录为 `build/conan/<family>/<configuration 小写>`。

### 2.6 Conan 缓存维护

```powershell
python build_tools/conan_cache.py list
python build_tools/conan_cache.py update ktm
python build_tools/conan_cache.py remove ktm --version 0.2.14 --force
python build_tools/conan_cache.py clear --dry-run
```

支持 `--version`、`--user`、`--channel`、`--reference`、`--package-id`、`--package-query`、
`--configuration`、`--force`、`--dry-run`。

### 2.7 打包发布

```powershell
python build_tools/pack.py                 # 输出 CoronaEditor/editor/CabbageEditor-YYYYMMDD.tar.gz
python build_tools/pack.py -o release.tar.gz
python build_tools/pack.py --dry-run       # 只预览文件清单
python build_tools/pack.py --exclude-workflow
```

前提：编辑器源码已由 CMake 拉取到 `CoronaEditor/editor`，否则脚本直接报错退出。

### 2.8 自测

```powershell
python -m pytest build_tools/tests -q
```

`pytest` 需要先装进当前环境；其余用例是标准库 `unittest` 风格，也可以用
`python -m unittest discover -s build_tools/tests -t build_tools/tests -p "test_*.py"` 运行
（`test_editor_copy_and_build.py` 依赖 pytest，需跳过或先安装）。

---

## 3. 目录规范

### 3.1 可以放

- CMake 模块（`corona_<topic>.cmake`）
- 参与配置/构建/依赖安装/部署/打包/测试编排的 Python 脚本
- 上述脚本的单元测试（`tests/`）
- 构建相关的说明文档（本 README）

### 3.2 禁止放

- **与构建无关的工具**：性能剖析、渲染图对比、图像回归、benchmark、日志分析等一律放 `tools/`
- 引擎源码、公开头文件、编辑器源码、第三方源码或二进制
- 构建产物与缓存：`build/`、`cmake-build-*`、`node_modules`、`__pycache__`、`*.pyc`
- 日志、临时文件、截图、生成的报告、证据文件
- 业务测试用例（放对应模块的 `tests/`）
- 个人 IDE 配置、绝对路径脚本（例如硬编码 `D:\...` 的一次性批处理）

### 3.3 判定标准

新增文件前先问一句：**它是否参与 configure / build / install / package / test 的编排？**

- 是 → 放 `build_tools/`，并按类型选择 `cmake/`、`scripts/` 或根目录
- 否 → 放 `tools/`（开发者辅助脚本）或对应模块目录，不要进 `build_tools/`

### 3.4 编码约定

- 命名：脚本 `snake_case.py`，CMake 模块 `corona_<topic>.cmake`，测试 `test_<subject>.py`
- `scripts/` 下的脚本必须能被 `python build_tools/scripts/<name>.py ...` 直接调用，
  不依赖 Conda 激活、不依赖 `tools` 包导入；CMake 侧通过绝对路径调用它们
- 需要定位仓库根时用 `Path(__file__).resolve().parents[N]`，禁止硬编码机器绝对路径
- `dev.py` / `conan_cache.py` / `horizon_workspace.py` 之间是**同目录扁平导入**，
  移动其中任一文件时必须同步修正 import，避免破坏入口脚本
- 新增、删除或重命名本目录文件后，**同步更新本 README 的文件清单**
