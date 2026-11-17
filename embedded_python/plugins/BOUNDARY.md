# 编辑器插件边界

Python 宿主注册 `ProjectArchive` 和 `ProjectFiles` 两个服务：它们保存场景，以及 Web
提交的文档和与文档配对的脚本字节。`FileScripts` 文件运行器随 script runtime 一起删除，
不再注册。生命周期由 `runtime/registry.py` 和 `runtime/editor_host.py` 管理。

`ProjectLauncher/templates/actor/demo.actor` 是原生文件管理器唯一的模板。场景、文件和
项目设置由 C++ 的 typed V8 具名绑定处理；Web 通过 `Frontend/src/api/editorApi.js`
访问这些接口。
