#pragma once
#include "hotfix/hotfix.h"
#include "hotfix/module_interface.h"

namespace corona_fixture {
class Object : public vision::RuntimeObject {
public:
    int state{0};
    [[nodiscard]] virtual int value() const;
    [[nodiscard]] const char* class_name() const noexcept override {
        return vision::type_string<Object>();
    }
    VS_HOTFIX_MAKE_RESTORE(RuntimeObject, state)
};
}
extern "C" vision::HotfixSystem* fixture_runtime();
extern "C" vision::ModuleInterface* fixture_registry();
