#include <corona/systems/ui/panel_resize_policy.h>

#include <cstdlib>

namespace Corona::Systems::UI {

bool should_apply_deferred_resize(int applied_w,
                                  int applied_h,
                                  int wanted_w,
                                  int wanted_h,
                                  std::chrono::milliseconds since_last_apply,
                                  const PanelResizeGate& gate) {
    // 尺寸没变就没有需要应用的东西：即使过了很久也不该去重建一块同尺寸的纹理。
    if (wanted_w == applied_w && wanted_h == applied_h) {
        return false;
    }

    if (std::abs(wanted_w - applied_w) >= gate.step_px) {
        return true;
    }
    if (std::abs(wanted_h - applied_h) >= gate.step_px) {
        return true;
    }

    // 慢慢拖：尺寸变化始终不过阈值时，由时间上限兜住（否则面板内容会一直停在旧尺寸）。
    return since_last_apply >= gate.max_interval;
}

}  // namespace Corona::Systems::UI
