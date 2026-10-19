#include <corona/systems/ui/mouse_move_dispatch.h>

#include <cstdlib>
#include <iostream>

using Corona::Systems::UI::MouseMoveDispatch;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    MouseMoveDispatch main, detached;
    require(main.take(1, 100, 200, 0), "First hover must reach the browser");
    for (int frame = 0; frame < 600; ++frame)
        require(!main.take(1, 100, 200, 0), "Idle frames must not generate mouse events or Python requests");
    require(main.take(1, 101, 200, 0), "Pointer movement must be forwarded");
    require(main.take(1, 101, 201, 0), "Vertical movement must be forwarded");
    require(main.take(1, 101, 201, 1), "Button and modifier changes must be forwarded");
    require(!main.take(1, 101, 201, 1), "A held button must not create idle movement");
    require(main.take(2, 101, 201, 1), "A newly hovered browser must receive its initial position");
    require(detached.take(2, 101, 201, 1), "Detached windows must have independent mouse state");
    main = {};
    require(main.take(2, 101, 201, 1), "Re-entering a panel must restore its hover state");
    std::cout << "MouseMoveDispatchTests passed\n";
}
