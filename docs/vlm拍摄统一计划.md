# VLM 拍摄统一计划

## Summary
将所有 VLM/审查拍摄统一到一个 Python 入口：基于目标或场景 AABB 自适应构图，固定只拍 4 个视角，只输出 `final_color`。旧的 8 视角、多 output mode、`base_color` 审查拍摄链路全部删除；旧入口只允许保留极薄兼容壳并立即转发到统一入口，避免 agent 或 workflow 误入旧路径。

本任务优先解决“审查看到的图不稳定、拍摄角度分叉、base_color 泄漏、主视口被扰动、旧 workflow 仍在拍 8 视角”等问题。多视角拍摄工具本身视为审查高风险入口，也一并收敛为 4 视角 `final_color`；普通单张截图调试能力可保留，但不得作为 VLM/审查内部实现。

## Current Problems
- `model_reviewer._capture_single_model` 当前已经使用独立 review camera 和 4 视角，但仍依赖旧 `scene_composition_workflow_v2.nodes_tier_review._calc_camera_pose`，姿态算法不够集中。
- `camera_tools.camera_multiview_capture` 和 `multi_view_capture.py` 仍暴露 `output_modes`，测试中还能传入 `base_color`，这是 agent 误入旧审查链路的主要风险之一。
- 旧 `scene_composition_workflow/capture_screenshots.py` 明确写死 `_SCENE_VIEW_ANGLES = 8`，并切换 `base_color`，这是 VLM 审查行为漂移的主要遗留入口。
- `nodes_tier_review.py`、`model_reviewer.py`、旧 retrieval workflow 里仍有 `base_color` 截图调用；其中 VLM/审查相关调用必须删除，不能只靠约定“不走这条路”。
- 拍摄元数据不统一，日志里很难判断某次 VLM 审查到底用了目标 AABB、场景 AABB、哪个相机、哪个 output mode、几张图成功。

## Key Changes
- 新增统一拍摄模块，例如 `vlm_capture.py`：
  - 输入：`scene_name`、可选 `actor_name`、`scope=actor|scene`、`output_dir`。
  - 输出：4 张 `final_color` 截图、拍摄元数据、跳过/超时状态。
  - 构图：优先 actor AABB；无 actor 时用 scene AABB；AABB 无效时使用保守默认中心和半径。
  - 视角：前、右前、左后、俯视斜角 4 张，全部根据 AABB 半径自动计算距离和 FOV。
- 拍摄实现统一使用隐藏 `vlm_review_camera`：
  - 不移动主相机。
  - 不改变用户视口输出模式。
  - 截图继续走 timeout + file-ready 检查。
  - output mode 强制 `final_color`，忽略外部传入的 `base_color/normal/position/object_id`。
- 调用方统一收口：
  - `model_reviewer._capture_single_model` 改为调用统一入口。
  - `camera_tools.camera_multiview_capture` 删除多 output mode 能力，固定走 4 视角 `final_color`。
  - `multi_view_capture.py` 场景级拍摄改为复用 unified scene capture，删除默认多 ring/多 mode 行为。
  - 旧 `scene_composition_workflow/capture_screenshots.py` 删除手写 8 视角和 `base_color` 逻辑，只保留薄壳调用统一入口。
- 日志与结果透明化：
  - 每轮记录 `scope`、目标 AABB、中心、半径、4 个 view pose、output mode、截图成功数量。
  - VLM 用户提示中显示“使用 4 视角 final_color 审查”，避免用户误以为有其它通道或更多视角参与。

## Interface Changes
- 新增统一函数：
  - `capture_vlm_views(scene_name, output_dir, actor_name=None, scope="actor", timeout_sec=5.0) -> VlmCaptureResult`
  - `VlmCaptureResult`: `status`、`output_dir`、`files`、`view_count`、`output_mode`、`target_bounds`、`center`、`radius`、`poses`、`camera_name`、`skipped_reason`
- 新增内部能力函数：
  - `resolve_vlm_target_bounds(scene, actor_name=None, scope="actor") -> TargetBounds`
  - `build_vlm_view_poses(bounds) -> list[ViewPose]`
  - `capture_pose_with_review_camera(scene, camera, pose, output_path, timeout_sec) -> CaptureFileResult`
- 保留现有工具名兼容，但删除旧参数能力：
  - `view_count` 不再作为可变配置，返回值固定为 `4`
  - `output_modes` 从多视角工具 schema 中删除，返回值改为 `output_mode: "final_color"`
  - 文档说明“多视角审查拍摄不支持多输出通道；需要调试其它通道时只能使用普通单张截图工具”

## Implementation Plan

### Phase 1: 建立统一核心，并立即切断新调用旧逻辑的可能
- 新增 `editor/plugins/AITool/cai_extensions/agent/vlm_capture.py`。
- 把 4 视角构图、AABB 解析、review camera 获取、截图 timeout/file-ready 检查集中到该模块。
- 统一模块只允许 `final_color`，内部常量命名为 `VLM_OUTPUT_MODE = "final_color"` 和 `VLM_VIEW_COUNT = 4`。
- 保留现有 `model_reviewer._capture_single_model` 外部签名，内部先委托 `capture_vlm_views(..., scope="actor")`。
- 删除 `model_reviewer._capture_single_model_main_camera_fallback`，不再保留主相机 fallback。
- 删除 `model_reviewer._capture_with_review_camera` 中设置 `base_color` 的逻辑；若函数保留，只作为统一入口的内部 helper，不能暴露为第二套 VLM 拍摄实现。
- 为 `vlm_capture.py` 增加纯函数测试，先覆盖姿态计算和 output mode 钳制，不依赖真实引擎。

### Phase 2: 迁移 Progressive/VLM 审查路径
- `scene_composer_progressive.py` 中所有 VLM advisory/final review 截图只调用 `model_reviewer._capture_single_model` 或新的 `capture_vlm_views`。
- `vlm_review_loop.py` 的用户提示补充 capture metadata：`4 views / final_color / actor|scene scope / success count`。
- VLM 审查报告中保留 `capture_result` 摘要，便于日志和 UI 解释“为什么只检查了这些视角”。

### Phase 3: 收口 MCP 拍摄工具
- `camera_tools.camera_multiview_capture` 删除 `output_modes` 输入，固定调用 unified capture。
- `multi_view_capture.py` 的 `scene_multi_view_capture` 对 VLM/审查用途改为调用 `capture_vlm_views(..., scope="scene")`。
- `multi_view_capture.py` 删除 `output_modes` schema 字段、循环输出多通道的实现和返回字段中的 `output_modes`。
- 更新 `test_camera_multiview_offscreen.py`：旧的“传入 base_color 后 review camera 是 base_color”断言删除，新增“多视角工具没有 output_modes，结果固定 final_color”的断言。

### Phase 4: 处理旧 Workflow
- `scene_composition_workflow/capture_screenshots.py` 删除 `_SCENE_VIEW_ANGLES = 8` 的直接拍摄逻辑，改为调用 unified scene capture。
- 旧 workflow 如果仍需要 `screenshots_dir` 字段，只从 `VlmCaptureResult.output_dir` 回填，避免下游 `review_scene.py` 破坏兼容。
- `nodes_tier_review.py` 中的 `base_color` 审查截图调用删除；`_calc_camera_pose` 不再被 VLM 路径 import。
- `model_retrieval_workflow/generate.py` 不再通过 `camera_multiview_capture` 获得多通道截图；若仍需多视图，只能得到 4 张 `final_color`。
- `model_retrieval_workflow/six_view_capture_tool.py` 的 `base_color` 若仅用于资产检索调试，可暂时保留在该文件内部；但不得被 VLM/场景审查调用，且不得复用 `camera_multiview_capture` 的旧多通道能力。

### Phase 5: 清理和防回归
- 搜索并确认 VLM 路径不再出现：
  - `_SCENE_VIEW_ANGLES = [0, 45, 90, 135, 180, 225, 270, 315]`
  - `set_output_mode("base_color")`
  - `{"output_mode": "base_color"}`
  - 多视角工具 schema 或返回值中的 `output_modes`
- 删除旧测试中期待 `base_color` 多视角输出的断言，不保留“传参后被钳制”的兼容测试；目标是 schema 层就不再提供旧参数。
- 保留非 VLM 材质、资源解析、普通截图中的 `base_color` 字段，不做全仓库误删。
- 在日志中增加统一前缀 `[VlmCapture]`，记录 target、bounds、poses、success/failed/skipped。

## Detailed Acceptance Criteria
- 一次 VLM actor 审查最多产生 4 张截图，且文件 metadata 中 `output_mode == "final_color"`。
- 一次 VLM scene 审查最多产生 4 张截图，且根据 scene AABB 自适应距离。
- 目标 actor AABB 为空时，不崩溃；降级为 scene AABB，再不行使用默认构图，并在 `skipped_reason` 或 metadata 中说明。
- VLM 截图不移动 active camera，不改变 active camera 的 output mode。
- 旧 scene workflow 即使被调用，也只能薄壳转发 unified capture，不存在 8 视角循环或 `base_color` 切换代码。
- 用户可见提示中出现“4 视角 final_color 审查”，低置信建议仍按现有 VLM review 逻辑输出。
- 多视角工具不再接受 `output_modes`；普通单张截图工具仍可被用户手动用于 `base_color/normal/object_id` 等调试用途，但 VLM 审查不会走这些模式。

## Test Plan
- 单元测试：
  - actor AABB 有效时生成 4 个视角，距离随 AABB 半径变化。
  - scene AABB 有效时生成 4 个场景视角。
  - AABB 为空/退化时使用默认构图且不崩。
  - 多视角工具 schema 不再包含 `output_modes`；旧调用传入该字段时应被拒绝或忽略，不产生 `base_color` 输出。
  - 主相机状态和 output mode 不被修改。
- 集成/静态测试：
  - 搜索确保 VLM 路径不再直接写死 `_SCENE_VIEW_ANGLES = 8`、`view_count=8`、`set_output_mode("base_color")`。
  - `test_vlm_review_loop.py` 增加 unified capture 覆盖。
  - 保留截图 timeout 测试，验证失败只 skip 不阻塞。
- 手动验证：
  - 生成一个室内场景，最终审查日志应显示 4 张 `final_color`。
  - VLM 审查提示应包含具体低置信建议，不再出现多通道或 base_color 泄漏。
  - 主窗口相机、输出通道和用户视口不被 VLM 拍摄打断。

## Suggested Verification Commands
- `python editor/plugins/AITool/cai_extensions/agent/test_vlm_review_loop.py`
- `python editor/plugins/AITool/cai_extensions/mcp/tools/test_camera_multiview_offscreen.py`
- `rg -n "_SCENE_VIEW_ANGLES|view_count\\s*=\\s*8|set_output_mode\\(\"base_color\"\\)|output_mode.?base_color|output_modes" editor/plugins/AITool/cai_extensions/agent editor/plugins/AITool/cai_extensions/mcp/tools editor/plugins/AITool/cai_extensions/flows`
- 手动跑一次生成：检查日志中 `[VlmCapture] scope=... output_mode=final_color view_count=4 success=...`

## Assumptions
- 覆盖范围按“全量统一”执行：progressive VLM、`camera_tools`、`multi_view_capture`、旧 scene workflow 都纳入。
- “拍摄工具只保留一个”指 VLM/审查多视角拍摄只保留一个统一实现；普通相机移动/普通单张截图工具可以保留，但不得作为 VLM 审查内部实现分叉。
- 当前工作区已有 gizmo 相关未提交修改，实施时需要避开无关文件，尤其不触碰已有 `ai_setting.py` 用户改动。
- `base_color` 作为材质字段和普通调试输出模式仍然合法；本计划只禁止它进入 VLM/审查拍摄链路。
- 如果构建环境缺 Windows SDK 或引擎运行条件不足，至少完成 Python 单元测试、静态搜索和日志路径检查。
