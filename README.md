# CoronaEngine

## 环境要求

当前工作流仅支持 **Windows x64**。

| 工具 | 要求 |
| --- | --- |
| Git | 克隆源码及管理锁定的 Horizon 工作区 |
| [Miniforge](https://github.com/conda-forge/miniforge) / [Miniconda / Conda](https://docs.conda.io/) | 管理开发工具 Python 环境和 Conan；无需单独安装 Python 或 Conan |
| CMake | 3.29 或更高版本 |
| Ninja | 必须在 PATH 中；工程使用 Ninja Multi-Config |
| Visual Studio 或 Build Tools | 安装“使用 C++ 的桌面开发”、MSVC x64/x86 工具集与 Windows SDK |
| CUDA Toolkit | 仅 Vision / Ocarina / Vision Hotfix 需要；安装后应存在 CUDA_PATH |

## 开发工具环境

VS Code / CMake Tools

使用 VS Code 的 CMake Tools 时，在设置中将 <code>CMake: Use VS Developer Environment</code> 设为 <code>always</code>，或加入用户设置：

~~~json
"cmake.useVsDeveloperEnvironment": "always"
~~~

否则 CMake Tools 单独启动的进程可能没有 MSVC 和 Windows SDK 环境，常见表现为 <code>fatal error C1083</code> 或找不到 <code>stddef.h</code> 等标准头文件。

## 通过 CMake preset 构建

使用 CMake Tools选择目标族 preset，例如 <code>core-debug</code>、<code>examples-debug</code>、<code>tests-debug</code>、<code>vision-debug</code>、<code>vision-tests-debug</code> 或 <code>vision-oidn-debug</code>，再执行 Configure 和 Build。

引擎核心测试统一放在根目录 `tests/`，按模块组织；目录约定和运行方式见 [测试说明](tests/README.md)。

## 引擎启动配置

`corona_engine.exe` 启动时读取**进程当前工作目录（cwd）**下的 `CoronaEngine.json`。
仓库根目录的 [CoronaEngine.default.json](CoronaEngine.default.json) 是默认模板。
在选定的工作目录中，将模板复制为 `CoronaEngine.json`，再修改配置；从仓库根目录启动时也遵循此规则。
工作目录由终端当前目录、IDE 的 `cwd` 或快捷方式的“起始位置”决定。修改文件后，重新启动引擎即可生效，无需重新编译。

当前 VS Code 启动配置把工作目录设为 EXE 所在目录，因此构建会在该目录的配置缺失时，从默认模板生成 `CoronaEngine.json`；
后续构建保留已有配置的本地修改。最终读取位置始终由启动时的工作目录决定。

```json
{
  "profiling": {
    "switch_profile": false
  }
}
```

把 `switch_profile` 改成 `true` 即可开启 PT/ReSTIR 切换耗时记录。优先级为：
`CORONA_SWITCH_PROFILE` 环境变量 > 配置文件 > 默认关闭。环境变量保持原有语义：`1` 开启，其他值关闭。
`tools/profile_render_switch.py` 仍会为其测试进程设置 `CORONA_SWITCH_PROFILE=1`。
启动日志会输出配置文件路径、是否加载成功及实际开关值。文件缺失时使用默认值；格式、字段名或类型错误会报错并终止启动。

这个文件由原生引擎入口在初始化前读取；`CoronaEditor.ini` 继续保存编辑器的项目状态，
`editor/config/app_config.py` 继续管理 Python 侧的应用配置。独立测试程序和其他嵌入式宿主仍可直接用环境变量控制 profiler。
