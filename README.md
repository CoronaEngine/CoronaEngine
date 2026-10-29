# CoronaEngine

## 环境要求

当前工作流仅支持 **Windows x64**。

| 工具 | 要求 |
| --- | --- |
| Git | 克隆源码及管理锁定的 Horizon 工作区 |
| [Miniforge](https://github.com/conda-forge/miniforge) / [Miniconda / Conda](https://docs.conda.io/) | 管理 Python、Conan 和 Node.js；无需单独安装这些工具 |
| CMake | 3.29 或更高版本 |
| Ninja | 必须在 PATH 中；工程使用 Ninja Multi-Config |
| Visual Studio 或 Build Tools | 安装“使用 C++ 的桌面开发”、MSVC x64/x86 工具集与 Windows SDK |
| Vulkan SDK | Vulkan 头文件、加载器与构建工具；Slang 由 Conan 提供 |

## 开发工具环境

CMake Configure 会自动创建或复用 `coronaengine-dev` 环境（可通过 `CORONA_CONDA_ENV` 指定环境名称）。
CMake 首次配置会将 [CoronaEditor](https://github.com/CoronaEngine/CoronaEditor) 浅克隆到仓库根目录的
`CoronaEditor/`，直接使用其中的 `assets/` 和 `editor/`，不会再嵌套一层 `CoronaEditor/`。
已有副本直接复用，不自动拉取或覆盖本地修改；需要更新时在该目录中手动执行 Git 更新。
配置时会递归初始化 CoronaEditor 声明的子模块，并检出其记录的提交。
Python 依赖检查使用 `CoronaEditor/editor/requirements.txt`；构建时仍将资源部署为可执行目录下的
`assets/` 和 `CabbageEditor/`，并自动构建前端。根目录原有的 `assets/`、`editor/` 不再参与构建。
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

引擎核心测试统一放在根目录 `tests/`，按模块组织；目录约定和运行方式见 [测试说明](tests/README.md)。
