#include <corona/systems/optics/optics_commands.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

using namespace Corona;
int checks = 0;
int failures = 0;

#define CHECK(condition) do { \
    ++checks; \
    if (!(condition)) { \
        ++failures; \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    } \
} while (false)

OpticsUpdateCommand drain_one(OpticsCommandChannel::Buffer& queue) {
    const auto commands = queue.drain();
    CHECK(commands.size() == 1);
    if (commands.size() != 1) std::exit(1);
    return commands.front();
}

OpticsUpdateCommand emissive_material(std::uintptr_t geometry = 100) {
    OpticsUpdateCommand command;
    command.optics_handle = 42;
    command.geometry_handle = geometry;
    command.fields = OpticsField::MaterialOverrides | OpticsField::Principled;
    command.material_overrides = std::make_shared<const Materials::Overrides>();
    auto material = std::make_shared<Materials::Principled>();
    material->surface.emissive = {2.f, 2.f, 2.f};
    // Vision reads emission from expressions, as supplied by both importers.
    material->expressions = std::make_shared<const nlohmann::json>(
        nlohmann::json{{"emission", {2.f, 2.f, 2.f}}});
    command.principled = material;
    return command;
}

void test_imported_emission_survives_visibility() {
    for (const bool material_first : {true, false}) {
        OpticsCommandChannel::Buffer queue;
        const auto material = emissive_material();
        OpticsUpdateCommand visible;
        visible.optics_handle = material.optics_handle;
        visible.fields = OpticsField::Visible;
        visible.visible = false;
        queue.push(material_first ? material : visible);
        queue.push(material_first ? visible : material);

        const auto pending = queue.peek(material.optics_handle);
        CHECK(pending && pending->principled == material.principled);
        CHECK(pending && pending->geometry_handle == 100);
        OpticsDevice device;
        device.geometry_handle = 100;
        const auto command = drain_one(queue);
        apply_optics_update(device, command);
        CHECK(!device.visible);
        CHECK(device.material_overrides == material.material_overrides);
        CHECK(device.principled == material.principled);
        if (device.principled) {
            CHECK(device.principled->expressions->at("emission") == nlohmann::json({2.f, 2.f, 2.f}));
        }
        CHECK(queue.stats().coalesced == 1);

        // The normal import publishes visible=true after its material command.
        queue.push(material);
        visible.visible = true;
        queue.push(visible);
        apply_optics_update(device, drain_one(queue));
        CHECK(device.visible && device.principled == material.principled);
    }
}

void test_independent_scalar_fields_and_latest_value() {
    OpticsCommandChannel::Buffer queue;
    OpticsField expected = OpticsField::None;
    const auto push = [&](OpticsField field, auto member, auto value) {
        OpticsUpdateCommand command;
        command.optics_handle = 42;
        command.fields = field;
        command.*member = value;
        expected = expected | field;
        queue.push(command);
    };
    push(OpticsField::Visible, &OpticsUpdateCommand::visible, false);
    push(OpticsField::EnableLighting, &OpticsUpdateCommand::enable_lighting, false);
    push(OpticsField::Metallic, &OpticsUpdateCommand::metallic, .1f);
    push(OpticsField::Roughness, &OpticsUpdateCommand::roughness, .2f);
    push(OpticsField::Subsurface, &OpticsUpdateCommand::subsurface, .3f);
    push(OpticsField::Specular, &OpticsUpdateCommand::specular, .4f);
    push(OpticsField::SpecularTint, &OpticsUpdateCommand::specular_tint, .5f);
    push(OpticsField::Anisotropic, &OpticsUpdateCommand::anisotropic, .6f);
    push(OpticsField::Sheen, &OpticsUpdateCommand::sheen, .7f);
    push(OpticsField::SheenTint, &OpticsUpdateCommand::sheen_tint, .8f);
    push(OpticsField::Clearcoat, &OpticsUpdateCommand::clearcoat, .9f);
    push(OpticsField::ClearcoatGloss, &OpticsUpdateCommand::clearcoat_gloss, .1f);
    push(OpticsField::Ambient, &OpticsUpdateCommand::ambient, ktm::fvec3{.1f, .2f, .3f});
    push(OpticsField::Diffuse, &OpticsUpdateCommand::diffuse, ktm::fvec3{.4f, .5f, .6f});
    push(OpticsField::SpecularColor, &OpticsUpdateCommand::specular_color, ktm::fvec3{.7f, .8f, .9f});
    push(OpticsField::Shininess, &OpticsUpdateCommand::shininess, 10.f);
    push(OpticsField::Metallic, &OpticsUpdateCommand::metallic, .95f);
    const auto command = drain_one(queue);
    CHECK(command.fields == expected);
    OpticsDevice device;
    apply_optics_update(device, command);
    CHECK(!device.visible && !device.bEnableLighting);
    CHECK(device.metallic == .95f && device.roughness == .2f);
    CHECK(device.subsurface == .3f && device.specular == .4f);
    CHECK(device.specularTint == .5f && device.anisotropic == .6f);
    CHECK(device.sheen == .7f && device.sheenTint == .8f);
    CHECK(device.clearcoat == .9f && device.clearcoatGloss == .1f);
    CHECK(device.ambient.x == .1f && device.ambient.y == .2f && device.ambient.z == .3f);
    CHECK(device.diffuse.x == .4f && device.diffuse.y == .5f && device.diffuse.z == .6f);
    CHECK(device.specular_color.x == .7f && device.specular_color.y == .8f && device.specular_color.z == .9f);
    CHECK(device.shininess == 10.f);
}

void test_material_replacement_and_explicit_clear() {
    OpticsCommandChannel::Buffer queue;
    const auto first = emissive_material();
    auto latest = emissive_material();
    queue.push(first);
    queue.push(latest);
    OpticsDevice device;
    device.geometry_handle = 100;
    apply_optics_update(device, drain_one(queue));
    CHECK(device.principled == latest.principled);
    CHECK(device.material_overrides == latest.material_overrides);

    queue.push(latest);
    latest.principled.reset();
    latest.material_overrides.reset();
    queue.push(latest);
    OpticsUpdateCommand visible;
    visible.optics_handle = 42;
    visible.fields = OpticsField::Visible;
    queue.push(visible);
    const auto cleared = drain_one(queue);
    CHECK(has_optics_field(cleared.fields, OpticsField::Principled));
    CHECK(has_optics_field(cleared.fields, OpticsField::MaterialOverrides));
    apply_optics_update(device, cleared);
    CHECK(!device.principled && !device.material_overrides);
}

void test_geometry_guard_and_partial_materials() {
    OpticsCommandChannel::Buffer queue;
    const auto material = emissive_material();
    OpticsUpdateCommand visible;
    visible.optics_handle = 42;
    visible.fields = OpticsField::Visible;
    visible.visible = false;
    queue.push(material);
    queue.push(visible);
    OpticsDevice device;
    device.geometry_handle = 200;
    apply_optics_update(device, drain_one(queue));
    CHECK(!device.visible); // Scalars still apply when the material owner differs.
    CHECK(!device.principled && !device.material_overrides);

    // A partial material update for the same owner retains the other payload.
    auto partial = emissive_material();
    partial.fields = OpticsField::Principled;
    queue.push(material);
    queue.push(partial);
    auto merged = drain_one(queue);
    CHECK(merged.principled == partial.principled);
    CHECK(merged.material_overrides == material.material_overrides);

    // Changing a guard, including guarded <-> unguarded, invalidates any
    // pending material field not supplied by the new owner, but keeps scalars.
    for (auto owners : {std::pair{100u, 200u}, std::pair{100u, 0u}, std::pair{0u, 200u}}) {
        for (auto field : {OpticsField::Principled, OpticsField::MaterialOverrides}) {
            queue.push(emissive_material(owners.first));
            queue.push(visible);
            partial = emissive_material(owners.second);
            partial.fields = field;
            queue.push(partial);
            merged = drain_one(queue);
            CHECK(merged.fields == (OpticsField::Visible | field));
            CHECK(merged.geometry_handle == owners.second);
            CHECK(!merged.visible);
            CHECK(bool(merged.principled) == (field == OpticsField::Principled));
            CHECK(bool(merged.material_overrides) == (field == OpticsField::MaterialOverrides));
            device = OpticsDevice{};
            device.geometry_handle = 200;
            apply_optics_update(device, merged);
            CHECK(!device.visible);
            CHECK(device.principled == merged.principled);
            CHECK(device.material_overrides == merged.material_overrides);
        }
    }
}

void test_distinct_optics_handles() {
    OpticsCommandChannel::Buffer queue;
    const auto first = emissive_material();
    auto second = emissive_material(200);
    second.optics_handle = 43;
    queue.push(first);
    queue.push(second);
    OpticsUpdateCommand visibility;
    visibility.optics_handle = 42;
    visibility.fields = OpticsField::Visible;
    visibility.visible = false;
    queue.push(visibility);
    const auto commands = queue.drain();
    CHECK(commands.size() == 2);
    if (commands.size() != 2) return;
    CHECK(commands[0].optics_handle == 43 && commands[0].principled == second.principled);
    CHECK(commands[1].optics_handle == 42 && commands[1].principled == first.principled);
    CHECK(commands[0].visible && !commands[1].visible);
}

} // namespace

int main() {
    test_imported_emission_survives_visibility();
    test_independent_scalar_fields_and_latest_value();
    test_material_replacement_and_explicit_clear();
    test_geometry_guard_and_partial_materials();
    test_distinct_optics_handles();
    std::printf("Optics commands: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
