# `SceneTools` 聚合边界

`plugins/SceneTools` 同时包含 native 聚合接口的兼容编排层和 Vision 旧宿主 facade。
当前 Vue、C++ 主流程、AITool 与 Script Runtime 直接使用 `scene.*`、`sceneTools.*`、
`viewport.*` manifest；`compat/legacy_scene_tools.py` 不在正常启动时注册，只能由旧宿主显式注册，
`main.py` 仅保留历史导入 shim。
C++ Engine/Editor Host 持有场景事实、revision、持久化和事件语义。

## Owner 映射

| 责任 | canonical owner | SceneTools 允许做什么 |
|---|---|---|
| 场景/Actor/视口业务命令 | C++ manifest + `CoronaEditorApi` | 转换参数、调用聚合 contract、返回结构化结果 |
| Vision native 加载、相机和渲染模式 | `sceneTools.*` / `viewport.*` manifest | 编排请求，不持有 Engine 对象 |
| Vision 旧 Python Scene fallback | `plugins/SceneTools/compat/legacy_vision_import_adapter.py` → `plugins/SceneTools/compat/legacy_vision_scene_adapter.py` | SceneTools 持有实现；`runtime/legacy_vision_*.py` 仅保留历史导入 shim；仅在明确的旧宿主兼容路径延迟解析旧 Scene |
| Vision 文档值转换 | `vision_document.py` | 处理文档路径、相机、shape 元数据、JSON path 变更和坐标转换；无 Scene/Actor/C++ 副作用 |
| Vision 几何值计算 | `vision_geometry.py` | 处理矩阵/TRS、坐标系转换和 primitive 顶点；无文件、Scene/Actor/C++ 副作用 |
| Vision storage | `vision_storage.py` | 生成 derived/runtime 路径并原子写入 JSON；不编排 proxy binding 或跨层契约 |
| Vision proxy 文件 | `vision_proxy.py` | 生成 primitive OBJ proxy、读取模型包围盒校正；不访问 Scene/Actor，不维护 binding 状态 |
| Vision binding identity | `vision_bindings.py` | 计算 shape identity、匹配历史 binding 和生成导入摘要；不访问 Scene/Actor |
| Vision binding sync | `compat/legacy_vision_binding_sync.py`（旧入口：`vision_binding_sync.py`） | 负责 legacy Actor 查找、过期 proxy 删除和 source path 回写；不负责公共 manifest、Scene 权威状态或普通 Actor 同步 |
| Vision 代理导入流程 | `compat/legacy_scene_tools.py` 的旧宿主 facade | 编排文档转换、proxy binding 和兼容结果；不定义新的跨层 schema；`main.py` 仅保留历史导入 shim |
| Vision actor helper | 已删除 `legacy_vision_import_helper.py` 与 `vision_import.py` | 原 helper 无仓内生产调用；受支持的 Vision 导入由 `legacy_vision_import_adapter.py` 和 native manifest 流程负责 |

## 约束

- 新业务必须使用 `scene.*`、`sceneTools.*`、`viewport.*` 等 manifest 聚合接口；
- 正常 runtime 启动不得注册 `SceneTools` Python facade；只有登记的旧宿主可以调用
  `register_legacy_python_script_services()` 显式启用它；
- 不得直接导入 `CoronaCore`、`scene_manager` 或底层 `Actor`/`Camera`/`Scene` 作为公共 API；
- 旧 Python Scene 只能通过 `plugins/SceneTools/compat/legacy_vision_import_adapter.py` 进入
  Vision fallback；`runtime/legacy_vision_*.py` 不得新增实现；
- Python handler 不得复制 C++ 的 schema、权限、revision、持久化或事件状态机；
- `legacy_vision_import_helper.py` 已删除；它没有仓内生产调用，且不属于当前受支持的 native Vision 导入路径；如外部宿主仍依赖旧路径，应通过回滚本次删除提交恢复；
- `vision_document.py` 只负责纯值转换，不得反向调用 SceneTools、runtime 或 native binding；有
  `source_path` 时才展开相对资源路径，缺少来源时保留相对引用，不使用进程 `cwd`；
- `vision_geometry.py` 只负责纯几何计算，不得写入 proxy 文件或访问 Scene/Actor；
- `vision_storage.py` 只负责路径和文件存储，不得访问 Scene/Actor 或直接调用 manifest；
- `vision_proxy.py` 只负责 proxy 文件和模型值计算，project root、shape identity 与 Actor binding 由 handler 提供和维护；
- `vision_bindings.py` 只负责值对象匹配和摘要统计，Actor 查找、删除和 native binding 更新必须留在 handler/adapter；
- 大型 Vision 逻辑拆分必须保持坐标系转换、proxy identity、绑定和错误语义不变。

## 待处理项和删除条件

`legacy_vision_import_helper.py` 和 `vision_import.py` 已从仓库删除。它们此前没有仓内生产
调用，当前 Vision 导入继续由 native manifest 与登记的 legacy adapter 负责；如发现外部
宿主依赖旧入口，使用删除提交的 revert 恢复兼容层后再进行有契约的迁移。

删除 `plugins/SceneTools/compat/legacy_vision_import_adapter.py` 和
`plugins/SceneTools/compat/legacy_vision_scene_adapter.py` 前必须满足：

- Vision native handler 覆盖所有受支持的导入、绑定、相机和场景切换路径；
- SceneTools 不再需要旧 Python Scene 的代理/持久化状态；
- MainView、AITool、Frontend Vision 面板和旧宿主回归通过；
- `API_OWNERSHIP.md` 中的 Vision legacy 行更新并记录回滚路径。
