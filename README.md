# CoronaEngine

## 环境要求

当前工作流仅支持 **Windows x64**。

| 工具 | 要求 |
| --- | --- |
| Git | 克隆源码及管理锁定的 Horizon 工作区 |
| [Miniconda / Conda](https://docs.conda.io/) | 管理开发工具 Python 环境和 Conan；无需单独安装 Python 或 Conan |
| CMake | 3.29 或更高版本 |
| Ninja | 必须在 PATH 中；工程使用 Ninja Multi-Config |
| Visual Studio 或 Build Tools | 安装“使用 C++ 的桌面开发”、MSVC x64/x86 工具集与 Windows SDK |
| CUDA Toolkit | 仅 Vision / Ocarina / Vision Hotfix 需要；安装后应存在 CUDA_PATH |

## 开发工具环境

首次使用时仅从 Conda Forge 创建 Conda 环境：

```text
conda create --yes --name coronaengine-dev --override-channels --channel conda-forge "python>=3.11" "conan>=2.28,<3"
```

后续通过 `conda run -n coronaengine-dev python tools/dev.py <command>` 运行开发工作流。若刚配置 Miniconda PATH，请重新打开终端或 IDE。

## 通过 CMake preset 构建

使用 CMake Tools选择目标族 preset，例如 <code>core-debug</code>、<code>examples-debug</code>、<code>tests-debug</code>、<code>vision-debug</code>、<code>vision-tests-debug</code> 或 <code>vision-oidn-debug</code>，再执行 Configure 和 Build。
