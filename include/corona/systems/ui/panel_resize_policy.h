#pragma once

#include <chrono>

namespace Corona::Systems::UI {

/**
 * @brief 「面板缩放合并应用」的两个阈值（引擎像素 / 时间）。
 *
 * 拖动浮动面板边框时，引擎每帧都会想改面板尺寸；实测每帧真的重建时，每次要
 * 销毁并重建整块 CEF 纹理、并让渲染进程整页重排（1.90–7.36ms，≈60 次/秒）。
 * 这两个常量决定"这次尺寸变化值不值得现在真的重建"：
 *   - `step_px`：单轴尺寸变化达到该值就应用（未应用期间面板内容按比例拉伸，
 *     最大失真 = step_px / 面板宽度，默认 64/1480 ≈ 4.3%，松手时精确收尾）；
 *   - `max_interval`：距上次应用的最长时间，兜住"慢慢拖、尺寸始终不过阈值"的情况。
 *
 * 它们是**待目视验收后可调的旋钮**：调小 step_px 更跟手但重建更多。
 */
struct PanelResizeGate {
    int step_px = 64;
    std::chrono::milliseconds max_interval{100};
};

/// 默认档。拖动结束后必须**无条件**精确应用一次（该强制路径不经过本策略）。
inline constexpr PanelResizeGate kPanelResizeGate{};

/**
 * @brief 判断一次延后的面板缩放是否应当现在应用。
 *
 * 纯函数：无副作用、不读时钟（间隔由调用方算好传入），因此阈值/时间上限/方向/
 * 「尺寸没变」等边界都可以被单测钉住（见 tests/systems/ui/test_panel_resize_policy.cpp）。
 *
 * @param applied_w  已应用的 CEF 缓冲宽度（BrowserTab::width）
 * @param applied_h  已应用的 CEF 缓冲高度（BrowserTab::height）
 * @param wanted_w   期间期望的宽度（BrowserTab::dock_width，已由调用方钳制）
 * @param wanted_h   期间期望的高度（BrowserTab::dock_height，已由调用方钳制）
 * @param since_last_apply 距上次应用到现在的时长
 * @param gate       阈值
 * @return true 表示现在应当应用（重建纹理 + CEF 重排）；false 表示继续延后。
 */
bool should_apply_deferred_resize(int applied_w,
                                  int applied_h,
                                  int wanted_w,
                                  int wanted_h,
                                  std::chrono::milliseconds since_last_apply,
                                  const PanelResizeGate& gate);

}  // namespace Corona::Systems::UI
