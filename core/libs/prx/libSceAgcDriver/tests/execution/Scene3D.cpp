#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 192;
constexpr std::uint32_t Height = 128;
constexpr std::array<std::uint8_t, 4> Background{16, 24, 40, 255};

alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};

// The driver caches one depth surface per guest address and clears it only when the surface is
// created, so a scenario cannot be reset by rewriting memory: it has to be handed a fresh address.
// Each scenario therefore draws into its own slot, and the draws within a scenario share the slot so
// they compare against each other's depth.
constexpr std::size_t DepthSlots = 24;
constexpr std::size_t DepthTexels = Width * Height;
alignas(256) std::array<float, DepthSlots * DepthTexels> Depth{};

// Near and far are far enough apart that no rounding can swap their order, and both sit inside the
// [0, 1] range that negativeOneToOne = false clips to.
constexpr float NearZ = 0.25f;
constexpr float FarZ = 0.75f;

alignas(256) constexpr std::array<std::uint32_t, 104> GeometryCode{
    0x8f6a9003, 0x94fe6ac1, 0xbf88000b, 0xd7650006, 0x000100c1, 0xd7660006, 0x00020cc1, 0x93ebff03,
    0x00040018, 0xd7460006, 0x04190c6b, 0x340c0c82, 0xd8340000, 0x00000506, 0xbf8cc07f, 0xbefe04c1,
    0xbf8a0000, 0x938dff02, 0x00090016, 0x9382ff03, 0x00040018, 0x938cff03, 0x00080008, 0xd7650009,
    0x000100c1, 0xd7660009, 0x000212c1, 0xd746000a, 0x04250c02, 0x7da8120c, 0xbf88002e, 0x361600ff,
    0x0000ffff, 0x2c180090, 0x361a02ff, 0x0000ffff, 0xd8d80000, 0x0b00000b, 0xd8d80000, 0x0c00000c,
    0xd8d80000, 0x0d00000d, 0xbf8cc07f, 0xe0382000, 0x8002100b, 0xe0382010, 0x8002140b, 0xe0382000,
    0x8002180c, 0xe0382010, 0x80021c0c, 0xe0382000, 0x8002200d, 0xe0382010, 0x8002240d, 0x161c1483,
    0x161e14ff, 0x00000060, 0xbf8c3f70, 0xdb7c0400, 0x0000100f, 0xdb7c0410, 0x0000140f, 0xdb7c0420,
    0x0000180f, 0xdb7c0430, 0x00001c0f, 0xdb7c0440, 0x0000200f, 0xdb7c0450, 0x0000240f, 0x4a501c81,
    0x4a521c82, 0x3450508a, 0x34525294, 0xd772002a, 0x04a6510e, 0xbf8cc07f, 0xbefe04c1, 0xbf8a0000,
    0x930e830d, 0xbf078002, 0xbf850003, 0x8f0f8c0d, 0x887c0f0e, 0xbf900009, 0x7da8140d, 0xbf880002,
    0xf8000941, 0x0000002a, 0xbefe04c1, 0x7da8140e, 0xbf88000a, 0x34561485, 0xdbfc0400, 0x2c00002b,
    0xdbfc0410, 0x3000002b, 0xbf8cc07f, 0xf80008cf, 0x2f2e2d2c, 0xf800020f, 0x33323130, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000,
};

constexpr ShaderRecompiler::MeshConfiguration MeshSetup{4u, 32u, 96u, 96u, 32u, 128u, 2048u, 0u, 4u};

struct Vertex {
    std::array<float, 4> position;
    std::array<float, 4> color;
};

std::array<std::uint8_t, 4> Rgb(std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b), 255};
}

const std::array<std::uint8_t, 4> NearColor = Rgb(220, 40, 40);
const std::array<std::uint8_t, 4> FarColor = Rgb(40, 220, 40);

// Only the color target lives in host memory; the depth target is a device-local image the driver
// owns, reset by giving the next scenario a different address rather than by writing memory.
void ClearTarget() {
    for (std::size_t i = 0; i < Pixels.size(); i += 4) {
        for (std::size_t c = 0; c < 4; ++c) Pixels[i + c] = std::byte{Background[c]};
    }
}

bool PixelIs(std::size_t offset, const std::array<std::uint8_t, 4>& color) {
    for (std::size_t c = 0; c < 4; ++c) {
        if (std::to_integer<std::uint8_t>(Pixels[offset + c]) != color[c]) return false;
    }
    return true;
}

std::string PixelText(std::size_t offset) {
    char text[64];
    std::snprintf(text, sizeof(text), "(%u, %u, %u, %u)", std::to_integer<unsigned>(Pixels[offset]), std::to_integer<unsigned>(Pixels[offset + 1]), std::to_integer<unsigned>(Pixels[offset + 2]), std::to_integer<unsigned>(Pixels[offset + 3]));
    return text;
}

std::size_t PixelOffset(std::uint32_t x, std::uint32_t y) {
    return (static_cast<std::size_t>(y) * Width + x) * 4u;
}

std::array<std::uint32_t, 4> VertexBufferDescriptor(const void* vertices, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(vertices);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (32u << 16u), count, 0x01016facu};
}

struct RasterState {
    bool depthTest = true;
    bool depthWrite = true;
    VkCompareOp compare = VK_COMPARE_OP_LESS;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool depthAttachment = true;
    // Which depth slot to bind. Draws that must see each other's depth share a slot.
    std::size_t depthSlot = 0;
};

// Draws the given triangles once, with the depth/cull state under test.
void Draw(AgcDriver::VulkanDevice& device, const std::vector<Vertex>& triangles, const RasterState& raster, float clearDepthForThisDraw = 1.0f) {
    const auto target = device.Target();
    const auto* vertices = triangles.data();
    std::vector<std::uint32_t> userData(12, 0u);
    const auto vertexBuffer = VertexBufferDescriptor(vertices, static_cast<std::uint32_t>(triangles.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), userData.begin() + 8);
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(triangles.size()), 0, 1, 0, false};
    const auto index = AgcDriver::Graphics::MeshIndexBufferDescriptor(draw, reinterpret_cast<std::uintptr_t>(GeometryCode.data()));
    std::copy(index.begin(), index.end(), userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord);
    const std::array<ShaderRecompiler::MemoryRegion, 1> geometryMemory{{{reinterpret_cast<std::uintptr_t>(GeometryCode.data()), std::as_bytes(std::span(GeometryCode))}}};
    ShaderRecompiler::RecompileRequest geometry{
        {ShaderStage::Mesh, reinterpret_cast<std::uintptr_t>(GeometryCode.data()), GeometryCode, 0, {}},
        {64, 0, userData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, geometryMemory},
        target,
        {0, 0, 0, ShaderRecompiler::MeshDrawPushOffsetBytes},
        ShaderRecompiler::GraphicsCompileContext{0, {}, MeshSetup, std::nullopt, {0, static_cast<std::uint32_t>(triangles.size()), 0, 1}}
    };
    const auto meshResult = ShaderRecompiler::Recompile(geometry);
    const auto meshPush = static_cast<std::uint32_t>(meshResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo pixel{};
    pixel.interpolatorCount = 1;
    pixel.interpolatorSettings[0] = 0x400u;
    pixel.targetOutputMode[0] = 9;
    pixel.targetExportMapping.fill(0xe4u);
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        {0, 0, meshPush, ShaderRecompiler::MeshDrawPushOffsetBytes - meshPush},
        std::nullopt
    };
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Mesh, &meshResult, 0},
        {ShaderStage::Fragment, &pixelResult, meshPush}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Geometry, 0x20u, 64, 64, MeshSetup, std::nullopt};
    if (raster.depthAttachment) state.depth = AgcDriver::Graphics::DepthTarget{reinterpret_cast<std::uintptr_t>(Depth.data() + raster.depthSlot * DepthTexels), 0, {Width, Height}, VK_FORMAT_D32_SFLOAT, clearDepthForThisDraw, 0};
    state.depthTest = raster.depthTest;
    state.depthWrite = raster.depthWrite;
    state.depthCompare = raster.compare;
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = raster.cull;
    state.frontFace = raster.front;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

// A full-target triangle whose winding in window space is the given orientation. `z` is the
// clip-space depth handed to the rasterizer, so overlapping triangles at different z let the
// depth test decide which one survives.
std::vector<Vertex> Fullscreen(float z, const std::array<std::uint8_t, 4>& color, bool counterClockwise) {
    const std::array<float, 4> rgba{color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, 1.0f};
    const float inset = 0.9f;
    // The viewport flips y (negative height), so a counter-clockwise triangle in NDC becomes
    // clockwise on screen. Both windings are spelled out instead of assumed.
    const std::array<std::array<float, 2>, 3> points = counterClockwise
        ? std::array<std::array<float, 2>, 3>{{{-inset, -inset}, {inset, -inset}, {0.0f, inset}}}
        : std::array<std::array<float, 2>, 3>{{{-inset, -inset}, {0.0f, inset}, {inset, -inset}}};
    std::vector<Vertex> triangles(3);
    for (std::size_t i = 0; i < 3; ++i) {
        triangles[i].position = {points[i][0], points[i][1], z, 1.0f};
        triangles[i].color = rgba;
    }
    return triangles;
}

std::size_t CenterOffset() {
    return PixelOffset(Width / 2u, Height / 2u);
}

void RequireCenter(const char* what, const std::array<std::uint8_t, 4>& expected) {
    const auto offset = CenterOffset();
    Require(PixelIs(offset, expected), std::string(what) + ": center is " + PixelText(offset) + ", expected (" + std::to_string(expected[0]) + ", " + std::to_string(expected[1]) + ", " + std::to_string(expected[2]) + ", " + std::to_string(expected[3]) + ")");
}

} // namespace


int main() {
    try {
#ifdef _WIN32
        Require(_putenv_s("APS5_NO_SHADER_DISK_CACHE", "1") == 0, "cannot set APS5_NO_SHADER_DISK_CACHE");
#else
        Require(::setenv("APS5_NO_SHADER_DISK_CACHE", "1", 1) == 0, "cannot set APS5_NO_SHADER_DISK_CACHE");
#endif
        const auto device = OpenVulkanTestDevice();
        if (device == nullptr) return VulkanTestSkipped;
        if (!device->Target().mesh.has_value()) {
            std::puts("Scene3D tests skipped: the device has no VK_EXT_mesh_shader");
            return VulkanTestSkipped;
        }

        // A lone triangle must land; every later assertion depends on the rasterizer working.
        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {});
        RequireCenter("a lone triangle", NearColor);

        // Each scenario below uses its own depth slot, so it starts from a depth buffer freshly
        // cleared to 1.0 and cannot be contaminated by the previous one.

        // Depth: a near triangle drawn first must survive a far one drawn over it.
        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 1});
        Draw(*device, Fullscreen(FarZ, FarColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 1});
        RequireCenter("near before far, LESS", NearColor);

        // ... and the same two triangles in the other order must still keep the near one, which is
        // only true if depth is compared rather than merely written.
        ClearTarget();
        Draw(*device, Fullscreen(FarZ, FarColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 2});
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 2});
        RequireCenter("far before near, LESS", NearColor);

        // With the test off the far triangle wins, proving the depth buffer is what decided the
        // two cases above rather than draw order alone.
        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {false, true, VK_COMPARE_OP_ALWAYS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 3});
        Draw(*device, Fullscreen(FarZ, FarColor, true), {false, true, VK_COMPARE_OP_ALWAYS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 3});
        RequireCenter("depth test disabled", FarColor);

        // GREATER flips the winner: only a real depth comparison explains this. It needs a 0.0 clear,
        // since GREATER against a 1.0 clear rejects every fragment.
        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, true, VK_COMPARE_OP_GREATER, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 4}, 0.0f);
        Draw(*device, Fullscreen(FarZ, FarColor, true), {true, true, VK_COMPARE_OP_GREATER, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 4}, 0.0f);
        RequireCenter("near before far, GREATER", FarColor);

        // Read-only depth (test on, write off) must not let the far triangle occlude the near one
        // across draws, because nothing was stored to compare against.
        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, false, VK_COMPARE_OP_LESS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 5});
        Draw(*device, Fullscreen(FarZ, FarColor, true), {true, false, VK_COMPARE_OP_LESS, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 5});
        RequireCenter("depth write disabled", FarColor);

        // Culling: a counter-clockwise triangle survives BACK culling and dies under FRONT.
        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 6});
        RequireCenter("counter-clockwise with back-face culling", NearColor);

        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, true, 7});
        RequireCenter("counter-clockwise with front-face culling", Background);

        // frontFace must actually reach the rasterizer: flipping it turns the same geometry from
        // front-facing into back-facing, so BACK culling now discards it.
        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE, true, 8});
        RequireCenter("frontFace flipped with back-face culling", Background);

        ClearTarget();
        Draw(*device, Fullscreen(NearZ, NearColor, true), {true, true, VK_COMPARE_OP_LESS, VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, true, 9});
        RequireCenter("frontFace flipped without culling", NearColor);

        std::puts("Scene3D tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
