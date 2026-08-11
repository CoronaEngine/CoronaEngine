# AI 工具职责边界

当前 AI 能力按工作流和执行层级分离。工具名称、层级和工作流归属的单一来源是
`editor/plugins/AITool/services/ai_tool_catalog.py`。

## 执行层级

### public

面向通用对话 Agent 的工具，只表达用户可理解的能力：

- 文案：`generate_product_text`、`generate_marketing_text`、`generate_creative_text`
- 媒体：`generate_image`、`generate_video_from_image`、`text_to_speech`、`generate_bgm_music`、`analyze_media`
- 3D 生成：`hunyuan_generate_3d`

通用 Agent 应使用 Quasar 的 `load_public_tools()`，不能直接使用完整 registry 列表。

### runtime_internal

由 AgentRuntime 编排器调用，不直接暴露给通用 Agent。包括规划、资源准备、布局、审查、批次、队列、状态、报告和审计工具，例如：

- `runtime.plan.extract`
- `runtime.asset.model.prepare`
- `runtime.placement.propose`
- `runtime.layout.apply_delta`
- `runtime.review.generate_adjustment_proposal`
- `runtime.scene_plan.persist`
- `runtime.queue.enqueue_graph`

Runtime Manifest 会将这些工具标记为 `layer=runtime_internal`。

### engine_native

只负责调用引擎原子能力，由专用 Provider 或 Runtime 使用：

- 模型/环境导入：`import_model`、`import_environment_component`、`remove_model`
- 场景变换：`set_actor_transform`、`transform_model`
- 场景快照：`get_scene_snapshot`
- 相机：`camera_move`、`camera_get`、`camera_focus`、`camera_list`、`camera_screenshot`、`camera_multiview_capture`
- 审查：`scene_rationality_review`

## 三条工作流

### 对话答疑

使用 `public` 中的文本和媒体工具。不得直接调用 Runtime 状态、批次、队列或引擎写入工具。

### 节点逻辑生成/修改

由节点图服务负责，操作包括：

`create_node`、`move_node`、`connect_nodes`、`edit_block_parameter`、
`set_transition_condition`、`run_node_graph`、`connect_object_reference`、
`select_existing_object`。

节点逻辑不与场景生成 Runtime 工具混用。

### 完整游戏/场景生成

由 AgentRuntime 统一编排：

1. 规划和场景元素分类；
2. 图片、模型和环境资源准备；
3. 布局提案和几何检查；
4. 通过 engine-native Provider 写入引擎；
5. 视觉审查和确认后的调整；
6. 批次、队列、状态和报告持久化。

## 单场景约束

AI 工具层不再提供 `scene_list`、`scene_get_actors`、`scene_query`。单场景查询通过
`get_scene_snapshot` 和 Runtime 状态完成；底层 `scene.list_routes` 仍保留给编辑器启动和
场景路由管理，不属于通用 AI 工具。
