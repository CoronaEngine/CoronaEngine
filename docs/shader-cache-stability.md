# PT / ReSTIR shader 缓存稳定性

资源注册返回的 bindless slot 是运行时数据。资源创建顺序或数量变化时，slot 可以变化；shader 的算法和布局没有变化时，slot 不应参与编译缓存标识。

## 实现

当前 Vision 实际链接 `.workspace/Horizon/modules/ocarina`。修改落在该依赖的工作树及 CoronaEngine 的 Vision 调用点中；Horizon 的迁移版 `src` 目录不在本次改动范围。

- `EncodedData<uint>::as_parameter()`：已解码的数据继续使用现有设备表达式；未解码的索引使用现有 captured resource 机制，追加为 kernel 的标量参数。
- `Registrable::index_var()`：公共资源读取、写入和采样统一使用上述参数。普通 `EncodedData::operator*()` 的常量语义保持不变。
- ReSTIR 的 reservoir base、FrameBuffer 的双缓冲 base、光谱表、alias 表及材质 LUT 使用设备端索引。双缓冲的 host 查询仍返回 host 整数。
- 首次捕获索引时惰性分配地址稳定的存储；资源和 shader 在热重载中一起移动时，参数地址保持有效。复制资源会深拷贝参数，向已捕获的参数复制赋值则原址更新。参数存储的所有者必须覆盖 shader 的使用期；替换一个已有 owner 时需同时替换其 shader。
- 构造一次 shader invocation 时，`ShaderArgumentPack` 复制当时的参数值；已经排队的 invocation 不受之后索引更新的影响。不能把已捕获参数改成 host getter；这个限制在 Release 中也检查。
- AST 的 Scope 哈希跳过纯注释节点。CUDA 的 AST / IR 输出显式关闭注释，并消除注释留下的分号、空 else 和 else-if 形式差异；默认诊断输出仍保留注释。代码生成缓存版本升为 2，因此升级后会有一次冷编译。

shader 的算法、类型、布局及实际编译选项仍影响缓存。跨不同场景若这些内容变化，编译仍然必要；本改动不承诺所有不同场景共享同一份 shader。

## 验证入口

构建 `corona_vision_shader_binding_tests` 和 `corona_vision_embedded_mode_switch_tests`。

`VisionShaderBindingTests` 检查同一 shader 在不同 slot 下标识一致、复用后读取各自的 buffer、参数更新在下次 invocation 生效、排队参数快照独立、资源移动及复制语义，以及注释中路径/行号/数量的变化不改变语义标识与编译源。

真实场景的跨进程检查：

```powershell
.\.venv\Scripts\python.exe tools/check_shader_cache_stability.py --scene E:/work/corona/CoronaExample/test_vision/render_scene/kitchen/vision_scene.json
```

脚本使用 `profile_process.run_bounded`，每个进程默认硬超时 300 秒，有 Windows Job Object 清理和独立日志。连续运行两次，各执行 PT → ReSTIR → PT → ReSTIR；第二次在每个新运行时创建视图前注册 7 个无用槽位。要求：

1. 两次运行阶段完整、进程成功退出，第二次确实执行槽位扰动，且 PT 的 visibility、ReSTIR 的 visibility / surfaces 实际 slot 均发生变化（PT 不分配 surfaces）。
2. 两次 shader 标识及数量相同。
3. 第二次所有 PTX 缓存命中。
4. 同一进程重复切换不再创建 shader。

工具不会删除已有缓存。结果写入 `build/cache-stability/<timestamp>/report.json`，并保存各进程的命令、工作目录、PID、超时、退出码及 stdout/stderr。报告只根据已有 PTX 日志判定命中，不推断 OptiX 驱动缓存命中。

Horizon 修复已合入并推送到 [`main`](https://github.com/CoronaEngine/Horizon/tree/main)，提交为 [`2607a3b380e526fb3c319168493119d9b93285d3`](https://github.com/CoronaEngine/Horizon/commit/2607a3b380e526fb3c319168493119d9b93285d3)。CoronaEngine 的 `.workspace/horizon.lock.json` 指向 `main` 的该提交；同步这个 lock 即可取得修复，无需再应用补丁。

修复基于 Horizon `67d2223ea02a408f71d7cc3abbc2b142c78414fc`。需要移植到其他 Horizon 分支时，可以 cherry-pick 上述修复提交；当前锁定版本已经包含修复，无需重复应用。

CoronaEngine 的调用点和测试依赖上述 Horizon API，应与更新后的 lock 一起使用。

## 首轮验证（2026-10-05，Horizon e3edffc6）

- 独立 `VisionShaderBindingTests` 通过，包含实际 CUDA 数据读取、参数移动/复制、快照及注释身份检查。
- 验证脚本的 6 项单元测试通过。
- 模式切换、场景资源复用、ReSTIR 及几何同步的 13 项 CTest 回归全部通过（175.95 秒）。连同独立 GPU 测试，共 14 项 CTest 通过，无最终失败或超时。
- RelWithDebInfo 下相关 Vision 模块及三个集成测试目标完整构建通过。
- kitchen 跨进程验证通过，报告：`build/cache-stability/kitchen-verified/report.json`。

| 进程 | 额外槽位 | PT visibility | ReSTIR visibility / surfaces | PTX hit / miss |
|---|---:|---:|---:|---:|
| 第一次 | 0 | 29 | 57 / 53 | 21 / 0 |
| 第二次 | 7 | 36 | 64 / 60 | 21 / 0 |

两轮使用相同 shader 标识；同一进程 PT → ReSTIR 重复切换均没有重新创建 shader。第二进程 PT → ReSTIR 的准备及首帧合计约 5.93 秒，重复切换约 144 毫秒；PTX 命中后仍需 DSL 构造、模块加载等工作，因此不是零耗时。

原始 `kitchen-shifted` 检查误要求 PT 也分配 surfaces，曾以校验失败退出；其第二个渲染进程实际上已 21 / 0 命中。按模式修正验证条件后重新运行得到上述通过报告。所有过程日志均保留，没有删除旧缓存或覆盖失败记录。

## 更新后复验（2026-10-06）

用户更新到 CoronaEngine `4fff0c20` 后，按新 lock 将 Horizon 从 `e3edffc6` 同步到 `67d2223e`；上述 Ocarina 修改完整保留，无冲突，也没有改写用户的新提交。

- 显式重新配置并构建四个相关测试目标及其依赖成功：`build/cache-stability/updated-build/`。
- 14 项 CTest 回归全部通过，79.15 秒；无失败、跳过或超时：`build/cache-stability/updated-regression/`。
- 6 项验证脚本单元测试通过：`build/cache-stability/updated-tool-tests/`。
- kitchen 两个独立进程均为 21 hit / 0 miss，第二个进程增加 7 个槽位、实际 buffer slot 改变后仍完全命中，重复切换没有创建 shader：`build/cache-stability/updated-kitchen/report.json`。
- 两仓库 `git diff --check` 通过；修复适用于当时锁定的 `67d2223e` 基线。复验时改动尚未提交；之后 Horizon 部分已按上文合入主分支并推送。
