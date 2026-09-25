#include "cef/scene_folder.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <string_view>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace Corona::Systems::UI::SceneFolders;

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void expect(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void write_text(const fs::path& path, std::string_view value) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << value;
}

struct TempDir {
    fs::path path = fs::temp_directory_path() / "corona_scene_folder_tests";
    TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void portable_route_validation_rejects_escape_and_absolute_paths() {
    expect(is_valid_asset_route("Assets/Models/a/model.glb"), "portable asset route should be valid");
    expect(!is_valid_asset_route("../model.glb"), "parent traversal should be rejected");
    expect(!is_valid_asset_route("D:/model.glb"), "absolute Windows path should be rejected");
    expect(!is_valid_asset_route("Resource/model.glb"), "route outside Assets should be rejected");
    expect(!is_valid_asset_route(""), "empty route should be rejected");
}

void scene_folder_detection_requires_versioned_scene_ini() {
    TempDir temp;
    write_text(temp.path / "scene.ini",
               "[format]\n"
               "type = corona_scene_folder\n"
               "version = 1\n"
               "[scene]\n"
               "name = Portable\n");
    const auto layout = detect_scene_folder(temp.path);
    expect(layout.has_value(), "portable folder should be detected");
    expect(layout->root == temp.path, "folder root should be retained");
    expect(layout->scene_file == temp.path / "scene.ini", "scene file should resolve under root");
    expect(layout->scene_name == "Portable", "scene name should be parsed");
}

void glb_import_is_content_addressed_and_manifest_validates() {
    TempDir temp;
    const auto source = temp.path / "source" / "chair.glb";
    const auto scene_root = temp.path / "scene";
    write_text(source, "glb payload");

    SceneAssetStore store(scene_root);
    const auto first = store.import_model(source);
    const auto second = store.import_model(source);
    expect(first.ok(), "GLB import should succeed");
    expect(second.ok(), "repeated GLB import should succeed");
    expect(first.main_route == second.main_route, "same content should reuse the same route");
    expect(is_valid_asset_route(first.main_route), "imported route should be portable");
    expect(fs::is_regular_file(scene_root / fs::path(first.main_route)), "imported model should exist");
    expect(store.write_manifest(), "manifest should be written");
    expect(store.validate_manifest().empty(), "fresh manifest should validate");

    {
        const auto manifest_path = scene_root / "assets.manifest.json";
        std::ifstream input(manifest_path);
        auto manifest = nlohmann::json::parse(input);
        manifest["bundles"][0]["sha256"] = std::string(64, '0');
        std::ofstream output(manifest_path, std::ios::trunc);
        output << manifest.dump(2);
    }
    expect(!store.validate_manifest().empty(), "corrupted bundle hash should fail validation");
    expect(store.write_manifest(), "manifest should be restorable after bundle hash test");

    write_text(scene_root / fs::path(first.main_route), "tampered");
    expect(!store.validate_manifest().empty(), "tampered resource should fail validation");
}

void obj_import_copies_material_and_texture_dependencies() {
    TempDir temp;
    const auto source_dir = temp.path / "source";
    write_text(source_dir / "chair.obj", "mtllib materials/chair.mtl\nv 0 0 0\n");
    write_text(source_dir / "materials" / "chair.mtl", "map_Kd ../textures/wood.png\n");
    write_text(source_dir / "textures" / "wood.png", "texture");

    SceneAssetStore store(temp.path / "scene");
    const auto result = store.import_model(source_dir / "chair.obj");
    expect(result.ok(), "OBJ bundle import should succeed");
    expect(result.files.size() == 3, "OBJ, MTL and texture should be copied");
    for (const auto& file : result.files) {
        expect(fs::is_regular_file(temp.path / "scene" / fs::path(file.route)),
               "every bundle file should exist");
    }
    expect(store.write_manifest(), "OBJ manifest should be written");
    std::ifstream manifest_stream(temp.path / "scene" / "assets.manifest.json");
    const auto manifest = nlohmann::json::parse(manifest_stream);
    const auto& bundle = manifest.at("bundles").at(0);
    expect(bundle.at("dependencies").size() == 2,
           "manifest should record the main resource dependency routes");
    expect(store.validate_manifest().empty(), "OBJ manifest relationships should validate");
}

void missing_obj_dependency_fails_without_creating_bundle() {
    TempDir temp;
    const auto source = temp.path / "source" / "broken.obj";
    write_text(source, "mtllib missing.mtl\nv 0 0 0\n");

    SceneAssetStore store(temp.path / "scene");
    const auto result = store.import_model(source);
    expect(!result.ok(), "missing dependency should fail import");
    expect(!result.diagnostics.empty(), "failure should report diagnostics");
    expect(!fs::exists(temp.path / "scene" / "Assets" / "Models"),
           "failed import should not leave a bundle");
}

void legacy_project_migrates_to_transactional_scene_folder() {
    TempDir temp;
    const auto legacy = temp.path / "legacy";
    const auto target = temp.path / "Portable";
    write_text(legacy / "project.ini",
               "[Project]\nname = Legacy\nmode = 3d\nentrance_scene = Scene/default.scene\n");
    write_text(legacy / "Scene" / "default.scene",
               "[base]\nname = Migrated\n"
               "[sun]\nsun_direction = 1, 2, 3\nenabled = true\n"
               "[actors]\n"
               "chair.actor_type = model\n"
               "chair.name = Chair\n"
               "chair.route = Resource/chair.glb\n"
               "chair.material.texture = Resource/chair.png\n"
               "chair.geometry.position = 1, 2, 3\n"
               "[scripts]\npath = Scripts/scene_script.py\n");
    write_text(legacy / "Resource" / "chair.glb", "model");
    write_text(legacy / "Resource" / "chair.png", "texture");
    write_text(legacy / "Scripts" / "scene_script.py", "print('portable')\n");
    const auto original_scene = sha256_file(legacy / "Scene" / "default.scene");

    LegacyMigrationRequest request{legacy / "project.ini", target, "Portable"};
    const auto result = migrate_legacy_scene(request);
    expect(result.ok(), "legacy project migration should succeed");
    expect(result.root == target, "migration should return the target root");
    expect(fs::is_regular_file(target / "scene.ini"), "migration should write scene.ini");
    expect(fs::is_regular_file(target / "assets.manifest.json"), "migration should write manifest");
    expect(detect_scene_folder(target).has_value(), "migrated target should be a portable scene");
    expect(sha256_file(legacy / "Scene" / "default.scene") == original_scene,
           "migration should not modify the legacy scene");

    auto ini = std::ifstream(target / "scene.ini");
    const std::string text((std::istreambuf_iterator<char>(ini)), std::istreambuf_iterator<char>());
    expect(text.find("\nmode =") == std::string::npos, "portable scene must not persist mode");
    expect(text.find("entrance_scene") == std::string::npos,
           "portable scene must not persist entrance_scene");
    expect(text.find("chair.route = Assets/Models/") != std::string::npos,
           "actor route should be rewritten into Assets");
    expect(text.find("chair.material.texture = Assets/Images/") != std::string::npos,
           "actor material texture should be rewritten into Assets");
    expect(text.find("path = Assets/Scripts/") != std::string::npos,
           "script route should be rewritten into Assets");
}

void legacy_template_script_placeholder_is_ignored_when_missing() {
    TempDir temp;
    const auto legacy = temp.path / "legacy";
    const auto target = temp.path / "Portable";
    write_text(legacy / "project.ini",
               "[Project]\nname = Legacy\nentrance_scene = Scene/default.scene\n");
    write_text(legacy / "Scene" / "default.scene",
               "[base]\nname = Legacy\n"
               "[actors]\n"
               "[scripts]\npath = Scripts/scene_script.py\n");

    const auto result = migrate_legacy_scene({legacy / "project.ini", target, "Portable"});
    expect(result.ok(), "missing legacy template script should not block migration");
    auto ini = std::ifstream(target / "scene.ini");
    const std::string text((std::istreambuf_iterator<char>(ini)), std::istreambuf_iterator<char>());
    expect(text.find("path = Assets/Scripts/") == std::string::npos,
           "missing legacy template script must not be copied into Assets");
}

void missing_legacy_asset_aborts_without_target_directory() {
    TempDir temp;
    const auto legacy = temp.path / "legacy";
    const auto target = temp.path / "Portable";
    write_text(legacy / "project.ini",
               "[Project]\nname = Legacy\nentrance_scene = Scene/default.scene\n");
    write_text(legacy / "Scene" / "default.scene",
               "[base]\nname = Broken\n"
               "[actors]\nchair.actor_type = model\nchair.route = Resource/missing.glb\n");

    const auto result = migrate_legacy_scene({legacy / "project.ini", target, "Portable"});
    expect(!result.ok(), "migration with a missing model should fail");
    expect(!result.diagnostics.empty(), "migration failure should list diagnostics");
    expect(!fs::exists(target), "failed migration should not leave a target directory");
    expect(fs::is_regular_file(legacy / "project.ini"), "legacy source should remain intact");
}

void new_scene_folder_has_no_mode_and_an_empty_valid_manifest() {
    TempDir temp;
    const auto target = temp.path / "NewScene";
    const auto created = create_scene_folder(target, "New Scene");
    expect(created.has_value(), "new portable scene should be created");
    expect(detect_scene_folder(target).has_value(), "new portable scene should be detectable");
    SceneAssetStore store(target);
    expect(store.validate_manifest().empty(), "new scene manifest should validate");
    auto stream = std::ifstream(target / "scene.ini");
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    expect(text.find("\nmode =") == std::string::npos, "new scene must not contain mode");
    expect(text.find("[actors]") != std::string::npos, "new scene should contain actors section");
}

void reopening_asset_store_preserves_existing_manifest_bundles() {
    TempDir temp;
    const auto scene = temp.path / "scene";
    write_text(temp.path / "one.glb", "one");
    write_text(temp.path / "two.glb", "two");
    {
        SceneAssetStore store(scene);
        expect(store.import_model(temp.path / "one.glb").ok(), "first model import should succeed");
        expect(store.write_manifest(), "first manifest should be written");
    }
    {
        SceneAssetStore store(scene);
        expect(store.import_model(temp.path / "two.glb").ok(), "second model import should succeed");
        expect(store.write_manifest(), "updated manifest should be written");
    }
    const auto manifest = nlohmann::json::parse(std::ifstream(scene / "assets.manifest.json"));
    expect(manifest["bundles"].size() == 2, "reopened store should retain the first bundle");
}

void corrupted_manifest_blocks_import_and_writeback() {
    TempDir temp;
    const auto scene = temp.path / "scene";
    write_text(scene / "assets.manifest.json", "{ definitely not json");
    write_text(temp.path / "model.glb", "model");
    const auto original = sha256_file(scene / "assets.manifest.json");

    SceneAssetStore store(scene);
    const auto imported = store.import_model(temp.path / "model.glb");
    expect(!imported.ok(), "a corrupt manifest must block importing new assets");
    expect(!imported.diagnostics.empty() && imported.diagnostics.front().code == "invalid_manifest",
           "a corrupt manifest should report an invalid_manifest diagnostic");
    expect(!store.write_manifest(), "a corrupt manifest must not be overwritten");
    expect(sha256_file(scene / "assets.manifest.json") == original,
           "blocked import must preserve the corrupt manifest for recovery");
}

void unsupported_manifest_version_blocks_import_and_writeback() {
    TempDir temp;
    const auto scene = temp.path / "scene";
    write_text(scene / "assets.manifest.json",
               R"({"format":"corona_scene_assets","version":2,"bundles":[]})");
    write_text(temp.path / "model.glb", "model");
    SceneAssetStore store(scene);
    const auto imported = store.import_model(temp.path / "model.glb");
    expect(!imported.ok(), "unsupported manifest version must block asset import");
    expect(!store.write_manifest(), "unsupported manifest version must not be overwritten");
}

void missing_manifest_in_existing_portable_scene_blocks_import() {
    TempDir temp;
    const auto scene = temp.path / "scene";
    expect(create_scene_folder(scene, "Missing manifest").has_value(), "portable scene should be created");
    fs::remove(scene / "assets.manifest.json");
    write_text(temp.path / "model.glb", "model");
    SceneAssetStore store(scene);
    expect(!store.import_model(temp.path / "model.glb").ok(),
           "missing manifest in an existing portable scene must block import");
    expect(!store.write_manifest(), "missing portable manifest must not be silently recreated");
}

void gltf_import_copies_external_buffers_and_images() {
    TempDir temp;
    const auto source = temp.path / "source";
    write_text(source / "scene.gltf",
               R"({"asset":{"version":"2.0"},"buffers":[{"uri":"mesh.bin"}],"images":[{"uri":"textures/albedo.png"}]})");
    write_text(source / "mesh.bin", "buffer");
    write_text(source / "textures" / "albedo.png", "image");
    SceneAssetStore store(temp.path / "portable");
    const auto result = store.import_model(source / "scene.gltf");
    expect(result.ok(), "glTF import should succeed");
    expect(result.files.size() == 3, "glTF should include buffer and image dependencies");
}

void gltf_import_decodes_percent_encoded_local_uris_and_rejects_remote_uris() {
    TempDir temp;
    const auto source = temp.path / "source";
    write_text(source / "scene.gltf",
               R"({"asset":{"version":"2.0"},"buffers":[{"uri":"mesh%20data.bin"}]})");
    write_text(source / "mesh data.bin", "buffer");
    SceneAssetStore store(temp.path / "portable");
    const auto imported = store.import_model(source / "scene.gltf");
    expect(imported.ok() && imported.files.size() == 2,
           "glTF percent-encoded local URI should resolve to the decoded file name");

    write_text(source / "remote.gltf",
               R"({"asset":{"version":"2.0"},"buffers":[{"uri":"https://example.test/mesh.bin"}]})");
    const auto remote = store.import_model(source / "remote.gltf");
    expect(!remote.ok(), "remote glTF dependencies must be rejected for portable scenes");
    expect(!remote.diagnostics.empty() && remote.diagnostics.front().code == "remote_dependency",
           "remote glTF dependency should have a deterministic diagnostic");
}

void obj_import_supports_multiple_material_libraries_on_one_line() {
    TempDir temp;
    const auto source = temp.path / "source";
    write_text(source / "scene.obj", "mtllib first.mtl second.mtl\nv 0 0 0\n");
    write_text(source / "first.mtl", "map_Kd first.png\n");
    write_text(source / "second.mtl", "map_Kd second.png\n");
    write_text(source / "first.png", "first");
    write_text(source / "second.png", "second");
    SceneAssetStore store(temp.path / "portable");
    const auto imported = store.import_model(source / "scene.obj");
    expect(imported.ok(), "OBJ with multiple mtllib entries should import");
    expect(imported.files.size() == 5, "both material libraries and textures should be archived");
}

void unsupported_model_extension_is_rejected() {
    TempDir temp;
    write_text(temp.path / "payload.txt", "not a model");
    SceneAssetStore store(temp.path / "portable");
    const auto imported = store.import_model(temp.path / "payload.txt");
    expect(!imported.ok(), "unsupported model extensions must not be archived as models");
    expect(!imported.diagnostics.empty() && imported.diagnostics.front().code == "unsupported_model",
           "unsupported models should return a deterministic diagnostic");
}

void actor_import_rewrites_model_path_into_portable_assets() {
    TempDir temp;
    const auto source = temp.path / "source";
    write_text(source / "chair.glb", "model");
    write_text(source / "chair.actor", "[base]\nname = Chair\npath = chair.glb\n");
    SceneAssetStore store(temp.path / "portable");
    const auto result = store.import_actor(source / "chair.actor", source / "chair.glb");
    expect(result.ok(), "actor import should succeed");
    expect(result.main_route.find("Assets/Actors/") == 0,
           "actor descriptor should be stored in Assets/Actors");
    auto stream = std::ifstream(temp.path / "portable" / fs::path(result.main_route));
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    expect(text.find("path = Assets/Models/") != std::string::npos,
           "portable actor should reference its imported model");
    expect(result.dependencies.size() == 1 && result.dependencies.front().find("Assets/Models/") == 0,
           "actor bundle should record its model bundle dependency");
    expect(store.write_manifest(), "actor manifest should be written");
    expect(store.validate_manifest().empty(), "actor manifest should validate");
}

void legacy_actor_model_path_is_resolved_beside_actor_before_project_root() {
    TempDir temp;
    const auto legacy = temp.path / "legacy";
    const auto target = temp.path / "Portable";
    write_text(legacy / "project.ini",
               "[Project]\nname = Legacy\nentrance_scene = Scene/default.scene\n");
    write_text(legacy / "Scene" / "default.scene",
               "[base]\nname = Actor migration\n[actors]\n"
               "chair.actor_type = actor\nchair.route = Actors/chair.actor\n");
    write_text(legacy / "Actors" / "chair.actor", "[base]\nname = Chair\npath = models/chair.glb\n");
    write_text(legacy / "Actors" / "models" / "chair.glb", "model");

    const auto result = migrate_legacy_scene({legacy / "project.ini", target, "Portable"});
    expect(result.ok(), "legacy actor model route should resolve relative to the actor descriptor");
}

void manifest_rejects_unlisted_but_existing_asset_route() {
    TempDir temp;
    const auto scene = temp.path / "scene";
    write_text(scene / "Assets" / "Models" / "manual.glb", "unlisted");
    SceneAssetStore store(scene);
    expect(store.write_manifest(), "empty manifest should be written");
    expect(!store.contains_route("Assets/Models/manual.glb"),
           "an existing file outside the manifest must not be trusted");
}

void dae_import_collects_declared_external_texture_and_rejects_missing_one() {
    TempDir temp;
    const auto source = temp.path / "source";
    write_text(source / "scene.dae", "<COLLADA><init_from>textures/albedo.png</init_from></COLLADA>");
    write_text(source / "textures" / "albedo.png", "image");
    SceneAssetStore store(temp.path / "portable");
    const auto imported = store.import_model(source / "scene.dae");
    expect(imported.ok(), "DAE external texture should be collected");
    expect(imported.files.size() == 2, "DAE bundle should contain its declared texture");

    write_text(source / "broken.dae", "<COLLADA><init_from>textures/missing.png</init_from></COLLADA>");
    const auto broken = store.import_model(source / "broken.dae");
    expect(!broken.ok(), "DAE with missing declared texture should fail");
}

// 真实 COLLADA（如 Mixamo 导出）里 <init_from> 出现在两个地方：<library_images>
// 中是文件路径，<profile_COMMON><surface> 中只是 image 的 id 引用。早先的实现不
// 分场合全文搜索，把 id "file1-image" 当文件名拼成 <模型目录>/file1-image，导致
// 整个模型以 missing_dependency 被拒。
void dae_import_ignores_surface_image_id_references() {
    TempDir temp;
    const auto source = temp.path / "source";
    write_text(source / "vampire.dae",
               "<COLLADA>\n"
               "  <library_images>\n"
               "    <image id=\"file1-image\" name=\"file1\">\n"
               "      <init_from>textures/diffuse.png</init_from>\n"
               "    </image>\n"
               "  </library_images>\n"
               "  <library_effects><effect><profile_COMMON>\n"
               "    <newparam sid=\"file1-surface\"><surface type=\"2D\">\n"
               "      <init_from>file1-image</init_from>\n"
               "    </surface></newparam>\n"
               "  </profile_COMMON></effect></library_effects>\n"
               "</COLLADA>\n");
    write_text(source / "textures" / "diffuse.png", "image");

    SceneAssetStore store(temp.path / "portable");
    const auto imported = store.import_model(source / "vampire.dae");
    expect(imported.ok(), "surface id reference must not fail the import");
    expect(imported.files.size() == 2, "only the DAE and its real texture should be bundled");
    for (const auto& file : imported.files) {
        expect(file.route.find("file1-image") == std::string::npos,
               "an image id must never be treated as a file dependency");
    }
}

void portable_scene_reopens_after_copy_to_another_root() {
    TempDir temp;
    const auto original = temp.path / "portable";
    expect(create_scene_folder(original, "Movable").has_value(), "portable scene should be created");
    write_text(temp.path / "source" / "model.glb", "portable model");
    SceneAssetStore original_store(original);
    expect(original_store.import_model(temp.path / "source" / "model.glb").ok(),
           "portable model should import before moving");
    expect(original_store.write_manifest(), "portable manifest should be written before moving");

    const auto moved = fs::current_path() / "corona_scene_folder_moved_test";
    std::error_code ec;
    fs::remove_all(moved, ec);
    fs::copy(original, moved, fs::copy_options::recursive);
    expect(detect_scene_folder(moved).has_value(), "copied scene should be detected at its new root");
    SceneAssetStore moved_store(moved);
    expect(moved_store.validate_manifest().empty(), "copied scene assets should validate at the new root");
    fs::remove_all(moved, ec);
}

void portable_scene_validation_checks_every_persisted_resource_field() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Validation").has_value(), "portable scene should be created");
    write_text(temp.path / "model.glb", "model");
    SceneAssetStore assets(scene);
    const auto model = assets.import_model(temp.path / "model.glb");
    expect(model.ok() && assets.write_manifest(), "model fixture should be archived");
    write_text(scene / "scene.ini",
               "[format]\ntype = corona_scene_folder\nversion = 1\n"
               "[scene]\nname = Validation\n"
               "[actors]\nchair.route = " + model.main_route +
                   "\nchair.material.texture = C:/external.png\n"
               "[scripts]\npath = ../outside.py\n"
               "[terrain]\npath = Resource/terrain.bin\n"
               "[vision_document]\nasset_root = C:/vision\n");

    const auto result = validate_portable_scene(scene, true);
    expect(!result.ok(), "unsafe non-model resource fields must fail portable validation");
    expect(result.asset_count == 1 && result.total_bytes == 5,
           "portable validation should report manifest asset statistics");
    std::set<std::string> fields;
    for (const auto& diagnostic : result.diagnostics) fields.insert(diagnostic.field);
    expect(fields.contains("actors.chair.material.texture"), "material texture should be validated");
    expect(fields.contains("scripts.path"), "script path should be validated");
    expect(fields.contains("terrain.path"), "terrain path should be validated");
    expect(fields.contains("vision_document.asset_root"), "Vision asset root should be validated");
}

void portable_scene_validation_decodes_embedded_vision_resource_paths() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Vision validation").has_value(), "portable scene should be created");
    write_text(scene / "scene.ini",
               "[format]\ntype = corona_scene_folder\nversion = 1\n"
               "[scene]\nname = Vision validation\n"
               "[vision]\nstorage = embedded\n"
               "[vision_document]\nversion = 1\nencoding = zlib_base64_json\nasset_root = Assets\n"
               "data = eAEBUQCu/3sic2hhcGUiOnsiZm4iOiJDOi9leHRlcm5hbC5nbGIifSwiaW1hZ2UiOnsidGV4dHVyZSI6IkFzc2V0cy9JbWFnZXMvYWJjL29rLnBuZyJ9fVljG8k=\n");
    const auto validation = validate_portable_scene(scene, true);
    expect(!validation.ok(), "absolute path inside embedded Vision data must fail validation");
    expect(std::any_of(validation.diagnostics.begin(), validation.diagnostics.end(), [](const auto& item) {
               return item.field == "vision_document.data.shape.fn";
           }),
           "Vision diagnostic should identify the exact embedded field");
}

void scene_document_store_commits_multiple_sections_in_one_snapshot() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Atomic").has_value(), "portable scene should be created");
    SceneDocumentStore document(scene);
    std::map<std::string, std::vector<std::string>> sections;
    sections["sun"] = {"[sun]", "enabled = false", "sun_direction = 0, 1, 0"};
    sections["grid"] = {"[grid]", "enabled = false"};
    sections["camera"] = {"[camera]", "count = 0"};
    std::vector<Diagnostic> diagnostics;
    expect(document.replace_sections(sections, diagnostics), "multi-section transaction should commit");
    expect(diagnostics.empty(), "successful transaction should have no diagnostics");
    auto text = std::ifstream(scene / "scene.ini");
    const std::string saved((std::istreambuf_iterator<char>(text)), std::istreambuf_iterator<char>());
    expect(saved.find("enabled = false") != std::string::npos, "updated sections should be persisted");
    expect(saved.find("[format]") != std::string::npos, "unmodified format section should be preserved");
}

void scene_document_store_recovers_interrupted_transaction() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Recovery").has_value(), "portable scene should be created");
    const auto original_hash = sha256_file(scene / "scene.ini");
    fs::copy_file(scene / "scene.ini", scene / ".scene.ini.backup", fs::copy_options::overwrite_existing);
    write_text(scene / ".scene-save.transaction", "scene.ini\n");
    write_text(scene / "scene.ini", "truncated");

    SceneDocumentStore recovered(scene);
    expect(sha256_file(scene / "scene.ini") == original_hash,
           "opening a document store should roll back an interrupted transaction");
    expect(!fs::exists(scene / ".scene-save.transaction"), "recovery marker should be removed");
    expect(!fs::exists(scene / ".scene.ini.backup"), "recovery backup should be removed");
}

void scene_document_store_reports_unrecoverable_transaction_without_deleting_marker() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Broken recovery").has_value(),
           "portable scene should be created");
    write_text(scene / ".scene-save.transaction", "scene.ini\n");

    const auto validation = validate_portable_scene(scene, true);
    expect(!validation.ok(), "a transaction without its backup must block opening");
    expect(!validation.diagnostics.empty() &&
               validation.diagnostics.front().code == "scene_recovery_failed",
           "failed recovery should return a deterministic diagnostic");
    expect(fs::is_regular_file(scene / ".scene-save.transaction"),
           "failed recovery must preserve its marker for manual repair");
}

void scene_document_store_can_remove_obsolete_sections() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Remove").has_value(), "portable scene should be created");
    write_text(scene / "scene.ini",
               "[format]\ntype = corona_scene_folder\nversion = 1\n"
               "[scene]\nname = Remove\n[vision]\nstorage = project_sidecar\n[actors]\n");
    SceneDocumentStore document(scene);
    std::vector<Diagnostic> diagnostics;
    expect(document.replace_sections({{"vision", {}}}, diagnostics),
           "empty replacement should remove an obsolete section");
    auto input = std::ifstream(scene / "scene.ini");
    const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    expect(saved.find("[vision]") == std::string::npos, "removed section must not remain on disk");
    expect(saved.find("[actors]") != std::string::npos, "following sections must remain intact");
}

void concurrent_section_updates_do_not_lose_each_other() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Concurrent").has_value(), "portable scene should be created");
    const auto update = [&](std::string section, std::string value) {
        SceneDocumentStore store(scene);
        std::vector<Diagnostic> diagnostics;
        expect(store.replace_sections({{section, {"[" + section + "]", "value = " + value}}}, diagnostics),
               "concurrent section update should commit");
    };
    std::thread one(update, "one", "1");
    std::thread two(update, "two", "2");
    std::thread three(update, "three", "3");
    one.join();
    two.join();
    three.join();
    auto input = std::ifstream(scene / "scene.ini");
    const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    expect(saved.find("[one]") != std::string::npos && saved.find("[two]") != std::string::npos &&
               saved.find("[three]") != std::string::npos,
           "all concurrent section updates should survive");
}

void cleanup_only_removes_unreferenced_bundles_and_supports_dry_run() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    expect(create_scene_folder(scene, "Cleanup").has_value(), "portable scene should be created");
    write_text(temp.path / "used.glb", "used");
    write_text(temp.path / "unused.glb", "unused");
    SceneAssetStore assets(scene);
    const auto used = assets.import_model(temp.path / "used.glb");
    const auto unused = assets.import_model(temp.path / "unused.glb");
    expect(used.ok() && unused.ok() && assets.write_manifest(), "cleanup fixtures should be archived");
    write_text(scene / "scene.ini",
               "[format]\ntype = corona_scene_folder\nversion = 1\n"
               "[scene]\nname = Cleanup\n[actors]\nitem.route = " + used.main_route + "\n");

    const auto preview = cleanup_portable_scene_assets(scene, true);
    expect(preview.ok() && preview.removed_bundles == 1, "dry run should identify one unused bundle");
    expect(fs::is_regular_file(scene / fs::path(unused.main_route)),
           "dry run must not remove the unused resource");

    const auto cleaned = cleanup_portable_scene_assets(scene, false);
    expect(cleaned.ok() && cleaned.removed_bundles == 1, "cleanup should remove one unused bundle");
    expect(fs::is_regular_file(scene / fs::path(used.main_route)), "referenced bundle must remain");
    expect(!fs::exists(scene / fs::path(unused.main_route)), "unused bundle should be removed");
    SceneAssetStore reopened(scene);
    expect(reopened.validate_manifest().empty(), "cleaned manifest should remain valid");
}

void generic_audio_import_uses_portable_audio_category() {
    TempDir temp;
    write_text(temp.path / "sound.wav", "audio");
    SceneAssetStore assets(temp.path / "portable");
    const auto imported = assets.import_file(temp.path / "sound.wav", "Audio");
    expect(imported.ok(), "audio file should be accepted by the generic asset importer");
    expect(imported.main_route.find("Assets/Audio/") == 0, "audio route should use Assets/Audio");
}

void legacy_audio_actor_migrates_into_portable_audio_category() {
    TempDir temp;
    const auto legacy = temp.path / "legacy";
    const auto target = temp.path / "portable";
    write_text(legacy / "project.ini",
               "[Project]\nentrance_scene = Scene/default.scene\n");
    write_text(legacy / "Scene" / "default.scene",
               "[base]\nname = Audio migration\n[actors]\n"
               "music.actor_type = audio\nmusic.route = media/theme.wav\n");
    write_text(legacy / "media" / "theme.wav", "audio");

    const auto migrated = migrate_legacy_scene({legacy / "project.ini", target, ""});
    expect(migrated.ok(), "legacy audio actor should migrate");
    auto input = std::ifstream(target / "scene.ini");
    const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    expect(saved.find("music.route = Assets/Audio/") != std::string::npos,
           "migrated audio actor should use Assets/Audio");
}

void existing_bundle_directory_with_wrong_content_is_rejected() {
    TempDir temp;
    const auto scene = temp.path / "portable";
    write_text(temp.path / "model.glb", "original");
    SceneAssetStore assets(scene);
    const auto first = assets.import_model(temp.path / "model.glb");
    expect(first.ok(), "first bundle import should succeed");
    write_text(scene / fs::path(first.main_route), "different content");
    const auto second = assets.import_model(temp.path / "model.glb");
    expect(!second.ok(), "an occupied hash-prefix directory with different content must be rejected");
    expect(!second.diagnostics.empty() && second.diagnostics.front().code == "bundle_collision",
           "bundle directory mismatch should report bundle_collision");
}

void legacy_ini_sections_are_read_case_insensitively() {
    TempDir temp;
    const auto legacy = temp.path / "legacy";
    const auto target = temp.path / "Portable";
    write_text(legacy / "project.ini",
               "[project]\nname = Legacy\nentrance_scene = Content/main.scene\n");
    write_text(legacy / "Content" / "main.scene",
               "[Base]\nname = Case migration\n[Actors]\n"
               "chair.actor_type = model\nchair.route = Resource/chair.glb\n");
    write_text(legacy / "Resource" / "chair.glb", "model");
    const auto migrated = migrate_legacy_scene({legacy / "project.ini", target, ""});
    expect(migrated.ok(), "legacy INI section names should be matched without case sensitivity");
    auto input = std::ifstream(target / "scene.ini");
    const std::string saved((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    expect(saved.find("chair.route = Assets/Models/") != std::string::npos,
           "case-insensitive actors section should still migrate its model");
}

void fbx_import_archives_only_loader_declared_textures() {
    TempDir temp;
    const auto fixture = fs::path(CORONA_SOURCE_DIR) / "assets" / "wolf" / "fbx";
    const auto source = temp.path / "source";
    fs::create_directories(source / "textures");
    fs::copy_file(fixture / "Wolf.fbx", source / "Wolf.fbx");
    for (const auto& texture : fs::directory_iterator(fixture / "textures")) {
        if (texture.is_regular_file()) {
            fs::copy_file(texture.path(), source / "textures" / texture.path().filename());
        }
    }
    write_text(source / "unrelated.png", "must not be archived");

    SceneAssetStore assets(temp.path / "portable");
    const auto imported = assets.import_model(source / "Wolf.fbx");
    expect(imported.ok(), "valid FBX fixture should import");
    expect(std::none_of(imported.files.begin(), imported.files.end(), [](const auto& file) {
               return file.source.filename() == "unrelated.png";
           }),
           "FBX import must not recursively archive unrelated sidecar files");
}

void usd_remote_dependency_is_rejected() {
    TempDir temp;
    write_text(temp.path / "remote.usda", "def Xform \"Root\" { asset source = @https://example.test/a.usd@ }\n");
    SceneAssetStore assets(temp.path / "portable");
    const auto imported = assets.import_model(temp.path / "remote.usda");
    expect(!imported.ok(), "remote USD dependency must be rejected");
    expect(!imported.diagnostics.empty() && imported.diagnostics.front().code == "remote_dependency",
           "remote USD dependency should report a deterministic diagnostic");
}

}  // namespace

int main() {
    portable_route_validation_rejects_escape_and_absolute_paths();
    scene_folder_detection_requires_versioned_scene_ini();
    glb_import_is_content_addressed_and_manifest_validates();
    obj_import_copies_material_and_texture_dependencies();
    missing_obj_dependency_fails_without_creating_bundle();
    legacy_project_migrates_to_transactional_scene_folder();
    legacy_template_script_placeholder_is_ignored_when_missing();
    missing_legacy_asset_aborts_without_target_directory();
    new_scene_folder_has_no_mode_and_an_empty_valid_manifest();
    reopening_asset_store_preserves_existing_manifest_bundles();
    corrupted_manifest_blocks_import_and_writeback();
    unsupported_manifest_version_blocks_import_and_writeback();
    missing_manifest_in_existing_portable_scene_blocks_import();
    gltf_import_copies_external_buffers_and_images();
    gltf_import_decodes_percent_encoded_local_uris_and_rejects_remote_uris();
    obj_import_supports_multiple_material_libraries_on_one_line();
    unsupported_model_extension_is_rejected();
    actor_import_rewrites_model_path_into_portable_assets();
    legacy_actor_model_path_is_resolved_beside_actor_before_project_root();
    manifest_rejects_unlisted_but_existing_asset_route();
    dae_import_collects_declared_external_texture_and_rejects_missing_one();
    dae_import_ignores_surface_image_id_references();
    portable_scene_reopens_after_copy_to_another_root();
    portable_scene_validation_checks_every_persisted_resource_field();
    portable_scene_validation_decodes_embedded_vision_resource_paths();
    scene_document_store_commits_multiple_sections_in_one_snapshot();
    scene_document_store_recovers_interrupted_transaction();
    scene_document_store_reports_unrecoverable_transaction_without_deleting_marker();
    scene_document_store_can_remove_obsolete_sections();
    concurrent_section_updates_do_not_lose_each_other();
    cleanup_only_removes_unreferenced_bundles_and_supports_dry_run();
    generic_audio_import_uses_portable_audio_category();
    legacy_audio_actor_migrates_into_portable_audio_category();
    existing_bundle_directory_with_wrong_content_is_rejected();
    legacy_ini_sections_are_read_case_insensitively();
    fbx_import_archives_only_loader_declared_textures();
    usd_remote_dependency_is_rejected();
    std::cout << "All scene folder tests passed\n";
    return 0;
}
