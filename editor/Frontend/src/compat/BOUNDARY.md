# Frontend 兼容边界

`src/compat` 是旧 CEF/宿主兼容目录，不是 Vue 业务、manifest transport 或 service owner。

## Owner 映射

| 路径 | canonical owner | 当前职责 | 删除条件 |
|---|---|---|---|
| `src/api/editorApi.js` | `src/api/editorApi.js` | manifest transport 和 JS contract adapter | 不删除；它是前端公共契约入口 |
| `src/services/*.js` | 对应 `src/api/editorApi.js` namespace | 领域 facade/请求生命周期和 UI 兼容编排 | 逐服务确认外部调用迁移后再删除 alias，不删除公共 contract |
| `src/utils/bridge.js` | `src/api` + `src/services` + `src/compat` | 历史导出 barrel，仅 re-export canonical owner | 外部旧宿主确认迁移且兼容回归通过 |
| `src/compat/legacyEditorAdapter.js` | `src/api/editorApi.js` | 旧宿主 raw CEF 查询入口 | 外部宿主不再在 Vue 启动前注入查询，并完成 raw CEF 回归 |
| `src/compat/legacyCameraLockPanel.js` | Vue camera-lock UI + viewport input adapter | 旧宿主相机跟随面板和键盘入口 | 旧宿主获得稳定 Vue Actor context，并完成首次加载/锁定/偏移回归 |
| `src/compat/logService.js` | runtime/application logging | 旧日志生命周期 no-op facade；仅供 bridge 和外部宿主 | 外部旧宿主不再调用 `setLogReady`/`setLogClose` |
| `src/compat/appService.js` | `editorApi.app` + Dock transport | 旧 Dock、CameraView 窗口和进程操作 facade；仅供历史 service import 和外部宿主 | 外部旧宿主完成 Dock/window 命令迁移并通过兼容回归 |
| `src/compat/sceneService.js` | `editorApi.scene` / `sceneTools` | 历史场景 service 方法名和参数适配 | 外部旧宿主完成聚合接口迁移 |
| `src/compat/projectService.js` | `editorApi.main` | 历史主视图/项目 service 方法名和 Dock 适配 | 外部旧宿主完成 `main.*`/`app.*` 迁移 |
| `src/compat/scriptingService.js` | `editorApi.scratch` | 历史 Blockly/角色脚本 service 方法名适配 | 外部旧宿主完成 Script Runtime 聚合接口迁移 |
| `src/compat/fileService.js` | `editorApi.files` | 历史文件 service 方法名适配 | 外部旧宿主完成 `files.*` 迁移 |
| `src/compat/projectSettingsService.js` | `editorApi.projectSettings` | 历史项目设置 service 方法名适配 | 外部旧宿主完成 `projectSettings.*` 迁移 |
| `src/utils/legacyEditorAdapter.js` | `src/compat/legacyEditorAdapter.js` | 历史 import 路径转发 | 外部插件无旧 import 后移除 |

## 约束

- `src/compat` 可以保留 raw CEF 协议，但不得向 active Vue 业务扩散；
- 生产页面直接使用 `src/api` 或对应 `src/services` owner，不导入 `src/utils/bridge.js`；
- `src/api/editorApi.js` 是唯一 transport owner，`bridge.js` 不得复制请求格式、schema 或事件协议；
- compat 面板不得持有 Scene/Actor 权威状态，场景操作必须经 manifest 聚合接口或登记的兼容入口；
- 新功能不得放入 `src/compat`，旧入口修改必须同步 `API_OWNERSHIP.md` 和回退测试。
