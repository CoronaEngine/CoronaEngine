# CoronaEngine

## 环境要求

当前工作流仅支持 **Windows x64**。

| 工具 | 要求 |
| --- | --- |
| Git | 构建时复用可用的 Git；缺失时由 Conda 自动安装。首次获取本仓库源码仍需 Git 或其他下载方式 |
| [Miniforge](https://github.com/conda-forge/miniforge) / [Miniconda / Conda](https://docs.conda.io/) | 管理 Python、Conan、Git、Ninja 和 Node.js；无需单独安装这些工具 |
| CMake | 3.29 或更高版本 |
| Ninja | 工程使用 Ninja Multi-Config；复用可用的 Ninja，缺失时由 Conda 自动安装 |
| Visual Studio 或 Build Tools | 安装“使用 C++ 的桌面开发”、MSVC x64/x86 工具集与 Windows SDK |
| Vulkan SDK | Vulkan 头文件、加载器与构建工具；Slang 由 Conan 提供 |

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

Horizon 和 CoronaEditor 统一由 `build_tools/git_workspace.py` 管理，分别位于引擎根目录的
`Horizon/` 和 `CoronaEditor/`。CMake bootstrap 及
`build_tools/dev.py install/configure/build` 会准备这两个工作区。

仓库地址、分支及完整 commit 固定在 `build_tools/source_workspaces.py` 中，不再读取 JSON lock 文件。
首次准备直接非浅克隆到根目录的 `Horizon/` 或 `CoronaEditor/`，检出指定 commit、建立跟踪分支，
并递归初始化该提交记录的子模块，不经过临时克隆目录。失败时清理本次新建的目录，以便重试。
已有副本只报告状态并直接使用：普通构建不会拉取、切换版本或更新子模块，本地修改和提交可以直接参与构建。
已有浅克隆也会复用，显式同步需要旧提交时才补全历史。

修改代码中的 commit 后，使用 `python build_tools/dev.py horizon-sync` 或
`python build_tools/dev.py editor-sync` 显式对齐对应工作区及子模块，然后重新 configure/build。
同步不会强制覆盖未提交修改，也不会重置含有额外本地提交的分支；子模块同步失败会报错，可修复后重试。
`python build_tools/dev.py status` 显示两者的版本和本地状态；`update` 只更新 Conan 依赖，不拉取源码仓库。

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
