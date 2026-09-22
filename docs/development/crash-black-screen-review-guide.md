# CoronaEngine 启动崩溃与黑屏修复审查指南

文档日期：2026-09-22。适用于当前 `new_horizon` 分支上尚未提交的修复。本文提供审查方法和已有证据，不代替审核人的结论。

## 1. 审查对象与基线

| 项目 | 本次审查基线 |
| --- | --- |
| CoronaEngine 工作区 | `E:/work/corona/CoronaEngine` |
| CoronaEngine HEAD | `82fe89a739a4d443ab91dea97a874a28636662f6` |
| 实际依赖的 Horizon | `.workspace/Horizon`，提交 `5b4d13fad3801c5d66a62fbaffcdce6cd2ff984d` |
| 构建配置 | 当前 CLion 的 `RelWithDebInfo` profile，MSVC，生成器 `Ninja` |
| 构建目录 | `cmake-build-relwithdebinfo`，相对于 CoronaEngine 根目录 |
| 状态 | 5 个源文件有未提交修改；未提交、未推送；另新增本审查文档 |

上面两个提交分别属于不同仓库。审核时应以锁文件和实际嵌入工作区为准，不能用旁边独立的 Horizon 仓库替代运行时依赖。

先在 CoronaEngine 根目录执行以下只读检查，确认审查期间基线没有变化：

```powershell
Set-Location E:/work/corona/CoronaEngine
git status --short
git rev-parse HEAD
git diff --stat
git diff --check
git -C .workspace/Horizon rev-parse HEAD
git -C .workspace/Horizon status --short
```

建议依次打开下面五个文件的 **Git diff**。行号可能随后续修改变化，函数名作为主要定位依据。

| 顺序 | 文件与入口 | 核心问题 |
| --- | --- | --- |
| 1 | [vulkan_backend.cpp](E:/work/corona/CoronaEngine/src/systems/ui/vulk/vulkan_backend.cpp:224)：`new_frame`、`rebuild`、`unregister_surface`、`shutdown` | 空提交记录是否被传入等待；释放资源前是否真正等待完成 |
| 2 | [display_system.cpp](E:/work/corona/CoronaEngine/src/systems/display/display_system.cpp:869)：`ensure_composite_resources` | 首次创建合成输出时是否等待了空记录 |
| 3 | [optics_system.cpp](E:/work/corona/CoronaEngine/src/systems/optics/optics_system.cpp:2935)：资源重建、提交记录更新 | 等待条件和实际 GPU 提交是否匹配 |
| 4 | [quad_compositor.cpp](E:/work/corona/CoronaEngine/src/systems/ui/vulk/quad_compositor.cpp:109)：`composite` | 索引存储格式、上传长度和绘制解释是否一致 |
| 5 | [ui_multisurface_smoke.cpp](E:/work/corona/CoronaEngine/src/systems/ui/tests/ui_multisurface_smoke.cpp:23)：`verify_quad_pixels`、`run_smoke` | 测试能否检出原缺陷，而不只是证明程序没有退出 |

## 2. 先确认两条独立的故障链

### 启动崩溃

第一帧尚未被 Display 消费，`consumed_receipt` 为默认空记录。旧代码在 `VulkanBackend::new_frame()` 中无条件调用 `HardwareExecutor::wait()`，随后等待实现解引用了空的内部数据。

原始 dump 分析定位到 `HardwareExecutor::wait+0x45`，异常为 `0xC0000005`。现场指令读取 `[rdi]`，而 `rdi == 0`；上层调用来自 `VulkanBackend::new_frame`。这比“启动几秒后退出”的日志现象更能说明因果关系。

审查时分别确认：dump 是否指向这个调用点；默认 `SubmitReceipt` 是否为 `serial == 0` 且无内部数据；修复是否只跳过“没有工作需要等待”的情况。不能把所有访问冲突都归因于这一次缺陷。

### 黑屏

UI 合成器原来上传 `uint32_t` 索引，当前 Horizon 绘制路径按 16 位索引解释。一个 quad 的索引本应为 `{0,1,2, 0,2,3}`，实际前六个 16 位数被读成 `{0,0,1, 0,2,0}`。两个三角形均有重复顶点，面积为零，UI 无法覆盖输出图像。

因此，“网页返回 200”“绘制提交成功”“没有 crash”均不足以证明界面显示正确；必须检查最终像素。

## 3. 崩溃修复的逐项检查

### 3.1 空记录与有效记录

- [ ] `new_frame` 仅在 `consumed_receipt.serial != 0` 时等待；后续帧的有效依赖仍然保留。
- [ ] Display 首次创建输出图像，以及 UI/Optics 首次创建或调整资源，均不会等待默认空记录。
- [ ] `serial` 和内部提交数据只能来自有效的 `commit()` 返回值，不存在人为只填非零 `serial` 的情况。当前防护并不是任意损坏 receipt 的通用校验器。
- [ ] 已提交任务后的资源替换仍然等待原任务完成，没有以“避免崩溃”为由跳过真实依赖。

### 3.2 `wait` 与 `wait_idle` 的用途

本次依赖中，`wait(receipt)` 为后续提交建立 GPU 依赖；`wait_idle(receipt)` 等待指定提交完成后才返回。代码审查要依据实际实现确认这一语义，不能只按函数名推测。

| 场景 | 审查准则 |
| --- | --- |
| UI 开始下一帧，后续还会提交 GPU 工作 | 有效记录走 `wait`，保留 GPU 间的先后关系 |
| 窗口移除、后端关闭、图像替换 | CPU 即将释放或替换资源，应先等待相关有效记录完成 |
| 从未提交过的表面 | 没有记录可以等待，直接进行合法的清理流程 |

重点检查 `shutdown`、`unregister_surface` 和 Optics 重建路径中由 `wait` 改成 `wait_idle` 的位置：CPU 等待是否确有必要；等待时是否持有其他线程完成提交所必需的锁；是否意外把正常每帧路径变成全局阻塞。本次不包含性能基准结果。

### 3.3 Optics 的提交记录必须对应实际工作

修改新增了三处 `hardware_->last_receipt` 更新：原生渲染提交、actor picking 提交、Vision 提交。

- [ ] 每次更新紧跟成功的 `commit()`，保存的是该 executor 实际返回的 receipt。
- [ ] 提前返回或未发生提交的分支不会用空值覆盖仍需等待的记录。
- [ ] 资源重建等待的记录覆盖最后一次使用相关资源的工作。
- [ ] 如果工作跨队列，核对队列顺序或显式依赖；不能仅凭“保存了最后一个 receipt”就认定所有先前任务都完成。

这些 Optics 改动虽然参与主程序构建，但 UI 冒烟测试不覆盖完整场景渲染、拾取和 Vision 工作流。审核人需要把这部分单独评估。

## 4. 黑屏修复的逐项检查

- [ ] 索引容器改为 `std::vector<uint16_t>`。
- [ ] `idx_bytes`、`HardwareBufferDesc::element_size` 和上传用的 `span` 同时改为 16 位，不能只改其中一处。
- [ ] 每个 quad 仍使用局部索引 `{0,1,2,0,2,3}`。
- [ ] 第 `i` 个 quad 使用 `first_index = i * 6`、`vertex_offset = i * 4`；这两个值以索引/顶点为单位，不是字节偏移。
- [ ] 不把合并后的全局顶点编号塞进 `uint16_t`。当前局部索引加顶点偏移的方式，不因总顶点数超过 65535 就自动溢出。
- [ ] 缓冲容量判断和重新分配使用相同的字节计算，窗口尺寸与裁剪逻辑保持原有含义。

可在 [Horizon 公共头文件](E:/work/corona/CoronaEngine/.workspace/Horizon/include/horizon.h:494) 中核对当前 `HardwareIndexType` 限制。后续如果 Horizon 支持可选索引格式，应重新检查接口契约，不能将本次结论无条件套用于新版本。

## 5. 回归测试究竟证明什么

### 像素测试

`verify_quad_pixels` 创建一个 32×16 的实际显示表面，通过 UI → Display 合成链路绘制红色纯色 quad 和带裁剪的蓝色纹理 quad。它读取的是 Horizon FrameHash 捕获的最终合成输出，不是浏览器页面截图。

| 像素位置 | 预期结果 | 检查目的 |
| --- | --- | --- |
| `(4,12)`、`(12,4)` | 红色，alpha 为 1 | 红色 quad 的两个三角形都绘制 |
| `(26,12)`、`(30,2)` | 蓝色，alpha 为 1 | 纹理采样、第二个 quad 的偏移与两组三角形 |
| `(20,8)` | 黑色，alpha 为 1 | 裁剪区外保持 Display 的不透明黑色背景 |

输出为 RGBA16F；`0x3c00` 表示半精度浮点数 1，不是 8 位通道值。测试使用已提供 GPU 到 CPU 同步及一致性处理的捕获路径，不能替换成未经同步的直接映射内存读取。

审核测试时还要确认：唯一临时文件名避免误读旧帧；读取长度完整；当前捕获实现首次建立设备、随后捕获，五次 present 能产生所需数据；异常路径先退休显示资源再销毁窗口；清理失败不会误报测试成功。

该测试依赖当前 Horizon 的 FrameHash 行为，使用 Windows 原生窗口和 `_putenv_s`。它会设置进程内诊断环境变量，其中 dump 路径在作用域结束时清除；不能将它直接当成平台无关、可任意嵌入长驻测试进程的工具。

### 生命周期测试

- `UiMultiSurfaceSmoke`：第一帧尚无提交记录时调用 `new_frame`/重建；多窗口、重复创建移除、未渲染表面的清理，以及新增的最终像素检查。该测试不启动 CEF。
- `UiSurfaceLifecycleTests`：表面生命周期状态与完成通知等已有逻辑。
- `UiSurfaceRemovalRaceTests`：移除、回调和帧协调的已有并发逻辑。不能将其通过等同于全部真实 GPU 竞态已排除。

## 6. 审核人如何复验

### 6.1 使用当前 CLion 配置构建

在 CLion 中选择现有 `RelWithDebInfo` profile，核对其构建目录为 `cmake-build-relwithdebinfo`、生成器为 `Ninja`。构建下列 target：

```text
corona_engine
corona_ui_multisurface_smoke
corona_ui_surface_lifecycle_tests
corona_ui_surface_removal_race_tests
```

也可以在 CoronaEngine 根目录的 PowerShell 中执行下面的等效调用。它从现有 CLion 缓存读取 CMake 路径，使用已经准备好的 Conan/MSVC 环境，不切换到另一套 preset，也不硬编码工具安装路径。

前提：该 CLion 构建目录已成功配置，依赖没有变化，测试 target 已启用。若前提不满足，先在原 CLion profile 中完成配置。

```powershell
conda activate coronaengine-dev
@'
from pathlib import Path
import os
import subprocess
import sys

root = Path.cwd()
build = root / "cmake-build-relwithdebinfo"
cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
cmake = Path(next(line.split("=", 1)[1] for line in cache.splitlines()
                  if line.startswith("CMAKE_COMMAND:INTERNAL=")))
sys.path.insert(0, str(root / "tools"))
from workflow import load_conan_build_environment
env = load_conan_build_environment(root, "RelWithDebInfo", "examples")
subprocess.run([
    str(cmake), "--build", str(build), "--config", "RelWithDebInfo",
    "--target", "corona_engine", "corona_ui_multisurface_smoke",
    "corona_ui_surface_lifecycle_tests", "corona_ui_surface_removal_race_tests",
    "--parallel", "1",
], env=env, check=True)

# Windows 下 PATH 键大小写可能不同，保留环境中的既有键。
path_key = next((key for key in env if key.upper() == "PATH"), "PATH")
env[path_key] = str(build / "examples" / "engine") + os.pathsep + env.get(path_key, "")
env["CORONA_RUN_GPU_SMOKE"] = "1"
subprocess.run([
    str(cmake.with_name("ctest.exe")), "--test-dir", str(build),
    "-C", "RelWithDebInfo", "--output-on-failure",
    "-R", "^(UiMultiSurfaceSmoke|UiSurfaceLifecycleTests|UiSurfaceRemovalRaceTests)$",
], env=env, check=True)
'@ | python -
```

验收时必须看到三项测试都执行并通过。GPU 测试没有启用时会返回跳过码 77；显示 `Skipped` 不能作为通过证据。`ninja: no work to do` 也只能说明现有产物无需重建，不能独立证明测试执行了。

### 6.2 运行真实主程序

使用现有 CLion `corona_engine` 运行配置，保留正确的资源工作目录。建议观察至少 30 秒，并记录以下结果；这些是审核人复验项目，不代表本次已逐项执行：

- [ ] 启动进入菜单，标题、按钮和背景实际可见。
- [ ] 调整窗口大小、最小化并恢复后，画面仍可恢复。
- [ ] 正常关闭窗口，进程退出码为 0，没有新的访问冲突。
- [ ] 如需批准 Optics 相关改动，进入实际场景，验证相机视口重建、actor picking 和使用到的 Vision 路径。

`OnLoadEnd status=200` 只证明页面加载；正常退出只证明该次运行生命周期完成。最终画面和运行路径覆盖必须另外记录。性能检查时应关闭 FrameHash 抓帧，以免诊断读回影响结果。

### 6.3 用对照实验确认测试能抓住缺陷

对照实验只在可安全恢复的审查副本中进行，保留其余修复和新增测试，每次只还原一个因素。不要对包含用户修改的工作区做整仓 reset 或整文件覆盖。

| 实验 | 暂时改变什么 | 预期 |
| --- | --- | --- |
| 崩溃对照 | 只去掉 `new_frame` 对空 `consumed_receipt` 的防护 | GPU 冒烟测试在初始空记录路径出现访问冲突；用栈确认仍是等待路径 |
| 黑屏对照 | 将索引容器、字节长度、element_size、上传 span 四处一致改回 32 位 | 像素测试失败，红色采样点读到黑色 |
| 恢复修复 | 撤销对照实验的临时改动，重新构建测试 | 三项测试全部通过 |

旧索引实测失败信息为：`expected half-float bits 15360,0,0,15360 but got 0,0,0,15360`。alpha 仍为 1 是 Display 背景的预期，不应要求整像素四个通道全为零。

## 7. 已有证据与验证边界

下表是 **2026-09-21 的历史验证记录**。2026-09-22 整理本指南时核对了源代码差异、配置及已有日志，未重新构建或运行主程序。

| 证据 | 记录与结论 |
| --- | --- |
| 原始 crash 调用栈 | [crash-analysis-before.log](E:/work/corona/CoronaEngine/build/crash-analysis-before.log)：等待路径空指针访问，调用来自 `new_frame` |
| 修复前 GPU 测试 | [crash-smoke-before.log](E:/work/corona/CoronaEngine/build/crash-smoke-before.log)：`UiMultiSurfaceSmoke` 出现访问冲突 |
| 主程序构建 | [black-screen-final-engine-build.log](E:/work/corona/CoronaEngine/build/black-screen-final-engine-build.log)：当前 CLion 配置的构建记录；当时构建命令退出码为 0 |
| 32 位索引对照 | [black-screen-negative-control.log](E:/work/corona/CoronaEngine/build/black-screen-negative-control.log)：预期红色而读到黑色，测试失败 |
| 恢复修复后的测试 | [black-screen-final-tests.log](E:/work/corona/CoronaEngine/build/black-screen-final-tests.log)：3/3 通过，0 失败，总计 3.72 秒 |
| 真实启动 | [black-screen-loaded-run.stdout.log](E:/work/corona/CoronaEngine/build/black-screen-loaded-run.stdout.log)：21:40:00 页面加载完成，21:40:18 引擎完成退出；该次运行工具记录的退出码为 0 |
| 实际最终画面 | [black-screen-loaded-frame.png](E:/work/corona/CoronaEngine/build/black-screen-loaded-frame.png)：从 GPU 最终 RGBA16F 输出转换，显示标题、菜单和背景 |

这些证据位于本地忽略的 `build` 目录，不会随源码提交自动保存。交给另一位审核人时应附上相关日志和截图；不能只交文档链接。

已执行过修改区域的格式检查和 `git diff --check`。这不等同于完整静态分析。本次验证以 Windows、RTX 2070 Max-Q 上的 UI 冒烟和启动菜单为主；未完成全项目测试、完整游戏流程、所有 Optics 路径、其他 GPU/驱动和性能回归测试。

排查期间还曾出现内存/显存分配失败，见 [black-screen-fixed-run.stderr.log](E:/work/corona/CoronaEngine/build/black-screen-fixed-run.stderr.log) 中的 `GL_OUT_OF_MEMORY`。这类故障没有包含在本次修复中；如复验再出现，应保留首个分配失败或异常栈单独归因，不能用最后一次成功启动掩盖它。

## 8. 审核结论记录模板

审核人可复制以下内容填写：

```text
审查版本：CoronaEngine HEAD / 工作区差异快照；Horizon commit
审查环境：CLion profile、生成器、编译器、GPU/驱动
结论：通过 / 需修改 / 证据不足

空 receipt 防护：是否仅跳过无提交情况，是否保留有效依赖
资源生命周期：CPU 释放前的等待、锁顺序、提交记录覆盖情况
索引契约：16 位容器/长度/上传一致，局部索引与顶点偏移正确
测试：实际执行的用例、是否出现 Skipped、对照实验结果
实机：已走过的界面与场景、最终画面、退出码
未覆盖：Optics/其他设备/性能/内存压力等具体项目
问题清单：文件与函数、触发条件、影响、证据、建议修复
```

批准应建立在代码契约与运行证据一致的基础上。若发现非空任务被跳过、资源提前释放、像素检查读了旧文件，或测试实际被跳过，应退回修改或补充验证。
