# Frontend API 边界

`src/api/editorApi.js` 是 C++ manifest/schema 在 Vue 侧的唯一公共契约 adapter 和
transport owner。它负责调用参数/返回值校验、caller mask、事件 manifest 和聚合
namespace；Vue、普通 Python 和 AITool 不应直接调用底层 `camera()`、`geometry()`、
`Actor` 或 `Scene` 对象。

## 约束

- 新的编辑器能力先进入 C++ manifest/schema，再在 `editorApi` 中提供聚合语义；
- `services/` 可以组合 `editorApi`，但不得复制 transport、schema、revision、错误
  或事件协议；
- `utils/bridge.js` 只能 re-export 本目录和 service owner，不能定义第二套 API；
- 低延迟视口/输入 bridge 是已登记的 adapter 通道，不替代公共 manifest contract；
- API key、Agent runtime、Scene/Actor 权威状态和 Python 私有模块不属于本目录。

## 删除条件

公共方法只能在 manifest、Python adapter、Vue/service 调用方和外部宿主完成迁移，且
通过契约回归后删除；删除旧 alias 时必须保留清晰的兼容提交和可回退路径。
