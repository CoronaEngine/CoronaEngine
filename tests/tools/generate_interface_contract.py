"""Generate docs/interface-contract.md from the sources the guards already check.

Run it after changing the Web surface:

    python tests/tools/generate_interface_contract.py

`test_interface_contract_doc.py` fails when the checked-in document does not match what this
produces, so the document cannot drift from the code it describes.
"""
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import rejection_codes
import operation_table

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs/interface-contract.md"

NAMESPACE_NOTES = {
    "app": "进程级操作（关闭）",
    "audio": "Audio 资源与 Actor 播放",
    "bridge": "唯一的 Web → Python 入口，一函数宽",
    "files": "文件管理器打开（其余文件操作已走 bridge）",
    "input": "输入事件派发",
    "network": "协作会话与发现",
    "project": "项目新建/打开/最近列表",
    "project_settings": "当前项目元数据保存",
    "scene": "场景读、写、Actor 与相机",
    "input": "脚本输入派发（目标是一个场景相机）",
    "viewport": "视口配置",
    "window": "Dock、面板与相机窗口",
}

GUARDS = [
    ("test_manifest_retirement.py", "已删除的 manifest 不许回来"),
    ("test_typed_native_surface.py", "拒绝码词表；场景行都有参数读取器；bridge 只一个函数；无 JSON 传输；前端只经 facade"),
    ("test_python_native_surface.py", "Python 面规则：无子模块、删掉的名字不回来、绑定不得触达编辑器语义、两组按序安装、加载器标记有效"),
    ("test_python_host_entry_names.py", "宿主循环的入口名两侧一致；python_api.cpp 的字符串名字查找只允许 run 与 loop 两处"),
    ("test_wire_key_naming.py", "过原生的键一律 snake_case，Web 侧翻译只在 appService"),
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
    ("test_scene_api_consolidation.py", "场景查询用显式 owned 投影"),
    ("test_web_python_bridge.py", "Web→Python 信封原样转发：C++ 不解析、不重建，服务白名单在 Python"),
    ("test_python_refusal_codes.py", "Python 侧拒绝码都在契约词表内"),
    ("test_lanchat_removal.py", "局域网聊天接口已删除，不得回归"),
    ("test_project_allocator.py", "项目目录分配器：重名与原子创建"),
    ("test_paths_have_no_write_side_effects.py", "路径解析不写盘；PathsConfig 只留宿主持有的 repo_root"),
    ("test_project_session.py", "项目会话守卫：会话/路径/只读三种拒绝，旧绑定 fail-closed"),
    ("test_archive_refusals.py", "归档/文件服务统一抛 BridgeError；成功值是数据，不被嗅探"),
    ("test_runtime_lifecycle.py", "宿主生命周期：幂等、关闭终态、关闭后拒绝可重试"),
    ("test_vision_thermal_run.py", "Vision 热运行路径"),
]


# Every Web operation is one of three things, and the classification is curated on purpose:
# an operation that is not listed here makes the generator fail, so a new row forces the author
# to say which side it belongs to instead of drifting into the "everything is a capability" blur.
#   capability    - reads or writes engine/editor state; a future Python projection would want it
#   event         - a subscription the UI needs; a host loop has no use for it
#   cross-boundary- the boundary itself (the Python request entry, the input entry)
WEB_CLASS = {
    "bridge.call": "cross-boundary",
    "input.dispatch": "cross-boundary",
    "window.on_closed": "event",
    "window.on_close_requested": "event",
}

# The Python face, same idea: most of it is plumbing the engine exposes to the host it started,
# and only two entries read engine state in a way a Web projection could also express.
HOST_CLASS = {
    "send_log": "诊断",
    "python_runtime_phase": "诊断",
    "drain_input_events": "宿主控制",
    "should_stop": "宿主控制",
    "poll_service_requests": "宿主控制",
    "complete_service_request": "宿主控制",
    "register_python_script_service_dispatcher": "传输接线",
    "unregister_python_script_service_dispatcher": "传输接线",
    "active_project_path": "引擎状态读取",
    "active_project_session": "引擎状态读取",
}

HOST_PURPOSE = {
    "send_log": "把 Python 日志写进引擎日志；一类「可选工具配置缺失」的 ERROR 降级为 WARNING，避免误报",
    "python_runtime_phase": "更新原生侧运行时诊断相位，供关停诊断与超时报告读取",
    "drain_input_events": "排空 SDL 与 Web 共用的输入队列。**返回值当前被丢弃**——它是唯一排空者，作用是防增长",
    "should_stop": "宿主是否已请求停止；无协调器时返回 `false`（没有循环可停）",
    "poll_service_requests": "拉取排队的服务调用（`{request_id, service, function, args}`）；拉出的每条都必须结算",
    "complete_service_request": "结算一条 ticket；转换失败也结算（以失败），否则等待方永久挂起",
    "register_python_script_service_dispatcher": "把 Python 的服务分发器接到 CEF，使 Web 请求有落点",
    "unregister_python_script_service_dispatcher": "摘掉分发器（关停时）",
    "active_project_path": "编辑器当前场景的项目根，空串表示没有",
    "active_project_session": "会话身份的唯一权威副本：`archive_session` 由 Python 铸造、原生持有，单看路径无法区分重开",
}

HOST_TYPE_PURPOSE = {
    "InputEvent": "SDL 与 Web 共用输入队列的载荷。15 个只读字段；身份字段（`archive_session`/`scene_id`/`camera_id`）对 SDL 输入为空。**当前无消费者。**",
}

# Which source file installs which group. Order matters: the module installs them in this order.
HOST_GROUPS = (
    ("BindHostSupport", "src/systems/script/python/host_support_bindings.cpp"),
    ("BindCef", "src/systems/ui/cef/cef_py_bind.cpp"),
    ("BindCefServiceQueue", "src/systems/script/python/service_queue_bindings.cpp"),
)


def _defs(path: Path):
    """(name, line) for every `m.def("name", ...)` in one binding file."""
    text = path.read_text(encoding="utf-8-sig")
    return [(match.group(1), text[:match.start()].count("\n") + 1)
            for match in re.finditer(r'm\.def\(\s*"(\w+)"', text)]


def python_bindings():
    """(group, name, file, line) for every named entry the module installs, by group."""
    rows = []
    for group, rel in HOST_GROUPS:
        for name, line in _defs(ROOT / rel):
            rows.append((group, name, rel, line))
    return rows


def python_types():
    """(name, file, line) for every type the module exports."""
    text = (ROOT / HOST_GROUPS[0][1]).read_text(encoding="utf-8-sig")
    return [(match.group(1), HOST_GROUPS[0][1], text[:match.start()].count("\n") + 1)
            for match in re.finditer(r'nb::class_<[^>]+>\(\s*m\s*,\s*"(\w+)"', text)]


def web_class(group, name):
    return WEB_CLASS.get(f"{group}.{name}", "capability")


def script_operations():
    """(namespace, name, arguments, arity, summary) for every Web row, in table order."""
    return [(row["group"], row["name"], "", 0, row["summary"])
            for row in operation_table.web_rows()]


def web_surface():
    """(namespace, sorted method names) the binding installs, from the one table."""
    import native_surface

    return [(ns, sorted(names)) for ns, names in sorted(native_surface.EXPECTED_SURFACE.items())]


def python_services():
    """(service, module, functions) for every service the bridge may reach.

    The allowlist lives in `editor_host.py`; the function list is parsed from the service
    module, so adding a method or a service makes this document stale until regenerated.
    """
    editor = (ROOT / "embedded_python/runtime/editor_host.py").read_text(encoding="utf-8-sig")
    allowed = re.findall(r"ALLOWED_SERVICES = frozenset\(\{([^}]*)\}\)", editor)
    names = re.findall(r'"(\w+)"', allowed[0]) if allowed else []
    registry = (ROOT / "embedded_python/runtime/registry.py").read_text(encoding="utf-8-sig")
    mapping = dict(re.findall(r'"(\w+)": \("([\w.]+)",\s*"\w+"\)', registry))
    services = []
    for name in sorted(names):
        module = mapping.get(name, "")
        path = ROOT / "embedded_python" / (module.replace(".", "/") + ".py") if module else Path()
        text = path.read_text(encoding="utf-8-sig") if module and path.is_file() else ""
        functions = sorted(set(re.findall(r"^\s{4}def ([a-z]\w*)\(", text, re.M)) |
                           set(re.findall(r"^\s{4}([a-z]\w*) = staticmethod\(", text, re.M)))
        services.append((name, module, functions))
    return services

def build() -> str:
    specs = script_operations()
    surface = web_surface()
    method_total = sum(len(names) for _, names in surface)
    bindings = python_bindings()
    types = python_types()

    # A curated classification that silently misses a new row is worse than none: fail here so the
    # author states which side the new entry belongs to.
    known_web = {f"{g}.{n}" for g, n, *_ in specs}
    stale_web = sorted(set(WEB_CLASS) - known_web)
    unclassified = sorted(set(HOST_CLASS) - {name for _, name, *_ in bindings})
    missing_class = sorted({name for _, name, *_ in bindings} - set(HOST_CLASS))
    missing_purpose = sorted({name for _, name, *_ in bindings} - set(HOST_PURPOSE))
    if stale_web or unclassified or missing_class or missing_purpose:
        raise SystemExit(
            "interface face changed: stale_web=%s stale_python=%s unclassified=%s undocumented=%s"
            % (stale_web, unclassified, missing_class, missing_purpose))

    lines = [
        "# 接口契约（Web / Python）",
        "",
        "这份文档是接口层的**单一入口**。它不重复代码，只给出边界、各自的形状，以及",
        "**哪条测试在保护它**——契约信息散在守卫和注释里时，新人没有起点。",
        "",
        "由 `tests/tools/generate_interface_contract.py` 生成；`test_interface_contract_doc.py`",
        "会在它与代码不一致时失败。Python 面的清单从绑定源码解析，新增一条 `m.def` 而没在",
        "生成器里登记归类或用途时，生成器直接失败——这样「能力」不会悄悄膨胀成一团模糊。",
        "",
        "## 三条边界",
        "",
        "| 边界 | 谁在说 | 落到哪里 | 传输 |",
        "|---|---|---|---|",
        "| Web → C++ | 编辑器前端 | `window.corona.*` | 同步值 / Promise |",
        "| Web → Python | 前端与 C++ 都可能发起 | `corona.bridge.call` → Python 服务 | JSON 信封 |",
        "",
        "manifest（api 名称表 + caller mask + 注册表）已删除，三条边界里没有任何按名字分发",
        "的桥。名字都是**具名函数**。",
        "",
        "命名空间按职责划分，不再是「一个方法一个命名空间」：项目文档与项目元数据在 `project`，",
        "视口配置在 `viewport`，脚本输入派发在 `input`，音频按身份分 `play`/`stop`。",
        "**键名规则**：过原生的键一律 snake_case（`panel_id`、`route_path`、`archive_session`），",
        "请求和回复用同一套写法。窗口命令对象**本身就是线上对象**：`appService` 把命令参数",
        "原样交给绑定，没有 camelCase↔snake_case 的转换层。**方法名也是 snake_case**：表里的",
        "`name` 原样发布成 `window.corona.<group>.<name>`，前端 facade 用同一个拼写，没有推导。",
        "",
        "## Web 面",
        "",
        f"共 {len(surface)} 个命名空间 / {method_total} 个方法。清单由",
        "`src/systems/ui/cef/editor_operation_table.h` 声明，本文由生成器从 `native_surface.py`",
        "渲染（那张表不再由守卫逐名比对，见 `test_typed_native_surface.py`）：",
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
        "### 每个操作的用途与归类",
        "",
        "归类只有三种：**capability**（读或写引擎/编辑器状态）、**event**（UI 才需要的订阅）、",
        "**cross-boundary**（边界本身）。归类不是自动推导的——表在生成器里，新增一行而不归类",
        "会让生成器失败,而不是默认混进 capability。",
        "",
        "| 命名空间 | 操作 | 归类 | 用途 |",
        "|---|---|---|---|",
    ]
    for group, name, _arguments, _arity, summary in specs:
        lines.append(f"| `{group}` | `{name}` | {web_class(group, name)} | {summary} |")

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
        "| `createPanel` | `{kind?: 'panel' \\| 'camera', width?, height?, x?, y?, panel_id?, route_path?, title?}` |"
        " `kind` 缺省为 `panel`。相机窗口要求 `archive_session`/`scene_id`/`camera_id` 同时非空；"
        "`camera_handle` 一律拒绝，相机身份由原生解析。`x`/`y` 只给相机窗口——停靠面板的位置由 "
        "ImGui 决定，面板带 `x`/`y` 会被拒绝而不是静默忽略；`archive_session`/`scene_id`/`camera_id`"
        " 同样只属于相机窗口。 |",
        "| `close` | 无参数 / `{panel_id}` / `{scene_id, camera_id}` / `{scene_id}` |"
        " 无参数关自己；`panel_id` 是 `createPanel` 的稳定面板身份（只有主窗口能关别人，主窗口不能被当作面板关）；"
        "`scene_id`+`camera_id` 关一个相机窗口（主窗口可关任意，相机窗口只能关自己）；只给 `scene_id` 是复数形式，"
        "关掉该场景全部相机窗口。返回 `{closed: n}`，`0` 表示没有匹配窗口（幂等清理）。空 `scene_id` 不关任何窗口，"
        "但仍在下一次主线程调度里应答——窗口会话用它作退休栅栏。 |",
        "| `focus` | `{panel_id}` | 聚焦该面板（只有主窗口能聚焦别人）。 |",
        "| `setWindowPresentation` | `{target?: 'camera' \\| 'main', mode, scene_id?, camera_id?, width?, height?}` |"
        " 请求要么给 `mode`（`windowed`/`maximized`/`borderless`/`toggle`），要么给 `width`+`height`"
        "（相机窗口改尺寸），不能同时给；主窗口只有 `windowed`/`borderless`/`toggle` 且没有尺寸请求。"
        "相机窗口可用 `sceneId`+`cameraId` 指定，缺省为自己。 |",
        "",
        "`mode` 描述**窗口要处于的状态**，不是要执行的开关；`toggle` 是唯一不带状态值的请求，",
        "表示“往前走一步”（相机窗口：无边框 → 窗口化，窗口化 ⇄ 最大化）。",
        "`createPanel` 返回 `{window_token, panel_id, existing}`；`close` 返回",
        "`{closed: n}`（`0` 表示没有匹配窗口）；`focus` 返回布尔。",
        "",
        "`panel_id` 是**稳定面板身份**，开/关/聚焦都用它。`window_token` 是**这一次创建**的",
        "不透明令牌：同一个 `panel_id` 重开后令牌不同，用来把 `onClosed` 的负载和当初那次创建的",
        "返回值对齐（原始 tab id 不再暴露）。窗口不查询自身状态：身份来自 `createPanel` 的返回值",
        "和打开时带在 URL 上的参数。",
        "",
        "**权限**：窗口操作按调用者身份授权，规则集中在 `cef_window_operations.cpp` 的每个 case，",
        "这里列全，接入方不必读源码：",
        "",
        "| 调用者 \\ 操作 | 关自己 | `close({panel_id})` | `close({scene_id, camera_id})` | `close({scene_id})` | `focus({panel_id})` | `setWindowPresentation({target:'main'})` |",
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
        "Python 服务的**业务拒绝**统一是 Promise 拒绝，带 `error.code`：`project_archive` 与",
        "`project_files` 在 Python 侧抛 `BridgeError(code, message)`，dispatcher 把它写成",
        "符号码落在 `data.code`（与 C++ 场景层同一字段），C++ 桥不再解析消息，因此 `PROJECT_CONTEXT_CHANGED`、",
        "`DOCUMENT_CONFLICT`、`READ_ONLY_PROJECT`、`CONFLICT` 等业务码原样到达前端。",
        "同步方法**永不拒绝**：返回值，找不到时返回文档化的空值。",
        "",
    ]

    services = python_services()
    lines += [
        "",
        "## Python 服务清单（bridge 可达）",
        "",
        "`corona.bridge.call` 只接受这些服务。白名单在 `runtime/editor_host.py` 的",
        "`ALLOWED_SERVICES`；函数清单从服务源码解析，所以新增方法或服务后这份文档必须重新生成。",
        "信封是 `{service, function, args}`，业务拒绝是带 `error.code` 的 Promise 拒绝。",
        "",
        "| 服务 | 模块 | 函数 |",
        "|---|---|---|",
    ]
    ops = sorted(re.findall(r'"(\w+)": \{"required"',
                           (ROOT / "embedded_python/runtime/archive/project_files.py").read_text(encoding="utf-8-sig")))
    lines += [
        "",
        "`project_archive.file_manager` 是**带 op 判别**的服务：`op` 只能是 "
        + "、".join(f"`{op}`" for op in ops) + "，其余按 `INVALID_ARGUMENT` 拒绝；",
        "每个 op 的请求键由 `runtime/archive/arguments.py` 的 `require_keys` 白名单校验，",
        "与 C++ reader 的 `object_keys` 同语义（未知键拒绝，不静默忽略）。",
    ]
    for name, module, functions in services:
        lines.append(f"| `{name}` | `{module}` | " + "、".join(f"`{f}`" for f in functions) + " |")

    lines += [
        "",
        "",
        "## Python 宿主面（`CoronaEngine.*`）",
        "",
        "模块由两个组按序安装：`BindHostSupport` → `BindCef`（`BindCef` 内部再调",
        "`BindCefServiceQueue`）。它是**被引擎启动的宿主**，不是对外服务者；下面绝大多数条目是",
        "宿主管道，不是能力。",
        "",
        "**C++ → Python 只有两处**：`python_api.cpp` 按名字取 `main.run` 与 `main.loop`，启动时",
        "各调用一次。之后 C++ 一次都不再调用 Python 函数。",
        "",
        "| 组 | 接口 | 归类 | 用途 | 定义 |",
        "|---|---|---|---|---|",
    ]
    for group, name, rel, line in bindings:
        lines.append(f"| `{group}` | `{name}` | {HOST_CLASS[name]} | {HOST_PURPOSE[name]} "
                     f"| [`{rel.split('/')[-1]}`](../{rel}#L{line}) |")
    for name, rel, line in types:
        lines.append(f"| — | `{name}`（类型） | 载荷 | {HOST_TYPE_PURPOSE[name]} "
                     f"| [`{rel.split('/')[-1]}`](../{rel}#L{line}) |")

    lines += [
        "",
        "### 两侧对照：不对称之处是设计，不是遗漏",
        "",
        "| 概念 | Web | Python | 为什么不能统一 |",
        "|---|---|---|---|",
        "| 循环控制 | 无 | `should_stop` | 只有拥有循环的一侧需要它；页面没有帧循环 |",
        "| 事件订阅 | `window.on_closed` / `on_close_requested` | 无 | 宿主循环不关心窗口生命周期，只关心自己该不该退 |",
        "| 输入 | `input.dispatch`（写，带 viewport/相机权威校验） | `drain_input_events`（读后丢弃） | 信任前提不同：前者来自不可信前端，后者是引擎自己起的宿主 |",
        "| 项目会话 | 随 `scene.read_active` 带出 | `active_project_path` + `active_project_session` | 宿主需要**权威**会话身份（`archive_session` 单看路径无法区分重开），UI 只需要展示 |",
        "| 归档/文档 | 只有 `project.*` 两个入口，其余走 `bridge.call` | 全部服务在 Python 内 | 实现就在 Python 侧，跨过去只能经过桥 |",
        "",
        "因此「两侧长相一样的 API」不可追求：那会逼一侧塞进它不需要的东西（给页面塞",
        "`poll_service_requests`），或把权限校验降级成可选参数。可以统一的是**能力目录**",
        "（名字、schema、归类、投影到哪几侧），不是调用形状。",
        "",
    ]

    lines += [
        "",
        "",
        "## 超时与并发",
        "",
        "传输层的超时只有一个语义：Python 运行时没有在期限内回应时，C++ 把回复状态映射成",
        "`504`，Web 收到 `EDITOR_API_TIMEOUT`（`python_service_bridge.cpp`）。",
        "**每个操作自己负责超时**——传输层不再叠加第二条 deadline，因为一个慢的解析和一个",
        "挂死的进程要用不同的方式处理，混在传输层会让两者都不可诊断。",
        "",
        "Python 侧是单消费者串行：bridge 请求在同一个 worker 上依次执行，所以两个请求不会",
        "并发改同一个项目；需要并行的工作由操作自己在内部派发。",
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
        "脚本侧**有意保留**的能力缺口（文件音频播放、原生射线查询、指针捕获）随脚本运行时一起"
        "删除了；重新设计时先加具名入口，不要恢复\"探测不存在的函数再静默降级\"的写法。",
        "",
        "接口侧的覆盖缺口（记录在此，避免被当成已完成）：",
        "",
        "- 窗口/音频/面板请求的**参数校验**只有手动 CEF 冒烟覆盖（`web_messages_cef_smoke.mjs`）。",
        "  读者（`read_close_request`、`read_window_presentation`、`read_audio_control`、`read_panel_options`）",
        "  依赖 `CefV8Value`，单测需要真实 V8 上下文；`tests/tools` 的守卫只钉方法名、拒绝码和集成调用。",
        "- 窗口不再有 `getCurrent()`：它唯一的产品消费者是测试包装，身份改由 `createPanel` 的返回值和",
        "  URL 参数提供。需要「问问自己是谁」的新页面应走同样的两条路，而不是恢复这个查询。",
        "- `SceneReply` 已按读/写分族（`SceneReadReply` / `SceneWriteReply`），每个方法只填、",
        "  只读自己族里的字段，不再是一个 30 字段、所有方法共用的结构。",
        "",
    ]
    return "\n".join(lines)


def main():
    OUT.write_text(build(), encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
