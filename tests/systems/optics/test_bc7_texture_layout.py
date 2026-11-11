"""CPU checks of production BC7 byte layouts and texture transfer accessors.

Extracts the real format enum/helpers and command accessors; no texture codec,
engine build or GPU is used. Legacy enum IDs and per-slice transfer semantics
are checked alongside BC7's 4x4, 16-byte block layout.
"""
from functools import lru_cache
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from unittest import SkipTest


ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'Horizon/src').is_dir())
TEMP = ROOT / '.workspace/tmp/rtxpt-corona-dsl-20261108/bc7-layout-tests'


def _definition(source, signature, semicolon=False):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end] + (';' if semicolon else '')


def _production():
    image = (ROOT / 'Horizon/src/core/image/image_base.h').read_text(encoding='utf-8-sig')
    command = (ROOT / 'Horizon/src/rhi/command.h').read_text(encoding='utf-8-sig')
    enum = _definition(image, 'enum struct PixelStorage : uint', True)
    helpers = image[image.index('OC_NDSC_INLINE'):image.index('class OC_CORE_API ImageBase')]
    image_accessors = '\n'.join(_definition(image, signature) for signature in (
        '[[nodiscard]] size_t pitch_byte_size()', '[[nodiscard]] size_t size_in_bytes()'))
    return (enum + '\n' + helpers, image_accessors,
            _definition(command, 'class BufferToTextureCommand :', True),
            _definition(command, 'class OC_RHI_API TextureOpCommand :', True))


PROGRAM = r'''
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#define OC_NDSC_INLINE [[nodiscard]] inline
#define OC_RHI_API
#define OC_ASSERT(condition) assert(condition)
using uint = std::uint32_t;
using uchar = std::uint8_t;
using handle_ty = std::uintptr_t;
template<class T> struct Vec2 { T x{}, y{}; };
template<class T> struct Vec4 { T x{}, y{}, z{}, w{}; };
using uchar2 = Vec2<uchar>; using uchar4 = Vec4<uchar>;
using uint2 = Vec2<uint>; using uint4 = Vec4<uint>;
using float2 = Vec2<float>; using float4 = Vec4<float>;
struct uint3 { uint x{}, y{}, z{}; };
namespace ocarina {
__FORMATS__
struct ImageProbe {
    uint2 resolution_;
    PixelStorage pixel_storage_;
    size_t pixel_num() const { return size_t(resolution_.x) * resolution_.y; }
    uint width() const { return resolution_.x; }
    uint height() const { return resolution_.y; }
    __IMAGE_ACCESSORS__
};
struct DataCopyCommand { DataCopyCommand(handle_ty, handle_ty, bool) {} };
struct DataOpCommand { DataOpCommand(handle_ty, handle_ty, size_t, bool) {} };
__BUFFER_COMMAND__
__TEXTURE_COMMAND__
struct TextureProbe : TextureOpCommand {
    TextureProbe(uint3 resolution, PixelStorage storage)
        : TextureOpCommand(0u, 0u, resolution, storage, true) {}
};
}
using namespace ocarina;
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
size_t tiled_bytes(uint width, uint height) {
    // Count physical 16-byte tiles, including partial right/bottom tiles.
    size_t result = 0u;
    for (uint y = 0u; y < height; y += 4u)
        for (uint x = 0u; x < width; x += 4u) result += 16u;
    return result;
}
void verify(PixelStorage storage, uint width, uint height, size_t row, size_t slice) {
    TextureProbe texture{{width, height, 1u}, storage};
    BufferToTextureCommand buffer{0u, 37u, 0u, storage, {width, height, 1u}, 3u, true};
    ImageProbe image{{width, height}, storage};
    require(texture.width_in_bytes() == row, "texture host transfer row pitch is incorrect");
    require(texture.size_in_bytes() == slice, "texture host transfer slice size is incorrect");
    require(buffer.width_in_bytes() == row, "buffer-to-texture row pitch disagrees");
    require(buffer.size_in_bytes() == slice, "buffer-to-texture slice size disagrees");
    require(buffer.buffer_offset() == 37u && buffer.level() == 3u, "transfer offset/mip metadata changed");
    require(texture_row_bytes(storage, width) == row && texture_size_bytes(storage, width, height) == slice,
            "shared texture byte helpers disagree with transfer layout");
    // CPU Image remains decoded; compressed bytes enter only the texture upload path.
    if (!is_block_compressed(storage))
        require(image.pitch_byte_size() == row && image.size_in_bytes() == slice, "legacy CPU ImageBase layout changed");
}
void enum_test() {
    const std::array<PixelStorage, 17> legacy{
        PixelStorage::BYTE1, PixelStorage::BYTE2, PixelStorage::BYTE4,
        PixelStorage::UINT1, PixelStorage::UINT2, PixelStorage::UINT4,
        PixelStorage::FLOAT1, PixelStorage::FLOAT2, PixelStorage::FLOAT4,
        PixelStorage::UNKNOWN, PixelStorage::UNORM1, PixelStorage::UNORM2,
        PixelStorage::HALF1, PixelStorage::HALF4, PixelStorage::USHORT1,
        PixelStorage::RGB10A2, PixelStorage::SNORM4};
    for (uint i = 0u; i < legacy.size(); ++i)
        require(static_cast<uint>(legacy[i]) == i, "BC7 insertion changed a legacy serialized enum ID");
    for (auto storage : legacy) require(!is_block_compressed(storage), "legacy format became block-compressed");
    require(static_cast<uint>(PixelStorage::BC7_UNORM) == 17u, "BC7_UNORM must append after legacy values");
    require(static_cast<uint>(PixelStorage::BC7_SRGB) == 18u, "BC7_SRGB must have its own appended ID");
    require(is_block_compressed(PixelStorage::BC7_UNORM) && is_block_compressed(PixelStorage::BC7_SRGB),
            "both BC7 color spaces must use block layout");
    require(format_size_in_bytes(PixelStorage::UNKNOWN) == 0u, "unknown format size changed");
    std::cout << "PASS all 17 legacy enum IDs and distinct appended BC7 formats\n";
}
void blocks_test() {
    unsigned cases = 0;
    for (auto storage : {PixelStorage::BC7_UNORM, PixelStorage::BC7_SRGB}) {
        for (uint height = 1u; height <= 17u; ++height)
            for (uint width = 1u; width <= 17u; ++width) {
                verify(storage, width, height, tiled_bytes(width, 1u), tiled_bytes(width, height));
                ++cases;
            }
        for (auto size : {uint2{127u, 255u}, uint2{1023u, 513u}, uint2{4095u, 2049u},
                          uint2{1u, 4097u}, uint2{4097u, 1u}}) {
            verify(storage, size.x, size.y, tiled_bytes(size.x, 1u), tiled_bytes(size.x, size.y));
            ++cases;
        }
    }
    std::cout << "PASS " << cases << " BC7 layouts including all small dimensions and NPOT edges\n";
}
void mips_test() {
    struct Case { uint width, height, levels; size_t bytes; };
    for (auto storage : {PixelStorage::BC7_UNORM, PixelStorage::BC7_SRGB}) {
        for (const auto test : std::array<Case, 7>{{
                {1u, 1u, 1u, 16u}, {4u, 4u, 3u, 48u}, {8u, 8u, 4u, 112u},
                {5u, 3u, 3u, 64u}, {7u, 9u, 4u, 144u}, {1u, 17u, 5u, 160u},
                {17u, 9u, 5u, 320u}}}) {
            uint width = test.width, height = test.height, levels = 0u;
            size_t bytes = 0u;
            for (;;) {
                TextureProbe level{{width, height, 1u}, storage};
                bytes += level.size_in_bytes();
                ++levels;
                if (width == 1u && height == 1u) break;
                width = width > 1u ? width / 2u : 1u;
                height = height > 1u ? height / 2u : 1u;
            }
            require(levels == test.levels && bytes == test.bytes, "NPOT BC7 mip chain lost partial blocks or small tail levels");
        }
    }
    std::cout << "PASS 14 BC7 mip chains with floor-halved extents and complete 16-byte tails\n";
}
void legacy_test() {
    struct Format { PixelStorage storage; uint bytes; };
    for (const auto format : std::array<Format, 16>{{
            {PixelStorage::BYTE1, 1u}, {PixelStorage::BYTE2, 2u}, {PixelStorage::BYTE4, 4u},
            {PixelStorage::UINT1, 4u}, {PixelStorage::UINT2, 8u}, {PixelStorage::UINT4, 16u},
            {PixelStorage::FLOAT1, 4u}, {PixelStorage::FLOAT2, 8u}, {PixelStorage::FLOAT4, 16u},
            {PixelStorage::UNORM1, 1u}, {PixelStorage::UNORM2, 2u}, {PixelStorage::HALF1, 2u},
            {PixelStorage::HALF4, 8u}, {PixelStorage::USHORT1, 2u}, {PixelStorage::RGB10A2, 4u},
            {PixelStorage::SNORM4, 4u}}}) {
        require(pixel_size(format.storage) == format.bytes, "legacy pixel stride changed");
        require(format_size_in_bytes(format.storage) == format.bytes, "legacy format stride changed");
        for (uint width : {1u, 3u, 4u, 5u, 17u}) for (uint height : {1u, 3u, 7u, 19u})
            verify(format.storage, width, height, size_t(width) * format.bytes,
                   size_t(width) * height * format.bytes);
        TextureProbe volume{{5u, 7u, 3u}, format.storage};
        BufferToTextureCommand buffer{0u, 0u, 0u, format.storage, {5u, 7u, 3u}, 0u, true};
        require(volume.depth() == 3u && buffer.depth() == 3u, "volume depth metadata changed");
        // The Vulkan 3D upload visitor multiplies this slice byte count by depth.
        require(volume.size_in_bytes() == 35u * format.bytes && buffer.size_in_bytes() == 35u * format.bytes,
                "command slice size started including depth and would double-count 3D uploads");
        require(texture_size_bytes(format.storage, 5u, 7u, 3u) == 105u * format.bytes,
                "whole-resource helper must retain the separate volume depth factor");
    }
    std::cout << "PASS all 16 uncompressed formats, 320 layouts and per-slice volume command ABI\n";
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "one fixture name is required");
        const std::string name = argv[1];
        if (name == "enum") enum_test();
        else if (name == "blocks") blocks_test();
        else if (name == "mips") mips_test();
        else if (name == "legacy") legacy_test();
        else throw std::runtime_error("unknown fixture");
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
'''


@lru_cache(maxsize=1)
def _executable():
    formats, image, buffer, texture = _production()
    compiler = shutil.which('g++')
    if not compiler:
        raise SkipTest('g++ is required for the standalone BC7 layout fixture')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='run-', dir=TEMP))
    source = directory / 'bc7_texture_layout.cpp'
    executable = directory / 'bc7_texture_layout.exe'
    program = PROGRAM.replace('__FORMATS__', formats).replace('__IMAGE_ACCESSORS__', image)
    program = program.replace('__BUFFER_COMMAND__', buffer).replace('__TEXTURE_COMMAND__', texture)
    source.write_text(program, encoding='utf-8')
    result = subprocess.run(
        [compiler, '-std=c++20', '-O2', str(source), '-o', str(executable)],
        text=True, capture_output=True, check=False,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return executable


def _run(fixture):
    result = subprocess.run([str(_executable()), fixture], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')


def test_bc7_formats_append_without_renumbering_existing_storage():
    _run('enum')


def test_bc7_small_and_npot_dimensions_cover_complete_blocks():
    _run('blocks')


def test_bc7_npot_mip_chains_keep_all_small_tail_blocks():
    _run('mips')


def test_uncompressed_layout_and_volume_command_semantics_are_unchanged():
    _run('legacy')


def test_bindless_bc7_route_preserves_descriptor_index_and_legacy_handle_path():
    rhi = (ROOT / 'Horizon/src/rhi/resources/bindless_array.cpp').read_text(encoding='utf-8-sig')
    vulkan = (ROOT / 'Horizon/src/backends/vulkan_compute/vk_bindless_array.cpp').read_text(encoding='utf-8-sig')
    texture = (ROOT / 'Horizon/src/backends/vulkan_compute/vk_texture.cpp').read_text(encoding='utf-8-sig')
    slang = (ROOT / 'Horizon/src/backends/vulkan_compute/builtin/slang/slang_device_math.slang').read_text(encoding='utf-8-sig')
    methods = '\n'.join((
        _definition(vulkan, 'size_t VkBindlessArray::emplace_texture3d(TextureDesc desc)'),
        _definition(vulkan, 'void VkBindlessArray::set_texture3d(handle_ty index, TextureDesc desc)'),
        _definition(rhi, 'size_t BindlessArray::emplace(const Texture3D &texture)'),
        _definition(rhi, 'void BindlessArray::set_texture3d(ocarina::handle_ty index,'),
    ))
    vk_format = _definition(texture, 'VkFormat pixel_storage_to_vk_format(')
    format_names = list(dict.fromkeys(re.findall(r'VK_FORMAT_[A-Z0-9_]+', vk_format)))
    vk_enum = 'enum VkFormat {' + ','.join(format_names) + '};'
    shader_decode = _definition(slang, 'float[N] oc_bindless_array_tex3d_sample<uint N>')
    # Preserve the actual shader descriptor-decoding body. Replace only its
    # Slang generic signature/result with a C++ descriptor-returning sample spy.
    shader_decode = ('template<unsigned N> OCTextureDesc decode_route(OCBindlessArrayDesc arr, '
                     'uint slot, float u, float v, float w = 0.f) ' +
                     shader_decode[shader_decode.index('{'):])
    route_program = PROGRAM.split('namespace ocarina {')[0] + r'''
namespace ocarina {
__FORMATS__
using ::handle_ty;
struct TextureDesc { handle_ty texture{}, surface{}; PixelStorage pixel_storage{}; };
struct TextureState {
    TextureDesc desc;
    mutable unsigned descriptor_reads{};
    PixelStorage pixel_storage() const { return desc.pixel_storage; }
    const TextureDesc &descriptor() const { ++descriptor_reads; return desc; }
};
struct Texture3D {
    TextureState state;
    mutable unsigned handle_reads{};
    const TextureState *impl() const { return &state; }
    handle_ty tex_handle() const { ++handle_reads; return state.desc.texture; }
};
struct VkBindlessArray {
    uint32_t packet{};
    handle_ty updated_slot{};
    size_t emplace_texture3d(handle_ty handle) noexcept { packet = uint32_t(handle); return 7u; }
    size_t emplace_texture3d(TextureDesc desc) noexcept;
    void set_texture3d(handle_ty slot, handle_ty handle) noexcept { updated_slot = slot; packet = uint32_t(handle); }
    void set_texture3d(handle_ty index, TextureDesc desc) noexcept;
};
struct BindlessArray {
    VkBindlessArray backend;
    VkBindlessArray *impl() { return &backend; }
    size_t emplace(const Texture3D &texture) noexcept;
    void set_texture3d(handle_ty index, const Texture3D &texture) noexcept;
};
__METHODS__
__VK_ENUM__
__VK_FORMAT__
struct OCTextureDesc { uint64_t tex_index, surf_index; uint pixel_storage, padding; };
struct OCBindlessArrayDesc { uintptr_t tex3d_slot; };
template<unsigned N> OCTextureDesc oc_tex3d_sample_float(OCTextureDesc desc, float, float, float) { return desc; }
__SHADER_DECODE__
}
using namespace ocarina;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        for (auto format : {PixelStorage::BC7_UNORM, PixelStorage::BC7_SRGB,
                            PixelStorage::BYTE4, PixelStorage::FLOAT4, PixelStorage::HALF4}) {
            const bool compressed = format == PixelStorage::BC7_UNORM || format == PixelStorage::BC7_SRGB;
            for (handle_ty index : {0u, 32768u, 49999u, 50000u}) {
                Texture3D image{{{index, 123u, format}}};
                BindlessArray array;
                require(array.emplace(image) == 7u, "RHI changed the backend's returned slot");
                require(image.state.descriptor_reads == unsigned(compressed) &&
                        image.handle_reads == unsigned(!compressed), "RHI dropped BC7 metadata or changed the legacy raw-handle path");
                const uint32_t expected = uint32_t(index) | (compressed ? 0x80000000u : 0u);
                require(array.backend.packet == expected, "BC7 route bit/index was not preserved during emplace");
                array.set_texture3d(37u, image);
                require(array.backend.updated_slot == 37u && array.backend.packet == expected,
                        "BC7 update changed the destination slot or lost its route tag");
                require(image.state.descriptor_reads == 2u * unsigned(compressed) &&
                        image.handle_reads == 2u * unsigned(!compressed), "update used a different metadata route from emplace");
                uint32_t packets[]{0u, expected, 0u};
                OCBindlessArrayDesc desc{reinterpret_cast<uintptr_t>(packets)};
                const auto decoded = decode_route<4>(desc, 1u, .2f, .7f);
                require(decoded.tex_index == index && decoded.surf_index == index,
                        "actual shader route decoder truncated an index above 32767");
                require(decoded.pixel_storage == (compressed ? 17u : 0u), "shader route decoder failed to select sampled-2D BC7 path");
                // Replacing BC7 with ordinary data must clear its previous route bit.
                Texture3D ordinary{{{index, 0u, PixelStorage::FLOAT4}}};
                array.set_texture3d(37u, ordinary);
                require(array.backend.packet == index, "updating to an ordinary texture retained a stale BC7 tag");
            }
        }
        require(pixel_storage_to_vk_format(PixelStorage::BC7_UNORM) == VK_FORMAT_BC7_UNORM_BLOCK,
                "BC7 linear texture mapped to the wrong Vulkan image format");
        require(pixel_storage_to_vk_format(PixelStorage::BC7_SRGB) == VK_FORMAT_BC7_SRGB_BLOCK,
                "BC7 sRGB image format was lost before hardware decode");
        require(VK_FORMAT_BC7_UNORM_BLOCK != VK_FORMAT_BC7_SRGB_BLOCK, "format-map fixture requires distinct color spaces");
        std::cout << "PASS production RHI/Vulkan emplace/update, shader route decode up to index50000 and BC7 format maps\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
'''
    route_program = route_program.replace('__FORMATS__', _production()[0]).replace('__METHODS__', methods)
    route_program = route_program.replace('__VK_ENUM__', vk_enum).replace('__VK_FORMAT__', vk_format)
    route_program = route_program.replace('__SHADER_DECODE__', shader_decode)
    compiler = shutil.which('g++')
    if not compiler:
        raise SkipTest('g++ is required for the standalone BC7 route fixture')
    TEMP.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='route-', dir=TEMP))
    source = directory / 'bc7_bindless_route.cpp'
    executable = directory / 'bc7_bindless_route.exe'
    source.write_text(route_program, encoding='utf-8')
    result = subprocess.run([compiler, '-std=c++20', '-O2', str(source), '-o', str(executable)],
                            text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(executable)], text=True, capture_output=True, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')
