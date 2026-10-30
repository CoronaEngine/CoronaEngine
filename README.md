# CoronaEngine

## 环境要求

当前工作流仅支持 **Windows x64**。

| 工具 | 要求 |
| --- | --- |
| Git | 克隆源码及管理 Horizon、CoronaEditor 工作区 |
| [Miniforge](https://github.com/conda-forge/miniforge) / [Miniconda / Conda](https://docs.conda.io/) | 管理 Python、Conan 和 Node.js；无需单独安装这些工具 |
| CMake | 3.29 或更高版本 |
| Ninja | 必须在 PATH 中；工程使用 Ninja Multi-Config |
| Visual Studio 或 Build Tools | 安装“使用 C++ 的桌面开发”、MSVC x64/x86 工具集与 Windows SDK |
| Vulkan SDK | Vulkan 头文件、加载器与构建工具；Slang 由 Conan 提供 |

## 开发工具环境

CMake Configure 会自动创建或复用 `coronaengine-dev` 环境（可通过 `CORONA_CONDA_ENV` 指定环境名称）。
Horizon 和 CoronaEditor 统一由 `builder/git_workspace.py` 管理，分别位于引擎根目录的
`Horizon/` 和 `CoronaEditor/`。CMake bootstrap 及
`builder/dev.py install/configure/build` 会准备这两个工作区。

仓库地址、分支及完整 commit 固定在 `builder/source_workspaces.py` 中，不再读取 JSON lock 文件。
首次准备直接非浅克隆到根目录的 `Horizon/` 或 `CoronaEditor/`，检出指定 commit、建立跟踪分支，
并递归初始化该提交记录的子模块，不经过临时克隆目录。失败时清理本次新建的目录，以便重试。
已有副本只报告状态并直接使用：普通构建不会拉取、切换版本或更新子模块，本地修改和提交可以直接参与构建。
旧的 `.workspace/Horizon/` 和 `.workspace/CoronaEditor/` 普通 Git 副本会整体迁移到引擎根目录，保留本地修改、
未跟踪文件及子模块；已有浅克隆也会复用，显式同步需要旧提交时才补全历史。

修改代码中的 commit 后，使用 `python builder/dev.py horizon-sync` 或
`python builder/dev.py editor-sync` 显式对齐对应工作区及子模块，然后重新 configure/build。
同步不会强制覆盖未提交修改，也不会重置含有额外本地提交的分支；子模块同步失败会报错，可修复后重试。
`python builder/dev.py status` 显示两者的版本和本地状态；`update` 只更新 Conan 依赖，不拉取源码仓库。

Python 依赖检查使用 `CoronaEditor/editor/requirements.txt`；构建时将资源部署为
可执行目录下的 `assets/` 和 `CabbageEditor/`，并自动构建前端。关闭编辑器构建仍准备该仓库，
因为它也提供共享资源。源码工作区通过进程锁保护，多个 preset 共用同一副本。
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
conda run -n coronaengine-dev python builder/dev.py build corona_engine
```

## 构建系统目录

根目录的 `CMakeLists.txt` 保留工具链初始化和 `project()`，其余顶层规则位于
`builder/cmake/corona_project.cmake`；根 `CMakePresets.json` 引用 `builder/CMakePresets.json`。
公共 CMake 模块、Conan profiles/recipes、构建辅助脚本及测试统一放在 `builder/`。
各模块自身的 `CMakeLists.txt` 继续与源码放在一起。

主工程 Conan 配方位于 `builder/conanfile.py`。手动安装依赖时使用
`conan install builder/conanfile.py`，并传入 `builder/conan/profiles/` 中对应的 profile
及所需选项；通常由 `builder/dev.py` 自动完成。
缓存管理命令为 `python builder/conan_cache.py`。

构建结果仍位于 `build/conan/<目标族>/<配置>/`，源码工作区仍位于根目录的
`Horizon/` 和 `CoronaEditor/`。从旧目录布局更新后，先执行一次 Configure 或
`builder/dev.py configure`，刷新生成文件中的脚本路径，再使用 `build-fast`。

构建工具测试使用开发环境运行：

```powershell
conda run -n coronaengine-dev python -m pytest builder/tests
```

引擎核心测试统一放在根目录 `tests/`，按模块组织；目录约定和运行方式见 [测试说明](tests/README.md)。
