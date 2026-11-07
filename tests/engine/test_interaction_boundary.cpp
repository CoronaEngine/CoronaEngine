/**
 * @file test_interaction_boundary.cpp
 * @brief SharedDataHub 跨线程交互边界契约测试（命令 + 快照）
 *
 * 这是第一个链接 CoronaEngine 的测试目标：SharedDataHub 的命令/快照语义
 * 只有在引擎库里才能被真正执行，P0 的 kernel 测试覆盖不到。
 *
 * 覆盖：
 * - 相机：播种快照、已提交值与待提交值分离、LatestWins 合并、释放后摘行
 * - 变换：命令叠加、快照发布、按 transform_handle 合并、销毁时丢弃待处理命令
 */

#include <corona/engine/engine_runtime_api.h>
#include <corona/shared_data_hub.h>
#include <corona/systems/mechanics/mechanics_commands.h>
#include <corona/systems/mechanics/mechanics_structural_commands.h>
#include <corona/systems/optics/optics_commands.h>
#include <corona/systems/acoustics/acoustics_commands.h>
#include <corona/systems/optics/environment_commands.h>

#include <cstdio>
#include <cstdint>

namespace {

int g_checks = 0;
int g_failures = 0;

void check_impl(bool ok, const char* text, const char* file, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "  [FAIL] %s:%d: %s\n", file, line, text);
    }
}

#define CHECK(cond) check_impl((cond), #cond, __FILE__, __LINE__)

using Corona::SharedDataHub;

ktm::fvec3 vec3(float x, float y, float z) {
    ktm::fvec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

void test_camera_seed_and_committed_read() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.camera_storage().allocate();
    CHECK(handle != 0);

    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->position = vec3(1.0f, 2.0f, 3.0f);
        camera->fov = 61.0f;
        camera->width = 800;
        camera->height = 600;
    }

    CHECK(!hub.camera_row(handle).has_value());

    hub.seed_camera_snapshot_from_storage(handle);
    const auto row = hub.camera_row(handle);
    CHECK(row.has_value());
    CHECK(row->position.x == 1.0f && row->position.y == 2.0f && row->position.z == 3.0f);
    CHECK(row->fov == 61.0f);
    CHECK(row->width == 800);
    CHECK(row->height == 600);

    hub.remove_camera_snapshot_row(handle);
    CHECK(!hub.camera_row(handle).has_value());
    hub.camera_storage().deallocate(handle);
}

void test_camera_pending_overlay() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.camera_storage().allocate();
    {
        auto camera = hub.camera_storage().acquire_write(handle);
        camera->position = vec3(0.0f, 0.0f, 0.0f);
        camera->fov = 45.0f;
        camera->width = 800;
        camera->height = 600;
    }
    hub.seed_camera_snapshot_from_storage(handle);

    Corona::CameraMoveCommand move{};
    move.camera_handle = handle;
    move.position = vec3(10.0f, 20.0f, 30.0f);
    move.forward = vec3(0.0f, 0.0f, 1.0f);
    move.world_up = vec3(0.0f, 1.0f, 0.0f);
    move.fov = 70.0f;
    hub.enqueue_camera_move(move);

    CHECK(hub.camera_view(handle).position.x == 10.0f);
    CHECK(hub.camera_view(handle).fov == 70.0f);
    CHECK(hub.camera_row(handle)->position.x == 0.0f);
    CHECK(hub.camera_row(handle)->fov == 45.0f);

    Corona::CameraStateUpdateCommand state{};
    state.camera_handle = handle;
    state.fields = Corona::CameraStateUpdateField::Size;
    state.width = 1024;
    state.height = 768;
    hub.enqueue_camera_state_update(state);
    CHECK(hub.camera_view(handle).width == 1024);
    CHECK(hub.camera_row(handle)->width == 800);

    for (const auto& incoming : hub.drain_camera_moves()) {
        if (auto camera = hub.camera_storage().try_acquire_write(incoming.camera_handle)) {
            camera->position = incoming.position;
            camera->forward = incoming.forward;
            camera->world_up = incoming.world_up;
            camera->fov = incoming.fov;
        }
    }
    for (const auto& update : hub.drain_camera_state_updates()) {
        if (auto camera = hub.camera_storage().try_acquire_write(update.camera_handle)) {
            if (Corona::has_camera_state_field(update.fields,
                                               Corona::CameraStateUpdateField::Size)) {
                camera->width = update.width;
                camera->height = update.height;
            }
        }
    }
    hub.publish_camera_snapshot_from_storage();

    CHECK(hub.camera_view(handle).position.x == 10.0f);
    CHECK(hub.camera_row(handle)->position.x == 10.0f);
    CHECK(hub.camera_row(handle)->width == 1024);

    hub.remove_camera_snapshot_row(handle);
    hub.camera_storage().deallocate(handle);
}

void test_camera_move_coalescing() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.camera_storage().allocate();

    for (int i = 0; i < 5; ++i) {
        Corona::CameraMoveCommand move{};
        move.camera_handle = handle;
        move.position = vec3(static_cast<float>(i), 0.0f, 0.0f);
        move.fov = 45.0f;
        hub.enqueue_camera_move(move);
    }
    CHECK(hub.camera_view(handle).position.x == 4.0f);

    const auto moves = hub.drain_camera_moves();
    CHECK(moves.size() == 1);
    CHECK(moves[0].position.x == 4.0f);

    // 出队顺序 = 各 key 最后一次提交的顺序（与线程调度无关）
    const auto other = hub.camera_storage().allocate();
    auto push_move = [&hub](std::uintptr_t camera, float x) {
        Corona::CameraMoveCommand move{};
        move.camera_handle = camera;
        move.position = vec3(x, 0.0f, 0.0f);
        move.fov = 45.0f;
        hub.enqueue_camera_move(move);
    };
    push_move(handle, 10.0f);
    push_move(other, 20.0f);
    push_move(handle, 11.0f);  // 覆盖 handle 的第一条，sequence 变成最后

    const auto ordered = hub.drain_camera_moves();
    CHECK(ordered.size() == 2);
    CHECK(ordered[0].camera_handle == other);
    CHECK(ordered[0].position.x == 20.0f);
    CHECK(ordered[1].camera_handle == handle);
    CHECK(ordered[1].position.x == 11.0f);

    hub.camera_storage().deallocate(other);
    hub.camera_storage().deallocate(handle);
}

void test_transform_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    const auto handle = hub.model_transform_storage().allocate();
    CHECK(handle != 0);
    {
        auto transform = hub.model_transform_storage().acquire_write(handle);
        transform->position = vec3(5.0f, 5.0f, 5.0f);
    }

    CHECK(!hub.transform_row(handle).has_value());
    CHECK(!hub.transform_view(handle).has_value());

    hub.publish_transform_snapshot_from_storage();
    CHECK(hub.transform_row(handle).has_value());
    CHECK(hub.transform_row(handle)->position.x == 5.0f);

    Corona::TransformUpdateCommand update{};
    update.transform_handle = handle;
    update.fields = Corona::TransformField::Position;
    update.position = vec3(9.0f, 0.0f, 0.0f);
    hub.enqueue_transform_update(update);
    hub.enqueue_transform_update(update);

    CHECK(hub.transform_view(handle)->position.x == 9.0f);
    CHECK(hub.transform_row(handle)->position.x == 5.0f);

    const auto drained = hub.drain_transform_updates();
    CHECK(drained.size() == 1);

    hub.enqueue_transform_update(update);
    hub.clear_transform_updates(handle);
    CHECK(hub.transform_view(handle)->position.x == 5.0f);
    CHECK(hub.drain_transform_updates().empty());

    hub.model_transform_storage().deallocate(handle);
}

void test_mechanics_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    auto& channel = Corona::MechanicsCommandChannel::instance();
    channel.commands().clear();

    const auto handle = hub.mechanics_storage().allocate();
    CHECK(handle != 0);
    {
        auto device = hub.mechanics_storage().acquire_write(handle);
        device->mass = 3.0f;
        device->restitution = 0.5f;
    }

    CHECK(!Corona::mechanics_row(handle).has_value());

    Corona::publish_mechanics_snapshot_from_storage();
    CHECK(Corona::mechanics_row(handle).has_value());
    CHECK(Corona::mechanics_row(handle)->mass == 3.0f);
    CHECK(Corona::mechanics_row(handle)->restitution == 0.5f);

    // 命令：pending 立刻可见，已提交值不变
    Corona::MechanicsUpdateCommand command{};
    command.mechanics_handle = handle;
    command.fields = Corona::MechanicsField::Mass | Corona::MechanicsField::BodyType;
    command.mass = 7.5f;
    command.body_type = Corona::BodyType::Static;
    channel.commands().push(command);

    CHECK(Corona::mechanics_view(handle)->mass == 7.5f);
    CHECK(Corona::mechanics_view(handle)->body_type == Corona::BodyType::Static);
    CHECK(Corona::mechanics_row(handle)->mass == 3.0f);
    CHECK(Corona::mechanics_row(handle)->body_type == Corona::BodyType::Dynamic);

    // LatestWins：同 handle 再推一条，合并成最后一条
    command.mass = 8.5f;
    channel.commands().push(command);
    const auto drained = channel.commands().drain();
    CHECK(drained.size() == 1);
    CHECK(drained[0].mass == 8.5f);
    CHECK(Corona::has_mechanics_field(drained[0].fields, Corona::MechanicsField::BodyType));

    // 模拟 Mechanics 应用后再发布：pending 视图与已提交值收敛
    for (const auto& incoming : drained) {
        if (auto device = hub.mechanics_storage().try_acquire_write(incoming.mechanics_handle)) {
            if (Corona::has_mechanics_field(incoming.fields, Corona::MechanicsField::Mass)) {
                device->mass = incoming.mass;
            }
            if (Corona::has_mechanics_field(incoming.fields, Corona::MechanicsField::BodyType)) {
                device->body_type = incoming.body_type;
            }
        }
    }
    Corona::publish_mechanics_snapshot_from_storage();
    CHECK(Corona::mechanics_view(handle)->mass == 8.5f);
    CHECK(Corona::mechanics_row(handle)->mass == 8.5f);

    channel.commands().clear();
    hub.mechanics_storage().deallocate(handle);
}

void test_optics_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    auto& channel = Corona::OpticsCommandChannel::instance();
    channel.commands().clear();

    const auto handle = hub.optics_storage().allocate();
    CHECK(handle != 0);
    {
        auto device = hub.optics_storage().acquire_write(handle);
        device->metallic = 0.25f;
        device->roughness = 0.75f;
    }

    CHECK(!Corona::optics_row(handle).has_value());
    Corona::publish_optics_snapshot_from_storage();
    CHECK(Corona::optics_row(handle).has_value());
    CHECK(Corona::optics_row(handle)->metallic == 0.25f);
    CHECK(Corona::optics_row(handle)->roughness == 0.75f);

    // 命令：pending 立刻可见，已提交值不变
    Corona::OpticsUpdateCommand command{};
    command.optics_handle = handle;
    command.fields = Corona::OpticsField::Metallic | Corona::OpticsField::Visible;
    command.metallic = 0.9f;
    command.visible = false;
    channel.commands().push(command);

    CHECK(Corona::optics_view(handle)->metallic == 0.9f);
    CHECK(Corona::optics_view(handle)->visible == false);
    CHECK(Corona::optics_row(handle)->metallic == 0.25f);
    CHECK(Corona::optics_row(handle)->visible == true);

    // LatestWins：同 handle 再推一条，合并成最后一条
    command.metallic = 0.95f;
    channel.commands().push(command);
    const auto drained = channel.commands().drain();
    CHECK(drained.size() == 1);
    CHECK(drained[0].metallic == 0.95f);
    CHECK(Corona::has_optics_field(drained[0].fields, Corona::OpticsField::Visible));

    for (const auto& incoming : drained) {
        if (auto device = hub.optics_storage().try_acquire_write(incoming.optics_handle)) {
            if (Corona::has_optics_field(incoming.fields, Corona::OpticsField::Metallic)) {
                device->metallic = incoming.metallic;
            }
            if (Corona::has_optics_field(incoming.fields, Corona::OpticsField::Visible)) {
                device->visible = incoming.visible;
            }
        }
    }
    Corona::publish_optics_snapshot_from_storage();
    CHECK(Corona::optics_view(handle)->metallic == 0.95f);
    CHECK(Corona::optics_row(handle)->metallic == 0.95f);
    CHECK(Corona::optics_row(handle)->visible == false);

    channel.commands().clear();
    hub.optics_storage().deallocate(handle);
}

void test_acoustics_command_and_snapshot() {
    auto& hub = SharedDataHub::instance();
    auto& channel = Corona::AcousticsCommandChannel::instance();
    channel.commands().clear();

    const auto handle = hub.acoustics_storage().allocate();
    CHECK(handle != 0);
    {
        auto device = hub.acoustics_storage().acquire_write(handle);
        device->volume = 0.4f;
        device->resource_id = 42;
    }

    Corona::publish_acoustics_snapshot_from_storage();
    CHECK(Corona::acoustics_row(handle).has_value());
    CHECK(Corona::acoustics_row(handle)->volume == 0.4f);
    CHECK(Corona::acoustics_row(handle)->resource_id == 42);

    // 先绑资源再读：pending 必须可见（脚本常见用法：set_audio_resource 后立刻 play）
    Corona::AcousticsUpdateCommand command{};
    command.acoustics_handle = handle;
    command.fields = Corona::AcousticsField::ResourceId | Corona::AcousticsField::Volume;
    command.resource_id = 99;
    command.volume = 0.8f;
    channel.commands().push(command);

    CHECK(Corona::acoustics_view(handle)->resource_id == 99);
    CHECK(Corona::acoustics_view(handle)->volume == 0.8f);
    CHECK(Corona::acoustics_row(handle)->resource_id == 42);  // 已提交值不变

    // 应用 + 发布后收敛
    const auto drained = channel.commands().drain();
    CHECK(drained.size() == 1);
    for (const auto& incoming : drained) {
        if (auto device = hub.acoustics_storage().try_acquire_write(incoming.acoustics_handle)) {
            if (Corona::has_acoustics_field(incoming.fields, Corona::AcousticsField::ResourceId)) {
                device->resource_id = incoming.resource_id;
            }
            if (Corona::has_acoustics_field(incoming.fields, Corona::AcousticsField::Volume)) {
                device->volume = incoming.volume;
            }
        }
    }
    Corona::publish_acoustics_snapshot_from_storage();
    CHECK(Corona::acoustics_row(handle)->resource_id == 99);
    CHECK(Corona::acoustics_view(handle)->resource_id == 99);

    // 无快照、只有 pending 时也要能读出来
    const auto fresh = hub.acoustics_storage().allocate();
    Corona::AcousticsUpdateCommand bind{};
    bind.acoustics_handle = fresh;
    bind.fields = Corona::AcousticsField::ResourceId;
    bind.resource_id = 7;
    channel.commands().push(bind);
    CHECK(Corona::acoustics_view(fresh)->resource_id == 7);
    channel.commands().clear();
    hub.acoustics_storage().deallocate(fresh);

    channel.commands().clear();
    hub.acoustics_storage().deallocate(handle);
}

void test_environment_boundary_stage() {
    auto& hub = SharedDataHub::instance();
    auto& channel = Corona::EnvironmentCommandChannel::instance();
    channel.commands().clear();

    const auto handle = hub.environment_storage().allocate();
    CHECK(handle != 0);

    // 边界阶段即使没有命令也会发布一次快照
    Corona::apply_pending_environment_updates();
    CHECK(Corona::environment_row(handle).has_value());
    CHECK(Corona::environment_row(handle)->fixed_dt > 0.0f);

    // 命令：pending 立刻可见，已提交值不变
    Corona::EnvironmentUpdateCommand command{};
    command.environment_handle = handle;
    command.fields = Corona::EnvironmentField::Gravity | Corona::EnvironmentField::FixedDt;
    command.gravity = vec3(0.0f, -20.0f, 0.0f);
    command.fixed_dt = 1.0f / 120.0f;
    channel.commands().push(command);

    CHECK(Corona::environment_view(handle)->gravity.y == -20.0f);
    CHECK(Corona::environment_view(handle)->fixed_dt == 1.0f / 120.0f);
    CHECK(Corona::environment_row(handle)->gravity.y != -20.0f);

    // LatestWins：同 handle 再推一条，只应用最后一条
    command.gravity = vec3(0.0f, -30.0f, 0.0f);
    channel.commands().push(command);

    Corona::apply_pending_environment_updates();
    CHECK(channel.commands().empty());
    CHECK(Corona::environment_row(handle)->gravity.y == -30.0f);
    CHECK(Corona::environment_row(handle)->fixed_dt == 1.0f / 120.0f);
    CHECK(Corona::environment_view(handle)->gravity.y == -30.0f);

    // 槽位确实被写了（DAG 侧看到的就是这个值）
    if (auto device = hub.environment_storage().try_acquire_read(handle)) {
        CHECK(device->gravity.y == -30.0f);
        CHECK(device->fixed_dt == 1.0f / 120.0f);
    } else {
        CHECK(false);
    }

    channel.commands().clear();
    hub.environment_storage().deallocate(handle);
}

void test_mechanics_structural_commands() {
    auto& hub = SharedDataHub::instance();
    auto& structural = Corona::MechanicsStructuralChannel::instance();
    structural.commands().clear();

    const auto handle = hub.mechanics_storage().allocate();
    int hits = 0;

    Corona::MechanicsCallbackBinding binding{};
    binding.mechanics_handle = handle;
    binding.kind = Corona::MechanicsCallbackBinding::Kind::OnMove;
    binding.on_move = [&hits]() { ++hits; };
    structural.commands().push(std::move(binding));

    // 应用之前槽位里没有回调
    if (auto device = hub.mechanics_storage().try_acquire_read(handle)) {
        CHECK(!device->on_move_callback);
    } else {
        CHECK(false);
    }

    CHECK(Corona::apply_pending_mechanics_structural_commands());
    CHECK(!Corona::apply_pending_mechanics_structural_commands());  // 幂等：没有命令

    if (auto device = hub.mechanics_storage().try_acquire_read(handle)) {
        CHECK(static_cast<bool>(device->on_move_callback));
        if (device->on_move_callback) {
            device->on_move_callback();
        }
    } else {
        CHECK(false);
    }
    CHECK(hits == 1);

    // Fifo：两条按序全量应用，后者覆盖前者（顺序不能被打乱）
    Corona::MechanicsCallbackBinding first{};
    first.mechanics_handle = handle;
    first.kind = Corona::MechanicsCallbackBinding::Kind::OnMove;
    first.on_move = [&hits]() { hits += 10; };
    structural.commands().push(std::move(first));
    Corona::MechanicsCallbackBinding second{};
    second.mechanics_handle = handle;
    second.kind = Corona::MechanicsCallbackBinding::Kind::OnMove;
    second.on_move = [&hits]() { hits += 100; };
    structural.commands().push(std::move(second));

    CHECK(Corona::apply_pending_mechanics_structural_commands());
    if (auto device = hub.mechanics_storage().try_acquire_read(handle)) {
        if (device->on_move_callback) {
            device->on_move_callback();
        }
    }
    CHECK(hits == 101);  // 1 + 100

    structural.commands().clear();
    hub.mechanics_storage().deallocate(handle);
}

void test_camera_release_queue_is_fifo() {
    auto& hub = SharedDataHub::instance();
    (void)hub.drain_camera_releases();  // 清掉其它用例可能残留的释放命令

    const auto first = hub.camera_storage().allocate();
    const auto second = hub.camera_storage().allocate();
    CHECK(first != 0 && second != 0);

    // 释放时要顺手清掉该相机的遥测
    hub.publish_camera_frame_timing(first, 1.5, 0.5, Corona::CameraVisionRenderMode::PathTracing);
    CHECK(hub.camera_frame_timing(first).has_value());

    hub.enqueue_camera_release({.camera_handle = first, .actor_pick_handle = 0});
    hub.enqueue_camera_release({.camera_handle = second, .actor_pick_handle = 0});

    const auto releases = hub.drain_camera_releases();
    CHECK(releases.size() == 2);        // Fifo：不合并
    CHECK(releases[0].camera_handle == first);
    CHECK(releases[1].camera_handle == second);
    CHECK(!hub.camera_frame_timing(first).has_value());
    CHECK(hub.drain_camera_releases().empty());  // 取空后不重复投递

    hub.camera_storage().deallocate(first);
    hub.camera_storage().deallocate(second);
}

void test_camera_viewport_queue_coalescing() {
    auto& hub = SharedDataHub::instance();
    (void)hub.drain_camera_viewport_updates();

    const auto camera = hub.camera_storage().allocate();
    for (int i = 0; i < 4; ++i) {
        hub.enqueue_camera_viewport_update({
            .camera_handle = camera,
            .surface = nullptr,
            .view_open = true,
            .x = i,
            .y = 0,
            .width = 100 + i,
            .height = 100,
            .render_width = 100,
            .render_height = 100,
        });
    }

    const auto updates = hub.drain_camera_viewport_updates();
    CHECK(updates.size() == 1);              // LatestWins
    CHECK(updates[0].width == 103);          // 最后一次提交生效
    CHECK(updates[0].x == 3);
    CHECK(hub.drain_camera_viewport_updates().empty());

    // 默认 surface 仍保持"单槽"语义（与视口队列不共用锁）
    hub.enqueue_default_surface_camera_update({.surface = nullptr, .width = 0, .height = 0});
    CHECK(!hub.take_default_surface_camera_update().has_value());  // 无效输入被丢弃
    hub.enqueue_default_surface_camera_update(
        {.surface = reinterpret_cast<void*>(0x1), .width = 64, .height = 32});
    const auto taken = hub.take_default_surface_camera_update();
    CHECK(taken.has_value());
    CHECK(taken->width == 64);
    CHECK(!hub.take_default_surface_camera_update().has_value());

    // 单槽语义：连续两次提交只保留最后一次
    hub.enqueue_default_surface_camera_update(
        {.surface = reinterpret_cast<void*>(0x2), .width = 10, .height = 20});
    hub.enqueue_default_surface_camera_update(
        {.surface = reinterpret_cast<void*>(0x3), .width = 30, .height = 40});
    const auto latest = hub.take_default_surface_camera_update();
    CHECK(latest.has_value());
    CHECK(latest->width == 30);
    CHECK(latest->height == 40);

    hub.camera_storage().deallocate(camera);
}

void test_viewport_ui_pointer_state_and_owner_cleanup() {
    auto& hub = SharedDataHub::instance();
    const auto camera = hub.camera_storage().allocate();
    const auto other = hub.camera_storage().allocate();
    using Cursor = Corona::ViewportUiCursorShape;
    CHECK(hub.viewport_ui_state(camera).pointer.cursor_shape == Cursor::Hidden);

    // Cursor state is now a camera-local session snapshot, not an event FIFO.
    hub.set_viewport_ui_mode(camera, Corona::ViewportUiMode::Stereo3D);
    Corona::ViewportUiCalibration calibration;
    calibration.lenticular_pitch = 6.f;
    hub.set_viewport_ui_calibration(camera, calibration);
    hub.set_viewport_ui_pointer(camera, {1.f, 2.f, Cursor::Hand, 101});
    const auto before = hub.viewport_ui_state(camera);
    hub.set_viewport_ui_pointer(camera, {3.f, 4.f, Cursor::Grabbing, 202});
    hub.set_viewport_ui_pointer(other, {5.f, 6.f, Cursor::Crosshair, 303});
    const auto latest = hub.viewport_ui_state(camera);
    CHECK(before.pointer.owner_context == 101 && before.pointer.x == 1.f);
    CHECK(latest.camera_handle == camera);
    CHECK(latest.pointer.x == 3.f && latest.pointer.y == 4.f);
    CHECK(latest.pointer.cursor_shape == Cursor::Grabbing);
    CHECK(latest.pointer.owner_context == 202);
    CHECK(latest.mode == Corona::ViewportUiMode::Stereo3D);
    CHECK(latest.calibration.lenticular_pitch == 6.f);

    // Retiring an old context must not remove the replacement window's cursor.
    hub.clear_viewport_ui_pointer(camera, 101);
    CHECK(hub.viewport_ui_state(camera).pointer.owner_context == 202);
    hub.clear_viewport_ui_pointer(camera, 202);
    CHECK(hub.viewport_ui_state(camera).pointer.cursor_shape == Cursor::Hidden);
    CHECK(hub.viewport_ui_state(other).pointer.owner_context == 303);
    hub.clear_viewport_ui_pointer(other, 0); // Unconditional session teardown.
    CHECK(hub.viewport_ui_state(other).pointer.cursor_shape == Cursor::Hidden);
    hub.set_viewport_ui_pointer(0, {1.f, 2.f, Cursor::Hand, 101});
    CHECK(hub.viewport_ui_state(0).pointer.cursor_shape == Cursor::Hidden);
    hub.camera_storage().deallocate(other);
    hub.camera_storage().deallocate(camera);
}

void test_camera_api_construction_does_not_self_deadlock() {
    // 回归用例：Camera 构造函数曾在**持有相机槽位写锁的作用域内**派生快照行，
    // 而派生函数内部又 try_acquire_read 同一个槽位 —— shared_timed_mutex 不可重入，
    // 于是创建相机的线程自死锁（编辑器"创建世界"卡死就是这么来的）。
    // 若该缺陷回归，本用例会挂住而不是失败。
    Corona::API::Camera camera;
    CHECK(camera.get_handle() != 0);

    const auto position = camera.get_position();
    CHECK(position[2] == -5.0f);   // 默认构造位置
    CHECK(camera.get_fov() == 45.0f);

    // 读路径走快照 ⊕ pending，构造后即可读到值（不必等 DAG tick）
    camera.set(std::array<float, 3>{1.0f, 2.0f, 3.0f},
               std::array<float, 3>{0.0f, 0.0f, 1.0f},
               std::array<float, 3>{0.0f, 1.0f, 0.0f},
               60.0f);
    const auto moved = camera.get_position();
    CHECK(moved[0] == 1.0f && moved[1] == 2.0f && moved[2] == 3.0f);
    CHECK(camera.get_fov() == 60.0f);
}

}  // namespace

int main() {
    std::printf("== Interaction boundary (command + snapshot) tests ==\n");
    test_camera_seed_and_committed_read();
    test_camera_pending_overlay();
    test_camera_move_coalescing();
    test_transform_command_and_snapshot();
    test_mechanics_command_and_snapshot();
    test_optics_command_and_snapshot();
    test_acoustics_command_and_snapshot();
    test_environment_boundary_stage();
    test_mechanics_structural_commands();
    test_camera_release_queue_is_fifo();
    test_camera_viewport_queue_coalescing();
    test_viewport_ui_pointer_state_and_owner_cleanup();
    test_camera_api_construction_does_not_self_deadlock();

    if (g_failures == 0) {
        std::printf("OK: %d checks passed\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d of %d checks failed\n", g_failures, g_checks);
    return 1;
}
