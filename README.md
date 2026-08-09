# CoronaEngine

## 环境要求

当前工作流仅支持 **Windows x64**。

| 工具 | 要求 |
| --- | --- |
| Git | 克隆源码及管理锁定的 Horizon 工作区 |
| [uv](https://docs.astral.sh/uv/) | 管理 Python 环境和 Conan；无需单独安装 Python 或 Conan |
| CMake | 3.29 或更高版本 |
| Ninja | 必须在 PATH 中；工程使用 Ninja Multi-Config |
| Visual Studio 或 Build Tools | 安装“使用 C++ 的桌面开发”、MSVC x64/x86 工具集与 Windows SDK |
| CUDA Toolkit | 仅 Vision / Ocarina / Vision Hotfix 需要；安装后应存在 CUDA_PATH |


## 通过 CMake preset 构建

若使用 CMake Tools，选择目标族 preset，例如 <code>core-debug</code>、<code>examples-debug</code>、<code>tests-debug</code>、<code>vision-debug</code>、<code>vision-tests-debug</code> 或 <code>vision-oidn-debug</code>，再执行 Configure 和 Build。根 CMakeLists.txt 会调用开发 bootstrap，生成并加载对应目标族和配置的 Conan toolchain。

旧的 <code>debug</code>、<code>release</code>、<code>relwithdebinfo</code> 和 <code>minsizerel</code> preset 保留为 <code>examples-*</code> 的兼容别名。新开发应直接选择带目标族前缀的 preset。
