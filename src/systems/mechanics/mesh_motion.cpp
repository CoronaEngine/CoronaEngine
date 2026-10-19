#include <corona/systems/mechanics/mechanics_system.h>
#include <corona/shared_data_hub.h>
#include "mechanics_internal.h"

namespace Corona::Systems {
void MechanicsSystem::submit_mesh_move(MeshMoveCommand command, std::function<void(MeshMoveEvent)> completion) {
    bool accepted=false;
    {
        std::lock_guard lock(mesh_move_mutex_);
        if(!impl_->shutdown_requested.load() && mesh_moves_.size()<16 &&
           std::none_of(mesh_moves_.begin(),mesh_moves_.end(),[&](const auto& q) { return q.command.actor==command.actor; })) {
            mesh_moves_.push_back({command,std::move(completion),std::chrono::steady_clock::now()});
            accepted=true;
        }
    }
    if(!accepted && completion) completion({});
}

void MechanicsSystem::process_mesh_moves() {
    std::vector<PendingMeshMove> pending;
    { std::lock_guard lock(mesh_move_mutex_); pending.swap(mesh_moves_); }
    auto& hub=SharedDataHub::instance();
    for(auto& request:pending) {
        auto run=[&]() -> MeshMoveEvent {
            const auto& q=request.command;
            auto unavailable=[&](const char* reason, std::uintptr_t handle=0) -> MeshMoveEvent {
                if(GpuPerformance::enabled()) CFW_LOG_INFO("Mesh movement pending: actor={} reason={} handle={}",q.actor,reason,handle);
                return {};
            };
            if(!q.actor || !q.camera || !std::isfinite(q.dt) || q.dt<=0 || q.dt>.05f ||
               (q.turn && !std::isfinite(q.facing)) ||
               std::any_of(q.velocity.begin(),q.velocity.end(),[](float x) { return !std::isfinite(x) || std::abs(x)>24; })) return {"invalid"};
            if(std::chrono::steady_clock::now()-request.submitted>std::chrono::seconds(2)) return {"expired"};
            if(!impl_->gpu_collision) return unavailable("pipeline");
            for(const auto& scene:std::as_const(hub.scene_storage())) {
                if(!scene.enabled || std::find(scene.camera_handles.begin(),scene.camera_handles.end(),q.camera)==scene.camera_handles.end() ||
                   std::find(scene.actor_handles.begin(),scene.actor_handles.end(),q.actor)==scene.actor_handles.end()) continue;
                const auto scene_handle=reinterpret_cast<std::uintptr_t>(&scene);
                std::vector<GpuCollisionPipeline::Instance> obstacles;
                std::vector<std::uintptr_t> obstacle_actors;
                GpuCollisionPipeline::Instance moving{};
                ModelTransform original;
                std::uintptr_t transform_handle{};
                for(auto actor_handle:scene.actor_handles) {
                    auto actor=hub.actor_storage().try_acquire_read(actor_handle);
                    if(!actor) return unavailable("actor",actor_handle);
                    std::unordered_set<std::uintptr_t> seen_geometry;
                    for(auto ph:actor->profile_handles) {
                        auto profile=hub.profile_storage().try_acquire_read(ph);
                        if(!profile) return unavailable("profile",ph);
                        // Runtime Actor profiles keep geometry on their components;
                        // geometry_handle is only populated by some import paths.
                        auto geometry_handle=profile->geometry_handle;
                        if(profile->mechanics_handle) {
                            auto body=hub.mechanics_storage().try_acquire_read(profile->mechanics_handle);
                            if(!body) return unavailable("mechanics",profile->mechanics_handle);
                            if(actor_handle!=q.actor && body->body_type==BodyType::Phantom) continue;
                            if(!geometry_handle) geometry_handle=body->geometry_handle;
                        }
                        if(profile->optics_handle) {
                            auto optics=hub.optics_storage().try_acquire_read(profile->optics_handle);
                            if(!optics) return unavailable("optics",profile->optics_handle);
                            if(actor_handle!=q.actor && !optics->visible) continue;
                            if(!geometry_handle) geometry_handle=optics->geometry_handle;
                        }
                        if(!geometry_handle && profile->acoustics_handle) {
                            auto acoustics=hub.acoustics_storage().try_acquire_read(profile->acoustics_handle);
                            if(!acoustics) return unavailable("acoustics",profile->acoustics_handle);
                            geometry_handle=acoustics->geometry_handle;
                        }
                        if(!geometry_handle || !seen_geometry.insert(geometry_handle).second) continue;
                        auto geometry=hub.geometry_storage().try_acquire_read(geometry_handle);
                        if(!geometry) return unavailable("geometry",geometry_handle);
                        auto resource=hub.model_resource_storage().try_acquire_read(geometry->model_resource_handle);
                        if(!resource || !resource->model_id) return unavailable("resource",geometry->model_resource_handle);
                        // Never replace unavailable/deforming triangles with a proxy.
                        if(!impl_->gpu_collision->contains(geometry_handle,resource->model_id)) return unavailable("triangles",geometry_handle);
                        auto transform=hub.model_transform_storage().try_acquire_read(geometry->transform_handle);
                        if(!transform) return unavailable("transform",geometry->transform_handle);
                        GpuCollisionPipeline::Matrix matrix{};
                        const auto m=transform->compute_matrix();
                        for(int c=0;c<4;++c) for(int r=0;r<4;++r) matrix[c*4+r]=m[c][r];
                        if(actor_handle==q.actor) {
                            // Compound moving actors need a combined reduction, never silently
                            // choose one profile and leave the others without collision.
                            if(moving.key) return {"unsupported_compound"};
                            moving={geometry_handle,matrix}; original=*transform;
                            transform_handle=geometry->transform_handle;
                        } else { obstacles.push_back({geometry_handle,matrix}); obstacle_actors.push_back(actor_handle); }
                    }
                }
                if(!moving.key) return unavailable("moving mesh");
                const std::array<float,3> position{original.position.x,original.position.y,original.position.z};
                auto& state=mesh_movers_[q.actor];
                if(state.geometry!=moving.key || state.scene!=scene_handle || state.last_position!=position)
                    state={0,false,position,moving.key,scene_handle};
                const float velocity=q.jump && state.grounded ? std::sqrt(2.f*19.6f*1.2f) : state.vertical_velocity;
                const float next_velocity=std::max(-24.f,velocity-19.6f*q.dt);
                const GpuCollisionPipeline::Point delta{q.velocity[0]*q.dt,(velocity+next_velocity)*.5f*q.dt,q.velocity[2]*q.dt};
                const float turn=q.turn ? std::clamp(std::remainder(q.facing-original.euler_rotation.y,6.28318530718f),-6.f*q.dt,6.f*q.dt) : 0.f;
                // ModelTransform composes Rz * Ry * Rx. Changing Euler yaw is
                // therefore a rotation about Rz's Y axis in world space.
                const GpuCollisionPipeline::Point axis{-std::sin(original.euler_rotation.z),std::cos(original.euler_rotation.z),0};
                const auto result=impl_->gpu_collision->move_mesh(moving,obstacles,delta,state.grounded && !q.jump,turn,position,axis);
                auto target=hub.model_transform_storage().try_acquire_write(transform_handle);
                if(!target) return {};
                const auto current=target->compute_matrix(), before=original.compute_matrix();
                for(int c=0;c<4;++c) for(int r=0;r<4;++r) if(current[c][r]!=before[c][r]) return {"superseded"};
                target->position.x+=result.displacement[0]; target->position.y+=result.displacement[1]; target->position.z+=result.displacement[2];
                target->euler_rotation.y=std::remainder(target->euler_rotation.y+result.turn,6.28318530718f);
                state.last_position={target->position.x,target->position.y,target->position.z};
                state.grounded=result.grounded;
                state.vertical_velocity=(result.grounded && next_velocity<0) || (result.ceiling && next_velocity>0) ? 0 : next_velocity;
                MeshMoveEvent event{"success",state.last_position,result.normal,result.grounded,result.ceiling,result.collided};
                if(result.obstacle>=0 && static_cast<std::size_t>(result.obstacle)<obstacle_actors.size()) event.other_actor=obstacle_actors[result.obstacle];
                event.triangle=result.triangle; event.other_triangle=result.obstacle_triangle;
                event.rotation={target->euler_rotation.x,target->euler_rotation.y,target->euler_rotation.z};
                return event;
            }
            return {"stale_scene"};
        };
        MeshMoveEvent event;
        try { event=run(); } catch(const std::exception& e) { event.status=std::string("error: ")+e.what(); }
        if(request.completion) request.completion(std::move(event));
    }
    std::erase_if(mesh_movers_,[&](const auto& item) { return !hub.actor_storage().try_acquire_read(item.first); });
}
}
