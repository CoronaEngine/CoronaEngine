// 「面板边框拖动合并应用」策略的单测。
//
// 背景（实测）：拖动浮动面板边框时，引擎每帧调一次 BrowserManager::resize_tab()，
// 而它每次都销毁并重建整块 CEF 纹理并让渲染进程整页重排；实测 120 步拖拽产生 120 次重建、
// 单次 1.90–7.36ms，即每秒 0.24–0.36 秒纯重建开销。
//
// 本策略决定"这一次尺寸变化是否值得现在真的重建"，是纯函数（无副作用、无时间源），
// 因此可以在这里把阈值/时间上限/方向/未变尺寸等边界全部钉住。
// 设计文档：docs/development/panel-resize-coalescing-design.md

#include <corona/systems/ui/panel_resize_policy.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (condition) {
        return;
    }
    std::cerr << "PanelResizePolicyTests failed: " << message << '\n';
    std::exit(1);
}

using std::chrono::milliseconds;

using Corona::Systems::UI::PanelResizeGate;
using Corona::Systems::UI::kPanelResizeGate;
using Corona::Systems::UI::should_apply_deferred_resize;

// 一个"远未到时间上限"的间隔：用来证明下面的判定确实由尺寸阈值起作用，而不是被时间兜住。
constexpr milliseconds kJustNow{16};
// 一个"已远超时间上限"的间隔。
constexpr milliseconds kLongAgo{10000};

void test_default_gate_values() {
    // 这两个常量是待目视后微调的旋钮；被意外改动应当立刻可见（而不是悄悄改变手感与开销）。
    require(kPanelResizeGate.step_px == 64,
            "default step must be 64 engine pixels (~51 physical px at 125% DPI)");
    require(kPanelResizeGate.max_interval == milliseconds(100),
            "default time cap must be 100ms");
}

void test_unchanged_size_never_applies() {
    // 尺寸没变就没有要应用的东西，即使过了很久。调用方本来也不该咨询，
    // 但纯函数的语义必须自洽：不能因为"时间到了"就去重建一块同尺寸的纹理。
    require(!should_apply_deferred_resize(1480, 790, 1480, 790, kLongAgo, kPanelResizeGate),
            "an unchanged size must never be applied, however long it has been");
}

void test_below_step_and_within_interval_does_not_apply() {
    require(!should_apply_deferred_resize(1480, 790, 1500, 790, kJustNow, kPanelResizeGate),
            "a 20px change right after the last apply must be deferred");
    require(!should_apply_deferred_resize(1480, 790, 1480, 810, kJustNow, kPanelResizeGate),
            "a 20px change on the height axis right after the last apply must be deferred");
}

void test_step_boundary_is_inclusive_and_one_pixel_short_is_not() {
    require(should_apply_deferred_resize(1480, 790, 1544, 790, kJustNow, kPanelResizeGate),
            "a change of exactly step_px must be applied");
    require(!should_apply_deferred_resize(1480, 790, 1543, 790, kJustNow, kPanelResizeGate),
            "a change of step_px - 1 must still be deferred");
    require(should_apply_deferred_resize(1480, 790, 1480, 854, kJustNow, kPanelResizeGate),
            "the height axis must open the gate on its own");
}

void test_shrink_is_gated_the_same_way_as_growth() {
    require(should_apply_deferred_resize(1480, 790, 1400, 790, kJustNow, kPanelResizeGate),
            "shrinking by 80px must be applied (the gate is direction independent)");
    require(!should_apply_deferred_resize(1480, 790, 1450, 790, kJustNow, kPanelResizeGate),
            "shrinking by 30px right after the last apply must be deferred");
    require(should_apply_deferred_resize(1480, 790, 1390, 860, kJustNow, kPanelResizeGate),
            "a mixed grow/shrink change must be applied when either axis crosses the step");
}

void test_time_cap_rescues_a_slow_drag() {
    // 慢慢拖：尺寸变化始终不过阈值，若没有时间上限就永远不重建（面板内容会一直陈旧）。
    require(!should_apply_deferred_resize(1480, 790, 1500, 790, milliseconds(99), kPanelResizeGate),
            "just under the time cap must still be deferred");
    require(should_apply_deferred_resize(1480, 790, 1500, 790, milliseconds(100), kPanelResizeGate),
            "reaching the time cap must force an apply");
}

void test_gate_is_tunable() {
    // 证明两个常量真的是旋钮：换一个 gate，同一组输入得到不同结论。
    const PanelResizeGate fine{16, milliseconds(1000)};
    require(should_apply_deferred_resize(1480, 790, 1500, 790, kJustNow, fine),
            "a finer step must apply a 20px change that the default gate defers");

    const PanelResizeGate coarse{4096, milliseconds(1000)};
    require(!should_apply_deferred_resize(1480, 790, 1500, 790, milliseconds(999), coarse),
            "a coarse step with a longer cap must keep deferring a 20px change");

    const PanelResizeGate impatient{4096, milliseconds(16)};
    require(should_apply_deferred_resize(1480, 790, 1500, 790, kJustNow, impatient),
            "a zero-ish time cap must apply any real change");
}

}  // namespace

int main() {
    test_default_gate_values();
    test_unchanged_size_never_applies();
    test_below_step_and_within_interval_does_not_apply();
    test_step_boundary_is_inclusive_and_one_pixel_short_is_not();
    test_shrink_is_gated_the_same_way_as_growth();
    test_time_cap_rescues_a_slow_drag();
    test_gate_is_tunable();
    std::cout << "PanelResizePolicyTests passed\n";
    return 0;
}
