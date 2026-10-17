"""CEF 位图的通道序必须在三处保持一致：纹理格式声明、view 侧转换、popup 侧转换。

背景：CEF 在 Windows 上给的是 BGRA。当前管线逐像素把 BGRA→RGBA 转掉，再把纹理声明为
`SRGBA8_UNORM`（`src/systems/ui/cef/cef_client.cpp` 与 `src/systems/ui/vulk/browser_manager_vulkan.cpp`）。

陷阱：`PopupOverlay::composite_over()` 是把 popup 像素 **memcpy 到同一块 view 缓冲**上
（参数名就叫 `view_rgba`），所以 view 侧与 popup 侧**必须处于同一通道序**。
只改一边（例如"按 P1-1 把纹理换成 SBGRA8_UNORM 并删掉 view 侧转换"，却忘了 popup 侧也会转换）
不会让任何现有测试变红——`CefPopupOverlayTests`
（`tests/systems/ui/test_cef_popup_overlay.cpp` 的 `test_update_pixels_converts_bgra_to_rgba_and_keeps_alpha`）
只验证 popup 自身契约，看的是"它转换了"，而不是"它与 view 一致"。

这条棘轮就是让这种"只改一边"无法静默通过。
"""

from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
UI_ROOT = REPO_ROOT / "src" / "systems" / "ui"

VIEW_SOURCE = UI_ROOT / "cef" / "cef_client.cpp"
POPUP_SOURCE = UI_ROOT / "cef" / "popup_overlay.cpp"
TEXTURE_SOURCE = UI_ROOT / "vulk" / "browser_manager_vulkan.cpp"

# view 侧转换：PET_VIEW 分支里逐像素交换 R/B。
VIEW_CONVERSION = "std::swap(pixels[i], pixels[i + 2]);"
# popup 侧转换：按通道索引把 B/R 对调（`pixels_[i*k+0] = src[i*k+2];`）。
POPUP_CONVERSION = "pixels_[i * kChannels + 0] = src[i * kChannels + 2];"

RGBA_FORMAT = "Horizon::Format::SRGBA8_UNORM"
BGRA_FORMAT = "Horizon::Format::SBGRA8_UNORM"

# 只检查 CEF 浏览器纹理那一处声明，避免被同文件里其它纹理格式干扰。
BROWSER_TEXTURE_MARKER = '"cef.browser_texture"'


def _read(path: Path) -> str:
    assert path.is_file(), f"缺少被检查的源文件：{path}"
    return path.read_text(encoding="utf-8")


def _browser_texture_declaration(text: str) -> str:
    """截取 `cef.browser_texture` 创建点附近的片段（格式声明与调试名在同一个表达式里）。"""
    index = text.find(BROWSER_TEXTURE_MARKER)
    assert index != -1, f"未找到 CEF 浏览器纹理的创建点（{BROWSER_TEXTURE_MARKER}）"
    return text[max(0, index - 800) : index + 200]


def test_browser_texture_declares_exactly_one_channel_order():
    declaration = _browser_texture_declaration(_read(TEXTURE_SOURCE))
    declares_rgba = RGBA_FORMAT in declaration
    declares_bgra = BGRA_FORMAT in declaration
    assert declares_rgba != declares_bgra, (
        "CEF 浏览器纹理必须且只能声明一种通道序（SRGBA8_UNORM 或 SBGRA8_UNORM），"
        f"实际：SRGBA8_UNORM={declares_rgba}, SBGRA8_UNORM={declares_bgra}"
    )


def test_view_and_popup_convert_the_bitmap_in_lockstep():
    view_converts = VIEW_CONVERSION in _read(VIEW_SOURCE)
    popup_converts = POPUP_CONVERSION in _read(POPUP_SOURCE)
    assert view_converts == popup_converts, (
        "view 侧与 popup 侧的 BGRA→RGBA 转换必须同步："
        "composite_over() 会把 popup 像素直接 memcpy 到 view 缓冲，只改一边会让浮层颜色错乱"
        f"（view_converts={view_converts}, popup_converts={popup_converts}）"
    )


def test_texture_format_agrees_with_the_cpu_conversion():
    declaration = _browser_texture_declaration(_read(TEXTURE_SOURCE))
    declares_rgba = RGBA_FORMAT in declaration
    view_converts = VIEW_CONVERSION in _read(VIEW_SOURCE)
    assert declares_rgba == view_converts, (
        "纹理格式声明与 CPU 转换必须互为反操作：转换过的缓冲是 RGBA（需 SRGBA8_UNORM），"
        f"未转换的是 BGRA（需 SBGRA8_UNORM）。实际：declares_rgba={declares_rgba}, "
        f"view_converts={view_converts}"
    )


if __name__ == "__main__":
    # 本仓环境没有 pytest，但这个棘轮必须真的被执行（否则它拦不住任何东西）：
    # 既保留 pytest 能发现的 test_* 函数，又允许 `python <file>` / ctest 直接运行。
    import sys

    failures: list[str] = []
    for name, candidate in sorted(globals().items()):
        if name.startswith("test_") and callable(candidate):
            try:
                candidate()
            except AssertionError as error:
                failures.append(f"{name}: {error}")

    for failure in failures:
        print(f"FAIL {failure}")
    print(f"{'FAILED' if failures else 'OK'}: {len(failures)} failure(s)")
    sys.exit(1 if failures else 0)
