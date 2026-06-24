# 2026-06-20 终稿执行方案

> 本节是当前终稿执行入口。下面旧正文和评审记录仍保留为历史审计材料，但其中 `origin/main = 5f5f6d41`、`HEAD = 47a1a7af`、`ahead 7 / behind 77`、`预测 10 个冲突文件` 等结论已经过期，不再作为本次 merge 的执行依据。

## 当前 Git 快照

- 当前分支：`merge_horizon_into_engine`
- 当前 HEAD：`7403d843e81e8a47b5d5868a4b326e7eaab0d121`
- 当前 `origin/main`：`518d112e12031bcd2f34f9fd8b33dad1036a3da0`
- 当前 merge base：`5f5f6d416f554dbab7a09590e9fe4c52ad79f878`
- 当前分支相对 `origin/main`：`ahead 9 / behind 145`
- 当前工作区状态：无 merge 进行中；除本文档外不应有其它工作区改动。

本次正式 merge 前必须先刷新并复核：

```powershell
git fetch origin
git rev-parse HEAD
git rev-parse origin/main
git merge-base HEAD origin/main
git rev-list --left-right --count HEAD...origin/main
git merge-tree --write-tree --name-only HEAD origin/main
```

如果 `origin/main` 已不再是 `518d112e12031bcd2f34f9fd8b33dad1036a3da0`，必须按新的 `merge-tree` 结果更新本节，不得继续沿用下面 3 个冲突文件的结论。

## 当前预测冲突范围

基于当前 `HEAD=7403d843e81e8a47b5d5868a4b326e7eaab0d121` 与 `origin/main=518d112e12031bcd2f34f9fd8b33dad1036a3da0` 的非破坏式 `git merge-tree --write-tree --name-only HEAD origin/main`，预测文本冲突文件为 3 个：

- `src/systems/optics/optics_system.cpp`
- `src/systems/ui/cef/browser_manager.h`
- `src/systems/ui/vulk/browser_manager_vulkan.cpp`

旧正文中列出的 10 个冲突文件属于 2026-06-19 旧快照，当前只作为历史背景；本次执行以这 3 个冲突文件和下方自动合并审查清单为准。

## 冲突处理策略

### `src/systems/optics/optics_system.cpp`

以 `origin/main` 的新业务功能为骨架，保留并移植本分支对 Horizon `main` API 的适配：

- 保留 main 新增的截图请求、离屏截图 target、LOD buffer 选择、Vision/geometry/material 更新和其它业务逻辑。
- 保留本分支的 `Horizon::` 命名空间类型、`Horizon::SubmitReceipt`、`last_receipt()`、`consumed_receipt`、`storeStorageDescriptor()`、`storeSampledDescriptor()` 和 `bind_storage_image()` 路径。
- ⚠️ **本文件最大、最高风险的冲突段（risk #9 的 Vision pipeline 重构）整段位于 `#ifdef CORONA_ENABLE_VISION` 内**。该宏由 `CORONA_BUILD_VISION` 决定，而后者仅在检测到 CUDA SDK 时默认 ON、否则被静默 force-OFF（只有一条 warning）。**因此 Vision OFF 的构建根本不会编译这段冲突解决代码，绿色构建不代表本文件已验证。** 验收本文件必须在 `CORONA_BUILD_VISION=ON`（即 CUDA SDK 可用）下进行，详见 §合并后构建与测试验收 的前置条件。
- 所有 Horizon 生成 push constant 的 `ktm::uvec2`、`ktm::fvec3`、`ktm::fvec4`、`ktm::fmat4x4` 写入继续走 `upload_value(...)` 包装，避免回退到不可平凡拷贝类型直接赋值。
- `target_visibility.record(...)` 应吸收 main 的 LOD 后 `render_ib/render_vb`，同时保持 Horizon `main` 生成 pipeline 类型和 descriptor 写法。
- 对 shared image 覆写前等待 `consumed_receipt`，提交后写回 `submit_receipt`；不要恢复旧的 `consumed_executor` 语义。

### `src/systems/ui/cef/browser_manager.h`

合并目标是“main 的延迟销毁功能 + 本分支的 Horizon receipt API”：

- include 必须保持 `#include "horizon.h"`，不得恢复 `<Horizon.h>`。
- `OwnedImage` 必须保留 `Horizon::HardwareImage image`、`Horizon::SubmitReceipt upload_receipt`、`width`、`height`。
- 公共接口保留 `get_texture_image(ImTextureID)` 和 `wait_for_texture_upload(ImTextureID)`。
- 吸收 main 的 `DeferredTextureDestroy`、`deferred_texture_destroys_`、`frame_index_` 和 `retire_deferred_tab_textures(bool force = false)`。
- 不得恢复 `wait_for_texture_uploads(HardwareExecutor&)`、未限定 `HardwareExecutor`、`texture_executor_`。

### `src/systems/ui/vulk/browser_manager_vulkan.cpp`

保留 main 的“关闭/resize 后延迟 4 帧销毁 browser texture”行为，但底层实现必须使用本分支的 Horizon `main` 同步模型：

- 保留 `descriptor_to_texture_id(uint32_t descriptor)` 的 `descriptor + 1` 映射，避免 `ImTextureID == 0` 与无效 texture 冲突。
- `create_browser_texture()` 继续创建 `Horizon::HardwareImageDesc::texture_2d(...)`，上传透明初始像素，并保存 `upload_receipt`。
- `update_texture()` 更新前等待旧 `upload_receipt`，上传后写回新的 `upload_receipt`。
- `destroy_tab_texture()` 不立即 erase GPU image；应把 `OwnedImage` 移入 `deferred_texture_destroys_`，记录 `frame_index_`，并把 tab 的 `texture_id` 置为 `k_invalid_texture_id`。
- `retire_deferred_tab_textures(force)` 负责在 `force=true` 或超过 4 帧后，等待对应 `upload_receipt` 再释放队列项。
- 不得恢复 `texture_executor_.waitForDeferredResources()`、`copyFrom(...)`、`consumer.wait(texture_executor_)` 或 `wait_for_texture_uploads(...)`。

## 自动合并文件审查清单

除了 3 个冲突文件，下列文件可能被 Git 自动合并但仍需人工审查：

- `misc/cmake/corona_third_party.cmake`（**最高优先级 / 对应硬性目标 #1**）：经核此文件只有本分支改过、`origin/main` 自 merge base 起未再动，所以 `git merge-tree` 把 Horizon 依赖**自动解析为本分支的 `GIT_TAG main`**——结果正确，但它既不在 3 个冲突文件、也不会进入冲突解决流程，**没有任何步骤强制有人去看它**。合并解析阶段必须显式确认 Horizon 依赖来自 `main`（不是 `merge`）；通过构建/运行验收并记录 SHA 后，最终可复现验收/交付 commit 应按 §依赖策略把 `GIT_TAG main` 替换为当时 `main` 实际解析到的 40 位 commit SHA，并在验收记录中保留该 SHA 来源。
- `src/systems/ui/cef/browser_manager.cpp`：必须保留 main 的 `++frame_index_`、`retire_deferred_tab_textures()` 和 `close_all_tabs()` 末尾 `retire_deferred_tab_textures(true)`，同时不能引入旧 `texture_executor_`。
- `src/systems/ui/vulk/vulkan_backend.cpp`：必须继续通过 `BrowserManager::get_texture_image()` 和 `wait_for_texture_upload(ImTextureID)` 绑定 CEF texture，不得恢复 `wait_for_texture_uploads(res.executor)`。
- `CMakeLists.txt`：必须继续保留 `BUILD_CORONA_TESTING` 下的 `CORONA_RESOURCE_BUILD_TESTS ON`，同时吸收 main 的新增构建/测试入口。
- `src/systems/optics/CMakeLists.txt`：必须吸收 main 新增的 `corona_vision_material_adapter_tests`，并保留既有 Vision 测试 target。
- `include/corona/shared_data_hub.h`、`include/corona/systems/optics/optics_system.h`、`src/systems/script/python/corona_engine_api.cpp`：确认自动合并后仍使用 `Horizon::` 类型、`SubmitReceipt`、`consumed_receipt` / `submit_receipt`，并保留 main 新增业务 API。

## 合并后静态硬门槛

冲突解决后必须先运行：

```powershell
git diff --name-only --diff-filter=U
git diff --check
rg -n '^<{7}|^={7}|^>{7}' --glob '!build/**' --glob '!cmake-build*/**' .
rg -n -e '#include[ <"]+Horizon\.h' -e 'texture_executor_|wait_for_texture_uploads|consumed_executor|waitForDeferredResources' -- src include
rg -n -e 'storeStorageDescriptor|storeSampledDescriptor|bind_storage_image|SubmitReceipt|last_receipt|consumed_receipt|submit_receipt' -- src include
rg -n -A4 'FetchContent_Declare\(Horizon' misc/cmake/corona_third_party.cmake
```

要求：

- 无未解决冲突文件。
- `git diff --check` 通过。
- 无 conflict marker。
- `src include` 中不得出现大写 `<Horizon.h>`、`texture_executor_`、`wait_for_texture_uploads`、`consumed_executor`、`waitForDeferredResources`。
- storage/sampled descriptor、receipt、`bind_storage_image` 命中必须覆盖 Display、Optics、CEF/UI Vulkan backend 关键路径。
- **`corona_third_party.cmake` 中 Horizon 必须来自 `main`，不得是 `merge` 或其它旧分支**——合并解析阶段应为 `GIT_TAG main`；最终可复现验收/交付阶段应为已记录、且确认来自当时 `main` 的 40 位 SHA。

## 合并后构建与测试验收

正式验收使用 MSVC Developer 环境与 `RelWithDebInfo`：

> **前置条件（硬性，否则下面命令会直接失败或假性通过）：必须 `CORONA_BUILD_VISION=ON`。**
> `CORONA_BUILD_VISION` 仅在检测到 CUDA SDK 时默认 ON，无 CUDA 时会被 `corona_options.cmake` **静默 force-OFF（只打一条 WARNING，不报错）**。一旦 Vision 被关掉：
> 1. 下面 build 命令里的 5 个 `corona_vision_*` test target **根本不存在**，`ninja` 会以 "unknown target" 失败；
> 2. 更危险的是 `optics_system.cpp` 的 Vision 冲突段（risk #9）全在 `#ifdef CORONA_ENABLE_VISION` 内，**Vision OFF 时不参与编译**，于是“构建通过”会掩盖本次合并最难的冲突解决完全未被验证（正撞硬性目标 #5 的“能编译一部分”陷阱）。
>
> 因此：配置后必须确认 cache 里 `CORONA_BUILD_VISION=ON`；若机器无 CUDA SDK，必须先装好 CUDA 或换有 CUDA 的机器，**不得在 Vision OFF 状态下宣布验收通过**。

```powershell
& $env:ComSpec /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --preset ninja-msvc'

& $env:ComSpec /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build --preset msvc-relwithdebinfo --target corona_engine'

& $env:ComSpec /d /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul && cmake --build --preset msvc-relwithdebinfo --target corona_resource_tests corona_network_protocol_tests corona_vision_render_mode_config_tests corona_vision_material_adapter_tests corona_vision_pipeline_key_tests corona_vision_scene_resource_tests corona_vision_geometry_gpu_resource_tests'

ctest --test-dir build -C RelWithDebInfo --output-on-failure -R "ResourceManagerTests|NetworkProtocolTests|VisionRenderModeConfigTests|VisionMaterialAdapterTests|VisionPipelineKeyTests|VisionSceneResourceTests|VisionGeometryGpuResourceTests"
```

验收记录必须包含：

- `cmake --preset ninja-msvc` 中 `BUILD_CORONA_EXAMPLES=ON`、`BUILD_CORONA_TESTING=ON`、`CORONA_BUILD_VISION=ON` 的实际 cache 状态。
- `corona_engine` target 构建结果。
- 上述测试 target 构建结果和 CTest 结果。
- `build/_deps/horizon-src` 实际解析到的 Horizon commit SHA；最终交付/验收 commit 中 `corona_third_party.cmake` 的 Horizon `GIT_TAG` 必须等于该 SHA。
- 如果 editor frontend 的 npm build warning 复现，只能在 C++ target 已成功后记录为非阻断 warning；不得用它掩盖第一处真实 C++/CMake 错误。

## 合并后运行验收

构建通过不是终点。本节硬性目标 #5 要求“引擎可运行、核心路径不黑屏/不丢刷新/不资源竞争”，必须实际跑 `corona_engine` 验证（详细判据见正文 §运行验收）：

最低运行验收（全部必须通过）：

- 引擎启动不崩溃；主 viewport 不黑屏；UI 能刷新；Optics final color 能输出。
- camera viewport 打开、关闭、resize 不崩溃（直接对应 Horizon `main` 缺失 `dafbb54d` 的 resize 回归风险）。
- CEF/ImGui UI texture 正常显示。
- 开启 `VK_LAYER_KHRONOS_validation` + synchronization validation 跑关键路径，日志中无 `VUID` / `SYNC-HAZARD` / `VK_ERROR` / validation error。这是“无 GPU read/write race、无旧图像提前释放、无 layout 错误”的硬判据，肉眼“没有明显问题”不作为通过标准。若当前没有开启 validation 的入口/启动参数，须先补。

## Horizon `main` 缺失修复门槛

切到 Horizon `main` 后丢失 `merge` 的 `2afbb937`（submit 前队列校验）与 `dafbb54d`（窗口缩放假死/`sourceImageID` 输入图缓存刷新）两笔修复（详见正文 §合并执行建议 #7、文末评审轮次 3/5）。完成前须二选一并留明确 owner：

- (a) 把这两笔等价逻辑移植进 Horizon `main` 并合入；或
- (b) 用 Horizon `main` 当前代码做专项运行/压力验证，证明队列异常路径与窗口 resize 不回退。

两者都做不到时，合并任务标记为 blocked、不得标记完成，且不允许靠切回 `merge` 蒙混。

## 完成定义

本次 merge 只有在以下条件全部满足后才能标记完成：

- 3 个冲突文件全部按本节策略解决。
- 自动合并审查清单全部完成（含 `misc/cmake/corona_third_party.cmake` 确认为来自 Horizon `main`：解析阶段为 `GIT_TAG main`，最终交付阶段为已记录的 main SHA）。
- 静态硬门槛全部通过。
- Horizon 依赖来自 `main`，已记录实际解析到的 Horizon commit SHA；最终交付/验收 commit 的 pin 值必须等于该记录 SHA。
- 验收构建在 `CORONA_BUILD_VISION=ON` 下完成（确认 cache 实为 ON、非被无 CUDA 静默 force-OFF）；否则 `optics_system.cpp` 的 Vision 冲突段未编译，验收无效。
- `RelWithDebInfo` 下 `corona_engine` 和列出的测试 target 构建通过。
- CTest 中列出的测试全部通过，或任何暂缓项都有明确原因、owner 和风险说明。
- §合并后运行验收 的“最低运行验收”全部通过（含 validation 层 0 error 硬判据）；扩展运行验收若有暂缓项须列明原因、负责人、风险。
- §Horizon `main` 缺失修复门槛 已满足（移植或专项验证二选一完成），无 blocked 项。
- 除本文档外，未引入与 merge 无关的工作区改动。

# Horizon Main API 接入分支合并需求

## 背景

当前工作分支是 `merge_horizon_into_engine`，目标是把 CoronaEngine 接入当前 Horizon 仓库的 `main` 分支 API，并在合并 CoronaEngine 主分支后保持引擎可构建、可运行。

当前 CoronaEngine 主分支快照相对本分支包含较多编辑器、UI、Optics、Vision 相关功能推进；本分支相对主分支主要包含 Horizon `main` API 迁移、shader 兼容、TBB/CMake 兼容、Display/Optics/UI Vulkan backend 资源同步修复。

本需求文档用于指导后续把 CoronaEngine `main` merge 进 `merge_horizon_into_engine` 时的冲突解决和验收。

## 硬性目标

1. 合并后 CoronaEngine 的 Horizon 依赖必须指向当前 Horizon 仓库的 `main` 分支。
2. 不允许通过回退旧 Horizon pin、切回旧兼容分支、或把 Horizon 依赖改成 CoronaEngine 主分支快照中的 `merge` 来掩盖迁移问题。
3. 合并后必须保留 CoronaEngine 主分支新增的编辑器、UI、camera viewport、3D UI、LightField UI warp、外部 Vision 场景导入、UI 图片导入等业务意图。
4. 合并后必须保留本分支为 Horizon `main` 新 API 所做的类型、descriptor、pipeline、runtime dependency 和 GPU 同步适配。
5. 验收标准不是“冲突消失”或“代码能编译一部分”，而是引擎在正式验收配置下可以运行，并且核心渲染/UI路径不黑屏、不丢刷新、不资源竞争。

## 已知 Git 状态

当前仓库检查结果：

- 当前分支：`merge_horizon_into_engine`
- 当前分支本地状态：无 merge 进行中；除本文档外无其它工作区改动。本文档当前是合并执行依据，若交给新 worktree 或新 AI，必须先确保它已提交，或明确要求在当前工作区继续执行。
- 本地 `origin/main` 快照：`5f5f6d41 尝试新版本（消除色散）`（2026-06-19 二次刷新；先前的 `b54c16f8`、更早的 `6144f6e7` 均已过时）
- 本分支 HEAD：`47a1a7af fix: Cmake tbb 报错`
- 本分支相对 `origin/main`：ahead 7 / behind 77

注意：`origin/main` 在 `b54c16f8` 之后又新增约 32 个 commit（主要是 **Vision pipeline 重构**：共享 Vision scene resource、pipeline runtime 所有权、持久化渲染模式、单全局 Vision pipeline 切换，外加 denoise/颜色、Optics 3D cursor、editor 前端）。已对 `5f5f6d41` 重跑非破坏性 merge 预测：**冲突文件仍是下列同样 10 个、merge base 仍为 `71b9e515` 未变**；但冲突内容显著变大（见 §预测冲突范围 与文末"评审轮次 6"）。正式开始合并前仍建议再刷新一次远端确认。

## 预测冲突范围

使用非破坏性 merge 预测后（已对 `origin/main = 5f5f6d41` 复核），预计有 10 个冲突文件（文件集稳定，但文末"评审轮次 6"显示 `optics_system.cpp` 等的冲突内容已随 main 的 Vision 重构大幅膨胀）：

- `include/corona/systems/display/display_system.h`
- `include/corona/systems/optics/optics_system.h`
- `misc/cmake/corona_third_party.cmake`
- `src/systems/display/display_system.cpp`
- `src/systems/optics/hardware.h`
- `src/systems/optics/optics_system.cpp`
- `src/systems/script/python/corona_engine_api.cpp`
- `src/systems/ui/cef/cef_client.cpp`
- `src/systems/ui/vulk/browser_manager_vulkan.cpp`
- `src/systems/ui/vulk/vulkan_backend.cpp`

这些冲突集中在渲染、UI、Optics、Python API、CEF、Horizon 依赖配置上，不能用简单的 ours/theirs 解决。

⚠️ **冲突文件 ≠ 全部工作面。** 还有一批两侧都改过、却被 git "无冲突"自动合并的文件，不进入冲突解决流程、却可能语义损坏，必须一并人工审查：`include/corona/shared_data_hub.h`、`src/systems/ui/cef/browser_manager.h`/`.cpp`、`src/systems/optics/vision/vision_zero_copy_bridge.*`、`src/systems/ui/imgui/imgui_ui.cpp`、`src/systems/ui/cef/browser_ui.cpp`。其中 `browser_manager.h` 已实测会自动合并出**两套互斥的 CEF 纹理上传同步设计且编译不过**，须与 `browser_manager_vulkan.cpp`、`vulkan_backend.cpp` 作为整体统一（详见文末"评审轮次 2"）。

## 冲突解决原则

### 依赖策略

`misc/cmake/corona_third_party.cmake` 中的 Horizon 依赖必须保留为：

```cmake
FetchContent_Declare(Horizon
    GIT_REPOSITORY https://github.com/CoronaEngine/Horizon.git
    GIT_TAG main
    EXCLUDE_FROM_ALL
)
```

如果 CoronaEngine 主分支引入了 `GIT_TAG merge`，合并时应改回 `main`。如果 `main` 上缺少主分支某个功能所需 API，应修复 CoronaEngine 或 Horizon `main` 的兼容问题，而不是切换依赖分支。

**可复现性（验收硬要求）**：`GIT_TAG main` 是会移动的分支引用，不能直接作为最终验收依据。正式验收时必须记录本次 `FetchContent` 实际解析到的 Horizon commit SHA；交付/验收 commit 上应以该 SHA pin（`GIT_TAG <sha>` 并注明它对应当时的 `main`），保证验收结果可被重放。

**注意 `main` 与 `merge` 已分叉、且不是 `main ⊇ merge`**：经核 `merge` 上有 `main` 没有的修复（见 §风险清单 #7、文末评审轮次 3/5），其中 `2afbb937`（submit 前队列校验）与 `dafbb54d` 的 `sourceImageID` 输入图缓存刷新 `main` 确实缺失。"修复 Horizon `main`" 的真实范围至少包含把这两笔移植进 `main`（见 §合并执行建议 #7）。`origin/main` pin `merge` 并非"掩盖"，而是其代码本就写在 `merge` 的 API/头文件形态上，故迁移到 `main` 是真实工作量、不是改个 pin。

### API 迁移

所有旧的 Horizon 类型和 include 方式需要迁移到 Horizon `main` 的新形态：

- 旧 `<Horizon.h>` include 应迁移到当前可用的 `horizon.h`。
- `HardwareImage`、`HardwareBuffer`、`HardwareExecutor` 等类型应带上 `Horizon::` 命名空间。
- 新 pipeline 生成类型如 `*_glsl_t` 应保留。
- `ImageUsage` 应按 Horizon `main` API 迁移为对应的新 usage flag 类型。
- `SubmitReceipt` 语义应保留，用于表达 GPU 提交和消费同步，而不是继续把 executor 当作完成状态。

### Descriptor 与资源跟踪

通过 bindless index 传给 shader 的 storage image 必须同时被 pipeline 显式注册：

- 写 push constant 时使用正确的 descriptor 获取函数。
- storage image 使用 `storeStorageDescriptor()`。
- sampled/ordinary image 使用对应的 sampled descriptor API。
- pipeline 侧必须调用 `bind_storage_image(slot, image)`，让 Horizon 的 barrier/layout/resource tracking 能看到真实依赖。

重点路径包括：

- Display composite：Optics image、UI image、composite output
- Optics lighting / sky / tonemap / debug resolve
- visibility debug resolve
- actor picking
- Optics UI overlay / LightField UI warp / composite
- Vision resolve
- CEF/ImGui offscreen texture上传和读取

### 主分支功能保留

合并时应以 CoronaEngine 主分支的新业务结构为骨架，移植 Horizon `main` API 适配，而不是删除主分支功能。

必须保留：

- camera viewport state 更新和 `SharedDataHub` 队列语义
- per-camera render backend 切换
- camera view open/close/resize state
- Viewport UI mode/calibration/pointer state
- LightField UI warp 相关 pipeline 和 shader
- UI 图片导入接口
- external Vision scene / actor binding 元数据
- **main 新增的共享 Vision scene resource 架构**（`vision_scene_resource.h`、`vision_pipeline_key.h`、`vision_render_mode_config`、持久化 Vision 渲染模式、单全局 Vision pipeline 切换、Pipeline 消费共享逻辑场景）——`b54c16f8` 之后 main 的大改，必须与本分支的 Horizon `main` Vision API 适配一起整合，不能简单选边
- secondary viewport 和 DisplaySystem surface 映射
- CEF offscreen UI 更新和 browser texture 生命周期管理

### Python API

`src/systems/script/python/corona_engine_api.cpp` 合并时必须同时保留两边意图：

- 保留主分支新增的 camera render backend、view state、size getter、actor guid、external Vision binding、image geometry API。
- 保留本分支为 Horizon `main` 做的 geometry/image/resource handle 迁移和同步修复。
- Camera / Actor / Geometry 的析构和移动语义不能导致 SharedDataHub 中残留悬挂 handle。

## 功能需求

### DisplaySystem

DisplaySystem 必须继续负责：

- 消费 Optics 输出图像和 UI 输出图像。
- 合成到最终输出图像。
- 把合成结果交给 displayer present。
- 正确等待上一帧 GPU 使用完成，避免 UI/Optics 写入与 Display 读取竞争。
- 正确处理 secondary viewport surface 的关闭和资源回收。

### OpticsSystem

OpticsSystem 必须继续支持：

- Native 渲染路径。
- Vision 渲染路径。
- per-camera render backend。
- camera viewport 更新。
- actor picking。
- screenshot 请求。
- debug output mode，包括 visibility buffer。
- follow-camera UI actor。
- 3D UI / LightField UI warp overlay。
- external Vision scene import 和动态同步。

### UI / Vulkan Backend

UI Vulkan backend 必须继续支持：

- main viewport 渲染。
- secondary viewport 渲染。
- ImGui draw data 上传和渲染。
- font atlas 上传同步。
- CEF browser texture 更新。
- ImTextureID 到 Horizon descriptor 的稳定映射。
- viewport render target resize 后不使用旧图像。

## 验收要求

### 构建验收

1. 所有冲突解决后，仓库不能含有 conflict marker。
2. `git diff --check` 必须通过。
3. 合并后必须跑静态迁移扫描，并人工确认无旧 API/旧同步设计残留：
   - `rg -n '#include[ <"]+Horizon\.h|texture_executor_|wait_for_texture_uploads' src include` 应为空，防止 Horizon 头文件大小写和 CEF 旧同步路径残留。
   - `rg -n '\b(HardwareExecutor|HardwareImage|HardwareBuffer|ImageUsage)\b' src include` 允许匹配 `Horizon::` 限定后的新类型，但不得有未限定旧类型或旧 usage 语义。
   - `rg -n 'storeStorageDescriptor|storeSampledDescriptor|bind_storage_image|SubmitReceipt|last_receipt|consumed_receipt|submit_receipt' src include` 用于复核 storage/sampled descriptor 分流和 receipt 同步链是否完整。
4. 使用 Visual Studio/MSVC 初始化环境构建，避免普通 PowerShell 引入额外噪声。
5. 正式验收配置优先使用 `RelWithDebInfo`。
6. 运行入口使用真实存在的 CMake target **`corona_engine`**（由 `examples/engine/CMakeLists.txt` 的 `corona_add_example(NAME corona_engine ...)` 注册；注意 `examples/engine` 是目录、不是 target）。`BUILD_CORONA_EXAMPLES` 默认 = `PROJECT_IS_TOP_LEVEL`（顶层构建即开启，但配置后仍应确认 cache 里确为 `ON`）。构建示例：`cmake --preset ninja-msvc && cmake --build --preset msvc-relwithdebinfo --target corona_engine`。（更正：原先写的 `corona_render_smoke_run` 在仓库中并不存在。）
7. 既有测试 target 也必须进入验收：`cmake --build --preset msvc-relwithdebinfo --target corona_resource_tests corona_network_protocol_tests`，再运行 `ctest --test-dir build -C RelWithDebInfo --output-on-failure -R "ResourceManagerTests|NetworkProtocolTests"`。
8. 构建日志中第一处真实错误必须被记录并修复，不能被 include spam 或低优先级 warning 淹没。

### 运行验收

最低运行验收：

- 引擎启动不崩溃。
- 主 viewport 不黑屏。
- UI 能刷新。
- Optics final color 能输出。
- camera viewport 打开、关闭、resize 不崩溃。
- CEF/ImGui UI texture 能正常显示。
- 开启 Vulkan validation layer + synchronization validation（`VK_LAYER_KHRONOS_validation` + sync val feature）跑关键路径，日志中无 `VUID` / `SYNC-HAZARD` / `VK_ERROR` / validation error。以此为"无 GPU read/write race、无旧图像提前释放、无 render target layout 错误"的**硬判据** —— 肉眼"没有明显问题"不作为通过标准。（若当前引擎没有开启 validation 的入口/启动参数，需求范围内须先补一个。）

扩展运行验收：

- Native / Vision backend 切换可用。
- LightField UI warp 可显示。
- UI 图片导入接口可用。
- actor picking 可返回正确 handle。
- visibility/debug output mode 可用。
- external Vision scene import 不破坏 Native 渲染路径。

## 风险清单

1. CoronaEngine 主分支快照中 Horizon 依赖为 `GIT_TAG merge`，与本需求要求的 Horizon `main` 冲突。
2. Horizon `main` API 变化可能与主分支新增 UI/Optics 功能同时作用在同一文件，简单选边会丢功能。
3. Vision 可能仍存在旧 hotfix 依赖或旧 Horizon target 假设，必须用真实构建日志定位第一处错误。
4. CEF、Python、TBB runtime 复制逻辑可能在 `RelWithDebInfo` 下暴露 Debug/Release 路径混用。
5. 通过 storage image descriptor index 访问的 shader 如果没有 `bind_storage_image` 注册，会导致 Horizon 无法追踪 layout/barrier，表现为黑屏或 stale UI。
6. Camera viewport 和 secondary viewport 生命周期如果和 GPU receipt 不匹配，可能出现关闭窗口后资源仍被 GPU 使用。
7. Horizon `main` 缺少 `merge` 的 `dafbb54d 修复窗口缩放假死`，切到 `main` 后 **resize 卡死可能回归**（直接撞 §运行验收 的 "resize 不崩溃"）；同样缺 `2afbb937` 的 submit 前队列校验，异常路径更易崩。（已实测，详见文末"评审轮次 3"。）
8. 两侧都改过的文件被 git 自动合并（无冲突标记）可能语义损坏，尤以 CEF `browser_manager.*` 为甚（已实测两套同步设计被拼合、编译不过），且这些文件不在上面 10 个冲突文件内。
9. `origin/main` 的 Vision pipeline 重构使 `optics_system.cpp` 冲突体量翻倍（main-vs-base 从 ~1183/337 增至 **~2329/403**）。这是合并里最大、最高风险的单点：main 的共享 Vision scene resource/runtime 重构 × 本分支的 Horizon `main` Vision API 适配，必须逐段对齐而非选边，否则要么丢 main 的新 Vision 架构、要么丢本分支的 GPU 同步/类型迁移。（已实测，详见文末"评审轮次 6"。）

## 合并执行建议

1. 合并前先刷新远端，确认 CoronaEngine `main` 和 Horizon `main` 都是最新状态。
2. 先解决 `misc/cmake/corona_third_party.cmake`，明确 Horizon `GIT_TAG main`。
3. 再解决 10 个冲突文件中的 shared API/header 类冲突，包括 DisplaySystem、OpticsSystem、VulkanBackend、Python API。
4. **核查并审查自动合并文件**：对两侧都改过、却被 git 无冲突合并的文件逐个 diff（至少 `browser_manager.h`/`.cpp`、`shared_data_hub.h`、`vision_zero_copy_bridge.*`、`imgui_ui.cpp`、`browser_ui.cpp`），重点核 `executor→receipt` 改名与 `Horizon::` 限定是否被 main 旧代码破坏。CEF 纹理上传同步必须把 `browser_manager.*` + `browser_manager_vulkan.cpp` + `vulkan_backend.cpp` 统一到 receipt 设计（已实测此处自动合并不过编译）。
5. 以主分支的功能结构为骨架，逐段移植 Horizon `main` API 适配。
6. 对每个 shader storage image index 写入点补齐 `bind_storage_image`（注意：CEF/ImGui/font atlas 走 sampled 路径、用 `storeSampledDescriptor`，不要在这些点误补 `bind_storage_image`）。
7. **处理 Horizon `main` 缺失的修复（硬门槛，非"可接受风险"）**：`main` 没有 `merge` 的 `2afbb937`（submit 前队列校验）与 `dafbb54d` 的 `sourceImageID` 输入图缓存刷新。完成前二选一并留明确 owner：(a) 把这两笔等价逻辑移植进 Horizon `main` 并合入；(b) 用 Horizon `main` 当前代码做**专项运行/压力验证**，提供证据证明异常队列路径与窗口 resize 不回退。**两者都做不到时，合并任务标记为 blocked、不得标记完成**；不允许靠切回 `merge` 蒙混。
8. 解决完冲突后先做 §构建验收 #3 的静态迁移扫描，再做 MSVC `RelWithDebInfo` 构建。
9. 构建失败时只追第一处真实错误，修复后重新提取下一处错误。
10. 构建通过后再做运行验收，重点看黑屏、UI刷新、camera viewport、CEF texture、Optics debug mode。

## 完成定义

本合并任务完成必须同时满足：

- CoronaEngine 成功合并当前主分支。
- Horizon 依赖来自当前 Horizon 仓库 `main`，且**验收已记录并 pin 实际解析到的 Horizon commit SHA**（见 §依赖策略 可复现性）。
- 当前分支的 Horizon `main` API 迁移意图未丢失。
- CoronaEngine 主分支新增的 UI/Optics/Vision/editor 意图未丢失。
- `RelWithDebInfo` 构建通过，且 target `corona_engine` 能运行。
- `corona_resource_tests`、`corona_network_protocol_tests` 与对应 CTest 项通过。
- **§运行验收 的"最低运行验收"全部通过**（不是"至少一个入口"）；扩展验收若有暂缓项，必须列明原因、负责人与风险。
- 关键渲染、UI、camera viewport、CEF texture 路径经运行验证，且 §运行验收 的 validation 层硬判据（无 `VUID`/`SYNC-HAZARD`/`VK_ERROR`）通过。
- §合并执行建议 #7 的 Horizon `main` 缺失修复门槛已满足（移植或专项验证二选一完成），无 blocked 项。

---

# 附录：历史评审记录（正文为准）

> 用途：以下为历史评审和证据链。执行时以正文（本附录之前的内容）为准；附录中的旧快照、旧命令和待裁决措辞不得覆盖正文最新基线。
>
> 核验快照：`HEAD = 47a1a7af`。`origin/main` 初评时为 `6144f6e7`，**2026-06-19 fetch 后为 `b54c16f8`**（见"远端核验结果"与"评审轮次 4"）；两个快照下 10 个冲突文件一致。下文若有 `6144f6e7` 字样均为初评历史记录，正文已统一更新为 `b54c16f8`。
>
> 本附录仍保留多轮评审原话，供追溯使用；当前定稿基线已在正文更新到 `origin/main = 5f5f6d41`、ahead 7 / behind 77。

## 评审轮次 1（reviewer: Claude Code, Opus 4.8）

### 先肯定：文档准确的部分（已逐项核实属实）

- git 状态全部正确：分支、HEAD、`origin/main` 快照、ahead 7 / behind 44。
- §预测冲突范围 的 **10 个冲突文件与 `git merge-tree HEAD origin/main` 的实际 CONFLICT 集完全一致**。
- §依赖策略 的 pin 描述属实：HEAD = `GIT_TAG main`，`origin/main` = `GIT_TAG merge`（`misc/cmake/corona_third_party.cmake:54`）。
- API 名称全部真实存在：`bind_storage_image`、`storeStorageDescriptor`、`SubmitReceipt`、`storeSampledDescriptor`，以及 `<Horizon.h>` → `horizon.h` 的大小写迁移。

下面是需要修正/补强/反驳的点。

### 远端核验结果（fetch 成功后补充，核验日 2026-06-19）

文档 §已知 Git 状态 写"fetch 因 TLS 失败"。本轮 fetch **已成功**，核到以下事实（与文档快照已有出入）：

1. **`origin/main` 已前移**：`6144f6e7` → **`b54c16f8 fix vision pipeline`**（+1 commit，改动 `vision/src/base/mgr/pipeline.cpp`，正落在 Vision 路径）。ahead/behind 现为 **7 / 45**。**但重算后 10 个冲突文件不变** —— 故 §预测冲突范围 的清单对此次更新仍成立（印证 R9：快照虽旧，结论暂稳）。

2. **Horizon `main` 与 `merge` 已经分叉（不是祖先关系）**，这是 R5 的关键证据：
   - `git ls-remote`：`main = f850f17a`，`merge = 930f73ad`。
   - `merge..main` / `main..merge` 计数 = **343 / 9** —— `main` 领先 343 个 commit，**但 `merge` 上有 9 个 commit 不在 `main`**：
     - `f4ffc018 fix: Handle GPU sync and cleanup in destroyImage`
     - `2afbb937 fix: Handle invalid/missing queues before submit`
     - `67258a63 添加遗留头文件`（很可能就是 `Horizon.h` 兼容头）
     - `d947b716 refactor: 基于 fix 迁移 vision hotfix`
     - `dafbb54d 修复窗口缩放假死`
     - `ba7cea14 Update DisplayManager.cpp`、`6a5b93ae 修改导出接口`、`41994352 fix: fix fix`、`930f73ad Merge 'whiteThrush'`
   - `main` 侧确有 `4cdd103d destroyImage逻辑修改`、`11bc430c fix: main 分支增加 hotfix` —— 说明 **部分** merge 修复在 main 上被独立重做过，但**并非全部** 9 个都能在 main 找到对应（如 `2afbb937 missing/invalid queues before submit`、`dafbb54d 窗口缩放假死` 未见明显对应）。

**已核验根因（`git ls-tree` 两侧 Horizon 树）**：Horizon `merge` 提供 `include/Horizon.h`（**大写**），Horizon `main` 已改名为 `include/horizon.h`（**小写**）。这就是 `origin/main` 代码用 `#include <Horizon.h>` 能对 `merge` 编译、而切到 `main` 后必须把所有 include 改成小写的根因（见 R11，大小写敏感平台会直接找不到头）。**结论：`origin/main` pin `merge` 不是"掩盖迁移问题"，而是其代码本就写在 `merge` 的 API/头文件形态上。** （更正：先前据 commit `67258a63 添加遗留头文件` 的 message 猜测"merge 保留兼容头"——经核该 commit 仅改 1 行 `.cpp`，与头文件无关，该猜测作废；真正证据是上面的头文件改名。）

### 高严重度

**R1（事实错误）验收入口 `corona_render_smoke_run` 不存在**
- 位置：§验收要求/构建验收 #5、§合并执行建议（多处）。
- 证据：`git grep -rn "smoke" HEAD` 在全仓库（除 `third_party/Python-*` 与无关文档外）查无 `corona_render_smoke_run`，无该 target 定义。
- 真实可运行入口：`examples/engine`（`examples/engine/main.cpp`，经 `corona_add_example` 注册）。且 `BUILD_CORONA_EXAMPLES` 默认 = `PROJECT_IS_TOP_LEVEL`（`misc/cmake/corona_options.cmake:25`），**顶层构建默认开启** —— 故"如果 examples 当前未启用"这一前提多半也不成立。
- 建议：把验收入口改成真实 target（`examples/engine`）；如确需独立 smoke target，须先在 `examples/` 新建再在文档引用。

**R2（覆盖盲区）冲突处理范围只圈了 10 个文本冲突文件，遗漏大量"静默自动合并但语义高耦合"的文件**
- 位置：§预测冲突范围、§冲突解决原则、§合并执行建议。
- 证据：`git merge-tree` 输出里除 10 个 CONFLICT 外，以下文件为 `Auto-merging`（不会进入冲突处理流程），却恰好落在本文档硬性目标/功能需求点名的子系统：
  - `include/corona/shared_data_hub.h` —— 承载 `SubmitReceipt submit_receipt / consumed_receipt`（即**硬性目标 #4 的 GPU 同步状态**）。两侧差异约 177 行，且 `origin/main` 侧**完全没有 `SubmitReceipt`**（`git grep -c SubmitReceipt origin/main` = 0）。
  - `src/systems/optics/vision/vision_zero_copy_bridge.cpp / .h` —— Vision 路径，正对应**风险 #3** 的旧 hotfix 担忧。
  - `src/systems/ui/cef/browser_manager.cpp / .h`、`browser_ui.cpp` —— CEF texture 生命周期。
  - `src/systems/ui/imgui/imgui_ui.cpp` —— ImGui draw data 上传。
- 关键点：**文本层 auto-merge 成功 ≠ 语义正确**。我已实测当前快照 merged `shared_data_hub.h` 仍保留 2 处 `SubmitReceipt`（字段未被丢弃），但"字段还在"不代表"`main` 新增的 camera viewport 队列等路径正确地 set/consume 了这些 receipt"。
- 建议：文档新增一节"**自动合并但需人工语义审查**"清单，要求对上述文件逐个 diff（尤以 `shared_data_hub.h`、vision bridge、`browser_manager`、`imgui_ui` 为重）。当前 §合并执行建议 完全没有覆盖它们。

**R3（可复现性缺口）`GIT_TAG main` 不是可复现的 pin**
- 位置：§硬性目标 #1、§依赖策略、§完成定义。
- 问题：`main` 是会移动的分支引用，`FetchContent` 每次拉取可能得到不同 Horizon commit；"正式验收"建立在不可复现依赖上，验收结论无法被重放。文档禁止一切其它 pin，却未处理可复现性。
- 建议：要求**记录并在验收 commit 固定本次实际解析到的 Horizon commit SHA**（或 CMake 用 `GIT_TAG <sha>` 并注明对应 main 的哪次提交）。"必须指向 main"可改为"必须等价于 main 的某个明确 SHA"。

### 中严重度

**R4（技术不精确/类别错误）把 CEF/ImGui texture 归入"storage image 必须 `bind_storage_image`"**
- 位置：§Descriptor 与资源跟踪 的"重点路径"列表项"CEF/ImGui offscreen texture 上传和读取"；§合并执行建议 #5。
- 证据：UI 纹理走 **sampled** 路径，用 `storeSampledDescriptor()`（`browser_manager_vulkan.cpp:61`、`vulkan_backend.cpp:465/468/661`），经 `ImTextureID ↔ descriptor` 映射，**不**调用 `bind_storage_image`。其正确性不变量是：上传完成等待（`wait_for_texture_upload`）、`ImTextureID`/descriptor 映射稳定、resize 后不复用旧图 —— 与 storage 注册无关。
- 风险：§合并执行建议 #5"对每个 shader storage image index 写入点补齐 `bind_storage_image`"若被机械套到 UI sampled 路径会写错。
- 建议：把 storage image 注册路径与 sampled texture 路径（CEF/ImGui/font atlas）**拆成两组**，各写各自不变量。（注：`visibilityImage` 在不同 pass 既走 storage 写又走 sampled 读，说明同一图像按 pass 选 API —— 现有"必须同时"表述未体现这一点。）

**R5（逻辑缺口 + 假设已被证伪）硬性目标 #2 禁掉了唯一显而易见的回退却无 fallback；"`main` ⊇ `merge`"假设不成立**
- 位置：§硬性目标 #2、§依赖策略。
- 已核验（见上"远端核验结果"#2）：Horizon `main` 与 `merge` **已分叉**，`merge` 有 **9 个 commit 不在 `main`**，含 GPU sync（`destroyImage`）、submit 前的非法/缺失队列处理、遗留头文件、vision hotfix 迁移、窗口缩放假死等修复；其中数个在 `main` **未见对应**。故把 `origin/main` 选 `merge` 定性为"掩盖迁移问题"是**对动机的误判** —— `merge` 更像是 Horizon 侧为保留这些修复/兼容头而存在的集成分支。
- 问题：若切到 `main` 后确实缺这些修复所对应的 API/行为，文档要求"修复 Horizon main"—— 那是另一个仓库、可能不可控、范围不封顶；而 #2 又禁止临时切 `merge`，可能**死锁**。文档未给"无法在合理范围修好 Horizon main 时怎么办"的出口。
- 建议：(a) §合并执行建议 第 1 步**前置**"逐一核对那 9 个 merge-only commit 在 main 是否有等价实现"；(b) 补"Horizon main 缺 API 时的决策树 + 升级/暂缓路径"；(c) 对 `merge` 改用中性表述。

**R6（验收无效）运行验收无法真正检测它要求的"无 GPU read/write race"**
- 位置：§运行验收"没有明显 GPU read/write race…"、风险 #5/#6。
- 问题：竞争/同步错误通常间歇出现，肉眼跑一遍看不出；"没有明显"是主观判据，与风险 #5/#6 的硬要求不匹配。
- 建议：运行验收**必须开启 Vulkan validation layer + synchronization validation**（`VK_LAYER_KHRONOS_validation` + sync val feature），以"validation 0 error"为硬判据。否则风险 #5/#6 实际上无验收手段。

### 低严重度 / 一致性

**R7** 文档未引用已存在的 `CMakePresets.json`，构建指引偏口语。证据：仓库含 `ninja-msvc`（configure）+ `msvc-relwithdebinfo`（build）等 preset，真实生成器是 **Ninja** 而非 VS 生成器。建议 §构建验收 用具体命令：Developer 环境下 `cmake --preset ninja-msvc` + `cmake --build --preset msvc-relwithdebinfo`。

**R8** 缺自动化测试，验收全靠人工目视。证据：已有 `corona_resource_tests`、`corona_network_protocol_tests` 等可执行测试 target。建议验收纳入跑既有测试，GPU 路径再辅以目视。

**R9（已核：快照确已过时，但结论暂稳）** §预测冲突范围 把"10 个冲突文件"当既定事实，依据是 §已知 Git 状态 自述的"未 fetch 陈旧快照"。本轮 fetch 后 `origin/main` 已从 `6144f6e7` 前移到 `b54c16f8 fix vision pipeline`（+1，Vision 路径），但**重算冲突集仍是同样 10 个文件**。建议把 §已知 Git 状态 的 `origin/main = 6144f6e7` 更新为 `b54c16f8`，并在 §预测冲突范围 注明"已对 b54c16f8 重算、结论不变；后续仍需在合并前再算一次"。

**R10** "完成定义"弱于"运行验收"：后者列了一长串最低/扩展项，前者只要求"至少一个实际运行入口通过"。两处门槛不一致，需明确哪条是判定基准。

**R11（已核实：硬约束，非仅护栏）** include 大小写在 Windows 验收构建里不会被发现，但在大小写敏感平台是硬错误。**已核证据**：`git ls-tree` 显示 Horizon `main` 只有 `include/horizon.h`（小写），Horizon `merge` 是 `include/Horizon.h`（大写）—— 头文件在两分支间被改名。故切到 `main` 后，任何残留的 `#include <Horizon.h>`（尤其自动合并/直取自 main 的文件）在 `ninja-linux-*` / `ninja-macos`（presets 自带）上会**直接找不到头文件**；而强制的 Windows MSVC 验收（NTFS 大小写不敏感）即使写错也能编译，掩盖该错。当前快照 merged tree 暂无大写 include（自动合并恰好取了 HEAD 的小写），但务必：合并后跑 `grep -rE '#include[ ]*[<"]Horizon\.h' src include` 必须为空，且至少跑一次 Linux preset 构建。（已删除原先重复编号的旧版 R11"护栏建议"——已被本条取代。）

### 给下一位评审的开放问题

- ~~1. Horizon `main` 与 `merge` 的差异与先后？~~ **已核（见远端核验结果 #2）**：已分叉，`merge` 有 9 个 main 没有的 commit。
- ~~3. 远端 `origin/main` 是否已超出本地快照？~~ **已核**：已前移到 `b54c16f8`（+1，Vision），10 个冲突文件不变。
- **2（已收口，见"评审轮次 2"）**：R2 中自动合并文件合并后语义是否正确？—— 已实测：CEF browser texture 路径**确有**具体不一致（详见轮次 2），其余 hot 文件在类型/API 轴上干净。
- **4（已收口，见"评审轮次 3"）**：那 9 个 merge-only commit 在 Horizon `main` 是否有等价实现？—— 已实测：大多有等价或无关，但 **2 个真实缺口**（`2afbb937` 队列校验、`dafbb54d` 窗口缩放假死）`main` 确实没有。详见轮次 3。

## 评审轮次 2（自动合并文件语义审查，已实测 —— 收口开放问题 #2）

> 方法：非破坏式。merge base = `71b9e515`，对 fresh `origin/main`(`b54c16f8`) 跑 `git merge-tree --write-tree` 得 merged tree `6538d4a1`，再逐个 `git cat-file` 检查合并后 blob，并和两侧 vs base 的 diff 对照。**未改动工作区、未真正 merge。**

### 关键背景：本分支的改动是"重命名 + 改类型"，而 main 仍用旧名

HEAD 对 Horizon 的迁移不是加法，而是**改名/改类型**（`git diff base..HEAD`）：

- `#include "Horizon.h"` → `"horizon.h"`；`HardwareImage/Buffer/Executor` → `Horizon::` 限定。
- `ImageDevice`：`HardwareExecutor executor` → `Horizon::SubmitReceipt submit_receipt`，`consumed_executor` → `consumed_receipt`。
- `BrowserManager`：删除 `HardwareExecutor texture_executor_`，改为 `Horizon::HardwareExecutor browser_upload_executor_` + 每图 `Horizon::SubmitReceipt upload_receipt`。

而 `origin/main` 仍用旧名（`HardwareExecutor`、`texture_executor_`、`<Horizon.h>`）。**当两侧改的是文件不同区域时，git 会"无冲突"地把两者拼在一起，于是 main 的旧名残留进合并结果——但这些文件不在 10 个冲突文件里，没人会去看。**

### 逐文件结论（已实测）

| 自动合并文件（均不在 10 冲突清单内） | 类型/API 轴结论 | 说明 |
|---|---|---|
| `include/corona/shared_data_hub.h` | **干净** | HEAD 的 `executor→receipt` 改名与 main 的 168 行新增在不同区域；merged blob 无未限定 `Hardware*`、无旧 `executor` 字段引用，`SubmitReceipt` 保留。仍需运行期确认 main 新增的 producer 路径有正确 set/consume receipt。 |
| `vision_zero_copy_bridge.h/.cpp` | **干净**（类型轴） | 注：`origin/main` 最新 `fix vision pipeline` 改的是 `vision/src/...`，非此 bridge。 |
| `imgui_ui.cpp`、`browser_ui.cpp` | **干净**（类型轴） | 两侧改动都不碰 texture/descriptor/receipt/Hardware，属功能层，需逻辑 review 但无 API 重整。 |
| **`browser_manager.h` / `browser_manager.cpp`（CEF 纹理路径）** | **❌ 已证实不一致** | 见下。 |

### ❌ 已证实的具体不一致：CEF 纹理上传同步存在两套互斥设计

同一件事（等待纹理上传完成）两边各写了一套，且 auto-merge 把两套拼在一起：

- **main（executor 派，旧 API）**：`browser_manager.h:97 void wait_for_texture_uploads(HardwareExecutor& consumer);` + 共享字段 `texture_executor_`；实现 `browser_manager_vulkan.cpp:86 consumer.wait(texture_executor_)`；调用方 `vulkan_backend.cpp:479 wait_for_texture_uploads(res.executor)`。
- **HEAD（receipt 派，新 API）**：`wait_for_texture_upload(ImTextureID)`（单数）+ 每图 `SubmitReceipt upload_receipt` + `browser_upload_executor_`；**删除了 `texture_executor_`**。

合并后（实测 merged blob）：

1. `browser_manager.h` 同时含**两个**方法（`wait_for_texture_upload` 与 `wait_for_texture_uploads`），且 main 的那个签名是**未限定** `HardwareExecutor&` → 对 Horizon `main`（`Horizon::HardwareExecutor`，无全局别名）是**未声明标识符，所有平台编译失败**。
2. HEAD 删掉了 `texture_executor_`，但 main 的 `wait_for_texture_uploads` 实现要 `consumer.wait(texture_executor_)` —— 字段已不存在 → **二次编译错误**。
3. 该 header（`browser_manager.h`）与 `browser_manager.cpp` **都不在 10 冲突文件清单**，而真正会被人工解决的 `browser_manager_vulkan.cpp`、`vulkan_backend.cpp` 在清单内 —— **但你无法在不改那两个自动合并文件的前提下把清单内文件解对**，因为四个文件共享同一个"executor 还是 receipt"的设计抉择。

### 轮次 2 结论

- 这是 **R2 的实锤**：文档"10 个冲突文件 = 工作面"的范围划定**不安全**。至少 **CEF browser texture 路径**横跨 2 个冲突文件 + 2 个自动合并文件，必须作为一个整体统一到 receipt 设计；§主分支功能保留/§UI 都点了"CEF browser texture"，却没说要统一这两套同步设计、也没提那两个自动合并文件。
- 建议在 §合并执行建议 增加一步：**"合并后，对所有自动合并且两侧都改过的文件（至少 `browser_manager.h/.cpp`、`shared_data_hub.h`、`vision_zero_copy_bridge.*`、`imgui_ui.cpp`、`browser_ui.cpp`）逐个 diff，重点核 `executor→receipt` 改名与 `Horizon::` 限定是否被 main 旧代码破坏。"**
- 公平起见：除 CEF 路径外，本轮在类型/API 轴未发现其它自动合并文件的硬伤；但"类型轴干净"不等于"运行期同步语义正确"（仍需 R6 的 validation 层运行验收）。

## 评审轮次 3（Horizon `main` 是否丢失了 `merge` 的修复，已实测 —— 收口开放问题 #4）

> 方法：fetch Horizon 两分支到临时 ref，对那 9 个 merge-only commit 逐个 `git show` 取其**特征符号/日志串**，再 `git grep` Horizon `main` 看是否存在等价实现。已用完即删临时 ref。

### 结论：`main` 丢失了 `merge` 的 2 个真实修复

| merge-only commit | 内容 | Horizon `main` 是否有等价 | 证据 |
|---|---|---|---|
| `f4ffc018` destroyImage GPU sync | destroy 前等所有 queue timeline semaphore | ✅ **有** | main 的 `ResourceManager.cpp` 有 `collectQueueSemaphores`(×4) + `vkWaitSemaphores`；对应 main 自己的 `4cdd103d destroyImage逻辑修改` |
| `6a5b93ae` create_exported_buffer | CUDA↔Vulkan 零拷贝导出 buffer | ✅ **有** | main 的 `device.h` 有 `create_exported_buffer`；CoronaEngine 两分支的 `vision_zero_copy_bridge.cpp:51` 都在用 |
| `2afbb937` invalid/missing queues before submit | 空队列/全部 timeline 失效时安全中止；`displayFrame` 加 `currentRecordQueue==nullptr` 守卫 | ❌ **没有** | main 查无 `consecutiveInvalidTimelineValues` / `No queues available for submission` / `currentQueues.empty()` |
| `dafbb54d` 修复窗口缩放假死（含其后续修订）| 输入图缓存按 `sourceImageID`/`incomingSourceImageID` 刷新、`presentOutOfDate` 后重建 swapchain、`currentFrame=0` 等 resize/present 处理 | ❌ **没有** | main 查无 `sourceImageID`/`incomingSourceImageID`；merge tip 有（×4）。**更正（轮次 5 已核）**：`USE_SAME_DEVICE` 不作为差异——它在 merge tip 与 main tip **都是注释状态**（`dafbb54d` 当时启用过，但被 merge 上后续提交改回）。main 的 resize 路径是另一套结构，未必能独立规避假死，需运行核。 |
| `67258a63` / `ba7cea14` | 各 1 行琐碎改动 | — | 不影响 |
| `d947b716` 迁移 vision hotfix | 新增整套 `src/hotfix/`（2258 行）子系统 | n/a | CoronaEngine 本分支 `4ba19e3a "去掉 hotfix"` 已主动移除 hotfix 依赖，**很可能是有意不要**，按"不需要"处理即可 |
| `41994352` / `930f73ad` | 大块 executor/device/display 重构（文件集重叠） | 部分 | 其离散修复已被上面抽样覆盖（destroyImage 有、queue 守卫无）；未逐行 diff，收益递减 |

### 这对本合并任务意味着什么

1. **直接打脸硬性目标 #2**：文档禁止用 `merge`、要求"切到 `main` 后修复 Horizon `main`"。现在"修复"的范围是**具体的**：把 `2afbb937` 和 `dafbb54d` 这两笔重新移植进 Horizon `main`（**跨仓库**改动），否则就接受回退风险。这正是 R5 说的"范围可能不封顶、且 #2 没给出口"的实证。
2. **`dafbb54d` 正撞验收红线**：§运行验收 明确要求"camera viewport 打开、关闭、**resize 不崩溃**"。`main` 缺这笔"窗口缩放假死"修复 —— 切到 `main` 很可能**重新引入** resize 卡死，而这恰恰是验收要抓的项。建议在 §风险清单 增列此条，并在 resize 验收时专门盯。
3. **`2afbb937` 是健壮性兜底**：队列耗尽/timeline 失效时优雅中止而非崩。缺它在压力/异常路径下更易崩，间歇难复现。

### 建议补进文档

- §硬性目标 #2 旁注："切到 `main` 会丢失 Horizon `merge` 的 `2afbb937`(队列校验) 与 `dafbb54d`(窗口缩放假死) 两笔修复；二选一：(a) 把这两笔移植进 Horizon `main` 并合入；(b) 显式接受并在验收中验证 `main` 的对应路径无回退。不允许靠切回 `merge` 蒙混 —— 但必须为这两笔留明确 owner/计划。"
- §风险清单 增列："Horizon `main` 缺 `dafbb54d`，resize 假死可能回归。"

## 评审轮次 4（reviewer: Codex, 2026-06-19，需要下一位 AI 裁决/反驳）

> 方法：基于当前工作区重新核验文档、CoronaEngine `origin/main`、本地 Horizon checkout 与 Horizon 远端引用。未真正 merge，未修改代码，仅追加评审意见。请下一位 AI 对每条标记 **接受 / 反驳 / 已转入正文 / 暂缓**，反驳时附证据。

### 本轮重新核验的事实

- 当前 CoronaEngine 工作区无 merge 进行中；除本文档外无其它工作区改动，本文档仍是 untracked。
- 当前 CoronaEngine `origin/main = b54c16f8 fix vision pipeline`，`HEAD = 47a1a7af fix: Cmake tbb 报错`，`HEAD...origin/main = ahead 7 / behind 45`。
- `git merge-tree --write-tree HEAD origin/main` 仍预测同样 10 个文本冲突文件，merged tree id 为 `6538d4a1be5c26df8fc4c248ee11beae363fe6f9`。
- Horizon 远端引用仍为 `main = f850f17a30abe4bbbc9c5a90f4e7c20167c10be5`，`merge = 930f73addddd0678850c15fbc34e4ab1fef00edd`；本地 Horizon `origin/merge...origin/main` 计数仍是 `9 / 343`。
- 当前仓库无 `cmake-build-relwithdebinfo/CMakeCache.txt`、无 `build-main/CMakeCache.txt`，所以不能再用旧缓存结论断言当前验收 target；当前源码里真实 example target 是 `corona_engine`，由 `examples/engine/CMakeLists.txt` 的 `corona_add_example(NAME corona_engine ...)` 注册。

### 新增/复核意见

**C1（P1，需求正文仍未解决可复现性）`GIT_TAG main` 是开发目标，但不能直接作为最终验收 pin**

- 位置：正文 §硬性目标 #1、§依赖策略、§完成定义。
- 证据：Horizon `main` 当前是远端分支引用 `f850f17a...`，会继续移动；CMake `FetchContent` 使用 `GIT_TAG main` 时，别人或 CI 以后重新配置可能解析到不同 commit。
- 建议给下一位 AI 裁决：是否把正文改为"合并目标必须来自 Horizon `main`，但正式验收必须记录实际解析到的 Horizon commit SHA；最终交付可使用该 SHA pin，并注明它对应当时的 `main`"。如果坚持 `GIT_TAG main` 不 pin，需要解释如何重放验收结果。

**C2（P1，`merge-only` 修复不能只写成可接受风险）缺 `2afbb937` / `dafbb54d` 应成为合并完成前的硬门槛或明确阻塞**

- 位置：正文 §风险清单 #7、§合并执行建议 #7；评审轮次 3。
- 已核事实：Horizon `origin/merge` 有 `2afbb937 fix: Handle invalid/missing queues before submit`，`origin/main` 查无 `consecutiveInvalidTimelineValues` / `No queues available for submission` / `currentQueues.empty()` 等特征；这条队列健壮性修复确实不在当前 `main`。
- 对 `dafbb54d` 的更正：该 commit 本身曾把 `//#define USE_SAME_DEVICE` 改为 `#define USE_SAME_DEVICE`，但当前 Horizon `origin/merge` tip 里 `USE_SAME_DEVICE` 又是注释状态。因此正文/表格不要把"当前 merge 分支启用了 USE_SAME_DEVICE"当成事实。当前 `merge` 相对 `main` 更稳定的可见差异是 `sourceImageID / incomingSourceImageID` 输入图缓存刷新、`presentOutOfDate` 后重建 swapchain、`currentFrame = 0` 等 resize/present 相关处理。
- 建议给下一位 AI 裁决：正文应避免说"可以显式接受风险后完成"。更稳妥的完成条件是：(a) 把 `2afbb937` 和 `dafbb54d` 中仍缺失的等价逻辑移植进 Horizon `main`；或 (b) 用 Horizon `main` 当前代码做专项运行/压力验证，证明队列异常路径与窗口 resize 不回退。否则合并任务标记为 blocked，而不是完成。

**C3（P1，正文与评审记录混杂，下一位 AI 应先收口文档结构）**

- 位置：从 `# 评审记录（待解决）` 开始的整段。
- 问题：正文已经吸收了部分评审结论，但评审记录中仍保留旧快照、重复编号、已更正的猜测和"下一位评审"措辞。例如评审记录开头仍写 `origin/main = 6144f6e7`，而正文已更新为 `b54c16f8`。
- 风险：后续执行者可能不知道哪些是正式需求、哪些只是历史争论。尤其是"corona_render_smoke_run 不存在"这类结论已经被正文修正，但 memory 里仍有旧验收路径，容易被再次带回。
- 建议给下一位 AI 裁决：把已接受结论折入正文，把历史评审移到单独 `docs/planning/horizon-main-merge-review-log-cn.md` 或降级为附录；正文只保留最终决策和硬验收门槛。

**C4（P2，验收入口需要写成 target 级别，而不是目录级别）**

- 位置：正文 §构建验收 #5。
- 证据：`examples/engine/CMakeLists.txt` 注册的是 `corona_engine` target；`examples/engine` 是目录，不是 CMake target。当前源码中未找到 `corona_render_smoke_run` target；旧 memory 中出现过该 target，但当前仓库没有对应源码或 CMake target。
- 建议给下一位 AI 裁决：把构建验收改为具体命令形态，例如在 MSVC Developer 环境中先 `cmake --preset ninja-msvc`，再 `cmake --build --preset msvc-relwithdebinfo --target corona_engine`。同时要求配置后确认 `BUILD_CORONA_EXAMPLES=ON`，因为默认开启不等于当前 cache 一定开启。

**C5（P2，GPU 同步验收标准仍过于主观）**

- 位置：正文 §运行验收 "没有明显 GPU read/write race..."。
- 问题："没有明显"无法验收 storage image barrier、receipt、layout race。此类问题可能只在特定窗口 resize、secondary viewport close、CEF texture 更新、Vision resolve 或高帧率下偶发。
- 建议给下一位 AI 裁决：把 Vulkan validation layer 和 synchronization validation 写成硬验收项。最低要求应包括：开启 `VK_LAYER_KHRONOS_validation` 和 sync validation 后跑 `corona_engine` 的关键路径，日志中无 `VUID` / `SYNC-HAZARD` / `VK_ERROR` / validation error。若当前 Horizon/Corona 没有开关，需求中应新增"补验证入口或启动参数"。

**C6（P2，自动合并文件清单还应绑定到具体检查命令）**

- 位置：正文 §预测冲突范围、§合并执行建议 #4。
- 已有结论正确：10 个文本冲突不是全部工作面，`browser_manager.h/.cpp` 等自动合并文件必须人工审查。
- 建议补强：下一位 AI 应要求合并后至少跑这些静态检查：
  - `rg -n "HardwareExecutor|HardwareImage|HardwareBuffer|ImageUsage|#include[ <\"]+Horizon\\.h|texture_executor_|wait_for_texture_uploads" src include`
  - `rg -n "storeStorageDescriptor|storeSampledDescriptor|bind_storage_image|SubmitReceipt|last_receipt|consumed_receipt|submit_receipt" src include`
  - 对 `browser_manager.h/.cpp`、`browser_manager_vulkan.cpp`、`vulkan_backend.cpp` 做成组 diff，确认 CEF texture 路径统一到 receipt 设计。

**C7（P3，完成定义弱于运行验收）**

- 位置：正文 §完成定义。
- 问题：完成定义只说"至少一个实际运行入口通过"，但 §运行验收列了 camera viewport、CEF texture、LightField UI warp、Native/Vision backend 等更细要求。两者门槛不一致。
- 建议给下一位 AI 裁决：完成定义应引用"最低运行验收全部通过"，扩展验收中若有暂缓项必须列明原因、负责人和风险，而不是用"至少一个入口"替代。

### 给下一位 AI 的建议处理顺序

1. 先决定 C1/C2：Horizon `main` 是否要最终 SHA pin、缺失 `merge-only` 修复是否必须移植或专项证明。这两个决定会影响整个合并策略。
2. 再清理 C3：把评审记录和正式需求分离，避免执行时读到互相矛盾的历史状态。
3. 最后处理 C4-C7：把验收命令、validation、自动合并文件检查和完成定义写成可执行门槛。

## 评审轮次 5（reviewer: Claude Opus 4.8，2026-06-19 —— 裁决轮次 4 + 新增核验）

> 方法：对轮次 4（Codex）逐条裁决；凡有事实分歧都用 git 重新核验（重新 fetch Horizon 两分支临时 ref，用完即删）。另补扫了前几轮没覆盖的"自动合并的构建文件"与"main 单独新增、原样带入"的文件。

### 对轮次 4（C1–C7）的裁决

- **C1（SHA pin 可复现性）—— 接受**，与 R3 同一问题。属用户政策决定，建议正文采纳"目标 = Horizon `main`，但验收须记录并 pin 实际解析到的 SHA"。尚未写入正文。
- **C2（merge-only 修复应是硬门槛 + USE_SAME_DEVICE 更正）—— 其更正成立，立场倾向接受**：
  - USE_SAME_DEVICE：**Codex 对，我原表述误导**。已核：`dafbb54d` 当时把它从注释改为启用，但 **merge tip 与 main tip 现在都是注释状态**（merge 后续提交又改回）。故它不是 merge-vs-main 差异，已更正轮次 3 表格。
  - **但核心结论不变**：`sourceImageID`/`incomingSourceImageID` 输入图缓存刷新 merge tip 有（×4）、main **没有**（已复核）；`2afbb937` 队列校验 main 也确无。切到 main 仍丢这两部分。
  - 立场：倾向接受 Codex 更严要求——`dafbb54d` 撞 resize 验收红线，不宜只列"可接受风险"。建议 §合并执行建议 #7 的 (b) 加"必须有专项运行/压力验证证据，否则任务 blocked"。属用户风险偏好。
- **C3（正文与评审记录混杂、旧快照）—— 接受，已部分修复**：评审记录开头 `6144f6e7` 已改 `b54c16f8`；重复编号的旧版 R11 已删、R10/R11 顺序理顺。是否把评审历史拆到单独 `horizon-main-merge-review-log-cn.md` 留用户定（我倾向保留单文件存审计链，可在正文顶部加一行"正文为准、文末为评审过程"）。
- **C4（`corona_engine` target）—— 接受，已改正文**：已核 `examples/engine/CMakeLists.txt` = `corona_add_example(NAME corona_engine ...)`，target 名为 `corona_engine`、`examples/engine` 是目录。§构建验收 #5 已改为 target + preset 命令。
- **C5（validation 层硬验收）—— 接受**，与 R6 同。建议 §运行验收 增"开 `VK_LAYER_KHRONOS_validation` + sync validation，日志无 `VUID`/`SYNC-HAZARD`/`VK_ERROR`"为硬判据。尚未写入正文。
- **C6（静态检查绑定到命令）—— 接受**。正文 §合并执行建议 #8 已含 `Horizon.h` 大小写 grep；可再补 C6 的 `rg` 符号扫描（Hardware*/receipt 两条）。
- **C7（完成定义弱于运行验收）—— 接受**，与 R10 同。建议"完成定义"引用"最低运行验收全部通过"，暂缓项须列原因/负责人。尚未写入正文。

### 新增核验（前几轮未覆盖的面）

- **N1（自动合并的构建文件）**：`CMakeLists.txt`（顶层）与 `src/systems/ui/CMakeLists.txt` 两侧都改过、被自动合并，不在 10 冲突文件内。已实测 `ui/CMakeLists.txt`：HEAD 删 `ktm`、main 加 `camera_viewport_manager.cpp`，合并结果**两者都在、未丢源** —— 本快照 OK。但属 R2 同类静默风险，**合并后须确认没有 target/源文件被自动合并吞掉**（顶层 `CMakeLists.txt` 未逐行核，留作检查项）。
- **N2（main 单独新增、原样带入的文件 —— 好消息，圈定风险边界）**：merged tree 里 main 新增、HEAD 没有的文件共 10 个（`vision_pipeline_key.h`、`vision_scene_resource.h`、`vision_render_mode_config.*`、`camera_viewport_manager.*`、4 个 `test_vision_*.cpp`）。逐个扫过：**全部在 Horizon-API 轴干净**（无大写 `<Horizon.h>`、无未限定 `Hardware*`、无 `ImageUsage`），新测试也已被 `src/systems/optics/CMakeLists.txt` 接上。**结论：类型轴的编译地雷面基本只剩 `browser_manager.h` 那处 CEF 串（R2/轮次 2），其余收敛。** 是对 R2 的有力边界确认。
- **N3（未验证的地基假设，建议升级为前置步骤）**：整套计划假设 **HEAD 当前能对 Horizon `main`(`f850f17a`) 编译通过**。Horizon main 比分叉点多 343 个 commit；若其中某次改动悄悄打破了 HEAD 的 API 用法，合并会连带继承，且与 CoronaEngine 侧合并无关。前几轮都没真正构建过 HEAD。建议合并前**先单独构建一次当前 HEAD（不 merge）**确认基线可编译，再叠加 merge —— 否则构建失败时无法区分是 merge 引入还是 Horizon main 漂移引入。

### 本轮已直接改动正文/记录

1. §构建验收 #5：`examples/engine` → target `corona_engine` + preset 命令（C4）。
2. 评审记录开头旧快照 `6144f6e7` → `b54c16f8`（C3）。
3. 删重复编号的旧 R11、理顺 R10/R11 顺序（C3）。
4. 轮次 3 表格 `dafbb54d` 行：去掉 USE_SAME_DEVICE 误导，改以 `sourceImageID` 为据（C2）。

### 留给用户拍板（尚未写入正文，属政策/风险决定）

- C1：是否要求验收 SHA pin。
- C2：`2afbb937`/`dafbb54d` 是"必须移植或专项验证"的硬门槛，还是允许显式接受。
- C5/C7：validation 层硬验收、完成定义对齐运行验收，是否写成强制门槛。
- C3：评审历史是否拆到单独文件。

## 评审轮次 6（reviewer: Claude Opus 4.8，2026-06-19 二次刷新 —— CoronaEngine main 又前移后重核）

> 触发：`origin/main` 从 `b54c16f8` 前移到 `5f5f6d41`（+约 32 commit）。方法：非破坏式重算 `merge-tree` + 逐文件复核，未改工作区、未真正 merge。

### 哪些结论不变（已复核仍成立）

- **冲突文件仍是同样 10 个**，merge base 仍为 `71b9e515`。
- **编译地雷面仍收敛在 `browser_manager.h`**：新 merged tree 无大写 `<Horizon.h>`，未限定 `Hardware*` 仅 `browser_manager.h` 1 处（CEF 两套上传同步设计仍被拼合，R2/轮次 2 不变）。
- `shared_data_hub.h`（`SubmitReceipt` 2 处保留）、`vision_zero_copy_bridge.*`、`imgui_ui.cpp`、以及全部 main-only 新增 Vision/test 文件 —— 类型/API 轴**仍干净**。
- **Horizon 未移动**（`main=f850f17a`、`merge=930f73ad`，与记录一致）→ 轮次 3 的两笔缺失修复（`2afbb937`/`dafbb54d`）与头文件改名（`Horizon.h`→`horizon.h`）结论**仍有效**，无需重做。

### 唯一显著变化：Vision 重构把 `optics_system.cpp` 冲突顶大了一倍

- `optics_system.cpp` main-vs-base：`b54c16f8` 时 ~1183/337 → `5f5f6d41` 时 **~2329/403**；在 `b54c16f8..5f5f6d41` 区间它单文件 +1520 行，是本次更新改动最大的文件。
- 内容是 main 的 Vision pipeline 重构：共享 Vision scene resource、runtime 所有权移出 Pipeline、持久化渲染模式、单全局 Vision pipeline 切换。
- 影响：`optics_system.cpp`（冲突文件）现在是合并里**最大、最高风险**的单点 —— 须把 main 的新 Vision 架构与本分支的 Horizon `main` Vision API 适配逐段对齐。已加为风险 #9，并把"共享 Vision scene resource 架构"补进 §主分支功能保留。
- 其余更新（`imgui_ui.cpp` +179、editor 前端 `CameraView`/`MainPage`、新 Vision 测试、denoise/颜色、Optics 3D cursor）不改变冲突文件集，`imgui_ui.cpp` 仍类型干净。

### 已据此更新正文

§已知 Git 状态（→ `5f5f6d41` / behind 77 / +32 commit 说明）、§预测冲突范围（复核说明）、§主分支功能保留（+共享 Vision scene resource 架构）、§风险清单 #9。其余正文门槛与文末轮次 1–5 结论不受影响。
