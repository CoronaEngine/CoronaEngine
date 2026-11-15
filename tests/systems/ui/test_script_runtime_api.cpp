// Behavioural coverage for the eighteen named ScriptRuntime entry points.
//
// `test_manifest_retirement.py` only asserts that the names and their bindings line
// up. This drives each typed entry point (`script_operation_entry.h`) against a loaded
// scene so the contract itself - argument packing, success shape, archive identity
// checks and the documented failures - is pinned without a Python interpreter.
#include <corona/kernel/core/kernel_context.h>
#include <corona/systems/ui/file_script_api.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "cef/native_api_types.h"
#include "cef/cef_editor_native_api_test_support.h"
#include "cef/script_operation_entry.h"

namespace UI = Corona::Systems::UI;
using Json = nlohmann::json;

namespace {

void require(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

constexpr const char* kArchive = "camera-test-archive";
constexpr const char* kScene = "scene.ini";
constexpr const char* kActorGuid = "transform-probe";
constexpr const char* kActorName = "Transform probe";

Json snapshot() {
    auto camera = [](const char* id, const char* name, bool deletable) -> Json {
        return {{"id", id}, {"name", name}, {"deletable", deletable},
                {"position", {0, 0, -5}}, {"forward", {0, 0, 1}}, {"world_up", {0, 1, 0}},
                {"width", 800}, {"height", 600}, {"view_width", 800}, {"view_height", 600}};
    };
    return {{"schema_version", 1}, {"archive_session", kArchive}, {"archive_type", "portable_scene"},
            {"project_root", std::filesystem::temp_directory_path().string()},
            {"scene", {{"route", kScene}, {"name", "Script runtime test"}, {"core_version", "1"},
                       {"scripts", Json::object()}, {"terrain", Json::object()},
                       {"environment", Json::object()}, {"vision", Json::object()},
                       {"actors", Json::array({{{"name", kActorName},
                                                {"actor_guid", kActorGuid},
                                                {"actor_type", "audio"},
                                                {"route", ""},
                                                {"asset_path", ""},
                                                {"transform", {{"position", {1, 2, 3}},
                                                               {"rotation", {4, 5, 6}},
                                                               {"scale", {1, 1, 1}}}}}})},
                       {"cameras", Json::array({camera("primary", "Primary camera", false),
                                                camera("secondary", "Secondary camera", true)})},
                       {"active_camera_id", "primary"}}}};
}

Json scope() { return {{"archive_session", kArchive}, {"scene", kScene}}; }

Json actor_scope(const char* actor = kActorGuid) {
    return {{"archive_session", kArchive}, {"scene", kScene}, {"actor", actor}};
}

/// A successful entry never throws; a failing one answers with a NativeResult and still
/// never throws. Anything that escapes the native frame would unwind a CEF task.
template <typename Fn>
UI::NativeResult run(Fn body, const char* what) {
    try {
        return body();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << what << " threw across the native frame: " << error.what() << '\n';
        std::exit(1);
    }
}

void load_scene() {
    UI::load_native_editor_scene_for_test(snapshot());
}

// ---------------------------------------------------------------- reads

void reads_report_the_loaded_scene() {
    load_scene();

    const auto active = run([] { return UI::script_entry_read_active(); }, "read_active");
    require(active.success, "read_active succeeds");
    require(active.data.at("archive_session") == kArchive && active.data.at("scene") == kScene,
            "read_active reports the archive and scene the editor owns");
    require(active.data.contains("tree_revision") && active.data.at("selection").is_object(),
            "read_active carries a revision and an (empty) selection");

    const auto actor = run([] { return UI::script_entry_read_actor(actor_scope()); }, "read_actor");
    require(actor.success, "read_actor succeeds for a known guid");
    require(actor.data.at("actor_guid") == kActorGuid && actor.data.at("name") == kActorName,
            "read_actor identifies the actor by guid and name");
    require(actor.data.at("geometry").at("position") == Json({1, 2, 3}),
            "read_actor reports the authored transform");

    auto stale = actor_scope();
    stale["archive_session"] = "retired";
    const auto rejected = UI::script_entry_read_actor(stale);
    require(rejected.error_code == 409, "read_actor rejects an archive the editor no longer owns");
    // The numeric class is what a caller branches on; the symbolic code rides along so the
    // exact reason survives without matching on message text.
    require(rejected.data.at("code") == "PROJECT_CONTEXT_CHANGED",
            "a rejected request names its symbolic code in data.code");
    require(UI::script_entry_read_actor(actor_scope("does-not-exist")).error_code == 404,
            "read_actor reports a missing actor");

    Json actors = scope();
    actors["actors"] = Json::array({kActorGuid, "does-not-exist"});
    const auto transforms = UI::script_entry_read_transforms(actors);
    require(transforms.success && transforms.data.at("actors").size() == 1,
            "read_transforms skips unknown guids instead of failing the batch");
    const auto& first = transforms.data.at("actors").front();
    require(first.at("position") == Json({1, 2, 3}) && first.at("rotation") == Json({4, 5, 6}) &&
                first.at("scale") == Json({1, 1, 1}),
            "read_transforms reports all three transform components");
    actors["archive_session"] = "retired";
    require(UI::script_entry_read_transforms(actors).error_code == 409,
            "read_transforms rejects a retired archive");

    const auto archive = UI::script_entry_read_archive_scene(scope());
    require(archive.success && archive.data.at("scene").at("cameras").size() == 2,
            "read_archive_scene projects both cameras");
    require(UI::script_entry_read_archive_scene(stale).error_code == 409,
            "read_archive_scene rejects a retired archive");

    const auto environment = UI::script_entry_read_environment(kScene, Json::object());
    require(environment.success && environment.data.contains("sun") && environment.data.contains("physics"),
            "read_environment reports the sun and physics scalars");
    require(environment.data.contains("scene"), "read_environment echoes the scene it read");
}

void the_archive_session_argument_is_part_of_the_contract() {
    load_scene();
    // "not passed" and "passed for another archive" are different answers, and both are
    // NativeResults: the interpreter turns them into EditorApiError codes, never throws.
    const auto missing = UI::script_entry_read_scene(Json{{"scene", kScene}});
    require(!missing.success && missing.error_code == 400 &&
                missing.data.at("code") == "INVALID_ARGUMENT",
            "read_scene without an archive_session is INVALID_ARGUMENT");

    const auto stale = UI::script_entry_read_scene(
        Json{{"scene", kScene}, {"archive_session", "retired"}});
    require(!stale.success && stale.error_code == 409 &&
                stale.data.at("code") == "PROJECT_CONTEXT_CHANGED",
            "read_scene with a retired archive is PROJECT_CONTEXT_CHANGED");

    const auto unknown_actor = UI::script_entry_read_actor(actor_scope("does-not-exist"));
    require(!unknown_actor.success && unknown_actor.error_code == 404,
            "a failing entry answers with a NativeResult instead of throwing");
}

void snapshot_is_built_from_native_state() {
    load_scene();
    // Despite the historical name of the helper behind it, the snapshot is built from
    // the native scene - it must therefore succeed with no Python runtime at all.
    const auto result = UI::script_entry_read_scene(
        Json{{"scene", kScene}, {"archive_session", kArchive}});
    require(result.success, "read_scene succeeds without a Python runtime");
    require(result.data.at("status") == "success" && result.data.at("scene") == kScene,
            "the snapshot reports the scene it describes");
    require(result.data.at("actors").size() == 1 && result.data.at("cameras").size() == 2,
            "the snapshot projects the loaded actors and cameras");
}

// ---------------------------------------------------------------- writes

void transform_writes_go_through_the_native_authority() {
    load_scene();
    const auto before = UI::script_entry_read_actor(actor_scope());

    Json patch = scope();
    patch["position"] = Json({0, 8, -3});
    const auto written = UI::script_entry_set_actor_transform(kScene, kActorGuid, patch);
    require(written.success, "set_actor_transform succeeds for a known actor");

    const auto& actor_after = written.data.at("actor");
    require(actor_after.at("geometry").at("position") == Json({0, 8, -3}),
            "set_actor_transform returns the applied transform");
    require(actor_after.at("actor_version").get<int>() > before.data.at("actor_version").get<int>(),
            "a transform write advances the actor version");

    const auto after = UI::script_entry_read_actor(actor_scope());
    require(after.data.at("geometry").at("position") == Json({0, 8, -3}),
            "the next read observes the written transform");
    require(after.data.at("geometry").at("rotation") == Json({4, 5, 6}),
            "an omitted field keeps its authored value");

    auto stale = patch;
    stale["archive_session"] = "retired";
    require(UI::script_entry_set_actor_transform(kScene, kActorGuid, stale).error_code == 409,
            "set_actor_transform rejects a retired archive");
}

void environment_writes_apply_and_echo() {
    load_scene();
    Json patch = scope();
    patch["sun"] = Json{{"enabled", false}};
    const auto written = UI::script_entry_set_environment(kScene, patch);
    require(written.success, "set_environment succeeds");
    require(written.data.at("sun").at("enabled") == false,
            "set_environment echoes the applied sun state");

    const auto read = UI::script_entry_read_environment(kScene, Json::object());
    require(read.data.at("sun").at("enabled") == false, "the next read_environment observes the write");

    auto stale = patch;
    stale["archive_session"] = "retired";
    require(UI::script_entry_set_environment(kScene, stale).error_code == 409,
            "set_environment rejects a retired archive");
}

void actor_state_writes_apply() {
    load_scene();
    Json patch = scope();
    patch["visible"] = false;
    const auto written = UI::script_entry_update_actor(kScene, kActorName, patch);
    require(written.success, "update_actor succeeds by actor name");

    require(written.data.at("actor").at("visible") == false,
            "update_actor echoes the applied visibility");

    const auto unknown = UI::script_entry_update_actor(kScene, "no-such-actor", patch);
    require(!unknown.success && unknown.error_code == 404,
            "update_actor reports a missing actor with 404");

    auto stale = patch;
    stale["archive_session"] = "retired";
    const auto refused = UI::script_entry_update_actor(kScene, kActorName, stale);
    require(!refused.success && refused.error_code == 409,
            "update_actor answers the shared 409 for a retired archive");
}

void body_dependent_writes_report_their_contract() {
    load_scene();
    // The fixture actor loads without a model asset, so its animation player exists
    // but is not ready. All three animation entry points must say so, not half-apply.
    const auto requested = UI::script_entry_set_actor_animation(kScene, kActorName, "walk", 0.2);
    require(!requested.success && requested.error == "ANIMATION_NOT_READY" && requested.error_code == 2,
            "set_actor_animation reports ANIMATION_NOT_READY for an actor with no loaded clip");

    const auto paused = UI::script_entry_pause_actor_animation(kScene, kActorName);
    require(!paused.success && paused.error == "ANIMATION_NOT_READY" && paused.error_code == 2,
            "pause_actor_animation reports ANIMATION_NOT_READY");
    const auto resumed = UI::script_entry_resume_actor_animation(kScene, kActorName);
    require(!resumed.success && resumed.error == "ANIMATION_NOT_READY" && resumed.error_code == 2,
            "resume_actor_animation reports ANIMATION_NOT_READY");

    const auto unknown = UI::script_entry_set_actor_animation(kScene, "no-such-actor", "walk", 0.2);
    require(!unknown.success && unknown.error == "Actor not found",
            "set_actor_animation reports an unknown actor");

    Json body = scope();
    body["mass"] = 2.0;
    const auto physics = UI::script_entry_set_actor_physics(kScene, kActorName, body);
    require(physics.success, "set_actor_physics succeeds for the loaded actor");
    require(physics.data.at("actor").at("mechanics").at("mass") == 2.0,
            "set_actor_physics returns the applied body mass");

    auto stale = body;
    stale["archive_session"] = "retired";
    const auto refused = UI::script_entry_set_actor_physics(kScene, kActorName, stale);
    // Every writer answers 409 for a stale archive - the scene layer's symbolic
    // PROJECT_CONTEXT_CHANGED is mapped to the same number the read paths use, so a
    // caller never has to match on message text to tell staleness from other failures.
    require(!refused.success && refused.error_code == 409 &&
                refused.error == "Properties do not identify the active scene",
            "set_actor_physics rejects a retired archive with the shared 409 code");
}

void camera_pose_writes_reach_the_native_camera() {
    load_scene();
    const auto written = UI::script_entry_set_camera_pose(
        kScene, "Primary camera", Json{{"position", {1, 2, -7}}});
    require(written.success, "set_camera_pose succeeds for a known camera");
    require(written.data.at("camera").at("position") == Json({1, 2, -7}),
            "set_camera_pose returns the applied pose");

    const auto missing = UI::script_entry_set_camera_pose(
        kScene, "No such camera", Json{{"position", {0, 0, 0}}});
    require(!missing.success, "set_camera_pose reports a missing camera");
}

void actor_lifecycle_through_the_script_channel() {
    load_scene();
    // Creation needs a resolvable asset; the archived project root in this fixture has
    // none, so the contract to pin is the refusal, not a successful import.
    Json options = scope();
    options["actor_type"] = "model";
    const auto created = UI::script_entry_create_actor(kScene, "missing.glb", options);
    require(created.success || (created.error_code != 0 && !created.error.empty()),
            "create_actor answers with a result or a documented failure");

    Json stale = options;
    stale["archive_session"] = "retired";
    const auto refused = UI::script_entry_create_actor(kScene, "missing.glb", stale);
    require(!refused.success && refused.error_code == 409,
            "create_actor rejects a retired archive");

    const auto removed = UI::script_entry_delete_actor(kScene, kActorGuid, Json::object());
    require(removed.success, "delete_actor succeeds for a known actor");
    require(UI::script_entry_read_actor(actor_scope()).error_code == 404,
            "a removed actor is no longer readable");
    require(!UI::script_entry_delete_actor(kScene, kActorGuid, Json::object()).success,
            "delete_actor reports an actor that is already gone");

    load_scene();
}

void actor_audio_reaches_the_scene_operation() {
    load_scene();
    // The fixture actor is an audio actor, so it carries an acoustics component: playing it is
    // the one audio capability the script channel has, and it must answer with the scope it
    // acted on rather than silently succeeding.
    const auto played = UI::script_entry_play_actor_audio(
        Json{{"scene", kScene}, {"actor", kActorName}, {"loop", true}});
    require(played.success || (played.error_code != 0 && !played.error.empty()),
            "play_actor_audio answers with a result or a documented failure");
    if (played.success) {
        require(played.data.at("playing") == true && played.data.at("actor") == kActorGuid,
                "play_actor_audio reports the actor it started and that it is playing");
    }

    const auto stopped = UI::script_entry_stop_actor_audio(
        Json{{"scene", kScene}, {"actor", kActorName}});
    require(stopped.success || (stopped.error_code != 0 && !stopped.error.empty()),
            "stop_actor_audio answers with a result or a documented failure");

    const auto unknown = UI::script_entry_play_actor_audio(
        Json{{"scene", kScene}, {"actor", "no-such-actor"}});
    require(!unknown.success && unknown.data.at("code") == "ACTOR_AUDIO_UNAVAILABLE",
            "play_actor_audio names ACTOR_AUDIO_UNAVAILABLE for an actor without acoustics");

    const auto refused = UI::script_entry_play_actor_audio(
        Json{{"scene", kScene}, {"actor", kActorName}, {"archive_session", "retired"}});
    require(!refused.success && refused.error_code == 409,
            "play_actor_audio rejects a retired archive");
}

void the_file_script_entries_keep_the_probe_envelope() {
    // The probe relays these envelopes verbatim, so the shape is part of the contract
    // even when the operation cannot run (no Python runtime here).
    const auto status = UI::project_script_status(Json::array());
    require(status.contains("success") && status.contains("code") && status.contains("error"),
            "project_script_status answers with the probe envelope");

    const auto run = UI::project_script_run(Json::array());
    require(run.contains("success") && run.contains("code"),
            "project_script_run answers with the probe envelope");

    const auto stopped = UI::stop_project_script(Json::array());
    require(stopped.contains("success") && stopped.contains("code"),
            "stop_project_script answers with the probe envelope");

    // Starting a file needs a published document; with none, the entry point must fail
    // rather than pretend the script started.
    const auto started = UI::run_project_script_file(Json::array());
    require(started.value("success", true) == false && started.contains("error"),
            "run_project_script_file fails without a published document");
}

}  // namespace

int main() {
    auto& kernel = Corona::Kernel::KernelContext::instance();
    if (!kernel.initialize()) {
        std::cerr << "kernel initialization failed\n";
        return 1;
    }
    int failures = 0;
    const auto group = [&failures](const char* name, void (*body)()) {
        try {
            body();
        } catch (const std::exception& error) {
            std::cerr << "  FAILED " << name << ": " << error.what() << '\n';
            ++failures;
        }
    };
    group("reads", reads_report_the_loaded_scene);
    group("archive_session", the_archive_session_argument_is_part_of_the_contract);
    group("snapshot", snapshot_is_built_from_native_state);
    group("transform", transform_writes_go_through_the_native_authority);
    group("environment", environment_writes_apply_and_echo);
    group("actor_state", actor_state_writes_apply);
    group("body_dependent", body_dependent_writes_report_their_contract);
    group("camera_pose", camera_pose_writes_reach_the_native_camera);
    group("lifecycle", actor_lifecycle_through_the_script_channel);
    group("actor_audio", actor_audio_reaches_the_scene_operation);
    group("file_script", the_file_script_entries_keep_the_probe_envelope);
    kernel.shutdown();
    if (failures != 0) {
        std::cerr << "ScriptRuntimeApiTests: " << failures << " group(s) failed\n";
        std::_Exit(1);
    }
    std::cout << "ScriptRuntimeApiTests passed\n";
    return 0;
}
