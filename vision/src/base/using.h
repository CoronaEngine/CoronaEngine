//
// Created by Z on 2026/3/14.
//

#pragma once

#include "hotfix/support.h"
#include "core/stl.h"
#include "core/util/hash.h"
#include "core/util/string_util.h"
#include "core/concurrency/thread_safety.h"
#include "core/runtime/dynamic_module.h"
#include "math/geometry.h"
#include "math/matrix_types.h"
#include "dsl/dsl.h"
#include "rhi/resources/dynamic_buffer.h"

namespace ocarina {
class Device;
class RHIContext;
}

namespace vision {

// ============================================================
// STL re-exports
// ============================================================
using ocarina::string;
using ocarina::string_view;
using ocarina::to_string;
using ocarina::vector;
using ocarina::map;
using ocarina::unordered_map;
using ocarina::set;
using ocarina::unordered_set;
using ocarina::array;
using ocarina::queue;
using ocarina::deque;
using ocarina::optional;
using ocarina::pair;
using ocarina::make_pair;
using ocarina::span;
using ocarina::function;

// Smart pointers
using ocarina::WP;
using ocarina::shared_ptr;
using ocarina::unique_ptr;
using ocarina::weak_ptr;
using ocarina::make_shared;
using ocarina::make_unique;
using ocarina::enable_shared_from_this;
using ocarina::dynamic_pointer_cast;
using ocarina::static_pointer_cast;
using ocarina::DCSP;
using ocarina::DCUP;

// ============================================================
// Filesystem
// ============================================================
namespace fs = ocarina::fs;

// ============================================================
// Basic scalar types
// ============================================================
using ocarina::uint;
using ocarina::uchar;
using ocarina::ulong;
using ocarina::half;
using ocarina::real;

// ============================================================
// Constants
// ============================================================
using ocarina::InvalidUI32;
using ocarina::Pi;
using ocarina::InvPi;
using ocarina::Inv2Pi;
using ocarina::Inv4Pi;
using ocarina::PiOver2;
using ocarina::PiOver4;
using ocarina::Sqrt2;
using ocarina::OneMinusEpsilon;
using ocarina::ShadowEpsilon;

// ============================================================
// Core base classes & hash
// ============================================================
using ocarina::RTTI;
using ocarina::Hashable;
using ocarina::Encodable;
using ocarina::Hash64;
using ocarina::hash64;

// ============================================================
// String utilities
// ============================================================
using ocarina::format;
using ocarina::to_lower;
using ocarina::to_upper;

// ============================================================
// Core utilities
// ============================================================
using ocarina::DynamicModule;
using ocarina::thread_safety;
using ocarina::ptr_t;
using ocarina::Device;
using ocarina::RHIContext;
using ocarina::DataAccessor;

// ============================================================
// DSL scalar types
// ============================================================
using ocarina::Float;
using ocarina::Uint;
using ocarina::Int;
using ocarina::Bool;
using ocarina::Half;
using ocarina::Expr;
using ocarina::EPort;
using ocarina::D;
using ocarina::H;
using ocarina::buffer_ty;
using ocarina::boolean_t;
using ocarina::is_vector3_expr_v;
using ocarina::is_all_vector3_expr_v;
using ocarina::is_scalar_v;
using ocarina::is_scalar_expr_v;
using ocarina::is_dsl_v;
using ocarina::var_t;
using ocarina::ray_t;
using ocarina::to_underlying;
using ocarina::radians;
using ocarina::offset_ray_origin;
using ocarina::make_ray;
using ocarina::scalar_t;
using ocarina::expr_value_t;
using ocarina::dispatch_dim;
using ocarina::dispatch_id;
using ocarina::dispatch_idx;
using ocarina::is_integral_expr_v;
using ocarina::is_all_integral_expr_v;
using ocarina::is_general_int_vector2_v;
using ocarina::is_ptr_v;
using ocarina::Pow;
using ocarina::basic_t;
using ocarina::PixelStorage;
using ocarina::Clock;
using ocarina::handle_ty;
using ocarina::synchronize;
using ocarina::addressof;
using ocarina::oc_memcpy;
using ocarina::degrees;

#define VS_MAKE_USING_VEC_TYPE(type, Type) \
    using ocarina::type##2;                \
    using ocarina::type##3;                \
    using ocarina::type##4;                \
    using ocarina::make_##type##2;         \
    using ocarina::make_##type##3;         \
    using ocarina::make_##type##4;         \
    using ocarina::Type##2;                \
    using ocarina::Type##3;                \
    using ocarina::Type##4;

VS_MAKE_USING_VEC_TYPE(int, Int)
VS_MAKE_USING_VEC_TYPE(uint, Uint)
VS_MAKE_USING_VEC_TYPE(float, Float)
VS_MAKE_USING_VEC_TYPE(half, Half)
VS_MAKE_USING_VEC_TYPE(bool, Bool)

#undef VS_MAKE_USING_VEC_TYPE

// oc_ prefixed DSL types
using ocarina::oc_float;
using ocarina::oc_int;
using ocarina::oc_uint;
using ocarina::oc_bool;
using ocarina::oc_half;
using ocarina::oc_float2;
using ocarina::oc_float3;
using ocarina::oc_float4;
using ocarina::oc_int2;
using ocarina::oc_int3;
using ocarina::oc_int4;
using ocarina::oc_uint2;
using ocarina::oc_uint3;
using ocarina::oc_uint4;
using ocarina::oc_bool2;
using ocarina::oc_bool3;
using ocarina::oc_bool4;
using ocarina::condition_t;

using ocarina::Matrix;
using ocarina::Polymorphic;
using ocarina::PolymorphicMode;
using ocarina::PolyEvaluator;
using ocarina::Var;
using ocarina::Vector;

#define VS_MAKE_USING_FLOAT_MATRIX_TYPE(row, col) \
    using ocarina::float##row##x##col;            \
    using ocarina::make_float##row##x##col;       \
    using ocarina::Float##row##x##col;

VS_MAKE_USING_FLOAT_MATRIX_TYPE(2, 2)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(2, 3)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(2, 4)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(3, 2)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(3, 3)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(3, 4)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(4, 2)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(4, 3)
VS_MAKE_USING_FLOAT_MATRIX_TYPE(4, 4)

#undef VS_MAKE_USING_FLOAT_MATRIX_TYPE

using ocarina::Frame;

using ocarina::LineSegment;
using ocarina::LineSegmentVar;
using ocarina::Ray;
using ocarina::RayVar;
using ocarina::Triangle;
using ocarina::TriangleHit;
using ocarina::TriangleHitVar;
using ocarina::TriangleVar;
using ocarina::Vertex;
using ocarina::VertexVar;

using ocarina::Box3f;
using ocarina::Box2f;

// ============================================================
// Core base classes
// ============================================================
using ocarina::Hashable;
using ocarina::Encodable;

// ============================================================
// GPU / RHI types
// ============================================================
using ocarina::Buffer;
using ocarina::BufferView;
using ocarina::ByteBufferView;
using ocarina::Image;
using ocarina::Texture2D;
using ocarina::Texture3D;
using ocarina::Accel;
using ocarina::Stream;
using ocarina::CommandBatch;
using ocarina::Command;
using ocarina::BufferUploadCommand;
using ocarina::Shader;
using ocarina::Callable;
using ocarina::Kernel;
using ocarina::BufferVar;
using ocarina::DynamicArray;
using ocarina::float_array;
using ocarina::float2_array;
using ocarina::float3_array;
using ocarina::float4_array;
using ocarina::uint_array;
using ocarina::int_array;
using ocarina::BindlessArray;

// ============================================================
// Memory / Resource management
// ============================================================
using ocarina::Managed;
using ocarina::RegistrableBuffer;
using ocarina::RegistrableManaged;
using ocarina::RegistrableTexture3D;
using ocarina::RegistrableCountedBuffer;
using ocarina::EncodedData;
using ocarina::BufferDesc;

// ============================================================
// Other types
// ============================================================
using ocarina::Type;
using ocarina::RHIResource;
using ocarina::array_float3;
using ocarina::type_dimension_v;
using ocarina::remove_device_t;
using ocarina::Env;
using ocarina::Container;
using ocarina::next_pow2;
using ocarina::Ulong;
using ocarina::oc_ulong;
using ocarina::Box2u;
using ocarina::cast;
using ocarina::as;
using ocarina::outline;
using ocarina::comment;
namespace concepts = ocarina::concepts;

}// namespace vision
