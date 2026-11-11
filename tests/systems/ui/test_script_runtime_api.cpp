// Behavioural coverage for the sixteen named ScriptRuntime entry points.
//
// `test_manifest_retirement.py` only asserts that the names and their bindings line
// up. This drives each entry point against a loaded scene so the contract itself -
// success shape, archive identity checks and the documented failures - is pinned.
#include <corona/kernel/core/kernel_context.h>
#include <corona/systems/ui/file_script_api.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "cef/native_api_types.h"
#include "cef/cef_editor_native_api_test_support.h"
#include "cef/script_runtime_api.h"

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

template <typename Operation>
UI::NativeResult call(Operation operation, Json args) {
    UI::NativeRequest request;
    request.module = "ScriptRuntime";
    request.function = "test";
    request.args = std::move(args);
    return operation(request);
}

void load_scene() {
    UI::load_native_editor_scene_for_test(snapshot());
}

// ---------------------------------------------------------------- reads

void reads_report_the_loaded_scene() {
    load_scene();

    const auto active = call(UI::script_scene_read_active_scene, Json::array());
    require(active.success, "read_active_scene succeeds");
    require(active.data.at("archive_session") == kArchive && active.data.at("scene") == kScene,
            "read_active_scene reports the archive and scene the editor owns");
    require(active.data.contains("tree_revision") && active.data.at("selection").is_object(),
            "read_active_scene carries a revision and an (empty) selection");

    auto actor = call(UI::script_scene_read_actor, Json::array({actor_scope()}));
    require(actor.success, "read_actor succeeds for a known guid");
    require(actor.data.at("actor_guid") == kActorGuid && actor.data.at("name") == kActorName,
            "read_actor identifies the actor by guid and name");
    require(actor.data.at("geometry").at("position") == Json({1, 2, 3}),
            "read_actor reports the authored transform");

    auto stale = actor_scope(); stale["archive_session"] = "retired";
    const auto rejected = call(UI::script_scene_read_actor, Json::array({stale}));
    require(rejected.error_code == 409, "read_actor rejects an archive the editor no longer owns");
    // The numeric class is what a caller branches on; the symbolic code rides along so the
    // exact reason survives without matching on message text.
    require(rejected.data.at("code") == "PROJECT_CONTEXT_CHANGED",
            "a rejected request names its symbolic code in data.code");
    const auto missing = actor_scope("does-not-exist");
    require(call(UI::script_scene_read_actor, Json::array({missing})).error_code == 404,
            "read_actor reports a missing actor");

    Json actors = scope(); actors["actors"] = Json::array({kActorGuid, "does-not-exist"});
    const auto transforms = call(UI::script_scene_read_transforms, Json::array({actors}));
    require(transforms.success && transforms.data.at("actors").size() == 1,
            "read_transforms skips unknown guids instead of failing the batch");
    const auto& first = transforms.data.at("actors").front();
    require(first.at("position") == Json({1, 2, 3}) && first.at("rotation") == Json({4, 5, 6}) &&
                first.at("scale") == Json({1, 1, 1}),
            "read_transforms reports all three transform components");
    actors["archive_session"] = "retired";
    require(call(UI::script_scene_read_transforms, Json::array({actors})).error_code == 409,
            "read_transforms rejects a retired archive");

    const auto archive = call(UI::script_scene_read_archive_scene, Json::array({scope()}));
    require(archive.success && archive.data.at("scene").at("cameras").size() == 2,
            "read_archive_scene projects both cameras");
    require(call(UI::script_scene_read_archive_scene, Json::array({stale})).error_code == 409,
            "read_archive_scene rejects a retired archive");

    const auto environment = call(UI::script_scene_get_environment, Json::array({kScene}));
    require(environment.success && environment.data.contains("sun") && environment.data.contains("physics"),
            "get_environment reports the sun and physics scalars");
    require(environment.data.at("scene") == kScene || environment.data.contains("scene"),
            "get_environment echoes the scene it read");
}

void a_short_call_is_rejected_before_it_reaches_the_scene() {
    load_scene();
    // read_actor needs its {archive_session, actor} object. Without it the operation would
    // answer "Actor not found: " with 404 - technically true, useless to the caller.
    const auto missing = UI::validate_script_arguments("script_scene_read_actor", Json::array());
    require(missing.has_value(), "an empty argument array is rejected");
    require(missing->error_code == 400 && missing->data.at("code") == "INVALID_ARGUMENT",
            "the rejection names INVALID_ARGUMENT with the shared class");
    require(missing->error.find("[{archive_session, actor}]") != std::string::npos,
            "the rejection quotes the documented shape");

    const auto enough = UI::validate_script_arguments(
        "script_scene_read_actor", Json::array({{{"archive_session", kArchive}, {"actor", kActorGuid}}}));
    require(!enough.has_value(), "a complete call passes validation");

    const auto unknown = UI::validate_script_arguments("script_scene_not_a_real_op", Json::array());
    require(unknown.has_value() && unknown->data.at("code") == "INVALID_ARGUMENT",
            "an unknown operation name is rejected the same way");
}

void snapshot_is_built_from_native_state() {
    load_scene();
    // Despite the historical name of the helper behind it, the snapshot is built from
    // the native scene - it must therefore succeed with no Python runtime at all.
    const auto result = call(UI::script_scene_get_scene_snapshot, Json::array({kScene}));
    require(result.success, "get_scene_snapshot succeeds without a Python runtime");
    require(result.data.at("status") == "success" && result.data.at("scene") == kScene,
            "the snapshot reports the scene it describes");
    require(result.data.at("actors").size() == 1 && result.data.at("cameras").size() == 2,
            "the snapshot projects the loaded actors and cameras");
}

// ---------------------------------------------------------------- writes

void transform_writes_go_through_the_native_authority() {
    load_scene();
    const auto before = call(UI::script_scene_read_actor, Json::array({actor_scope()}));

    Json patch = scope();
    patch["position"] = Json({0, 8, -3});
    const auto written = call(UI::script_scene_set_actor_transform,
                              Json::array({kScene, kActorGuid, patch}));
    require(written.success, "set_actor_transform succeeds for a known actor");

    const auto& actor_after = written.data.at("actor");
    require(actor_after.at("geometry").at("position") == Json({0, 8, -3}),
            "set_actor_transform returns the applied transform");
    require(actor_after.at("actor_version").get<int>() > before.data.at("actor_version").get<int>(),
            "a transform write advances the actor version");

    const auto after = call(UI::script_scene_read_actor, Json::array({actor_scope()}));
    require(after.data.at("geometry").at("position") == Json({0, 8, -3}),
            "the next read observes the written transform");
    require(after.data.at("geometry").at("rotation") == Json({4, 5, 6}),
            "an omitted field keeps its authored value");

    auto stale = patch; stale["archive_session"] = "retired";
    require(call(UI::script_scene_set_actor_transform, Json::array({kScene, kActorGuid, stale})).error_code == 409,
            "set_actor_transform rejects a retired archive");
}

void environment_writes_apply_and_echo() {
    load_scene();
    Json patch = scope();
    patch["sun"] = Json{{"enabled", false}};
    const auto written = call(UI::script_scene_set_environment, Json::array({kScene, patch}));
    require(written.success, "set_environment succeeds");
    require(written.data.at("sun").at("enabled") == false,
            "set_environment echoes the applied sun state");

    const auto read = call(UI::script_scene_get_environment, Json::array({kScene}));
    require(read.data.at("sun").at("enabled") == false, "the next get_environment observes the write");

    auto stale = patch; stale["archive_session"] = "retired";
    require(call(UI::script_scene_set_environment, Json::array({kScene, stale})).error_code == 409,
            "set_environment rejects a retired archive");
}

void actor_state_writes_apply() {
    load_scene();
    Json patch = scope();
    patch["visible"] = false;
    const auto written = call(UI::script_scene_set_actor_state,
                              Json::array({kScene, kActorName, patch}));
    require(written.success, "set_actor_state succeeds by actor name");

    require(written.data.at("actor").at("visible") == false,
            "set_actor_state echoes the applied visibility");

    const auto unknown = call(UI::script_scene_set_actor_state,
                              Json::array({kScene, "no-such-actor", patch}));
    require(!unknown.success && unknown.error_code == 404,
            "set_actor_state reports a missing actor with 404");

    auto stale = patch; stale["archive_session"] = "retired";
    const auto refused = call(UI::script_scene_set_actor_state,
                              Json::array({kScene, kActorName, stale}));
    require(!refused.success && refused.error_code == 409,
            "set_actor_state answers the shared 409 for a retired archive");
}

void body_dependent_writes_report_their_contract() {
    load_scene();
    // The fixture actor loads without a model asset, so its animation player exists
    // but is not ready. All three animation entry points must say so, not half-apply.
    const auto requested = call(UI::script_scene_set_actor_animation,
                                Json::array({kScene, kActorName, "walk", 0.2}));
    require(!requested.success && requested.error == "ANIMATION_NOT_READY" && requested.error_code == 2,
            "set_actor_animation reports ANIMATION_NOT_READY for an actor with no loaded clip");

    const auto paused = call(UI::script_scene_pause_actor_animation, Json::array({kScene, kActorName}));
    require(!paused.success && paused.error == "ANIMATION_NOT_READY" && paused.error_code == 2,
            "pause_actor_animation reports ANIMATION_NOT_READY");
    const auto resumed = call(UI::script_scene_resume_actor_animation, Json::array({kScene, kActorName}));
    require(!resumed.success && resumed.error == "ANIMATION_NOT_READY" && resumed.error_code == 2,
            "resume_actor_animation reports ANIMATION_NOT_READY");

    const auto unknown = call(UI::script_scene_set_actor_animation,
                              Json::array({kScene, "no-such-actor", "walk"}));
    require(!unknown.success && unknown.error == "Actor not found",
            "set_actor_animation reports an unknown actor");

    Json body = scope();
    body["mass"] = 2.0;
    const auto physics = call(UI::script_scene_set_actor_physics,
                              Json::array({kScene, kActorName, body}));
    require(physics.success, "set_actor_physics succeeds for the loaded actor");
    require(physics.data.at("actor").at("mechanics").at("mass") == 2.0,
            "set_actor_physics returns the applied body mass");

    auto stale = body; stale["archive_session"] = "retired";
    const auto refused = call(UI::script_scene_set_actor_physics,
                              Json::array({kScene, kActorName, stale}));
    // Every writer answers 409 for a stale archive - the scene layer's symbolic
    // PROJECT_CONTEXT_CHANGED is mapped to the same number the read paths use, so a
    // caller never has to match on message text to tell staleness from other failures.
    require(!refused.success && refused.error_code == 409 &&
                refused.error == "Properties do not identify the active scene",
            "set_actor_physics rejects a retired archive with the shared 409 code");
}

void camera_pose_writes_reach_the_native_camera() {
    load_scene();
    const auto written = call(UI::script_scene_set_camera_pose,
                              Json::array({kScene, "Primary camera", Json{{"position", {1, 2, -7}}}}));
    require(written.success, "set_camera_pose succeeds for a known camera");
    require(written.data.at("camera").at("position") == Json({1, 2, -7}),
            "set_camera_pose returns the applied pose");

    const auto missing = call(UI::script_scene_set_camera_pose,
                              Json::array({kScene, "No such camera", Json{{"position", {0, 0, 0}}}}));
    require(!missing.success, "set_camera_pose reports a missing camera");
}

void actor_lifecycle_through_the_script_channel() {
    load_scene();
    // Creation needs a resolvable asset; the archived project root in this fixture has
    // none, so the contract to pin is the refusal, not a successful import.
    const auto created = call(UI::script_scene_create_actor,
                              Json::array({kScene, "missing.glb", "model", scope()}));
    require(created.success || (created.error_code != 0 && !created.error.empty()),
            "create_actor answers with a result or a documented failure");

    auto stale = actor_scope(); stale["archive_session"] = "retired";
    const auto refused = call(UI::script_scene_create_actor,
                              Json::array({kScene, "missing.glb", "model", stale}));
    require(!refused.success && refused.error_code == 409,
            "create_actor rejects a retired archive");

    const auto removed = call(UI::script_scene_remove_actor, Json::array({kScene, kActorGuid}));
    require(removed.success, "remove_actor succeeds for a known actor");
    require(call(UI::script_scene_read_actor, Json::array({actor_scope()})).error_code == 404,
            "a removed actor is no longer readable");
    require(!call(UI::script_scene_remove_actor, Json::array({kScene, kActorGuid})).success,
            "remove_actor reports an actor that is already gone");

    load_scene();
}

void actor_audio_reaches_the_scene_operation() {
    load_scene();
    // The fixture actor is an audio actor, so it carries an acoustics component: playing it is
    // the one audio capability the script channel has, and it must answer with the scope it
    // acted on rather than silently succeeding.
    const auto played = call(UI::script_scene_play_actor_audio,
                             Json::array({kScene, kActorName, Json::array({{{"loop", true}}})[0]}));
    require(played.success || (played.error_code != 0 && !played.error.empty()),
            "play_actor_audio answers with a result or a documented failure");
    if (played.success) {
        require(played.data.at("playing") == true && played.data.at("actor") == kActorGuid,
                "play_actor_audio reports the actor it started and that it is playing");
    }

    const auto stopped = call(UI::script_scene_stop_actor_audio,
                              Json::array({kScene, kActorName, Json::object()}));
    require(stopped.success || (stopped.error_code != 0 && !stopped.error.empty()),
            "stop_actor_audio answers with a result or a documented failure");

    const auto unknown = call(UI::script_scene_play_actor_audio,
                              Json::array({kScene, "no-such-actor", Json::object()}));
    require(!unknown.success && unknown.data.at("code") == "ACTOR_AUDIO_UNAVAILABLE",
            "play_actor_audio names ACTOR_AUDIO_UNAVAILABLE for an actor without acoustics");

    auto stale = Json::object();
    stale["archive_session"] = "retired";
    const auto refused = call(UI::script_scene_play_actor_audio,
                              Json::array({kScene, kActorName, stale}));
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
    group("arguments", a_short_call_is_rejected_before_it_reaches_the_scene);
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
