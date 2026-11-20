#include "fixture.h"
extern int fixture_implementation();
extern int fixture_base();
int corona_fixture::Object::value() const {
    return fixture_implementation() + fixture_base();
}
VS_REGISTER_HOTFIX(corona_fixture, Object)
extern "C" vision::HotfixSystem* fixture_runtime() { return &vision::HotfixSystem::instance(); }
extern "C" vision::ModuleInterface* fixture_registry() { return &vision::ModuleInterface::instance(); }
VS_EXPORT_API vision::HotfixSystem* fixture_patch_runtime() { return &vision::HotfixSystem::instance(); }
