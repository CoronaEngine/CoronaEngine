# Runtime 项目兼容 wrapper 收敛设计

## 目标

整理 `editor/runtime` 根目录中的历史项目导入 wrapper，并将项目复制语义迁移到
native 聚合接口。本设计已被后续实现 supersede；本文保留为历史决策记录。

## 当前状态

- `runtime/project_templates.py` 是模板和 project.ini 初始化的 canonical owner；
- `runtime/project_templates.py` 仍是模板和 project.ini 初始化的 canonical owner；
- `project.copyExistingToData` 已成为已有项目复制、重名、路径规范化和失败回滚的
  native owner；
- `runtime/legacy_project_copy.py`、`runtime/project_copy.py` 及相关兼容路径均已删除；
- Python/Vue 通过 `CoronaEditorApi.project` / `editorApi.project` 使用同一聚合契约。

## 设计决策

1. 已完成的 owner 迁移：native manifest 声明
   `project.copyExistingToData`，C++ handler 负责文件系统语义。
2. Python 和 Frontend 仅提供薄 wrapper，不复制 ProjectCopy schema 或实现。
3. 删除旧 Python 实现前，保留契约、路径边界和失败回滚验证。

## 验收

- manifest、Python、Frontend wrapper 三者名称一致；
- native handler 覆盖复制、重名、路径规范化和失败清理；
- 旧 Python 复制实现不存在；
- ProjectLauncher、runtime、API 和路径边界测试通过。
