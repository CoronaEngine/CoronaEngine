# Runtime 项目兼容 wrapper 收敛设计

## 目标

整理 `editor/runtime` 根目录中的历史项目导入 wrapper，将其集中到
`editor/runtime/compat/`，同时保留旧模块路径，避免外部宿主和旧项目工具失效。

## 当前状态

- `runtime/project_templates.py` 是模板和 project.ini 初始化的 canonical owner；
- `runtime/legacy_project_copy.py` 是旧项目复制/打开行为的兼容实现 owner；
- `runtime/project_support.py` 聚合转发 `project_templates` 与 `scene_support`；
- `runtime/project_copy.py` 保留旧 `ProjectCopy` 和 `core_path` 注入语义；
- 新项目生命周期已使用 native `project.*`，旧 wrapper 只服务外部宿主和历史导入。

## 设计决策

1. 新增 `runtime/compat/legacy_project_support.py` 和
   `runtime/compat/legacy_project_copy.py` 作为 wrapper owner。
2. `runtime/project_support.py`、`runtime/project_copy.py` 保留为单向旧路径 shim。
3. `plugins/ProjectLauncher/compat/legacy_project_copy.py` 改为转发到新的 runtime
   compat owner，保留 `ProjectCopy`、`core_path` 和返回语义。
4. 不移动 `project_templates.py`、`scene_support.py`、`legacy_project_copy.py`，不修改
   native manifest、项目生命周期或文件复制行为。

## 验收

- 新旧模块导入得到等价对象或等价 wrapper 行为；
- `core_path` 注入仍能影响历史复制流程；
- runtime 根目录只保留 shim，compat owner 在文档中登记；
- ProjectLauncher、runtime 和路径边界测试通过。
