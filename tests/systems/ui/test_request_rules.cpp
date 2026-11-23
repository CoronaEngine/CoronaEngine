// Reader rules that do not need a V8 context: the V8 adapter parses strings and booleans,
// these decide which request shapes are legal. They live in a header so a plain test can
// cover them; the CEF smoke still covers the adapter end to end.
#include "cef/cef_request_rules.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (condition) return;
    std::cerr << "RequestRulesTests failed: " << message << '\n';
    std::exit(1);
}

using Corona::Systems::UI::RequestRules::CloseIdentity;
using Corona::Systems::UI::RequestRules::PresentationKind;

}  // namespace

int main() {
    // close(): exactly one identity kind.
    require(Corona::Systems::UI::RequestRules::close_identity(true, false, false) == CloseIdentity::Panel,
            "a panel id alone is a panel close");
    require(Corona::Systems::UI::RequestRules::close_identity(true, true, false) == CloseIdentity::None,
            "a panel id plus a scene is ambiguous");
    require(Corona::Systems::UI::RequestRules::close_identity(true, false, true) == CloseIdentity::None,
            "a panel id plus a camera is ambiguous");
    require(Corona::Systems::UI::RequestRules::close_identity(false, true, true) == CloseIdentity::Camera,
            "scene plus camera is one camera surface");
    require(Corona::Systems::UI::RequestRules::close_identity(false, true, false) == CloseIdentity::Scene,
            "a bare scene is the plural close");
    require(Corona::Systems::UI::RequestRules::close_identity(false, false, true) == CloseIdentity::None,
            "a camera without its scene names nothing");
    require(Corona::Systems::UI::RequestRules::close_identity(false, false, false) == CloseIdentity::None,
            "an empty object names nothing");

    // setWindowPresentation(): a state is a mode or a size, never both.
    require(Corona::Systems::UI::RequestRules::presentation_kind(true, false, false) == PresentationKind::Mode,
            "a mode alone is a mode");
    require(Corona::Systems::UI::RequestRules::presentation_kind(false, true, false) == PresentationKind::Size,
            "a size alone is a size");
    require(Corona::Systems::UI::RequestRules::presentation_kind(true, true, false) == PresentationKind::None,
            "a mode plus a size is ambiguous");
    require(Corona::Systems::UI::RequestRules::presentation_kind(false, false, false) == PresentationKind::None,
            "no state names nothing");
    require(Corona::Systems::UI::RequestRules::presentation_kind(false, true, true) == PresentationKind::None,
            "the main window has no size request");
    require(Corona::Systems::UI::RequestRules::presentation_kind(true, false, true) == PresentationKind::Mode,
            "the main window accepts a mode");

    // The main window owns an OS window: windowed, borderless, toggle, never maximized.
    require(Corona::Systems::UI::RequestRules::main_accepts_mode(false), "windowed and borderless are fine");
    require(!Corona::Systems::UI::RequestRules::main_accepts_mode(true), "maximized belongs to cameras");

    std::cout << "RequestRulesTests passed\n";
    return 0;
}
