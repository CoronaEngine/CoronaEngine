#pragma once
#include "base/using.h"

/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

// Native two-slot InteriorList algorithm, expressed through the renderer DSL.
// Copyright (c) 2025 NVIDIA CORPORATION. Original notice retained in the
// offline matched source used by the differential tests.
namespace vision::rtxpt_gi {
constexpr uint NativeNoInteriorMaterial = 0xffffffffu;
constexpr uint NativeMaxRejectedIntersections = 16u;
struct InteriorState { Uint2 slots{make_uint2(0u)}; Uint rejected_hits{0u}; };

[[nodiscard]] inline Uint interior_outside_id(const Uint2 &slots, const Uint &material_id,
                                              const Bool &entering) noexcept {
    Uint outside = select(slots.x != 0u, slots.x & 0x0fffffffu, NativeNoInteriorMaterial);
    $if(!entering && outside == material_id) {
        outside = select(slots.y != 0u, slots.y & 0x0fffffffu, NativeNoInteriorMaterial);
    };
    return outside;
}
[[nodiscard]] inline Bool interior_true_hit(const Uint2 &slots, const Uint &priority) noexcept {
    return priority == 0u || priority >= (slots.x >> 28u);
}
[[nodiscard]] inline Uint2 interior_update(const Uint2 &input, const Uint &material_id,
                                           const Uint &input_priority, const Bool &entering) noexcept {
    Uint2 slots = input;
    Uint priority = select(input_priority == 0u, 15u, input_priority);
    Uint value = (priority << 28u) | (material_id & 0x0fffffffu);
    $if(entering && slots.x == 0u) { slots.x = value; }
    $elif(!entering && slots.x != 0u && (slots.x & 0x0fffffffu) == material_id) { slots.x = 0u; }
    $elif(entering && slots.y == 0u) { slots.y = value; }
    $elif(!entering && slots.y != 0u && (slots.y & 0x0fffffffu) == material_id) { slots.y = 0u; };
    $if(slots.x < slots.y) { Uint previous = slots.x; slots.x = slots.y; slots.y = previous; };
    return slots;
}
}// namespace vision::rtxpt_gi
