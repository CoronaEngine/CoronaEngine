# CoronaEngine

## 环境要求

当前工作流仅支持 **Windows x64**。

| 工具 | 要求 |
| --- | --- |
| Git | 构建时复用可用的 Git；缺失时由 Conda 自动安装。首次获取本仓库源码仍需 Git 或其他下载方式 |
| [Miniforge](https://github.com/conda-forge/miniforge) / [Miniconda / Conda](https://docs.conda.io/) | 管理 Python、Conan、Git、Ninja 和 Node.js；无需单独安装这些工具 |
| CMake | 4.0 或更高版本，与 Horizon 的最低要求一致 |
| Ninja | 工程使用 Ninja Multi-Config；复用可用的 Ninja，缺失时由 Conda 自动安装 |
| Visual Studio 或 Build Tools | 安装“使用 C++ 的桌面开发”、MSVC x64/x86 工具集与 Windows SDK |
| Vulkan 开发组件 | Vulkan-Headers、Vulkan Loader、Volk、VMA 与 Slang 由 Conan 管理；默认构建无需安装 Vulkan SDK |

## 开发工具环境

CMake Configure 会自动创建或复用 `coronaengine-dev` 环境（可通过 `CORONA_CONDA_ENV` 指定环境名称）。
首次调用 Git 前会检查 `git --version`，复用 PATH 或当前 Conda 环境中可用的 Git。
缺失或无法运行时，通过 conda-forge 安装到运行 `dev.py` 的 Python 所属 Conda 环境，安装后复检；
已有 Git 包但程序损坏时执行修复。新装 Git 的路径只加入当前进程及子进程的 PATH，不修改系统环境变量。
该检查覆盖 CMake bootstrap 和直接运行 `dev.py` 的构建、同步及状态命令；`clean` 不需要 Git。
直接使用脚本时，应通过 `conda run -n coronaengine-dev python build_tools/dev.py ...` 或激活该环境运行。
Git、Ninja 与 Node.js 安装共用环境目录内的 `conda-meta/corona-tools.lock`，避免多个 preset 同时修改环境。

Ninja 在 Conan 编译依赖前检查：优先复用当前构建目录缓存、PATH 或 Conda 环境中能通过
`ninja --version` 检查的程序，否则由 conda-forge 自动安装或修复到当前 Python 所属 Conda 环境。
路径随构建环境传回外层 CMake，不修改系统 PATH。CMake 在 `project()` 前修复失效的 Ninja 缓存；
`build-fast` 遇到失效缓存时先重新配置再构建。`status`、源码同步和 `clean` 不安装 Ninja，
`conan_cache.py update` 也会在编译依赖前准备 Ninja。

Vulkan Headers 与 Loader 使用匹配的 `1.4.350.0` 版本。Ocarina 显式链接 Conan 的
`Vulkan::Loader`；Windows 构建将该包的 `vulkan-1.dll` 部署到后端及可执行文件目录，
并在复制 CEF 后重新部署，避免被 CEF 自带版本覆盖。运行渲染器仍需支持所需 Vulkan 功能的 GPU 驱动。
Vulkan SDK 的验证层和调试工具可按需安装。ReBLUR 使用仓库内的 CPU 调度器和 Ocarina C++ EDSL 内核，无需外部降噪 SDK、DLL 或 NVRHI。

Horizon 和 CoronaEditor 统一由 `build_tools/git_workspace.py` 管理，分别位于引擎根目录的
`Horizon/` 和 `CoronaEditor/`。CMake bootstrap 及
`build_tools/dev.py install/configure/build` 会准备这两个工作区。

仓库地址、分支及完整 commit 统一固定在 `build_tools/source_workspaces.py` 中。
每次更新锁定提交时，必须保证该提交已包含引擎所需的功能；功能实现随对应依赖仓库的提交交付。
构建直接使用锁定源码，不再应用引擎维护的 Horizon 补丁，也不读取额外的补丁基线或 JSON lock 文件。
首次准备直接非浅克隆到根目录的 `Horizon/` 或 `CoronaEditor/`，检出指定 commit、建立跟踪分支，
并递归初始化该提交记录的子模块，不经过临时克隆目录。失败时清理本次新建的目录，以便重试。
Python 中的 commit 是构建基准。已有副本的 HEAD 与它一致时直接复用；不一致时切换到该 commit 并更新子模块，目标提交本地缺失时才获取远端。
切换由普通 Git checkout 保护：不冲突的修改及未跟踪文件会保留，不因工作区有修改就直接报错。
只有切换确实会覆盖本地内容、目标提交不可用或子模块同步失败等实际错误才终止；不会退回使用其他版本继续构建。
HEAD 已一致时仍允许本地修改参与构建。
同步保留含有额外提交的分支，必要时检出为 detached HEAD；已有浅克隆在需要目标提交时才补全历史。

源码准备、显式同步和 `status` 使用 `git ls-remote` 查询 Python 中仓库地址及分支的最新 commit，
与 Python 固定值不一致时只输出警告，不修改锁定值、不跟随远端更新。该查询不更新本地引用，不依赖可能过期的 `origin/main`。
查询超时（每个工作区 10 秒）或远端不可用时也仅警告；获取缺失的锁定提交仍必须成功。
未触发重新配置的纯 Ninja 增量构建不执行 Python，也不重复查询远端；可运行 `dev.py status` 主动检查。

每次 Ninja 构建检查 `source_workspaces.py` 和 Git 的 HEAD、当前分支引用及 packed-refs 是否变化，
并检测这些元数据文件的出现和删除。变化时自动重新配置，在读取依赖项目的 CMake 规则前按 Python 固定值同步源码，
再刷新依赖和继续本次构建；默认构建及指定 target 均适用。未变化时不重复运行 Python 检查或重新配置。
不生成额外的版本标记文件；Git 路径由 Git 自身解析，兼容普通克隆及 linked worktree。

仍可使用 `python build_tools/dev.py horizon-sync` 或 `editor-sync` 显式同步，包括 HEAD 已一致时修复子模块。
同步不会强制覆盖修改；获取提交或更新子模块失败会终止当前构建，可处理后显式同步重试。
`python build_tools/dev.py status` 只显示源码版本和本地状态；`update` 准备指定源码版本并额外更新 Conan 依赖。

Python 依赖检查使用 `CoronaEditor/editor/requirements.txt`；构建时将资源部署为
可执行目录下的 `assets/` 和 `CabbageEditor/`，并自动构建前端。关闭编辑器构建仍准备该仓库，
因为它也提供共享资源。源码工作区通过进程锁保护，多个 preset 共用同一副本。
锁文件位于 `build/.locks/horizon.lock` 和 `build/.locks/editor.lock`，进程退出即释放锁。
`clean` 清理构建产物时保留 `build/.locks/`；`rebuild` 只清理当前目标族和配置的构建目录。
Python 层使用固定的 PyTorch/torchvision CPU wheels，不需要 CUDA。缺包或版本不符时，pip 按完整
`requirements.txt` 安装；安装或复检失败会终止配置。
启用编辑器时，CMake 使用该环境的 Python 自动检查 Node.js **22.19.0** 和 npm；
缺失、版本不符或安装损坏时，通过 conda-forge 安装或修复到同一环境，已有环境也会补齐。
首次安装需要联网，后续配置在工具可用时直接复用，不调用 Conda 安装。
前端构建使用检查得到的绝对路径，无需手动激活环境，也不依赖系统 Node.js 或仓库内的副本。
`node_modules` 仍保留在前端构建目录中。关闭 `BUILD_CORONA_EDITOR` 时不检查或安装 Node.js。

VS Code / CMake Tools

使用 VS Code 的 CMake Tools 时，在设置中将 <code>CMake: Use VS Developer Environment</code> 设为 <code>always</code>，或加入用户设置：

~~~json
"cmake.useVsDeveloperEnvironment": "always"
~~~

否则 CMake Tools 单独启动的进程可能没有 MSVC 和 Windows SDK 环境，常见表现为 <code>fatal error C1083</code> 或找不到 <code>stddef.h</code> 等标准头文件。

## 通过 CMake preset 构建

使用 CMake Tools选择目标族 preset，例如 <code>core-debug</code>、<code>examples-debug</code>、<code>tests-debug</code>、<code>vision-debug</code> 或 <code>vision-tests-debug</code>，再执行 Configure 和 Build。

```powershell
cmake --preset examples-relwithdebinfo
cmake --build --preset examples-relwithdebinfo --target corona_engine
```

也可以通过开发脚本依次准备依赖、配置和编译：

```powershell
conda run -n coronaengine-dev python build_tools/dev.py build corona_engine
```

## 构建系统目录

根目录的 `CMakeLists.txt` 保留工具链初始化和 `project()`，其余顶层规则位于
`build_tools/cmake/corona_project.cmake`；根 `CMakePresets.json` 引用 `build_tools/CMakePresets.json`。
公共 CMake 模块、Conan profiles/recipes、构建辅助脚本及测试统一放在 `build_tools/`。
各模块自身的 `CMakeLists.txt` 继续与源码放在一起。

主工程 Conan 配方位于 `build_tools/conanfile.py`。手动安装依赖时使用
`conan install build_tools/conanfile.py`，并传入 `build_tools/conan/profiles/` 中对应的 profile
及所需选项；通常由 `build_tools/dev.py` 自动完成。
缓存管理命令为 `python build_tools/conan_cache.py`。

构建结果仍位于 `build/conan/<目标族>/<配置>/`，源码工作区仍位于根目录的
`Horizon/` 和 `CoronaEditor/`。从旧目录布局更新后，先执行一次 Configure 或
`build_tools/dev.py configure`，刷新生成文件中的脚本路径，再使用 `build-fast`。

构建工具测试使用开发环境运行：

```powershell
conda run -n coronaengine-dev python -m pytest build_tools/tests
```

引擎核心测试统一放在根目录 `tests/`，按模块组织；目录约定和运行方式见 [测试说明](tests/README.md)。

## 本地文件与临时目录

根目录保留源码、项目配置和正式工具入口。构建产物放在 `build/`；CLion 使用
`cmake-build-relwithdebinfo/`。`Horizon/`、`CoronaEditor/` 是构建管理的源码工作区。

临时文件统一放在已被 Git 忽略的 `.workspace/`，不要再在根目录新建缓存、日志或实验目录：

| 路径 | 用途 |
| --- | --- |
| `.workspace/tmp/<任务>/` | 一次性脚本和验证输出 |
| `.workspace/experiments/<任务>/` | 实验及性能对比资料 |
| `.workspace/cache/` | 可再生缓存；pytest 缓存位于其下的 `pytest/` |
| `.workspace/cache/tools/` | 本地下载的工具 |
| `.workspace/downloads/` | 下载资料 |
| `.workspace/notes/` | 本地计划和交接笔记 |
| `.workspace/archive/` | 需要保留的旧资料和录制结果 |

可复用脚本放入 `tools/`，构建专用脚本放入 `build_tools/`。Mitsuba 下载入口为
`tools/download_mitsuba_scenes.cmd`，默认下载到 `.workspace/downloads/mitsuba/`。
批量出图入口为 `tools/render_mitsuba_scenes.py`，统一使用引擎正常截图接口；参数和采样口径见
[Mitsuba 批量截图说明](docs/mitsuba-batch-rendering.md)。
运行引擎时以可执行文件目录为工作目录，避免运行时缓存散落到仓库根目录。
