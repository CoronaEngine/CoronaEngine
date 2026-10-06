#include "optics_debug_labels.h"

#include <iostream>

int main() {
    using Corona::Systems::OpticsDetail::debug_labels_enabled;
    if (debug_labels_enabled(false, false) || debug_labels_enabled(true, false) ||
        !debug_labels_enabled(false, true) || !debug_labels_enabled(true, true)) {
        std::cerr << "debug labels must depend only on their explicit switch\n";
        return 1;
    }
    return 0;
}
