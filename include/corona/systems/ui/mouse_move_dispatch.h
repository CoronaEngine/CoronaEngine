#pragma once

#include <cstdint>

namespace Corona::Systems::UI {

class MouseMoveDispatch {
   public:
    bool take(int browser_id, int x, int y, std::uint32_t modifiers) {
        if (browser_id_ == browser_id && x_ == x && y_ == y && modifiers_ == modifiers)
            return false;
        browser_id_ = browser_id;
        x_ = x;
        y_ = y;
        modifiers_ = modifiers;
        return true;
    }

   private:
    int browser_id_ = -1;
    int x_ = 0;
    int y_ = 0;
    std::uint32_t modifiers_ = 0;
};

}  // namespace Corona::Systems::UI
