# `SceneTools` 聚合边界

`plugins/SceneTools` 负责 native 聚合接口和 Vision 文档值编排；Vision 导入通过 native scene snapshot
与 aggregate mutation 完成，不再持有旧 Python Scene facade。
当前 Vue、C++ 主流程、AITool 与 Script Runtime 直接使用 `scene.*`、`sceneTools.*`、
`viewport.*` manifest；`main.py` 是 Python service adapter，只负责参数转换和编排。
C++ Engine/Editor Host 持有场景事实、revision、持久化和事件语义。

## Owner 映射

| 责任 | canonical owner | SceneTools 允许做什么 |
|---|---|---|
| 场景/Actor/视口业务命令 | C++ manifest + `CoronaEditorApi` | 转换参数、调用聚合 contract、返回结构化结果 |
| Vision native 加载、相机和渲染模式 | `sceneTools.*` / `viewport.*` manifest | 编排请求，不持有 Engine 对象 |
| Vision native 导入与 binding | `vision_import.py` 的 native snapshot/save/create/remove 编排 + `vision_binding_sync.py` | 通过 `scene.*`、`sceneTools.*` 和 `main.scene_save` 聚合契约管理文档与 Actor；不读取 Python Scene |
| Vision 文档值转换 | `vision_document.py` | 处理文档路径、相机、shape 元数据、JSON path 变更和坐标转换；无 Scene/Actor/C++ 副作用 |
| Vision 几何值计算 | `vision_geometry.py` | 处理矩阵/TRS、坐标系转换和 primitive 顶点；无文件、Scene/Actor/C++ 副作用 |
| Vision storage | `vision_storage.py` | 生成 derived/runtime 路径并原子写入 JSON；不编排 proxy binding 或跨层契约 |
| Vision proxy 文件 | `vision_proxy.py` | 生成 primitive OBJ proxy、读取模型包围盒校正；不访问 Scene/Actor，不维护 binding 状态 |
| Vision binding identity | `vision_bindings.py` | 计算 shape identity、匹配历史 binding 和生成导入摘要；不访问 Scene/Actor |
| Vision binding sync | `vision_binding_sync.py` | 通过 native value object 查找和删除过期 proxy；不负责公共 manifest、Scene 权威状态或普通 Actor 同步 |
| Vision 代理导入流程 | `main.py` + `vision_import.py` | 编排文档转换、proxy binding 和 native aggregate 结果；不定义新的跨层 schema |
| Vision actor helper | 已删除 `legacy_vision_import_helper.py` 与旧兼容导入实现 | 原 helper 无仓内生产调用；受支持的 Vision 导入由 canonical `vision_import.py` 和 native manifest 流程负责 |

## 约束

- 新业务必须使用 `scene.*`、`sceneTools.*`、`viewport.*` 等 manifest 聚合接口；
- 正常 runtime 启动按 registry 清单注册服务；SceneTools 不得绕过 manifest 取得底层对象；
- 不得直接导入 `CoronaCore`、`scene_manager` 或底层 `Actor`/`Camera`/`Scene` 作为公共 API；
- Vision 导入不得读取 Python Scene；所有 Scene/Actor 状态必须通过 native snapshot、aggregate
  mutation 和 `main.scene_save` 访问；`runtime/legacy_vision_*.py` 不得新增实现；
- Python handler 不得复制 C++ 的 schema、权限、revision、持久化或事件状态机；
- `legacy_vision_import_helper.py` 已删除；它没有仓内生产调用，且不属于当前受支持的 native Vision 导入路径；如外部宿主仍依赖旧路径，应通过回滚本次删除提交恢复；
- `vision_document.py` 只负责纯值转换，不得反向调用 SceneTools、runtime 或 native binding；有
  `source_path` 时才展开相对资源路径，缺少来源时保留相对引用，不使用进程 `cwd`；
- `vision_geometry.py` 只负责纯几何计算，不得写入 proxy 文件或访问 Scene/Actor；
- `vision_storage.py` 只负责路径和文件存储，不得访问 Scene/Actor 或直接调用 manifest；
- `vision_proxy.py` 只负责 proxy 文件和模型值计算，project root、shape identity 与 Actor binding 由 handler 提供和维护；
- `vision_bindings.py` 只负责值对象匹配和摘要统计，Actor 查找、删除和 native binding 更新必须留在 handler/adapter；
- 大型 Vision 逻辑拆分必须保持坐标系转换、proxy identity、绑定和错误语义不变。

## 迁移状态

`legacy_vision_import_helper.py` 和旧兼容导入实现已从仓库删除。它们此前没有仓内生产
调用；当前受支持的 Vision 导入由 canonical `vision_import.py` 和 native manifest 负责。
如发现外部宿主依赖旧入口，使用删除提交的 revert 恢复兼容层后再进行有契约的迁移。


- Vision native handler 覆盖所有受支持的导入、绑定、相机和场景切换路径；
- SceneTools 不再需要旧 Python Scene 的代理/持久化状态；
- MainView、AITool、Frontend Vision 面板和旧宿主回归通过；
- `API_OWNERSHIP.md` 中的 Vision legacy 行更新并记录回滚路径。
