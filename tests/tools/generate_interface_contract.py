"""Generate docs/interface-contract.md from the sources the guards already check.

Run it after changing the Web surface or the ScriptRuntime table:

    python tests/tools/generate_interface_contract.py

`test_interface_contract_doc.py` fails when the checked-in document does not match what this
produces, so the document cannot drift from the code it describes.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import rejection_codes

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "src/systems/ui/cef/script_runtime_api.h"
SURFACE = ROOT / "tests/tools/test_typed_native_surface.py"
OUT = ROOT / "docs/interface-contract.md"

NAMESPACE_NOTES = {
    "app": "进程级操作（关闭）",
    "audio": "Audio 资源与 Actor 播放",
    "bridge": "唯一的 Web → Python 入口，一函数宽",
    "files": "文件管理器打开（其余文件操作已走 bridge）",
    "gizmo": "视口 gizmo 指针与目标",
    "input": "输入事件派发",
    "network": "协作会话与发现",
    "project": "项目新建/打开/最近列表",
    "project_settings": "当前项目元数据保存",
    "scene": "场景读、写、Actor 与相机",
    "viewport": "视口配置与拾取",
    "window": "Dock、面板与相机窗口",
}

GUARDS = [
    ("test_manifest_retirement.py", "已删除的 manifest 不许回来"),
    ("test_typed_native_surface.py", "Web 面方法 + 拒绝码词表"),
    ("test_script_runtime_contract.py", "脚本绑定：名字 / 参数位 / 数量 / 实现不越界"),
    ("test_interface_contract_doc.py", "这份文档与代码一致"),
    ("test_docs_reference_live_files.py", "文档不指向已删除的文件"),
    ("test_editor_settings_owner.py", "编辑器 ini 只有一个 Python 所有者"),
    ("test_reply_shapes.py", "Python 产出的回复透传，引擎产出保留类型"),
    ("test_integration_surface.py", "集成冒烟只调用、只断言仍然存在的原生命中"),
    ("test_rejection_vocabulary.py", "拒绝码词表：源码只发已声明的码；稳定码可分支、细节码只进日志"),
    ("test_window_permission_matrix.py", "每个窗口方法都带调用者检查，例外必须逐条说明"),
    ("test_geometry_external_access.py", "外部只读几何场景快照，写一律走命令通道"),
    ("test_python_runtime_deploy.py", "Python 运行时部署是增量的：未变化的树只做 stat"),
    ("test_source_pins.py", "pin 改写幂等；只有已推送到远程的提交才算可 pin"),
    ("test_frontend_facade.py", "原生方法↔facade 一一对应；前端只经 facade 触达原生对象"),
    ("test_frontend_regressions.py", "逐个跑前端窗口/音频/facade 回归文件（带超时）"),
    ("test_web_layer_budget.py", "接口核心行数 ratchet（度量在 web_layer_budget.py）"),
    ("test_scene_api_consolidation.py", "场景 V8 只保留合并后的方法名与总数"),
    ("test_web_python_bridge.py", "Web→Python 信封格式与错误码映射"),
    ("test_python_refusal_codes.py", "Python 侧拒绝码都在契约词表内"),
    ("test_lanchat_removal.py", "局域网聊天接口已删除，不得回归"),
    ("test_project_allocator.py", "项目目录分配器：重名与原子创建"),
    ("test_vision_thermal_run.py", "Vision 热运行路径"),
]


def script_operations():
    header = HEADER.read_text(encoding="utf-8-sig")
    rows = re.findall(
        r'\{"(script_scene_\w+)",\s*"((?:[^"\\]|\\.)*)",\s*(\d+),\s*\n\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}',
        header)
    specs = []
    for name, arguments, arity, summary in rows:
        text = " ".join(re.findall(r'"((?:[^"\\]|\\.)*)"', summary)).strip()
        specs.append((name, arguments, arity, text))
    return specs


def web_surface():
    source = SURFACE.read_text(encoding="utf-8-sig")
    block = source[source.index("EXPECTED_SURFACE = {"):source.index("\n}\n", source.index("EXPECTED_SURFACE"))]
    entries = re.findall(r'^    "(\w+)": \{(.*?)\},?$', block, re.M | re.S)
    return [(ns, sorted(set(re.findall(r'"(\w+)"', body)))) for ns, body in entries]


def build() -> str:
    specs = script_operations()
    surface = web_surface()
    method_total = sum(len(names) for _, names in surface)

    lines = [
        "# 接口契约（Web / ScriptRuntime）",
        "",
        "这份文档是接口层的**单一入口**。它不重复代码，只给出三条边界、各自的形状，以及",
        "**哪条测试在保护它**——契约信息散在守卫和注释里时，新人没有起点。",
        "",
        "由 `tests/tools/generate_interface_contract.py` 生成；`test_interface_contract_doc.py`",
        "会在它与代码不一致时失败。",
        "",
        "## 三条边界",
        "",
        "| 边界 | 谁在说 | 落到哪里 | 传输 |",
        "|---|---|---|---|",
        "| Web → C++ | 编辑器前端 | `window.corona.*` | 同步值 / Promise |",
        f"| ScriptRuntime → C++ | 角色脚本（Python） | {len(specs)} 条 `script_scene_*` 具名绑定 | JSON 数组字符串 |",
        "| Web → Python | 前端与 C++ 都可能发起 | `corona.bridge.call` → Python 服务 | JSON 信封 |",
        "",
        "manifest（api 名称表 + caller mask + 注册表）已删除，三条边界里没有任何按名字分发",
        "的桥。名字都是**具名函数**。",
        "",
        "命名空间按职责划分，不再是「一个方法一个命名空间」：项目文档与项目元数据在 `project`，",
        "gizmo 叠加层在 `viewport`，脚本输入派发在 `scene`，音频按身份分 `play`/`stop`。",
        "",
        "## Web 面",
        "",
        f"共 {len(surface)} 个命名空间 / {method_total} 个方法。清单由",
        "`tests/tools/test_typed_native_surface.py` 精确钉死（多一个、少一个都会失败）：",
        "",
        "| 命名空间 | 说明 |",
        "|---|---|",
    ]
    for ns, _ in surface:
        lines.append(f"| `{ns}` | {NAMESPACE_NOTES.get(ns, '')} |")

    lines += [
        "",
        "### 全部方法清单",
        "",
        "| 命名空间 | 方法 |",
        "|---|---|",
    ]
    for ns, names in surface:
        lines.append(f"| `{ns}` | " + "、".join(f"`{n}`" for n in names) + " |")

    lines += [
        "",
        "### 面板与窗口：三个合并后的入口",
        "",
        "同一件事（打开一个面板、关闭一个窗口、改变窗口呈现）以前按“哪个面板 / 哪个开关”拆成",
        "多个方法。现在各留一个，差异用参数表达。**面板停靠由 ImGui 负责**：面板打开后就是一个",
        "ImGui 窗口，用户拖动标签页停靠，布局由 imgui.ini 记住，Web 层不再发送停靠位置。",
        "",
        "| 方法 | 请求 | 说明 |",
        "|---|---|---|",
        "| `createPanel` | `{kind?: 'panel' \\| 'camera', width?, height?, x?, y?, panelId?, routePath?, title?}` |"
        " `kind` 缺省为 `panel`。相机窗口要求 `archiveSession`/`sceneId`/`cameraId` 同时非空；"
        "`cameraHandle` 一律拒绝，相机身份由原生解析。`x`/`y` 只给相机窗口——停靠面板的位置由 "
        "ImGui 决定，面板带 `x`/`y` 会被拒绝而不是静默忽略；`archiveSession`/`sceneId`/`cameraId`"
        " 同样只属于相机窗口。 |",
        "| `close` | 无参数 / `{panelId}` / `{sceneId, cameraId}` / `{sceneId}` |"
        " 无参数关自己；`panelId` 是 `createPanel` 的稳定面板身份（只有主窗口能关别人，主窗口不能被当作面板关）；"
        "`sceneId`+`cameraId` 关一个相机窗口（主窗口可关任意，相机窗口只能关自己）；只给 `sceneId` 是复数形式，"
        "关掉该场景全部相机窗口。返回 `{closed: n}`，`0` 表示没有匹配窗口（幂等清理）。空 `sceneId` 不关任何窗口，"
        "但仍在下一次主线程调度里应答——窗口会话用它作退休栅栏。 |",
        "| `focus` | `{panelId}` | 聚焦该面板（只有主窗口能聚焦别人）。 |",
        "| `setWindowPresentation` | `{target?: 'camera' \\| 'main', mode, sceneId?, cameraId?, width?, height?}` |"
        " 请求要么给 `mode`（`windowed`/`maximized`/`borderless`/`toggle`），要么给 `width`+`height`"
        "（相机窗口改尺寸），不能同时给；主窗口只有 `windowed`/`borderless`/`toggle` 且没有尺寸请求。"
        "相机窗口可用 `sceneId`+`cameraId` 指定，缺省为自己。 |",
        "",
        "`mode` 描述**窗口要处于的状态**，不是要执行的开关；`toggle` 是唯一不带状态值的请求，",
        "表示“往前走一步”（相机窗口：无边框 → 窗口化，窗口化 ⇄ 最大化）。",
        "`createPanel` 返回 `{window_token, panel_id, existing}`，其余窗口方法返回布尔。",
        "",
        "`panelId` 是**稳定面板身份**，开/关/聚焦都用它。`windowToken` 是**这一次创建**的",
        "不透明令牌：同一个 `panelId` 重开后令牌不同，用来把 `onClosed` 的负载和当初那次创建的",
        "返回值对齐（原始 tab id 不再暴露）。窗口不查询自身状态：身份来自 `createPanel` 的返回值",
        "和打开时带在 URL 上的参数。",
        "",
        "**权限**：窗口操作按调用者身份授权，规则集中在 `cef_window_operations.cpp` 的每个 case，",
        "这里列全，接入方不必读源码：",
        "",
        "| 调用者 \\ 操作 | 关自己 | `close({panelId})` | `close({sceneId, cameraId})` | `close({sceneId})` | `focus({panelId})` | `setWindowPresentation({target:'main'})` |",
        "|---|---|---|---|---|---|---|",
        "| 主工作区 | ✗（主窗口不是面板） | ✓ 除主窗口外的任意面板 | ✓ 任意相机 | ✓ 该场景的全部相机窗口 | ✓ 任意面板 | ✓ |",
        "| 面板 | ✓ | ✗ | ✗ | ✗ | ✓ 只有自己 | ✗ |",
        "| 相机窗口 | ✓ | ✗ | ✓ 只有自己 | ✓ 仅当该场景只剩自己 | ✓ 只有自己 | ✗ |",
        "",
        "`createPanel` 不限调用者；`createCameraView` 也不限主窗口——它由相机租约（场景身份 + ",
        "`camera_authority` 是否仍 active）授权，因为 SceneTools 自己就在一个原生 Dock 上下文里。",
        "越权一律 `PERMISSION_DENIED`，不会静默忽略。",
        "",
        "**失败形状**：Promise 拒绝时携带下表的码，原样透传给 Web。`rejection_codes.py`",
        "声明全集，`test_rejection_vocabulary.py` 保证源码只发这些码。",
        "",
        "| 分组 | 谁给出 | 码 |",
        "|---|---|---|",
        "| 基础设施 | V8 绑定本体，原生工作开始前 | "
        + "、".join(f"`{code}`" for code in sorted(rejection_codes.INFRASTRUCTURE)) + " |",
        "| 窗口 | 原生窗口宿主（窗口消失、建不出来） | "
        + "、".join(f"`{code}`" for code in sorted(rejection_codes.WINDOW)) + " |",
        "| 场景/归档 | 场景与归档层（对象不存在、只读、冲突…） | "
        + "、".join(f"`{code}`" for code in sorted(rejection_codes.SCENE)) + " |",
        "| Web → Python | bridge 传输与 Python 服务 | "
        + "、".join(f"`{code}`" for code in sorted(rejection_codes.BRIDGE)) + " |",
        "",
        "**可分支的稳定码**：" + "、".join(f"`{code}`" for code in sorted(rejection_codes.STABLE)) + "。",
        "Web 可以按这些码决定重试、禁用控件或提示；其余是**细节码**，照常拒绝，但只应进日志——",
        "细节码会随实现改名或合并。稳定码的每一次增删都是契约变更，需要写进",
        "`docs/interface-decisions.md`。",
        "",
        "Python 服务的**业务拒绝**统一是 Promise 拒绝，带 `error.code`：`ProjectArchive` 与",
        "`ProjectFiles` 在 Python 侧抛 `BridgeError(code, message)`，dispatcher 把它写成",
        "`CODE: message`，C++ 桥只从消息前缀取码，因此 `PROJECT_CONTEXT_CHANGED`、",
        "`DOCUMENT_CONFLICT`、`READ_ONLY_PROJECT`、`CONFLICT` 等业务码原样到达前端。",
        "`FileScripts` 是例外：它的回复是**运行状态记录**（`status` 是",
        "`starting/running/completed/error` 状态机），按值返回，由 Blockly 面板按状态渲染。",
        "只有传输或服务本身失败（信封解析、服务未启动、Python 抛未知异常）才回落到",
        "`SERVICE_FAILED`。",
        "同步方法**永不拒绝**：返回值，找不到时返回文档化的空值。",
        "",
        "## ScriptRuntime 面",
        "",
        f"{len(specs)} 条具名绑定。调用方传一个 JSON 数组的字符串；`arity` 是运行时强制的下限",
        "（传少了直接 400，不会进到场景逻辑里）。",
        "",
        "| 绑定 | 参数 | arity | 说明 |",
        "|---|---|---|---|",
    ]
    for name, arguments, arity, summary in specs:
        lines.append(f"| `{name.replace('script_scene_', '')}` | `{arguments}` | {arity} | {summary} |")

    lines += [
        "",
        "**失败形状**：`{success: false, error, error_code, data}`。",
        "",
        "- `error_code` 是数字类：**409** 归档过期、**403** 只读、**404** Actor 不存在、",
        "  **400** 参数不合法、**2** 其它。",
        "- `data.code` 保留场景层的符号码（`PROJECT_CONTEXT_CHANGED`、`ACTOR_NOT_READY`…）。",
        "  按数字分支，日志里引用符号。Python 侧抛出的异常形如",
        "  `RuntimeError(\"PROJECT_CONTEXT_CHANGED: ...\")`。",
        "",
        "## 文件脚本路由",
        "",
        "测试探针用 `Scripts.*` 名字和 C++ 通信，但生产代码里没有名字分发：",
        "[`file_script_api.h`](../include/corona/systems/ui/file_script_api.h) 提供五条具名入口，",
        "名字到函数的映射留在探针内部。",
        "",
        "## 保护这份契约的守卫",
        "",
        "| 守卫 | 保护什么 |",
        "|---|---|",
    ]
    for name, what in GUARDS:
        lines.append(f"| [`{name}`](../tests/tools/{name}) | {what} |")

    lines += [
        "",
        "## 已知缺口",
        "",
        "`embedded_python/script_runtime/BOUNDARY.md` 记录了脚本侧**有意保留**的能力缺口：",
        "文件音频播放（Actor 自带音频已支持）、原生射线查询、指针捕获。补的时候要先在",
        "`script_runtime_api.h` 加具名入口，而不是恢复\"探测一个不存在的函数再静默降级\"的写法。",
        "",
        "接口侧的覆盖缺口（记录在此，避免被当成已完成）：",
        "",
        "- 窗口/音频/面板请求的**参数校验**只有手动 CEF 冒烟覆盖（`web_messages_cef_smoke.mjs`）。",
        "  读者（`read_close_request`、`read_window_presentation`、`read_audio_control`、`read_panel_options`）",
        "  依赖 `CefV8Value`，单测需要真实 V8 上下文；`tests/tools` 的守卫只钉方法名、拒绝码和集成调用。",
        "- 窗口不再有 `getCurrent()`：它唯一的产品消费者是测试包装，身份改由 `createPanel` 的返回值和",
        "  URL 参数提供。需要「问问自己是谁」的新页面应走同样的两条路，而不是恢复这个查询。",
        "- `SceneReply` 的 30 个字段每一个都被 `cef_v8_binding.cpp` 读取，所以没有拆成 `std::variant`：",
        "  变体只会改变回复形状，不会减少任何载荷。",
        "",
    ]
    return "\n".join(lines)


def main():
    OUT.write_text(build(), encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
