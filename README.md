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
| Vulkan SDK | Vulkan 头文件、加载器与构建工具；Slang 由 Conan 提供 |

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

Vision 在所有构建目标族中始终启用，计算设备固定为 Horizon/Ocarina Vulkan Compute。
引擎配置不再包含 `vision.backend`；旧配置应删除整个 `vision` 节。
独立 `vision-eval`、`vision-gui` 和 SDK 同样固定使用 Vulkan，不再接受设备后端选择参数。
SDK ABI 已升至 `0x00020000`；使用旧版结构或接口的调用方需要重新编译。
相机仍可选择常规渲染或 `vision`，并独立选择 PT、ReSTIR、降噪器和降噪开关。
计算设备由 OpticsSystem 持有，场景切换时复用；关闭时先释放共享输出、管线与场景资源，
再显式销毁设备，避免依赖进程终止时不会执行的静态析构。
Vulkan 运行文件（后端 DLL、Slang DLL 和 `slang/` 内建着色器）由 `corona_engine_vision_runtime` 自动部署。

Vision 通过可导出的 Vulkan 缓冲区向引擎传递线性 HDR 颜色，显示端应用曝光、ACES 和 sRGB 转换。
Vulkan 共享内存校验 GPU/驱动 UUID 并执行跨实例资源所有权交接；不支持共享时使用 CPU 回读。
`CORONA_VISION_DISABLE_ZERO_COPY=1` 可强制回读，用于排查共享输出问题。

受散热限制的对照测试可使用 `tools/vision_thermal_run.py` 启动进程。它持续记录 NVIDIA 温度，
默认到 54°C 提前终止本次测试的进程树，遥测失败也会停止测试；这不能代替硬件自身的温度保护。
可用 `--limit 60` 将停止阈值明确设为 60°C，程序不接受更高值；温度约每 100 ms 采样一次。
运行记录同时保存实际可执行文件和后端 DLL 的 SHA-256，便于确认测试所用构建。
启动前若 NVIDIA 仍报告 `corona_engine.exe` 或 `vision-eval.exe` 的 GPU 上下文，测试会拒绝启动。
退出后同时检查进程树和驱动上下文是否回收，记录各 GPU 的显存初值、峰值、结束值与差值；
显存差值是整张 GPU 的读数，也可能受其它桌面应用影响，不能单独作为内存泄漏证据。
温度中止、异常退出、遥测失败或回收不完整都会返回非零退出码，阻止批量测试继续运行。
静态前端验证可设置 `CORONA_OPTICS_MAX_FPS=2`。独立 `vision-eval` 可设置
`VISION_EVAL_FRAME_INTERVAL_MS=500` 在帧间休息；输出的渲染计时不包含休息时间，
`frame_interval_ms` 会写入指标，因此该场景下的 `average_fps` 表示渲染耗时的倒数，而非实际显示帧率。
对比不同构建的画质与性能时，使用相同分辨率、相机、采样数、ReSTIR 参数、
Stable Planes、降噪器与历史帧数；原始图像与各次测试记录分别保存。
