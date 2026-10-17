# ==============================================================================
# Mechanics System
# ==============================================================================

corona_add_system(mechanics
    SOURCES
        mechanics_system.cpp
        mechanics_lifecycle.cpp
        mechanics_ik.cpp
        gpu_skinning.cpp
        gpu_skinning.h
        gpu_collision.cpp
        gpu_collision.h
        gpu_collision_math.h
        gpu_broadphase.cpp
        gpu_broadphase.h
        gpu_rigid_integration.cpp
        gpu_rigid_integration.h
        gpu_rigid_state.h
        gpu_contact_solver.cpp
        gpu_contact_solver.h
        gpu_floor_sleep.cpp
        gpu_floor_sleep.h
        mechanics_internal.h
        ik_runtime.h
    DEPENDENCIES
        Horizon
        corona::resource::manager
)
