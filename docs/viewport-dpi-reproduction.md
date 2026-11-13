> 注：文中的 `.workspace/tmp/...` 复现脚本与 JSON 是当时的临时产物，未随仓库保存。

# DPI／视口缩小、偏移与留黑：复现交接

整理日期：2026-11-01。供 UI／CEF、相机 API、Optics、Display 负责人排查。

2026-11-04 修复：`update_camera_view` 现在仅在请求包含相应字段时更新渲染尺寸或窗口几何；
移动速度使用独立状态命令。Main 视口与其他 Web 视口共同遵循 DOM 展示矩形，Display 根据
源图宽高比居中等比显示，固定 1080p 的批量预览也同步适配展示矩形。拾取、gizmo 与 Web
原生光标共享该像素取整规则进行反向坐标映射；黑边不触发拾取或 gizmo，新位置在黑边内时
不会移动 gizmo，已捕获的拖动仍可释放或取消。下文保留历史复现依据；
本次运行验证记录位于 `.workspace/tmp/viewport-fit/`。当前 CEF 使用逻辑像素和
`device_scale_factor=1`，保留原有 SDL 到物理像素换算；本次不将坐标数学测试冒充真实跨屏 DPI 验收。

**已确认的稳定触发链：内嵌画面已铺满时，只设置渲染分辨率，会把保存的旧视图矩形重新写入运行时相机，导致画面缩小、偏移并持续留黑。100% DPI 即可复现。**

## 1. 版本与验证范围

| 项目 | 版本／状态 |
| --- | --- |
| 建议接手分支 | `vision_refactor` |
| 当前回退提交 | `b2129a049384dbbf176beb0f21e10b8bcd909167` |
| 已回退的 DPI／合成修改 | `2b4b9ce049243ad343b18fef0a3057de070a5b7d` |
| 历史实际复现的 main | `781907e59b6f6449082e5801c7e3a2d4f45b22b6`、`75fa8f53bd19948a132fea7daa7ef7b901444bc3` |
| 历史复现使用的 CoronaEditor | `3e0edf418d1963e990589435892526daaf3952e1` |

以下画面、尺寸与恢复结果来自 2026-10-31 的实际运行记录。当前回退版本已核对相关源码仍存在该覆盖链，**本次整理没有重新运行引擎／GPU 复现**。不要将上述历史 main 版本称为当前最新 main。

## 2. 前置条件

历史实测环境：Windows、RelWithDebInfo、Kitchen（`scene_v3.xml`）、相机 `source-camera`、Vision `path_tracing`、渐进累积开启、降噪关闭、Stable Planes 开启，渲染输出固定为 `1920×1080`。

| 状态 | 历史实测数值 |
| --- | --- |
| CEF `devicePixelRatio` | `1`，即本次测试为 100% 缩放 |
| 当前内嵌展示矩形 | `x=0, y=0, width=1536, height=825` |
| 相机保存的视图配置 | `view_open=false, view_x=120, view_y=120, view_width=960, view_height=540` |
| 渲染输出尺寸 | `1920×1080`，触发前后保持不变 |

关键条件是：**保存的 `view_*` 矩形与当前内嵌展示矩形不同；随后仅调用分辨率更新接口；期间没有新的实时视口请求覆盖它。** 实际窗口尺寸可以不同于表中数值。

PT、Stable Planes 等是本次实测配置，尚未通过逐项切换证明它们是触发所必需的。高 DPI 和最大化均不是这条持续留黑链的必要条件。

## 3. 最短复现步骤

1. 打开 Kitchen，使用上述配置，等待内嵌画面正常铺满窗口。
2. 核对相机保存的 `view_*` 与当前 DOM 视口不同。例如保存矩形为 `960×540+120,120`，实际内嵌矩形为 `1536×825+0,0`。如果数值相同，可在测试场景副本中先设置一组不同的保存值，再提交实时视口建立正常基准，见下一节。
3. 保持窗口、布局和相机不动，**只调用**：

   ```text
   SceneTools.update_camera_view(
       "scene_v3.xml", "source-camera",
       {"width": 1920, "height": 1080}
   )
   ```

   不附带任何 `view_*` 字段。即使当前已经是 `1920×1080`，再次提交相同分辨率也能触发历史复现中的问题。

4. 等待至少 3 秒，并确认期间仍有新帧产生。历史实际表现为画面缩到 `960×540+120,120`，外部区域持续留黑，渲染输出仍为 `1920×1080`。
5. 只重新提交当前实时 DOM 边界的 `setCameraViewport`，等待新帧；历史实测恢复为铺满窗口。

第 3～4 步之间不要拖动、最大化、还原或切换布局，这些操作可能重发视口请求并掩盖持续问题。

**预期：** 仅更新渲染分辨率，不改变图像在窗口内的展示位置和尺寸。**实际：** 保存的旧矩形覆盖实时内嵌矩形。

## 4. 接口复现示例

在引擎启动前设置 `CORONA_CEF_REMOTE_DEBUG_PORT=9223`，再按项目正常方式启动引擎并打开上述场景。历史测试使用 2～5 FPS 限帧；限帧是测试配置，不是触发条件。

以下代码使用仓库现有的 `tools/vision_frontend_probe.py`。Python 环境需包含 `websocket-client`；将代码保存为 `.workspace/tmp/viewport-fit/repro_viewport.py`。在仓库根目录的 PowerShell 中设置 `$env:PYTHONPATH=(Get-Location).Path`，再运行 `python -B .workspace/tmp/viewport-fit/repro_viewport.py`，确保脚本可导入仓库的 `tools` 模块。端口、场景路由和相机 ID 必须对应当前进程。

代码会调用会保存相机配置的接口，**请在测试场景副本上执行**。准备阶段主动写入旧矩形，是为了避免其他机器的保存值恰好与当前视口一致；触发阶段仍只提交 `width/height`。

```python
import time
from tools.vision_frontend_probe import Frontend

frontend = Frontend(port=9223)
scene, camera = "scene_v3.xml", "source-camera"

def api(name, args):
    result = frontend.api(name, args)
    if not result.get("success"):
        raise RuntimeError(result)
    return result["data"]

try:
    # 准备：设置一组与当前内嵌视口不同的保存值。
    api("SceneTools.update_camera_view", [scene, camera, {
        "width": 1920, "height": 1080,
        "view_open": False, "view_x": 120, "view_y": 120,
        "view_width": 960, "view_height": 540,
    }])
    time.sleep(3)
    snapshot = api("SceneTools.get_scene_snapshot", [scene])
    cameras = snapshot.get("cameras") or [snapshot["camera"]]
    current = next(c for c in cameras
                   if c.get("camera_id", c.get("id")) == camera)
    handle = int(current["handle"])  # 每次启动重新读取，不复用旧 handle。

    def bind_live_viewport():
        expression = """(() => {
          const element = document.querySelector('[data-viewport-pick-surface]');
          if (!element) throw Error('请先打开场景的内嵌视口');
          const r = element.getBoundingClientRect(), d = devicePixelRatio || 1;
          const accepted = window.coronaBridge.setCameraViewport(
            HANDLE, Math.round(r.x*d), Math.round(r.y*d),
            Math.round(r.width*d), Math.round(r.height*d), 1920, 1080);
          return {accepted, dpr: d, rect: {x:r.x,y:r.y,width:r.width,height:r.height}};
        })()""".replace("HANDLE", str(handle))
        result = frontend.evaluate(expression)
        if not result.get("accepted"):
            raise RuntimeError(result)
        print("实时视口请求：", result)

    # 基准：重新绑定当前展示矩形，渲染仍固定为 1080p。
    bind_live_viewport()
    input("等待画面铺满；保持窗口不动，按回车触发：")

    # 触发：不传 view_*，仅重复提交相同渲染分辨率。
    api("SceneTools.update_camera_view", [
        scene, camera, {"width": 1920, "height": 1080}
    ])
    for _ in range(6):
        time.sleep(1)
        print("帧时间：", api("SceneTools.get_frame_timing", [scene, camera]))
    input("记录画面缩小／偏移／留黑；按回车重新提交实时视口：")
    bind_live_viewport()
    input("等待新帧，记录画面是否恢复铺满；按回车结束：")
finally:
    frontend.socket.close()
```

该示例依据当前接口整理，已做语法与调用入口核对，未在当前回退版本执行。若基准阶段没有铺满，先排查视口绑定，不能据此认定第 3 步的问题已复现。若没有出现持续留黑，检查保存值是否确实不同、当前相机是否正确、期间是否另有 `setCameraViewport` 请求刷新了边界。

## 5. 已定位的覆盖链

以下路径与行号按当前 `b2129a04` 核对；前端文件来自独立 CoronaEditor 工作区。

| 环节 | 排查位置 | 行为 |
| --- | --- | --- |
| API 入口 | `src/systems/ui/cef/editor_scene_service.cpp:7828` | 缺省 `view_*` 使用相机保存值；`7846–7849` 即使只传分辨率，仍调用 `set_size` 和 `set_view_state` |
| 状态入队 | `src/engine/engine_runtime_api.cpp:2493` | `set_view_state` 打包完整 `CameraStateUpdateField::ViewState` |
| 实时视口 | `src/systems/optics/optics_system.cpp:712` | 写入实时 `view_*`，设置 `viewport_rect_active=true` |
| 旧状态覆盖 | 同文件 `828` | ViewState 再次写入同组 `view_*`，未清除上述激活状态 |
| 同帧顺序 | 同文件 `3241` | 先处理 viewport，再处理 state；同批次旧状态最后覆盖 |
| 展示输出 | 同文件 `478` | 内嵌视口激活且 `view_open=false` 时，用被覆盖的 `view_*` 输出矩形 |
| 前端去重 | `CoronaEditor/editor/Frontend/src/views/layout/MainPage.vue:1100` | `1119–1121` 按 handle、边界、渲染尺寸去重；签名不变时不重发 |

`get_scene_snapshot` 中的 `view_*` 是保存状态，不能单独用来证明实时 Optics 矩形。历史基准正常时和触发异常后，快照都能返回同一组旧值；需要结合实际画面、实时视口提交日志和连续帧记录判断。

建议首先修正接口的字段更新语义：只请求分辨率时不回放旧视图几何；同时检查仅修改 `move_speed` 的路径，以及保存的独立窗口几何与实时内嵌矩形共用字段的问题。仅调整两个队列的处理顺序无法解决跨帧到达时的覆盖。

## 6. 与持续留黑分开验收的现象

- **最大化时短暂留黑：** 历史运行中，UI 已扩到 `1920×1009`，旧 Optics 图像和矩形仍为 `1536×825`，新目标准备期间右侧／底部会留黑。稳定后的最大化、还原画面均曾铺满。单纯最大化不是上一节持续状态覆盖的充分条件。
- **高 DPI 对齐：** 曾做坐标映射单元测试及人工修改 DPR 的桥接验证；原始物理桌面为 100% 缩放，没有完成真实高 DPI／跨屏的完整回归。不要将修改网页 DPR 的结果等同于 Windows 跨屏 DPI 验收。
- **被回退的 `2b4b9ce0`：** 涉及 CEF 来源尺寸、UI 物理坐标映射、最新请求保留、UI 视口元数据与 Display 合成。它没有修改上述 `update_camera_view` 无条件回放状态的逻辑。当前这些修改已回退，请负责人按最终修复方案判断是否需要重新实现其中部分行为。

## 7. 修复验收

| 操作 | 通过标准 |
| --- | --- |
| 内嵌视口已铺满，仅改变或重复提交 `1920×1080` | 展示矩形不回退；没有持续偏移、缩小或留黑 |
| 仅修改移动速度 | 当前展示矩形不变 |
| viewport／state 在同帧及相邻帧到达 | 非几何设置不覆盖实时内嵌矩形 |
| 最大化、还原、拖动面板 | 记录过渡帧与稳定帧；稳定后铺满目标视口 |
| 100%、125%、150%、跨屏缩放 | 对照 DOM、DPR、原生物理面板、Optics 和 Display 矩形检查一致性 |
| 独立相机窗口打开、关闭、移动与改尺寸 | 保持显式窗口操作语义 |
| PT 与 ReSTIR | 分别验证，确认实际模式与输出尺寸；直接调用 API 后控件文字可能滞后 |

高 DPI、跨屏及 ReSTIR 的完整运行回归仍待负责人执行。回退时通过的 `PublishedImageTests`、`CameraViewportManagerTests` 不覆盖上述两个 API 竞争同组字段的情形。

## 8. 历史证据位置

本机目录：`.workspace/main-viewport-audit-20261031/`（被 Git 忽略，不会随分支拉取）。本文的核心复现步骤可单独转发；需要核对原始截图／日志时，再附上该目录中的材料：

- `verification.json`：版本、配置、步骤、像素采样及验证限制。
- `tip-final-baseline-state.json`、`tip-main-trigger-state.json`：正常／异常时的 DOM、保存相机状态与帧时间。
- `tip-main-trigger-resolution-only.json`：仅分辨率请求的返回结果。
- `native-pipeline-evidence.log`：实际 PT、1080p 输出与实时视口提交日志。
- `tip-main-baseline.png`、`tip-main-reproduced.png`、`tip-main-recovered.png`：同一进程中的正常、触发、恢复对照。

历史右侧采样区 `x=1150..1350, y=350..600` 的纯黑占比依次为 `0 → 1 → 0`；异常状态连续观察至少 3 秒且仍有新帧，支持“持续错误矩形”的判断。
