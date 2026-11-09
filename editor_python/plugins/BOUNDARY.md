# 编辑器插件边界

Python 宿主注册 `ProjectArchive`、`ProjectFiles` 和 `FileScripts`。前两者保存场景以及 Web 提交的文档和脚本。文件运行器在同一解释器中运行指定文件，管理输入、停止和内存恢复。生命周期由 `runtime/registry.py` 和 `runtime/editor_host.py` 管理。

`ProjectLauncher/templates/actor/demo.actor` 是原生文件管理器使用的模板。场景、文件和项目设置由 C++ manifest 处理。Web 通过 `Frontend/src/api/editorApi.js` 访问这些接口。
