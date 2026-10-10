//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/host_passes/vr_overlay_pass.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "common/assert.h"
#include "video_core/host_shaders/fs_tri_vert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan::HostPasses {

namespace {

/// What every draw of the pass is told (push constants, at most 128 bytes).
struct PushData {
    std::array<float, 4> rect;   ///< the part of the frame drawn to: x, y, width, height
    std::array<float, 4> fov;    ///< middle x, y (-1..1), inner ring, outer ring
    std::array<float, 4> extra;  ///< tint alpha, outer ring 4x4, gaze mark, 0
    u32 mode;                    ///< 0: foveation tint, 1: a line of text
    u32 count;                   ///< characters in `text`
    u32 scale;                   ///< pixels of the frame for one of the font
    u32 pad;
    std::array<u32, 12> text;    ///< up to 48 characters, four to a word
};
static_assert(sizeof(PushData) == 112);

constexpr u32 MaxLine = 48;
constexpr u32 GlyphWidth = 8;
constexpr u32 GlyphHeight = 16;

// The font: DejaVu Sans Mono Bold (Bitstream Vera license, DejaVu changes public domain), made
// into 8x16 pixels for the characters 32 to 126; four rows of eight pixels to a word, the
// first row in the lowest byte, the leftmost pixel in the lowest bit.
constexpr const char* OverlayFrag = R"GLSL(
#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(push_constant) uniform PushData {
    vec4 rect;
    vec4 fov;
    vec4 extra;
    uint mode;
    uint count;
    uint scale;
    uint pad;
    uvec4 text[3];
} pc;

const uint FONT[380] = uint[](
@FONT@
);

uint CharAt(uint i) {
    uint word = pc.text[i / 16u][(i / 4u) % 4u];
    return (word >> ((i % 4u) * 8u)) & 0xFFu;
}

void main() {
    vec2 local = gl_FragCoord.xy - pc.rect.xy;
    if (pc.mode == 0u) {
        // The eye's picture, -1..1 each way; the rings are ellipses over it, as the shading
        // rate map's are.
        vec2 p = local / pc.rect.zw * 2.0 - 1.0;
        float d = length(p - pc.fov.xy);
        vec3 tint;
        if (d < pc.fov.z) {
            tint = vec3(0.0, 1.0, 0.2);
        } else if (d < pc.fov.w || pc.extra.y < 0.5) {
            tint = vec3(1.0, 0.85, 0.0);
        } else {
            tint = vec3(1.0, 0.1, 0.1);
        }
        float alpha = pc.extra.x;
        // A thin line where the rate changes.
        float edge = min(abs(d - pc.fov.z), abs(d - pc.fov.w));
        if (edge < 0.006) {
            alpha = min(1.0, alpha * 3.0);
        }
        // Where the eyes look: a small white ring.
        if (pc.extra.z > 0.5) {
            vec2 to_gaze = (p - pc.fov.xy) * 0.5 * pc.rect.zw;
            float r = length(to_gaze);
            if (r > 9.0 && r < 13.0) {
                color = vec4(1.0, 1.0, 1.0, 0.9);
                return;
            }
        }
        color = vec4(tint, alpha);
        return;
    }
    // A line of text on a dark band.
    uvec2 cell = uvec2(local) / pc.scale;
    uint index = cell.x / 8u;
    uint column = cell.x % 8u;
    uint row = cell.y;
    if (index < pc.count && row < 16u) {
        uint ch = CharAt(index);
        if (ch >= 32u && ch < 127u) {
            uint word = FONT[(ch - 32u) * 4u + row / 4u];
            uint bits = (word >> ((row % 4u) * 8u)) & 0xFFu;
            if (((bits >> column) & 1u) != 0u) {
                color = vec4(1.0, 1.0, 1.0, 1.0);
                return;
            }
        }
    }
    color = vec4(0.0, 0.0, 0.0, 0.6);
}
)GLSL";

} // namespace

void VrOverlayPass::Create(const Instance& instance, vk::Format format) {
    device = instance.GetDevice();
    // SHADPS4_VR_OVERLAY_SCALE=<1..4>: pixels of the frame for one of the font (2 unless told).
    if (const char* value = std::getenv("SHADPS4_VR_OVERLAY_SCALE"); value != nullptr) {
        scale = std::clamp(static_cast<u32>(std::atoi(value)), 1u, 4u);
    }
    if (const char* value = std::getenv("SHADPS4_VR_OVERLAY_POS"); value != nullptr) {
        char* end = nullptr;
        position_x = std::clamp(std::strtof(value, &end), 0.0f, 1.0f);
        if (end != nullptr && *end == ',') {
            position_y = std::clamp(std::strtof(end + 1, nullptr), 0.0f, 1.0f);
        }
    }

    std::string frag_source{OverlayFrag};
    const std::string font_marker = "@FONT@";
    frag_source.replace(frag_source.find(font_marker), font_marker.size(), R"FONT(
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x18180000u, 0x18181818u, 0x18000018u, 0x00000018u,
    0x66240000u, 0x00006666u, 0x00000000u, 0x00000000u, 0x48000000u, 0x24fefe68u, 0x12367f36u, 0x00000012u,
    0x18000000u, 0x1e1e7e7cu, 0x7e7a787cu, 0x0000183cu, 0x0e000000u, 0x304f090bu, 0xf090f066u, 0x00000060u,
    0x3e3c0000u, 0x1e0c0e06u, 0xfff3f3fbu, 0x000000feu, 0x18180000u, 0x00001818u, 0x00000000u, 0x00000000u,
    0x18300000u, 0x0c1c1818u, 0x18180c0cu, 0x00203018u, 0x0c0c0000u, 0x38381818u, 0x18183830u, 0x00040c18u,
    0x5a180000u, 0x5a7e3c7eu, 0x00000018u, 0x00000000u, 0x00000000u, 0x7f181808u, 0x181818ffu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x18180000u, 0x000c0c18u, 0x00000000u, 0x00000000u, 0x00003c3cu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x18180000u, 0x00000018u, 0x60400000u, 0x18303060u, 0x040c0818u, 0x00000206u,
    0x7e1c0000u, 0x7f666666u, 0x7e666666u, 0x0000003cu, 0x1e180000u, 0x1818181eu, 0xfe181818u, 0x0000007eu,
    0x7e1e0000u, 0x30606072u, 0x7f060c18u, 0x0000007eu, 0x7e1e0000u, 0x3c786060u, 0x7f606070u, 0x0000003eu,
    0x70300000u, 0x76747c78u, 0x707eff73u, 0x00000020u, 0x7e3e0000u, 0x7e3e0606u, 0x7e606060u, 0x0000003eu,
    0x7c380000u, 0x7e3e060eu, 0x7e66e666u, 0x0000003cu, 0x7e7e0000u, 0x30307070u, 0x0c1c1818u, 0x0000000cu,
    0x7e3c0000u, 0x3c7e6666u, 0x7e666666u, 0x0000003cu, 0x7e1c0000u, 0x7e676766u, 0x3e60607eu, 0x0000001eu,
    0x00000000u, 0x18181800u, 0x18180000u, 0x00000018u, 0x00000000u, 0x18181800u, 0x18180000u, 0x000c1c18u,
    0x00000000u, 0x1e78e000u, 0xc0f83e07u, 0x00000000u, 0x00000000u, 0x7eff0000u, 0x00ffff00u, 0x00000000u,
    0x00000000u, 0x781e0700u, 0x030f7ce0u, 0x00000000u, 0x7e3c0000u, 0x18306060u, 0x18001818u, 0x00000018u,
    0x38000000u, 0xd9fbc27eu, 0xfbc9cdcdu, 0x007c4e03u, 0x3c180000u, 0x663c3c3cu, 0xc3677e7eu, 0x000000c3u,
    0x7f1e0000u, 0x3f7f6767u, 0x7fe7e767u, 0x0000003eu, 0x7c780000u, 0x0606064eu, 0x7c0e0606u, 0x00000078u,
    0x3e0e0000u, 0xe6e66676u, 0x7e6666e6u, 0x0000001eu, 0x7e7e0000u, 0x7e7e0606u, 0x7e060606u, 0x0000007eu,
    0x7e7e0000u, 0x7e7e0606u, 0x06060606u, 0x00000006u, 0x7c380000u, 0x7706064eu, 0xfec6c6f7u, 0x00000078u,
    0x66420000u, 0x7e7e6666u, 0x66666666u, 0x00000066u, 0x7e7e0000u, 0x18181818u, 0x7e181818u, 0x0000007eu,
    0x7c3c0000u, 0x60606070u, 0x3f706060u, 0x0000003eu, 0x67c20000u, 0x1f1f3f77u, 0xe767373fu, 0x000000c6u,
    0x06040000u, 0x06060606u, 0xfe060606u, 0x000000feu, 0xe7660000u, 0xdbffffe7u, 0xc3c3c3dbu, 0x000000c3u,
    0x67460000u, 0x7b6f6f6fu, 0x6373737bu, 0x00000062u, 0x7e3c0000u, 0xe7e76766u, 0x7e66e7e7u, 0x0000003cu,
    0x7e1e0000u, 0x7ee6e6e6u, 0x0606063eu, 0x00000006u, 0x7e3c0000u, 0xe7e76766u, 0x7e66e7e7u, 0x0020603cu,
    0x7e1e0000u, 0x3e666676u, 0xe666763eu, 0x000000c6u, 0x7e3c0000u, 0x3c0e0646u, 0x7e606078u, 0x0000003eu,
    0xff7e0000u, 0x18181818u, 0x18181818u, 0x00000018u, 0xe7420000u, 0xe7e7e7e7u, 0x7e66e7e7u, 0x0000003cu,
    0xe3430000u, 0x66666666u, 0x3c3c3c3eu, 0x0000001cu, 0xc3810000u, 0xdbdbdbc3u, 0x66667fffu, 0x00000066u,
    0x67430000u, 0x183c3c66u, 0x66663c3cu, 0x000000c3u, 0xe7c30000u, 0x3c3c7e66u, 0x18181818u, 0x00000018u,
    0xfe7e0000u, 0x18387060u, 0xff060e1cu, 0x000000feu, 0x18380000u, 0x18181818u, 0x18181818u, 0x00383818u,
    0x06020000u, 0x080c0c06u, 0x20301018u, 0x00004060u, 0x181c0000u, 0x18181818u, 0x18181818u, 0x001c1c18u,
    0x3c180000u, 0x0000667eu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0xff000000u,
    0x180c0400u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x60667e00u, 0x7e67677eu, 0x0000007eu,
    0x06060000u, 0xe67e7e06u, 0x7ee6e6e6u, 0x0000003eu, 0x00000000u, 0x067e7c00u, 0x7c0e0606u, 0x0000007cu,
    0x60600000u, 0x677e7e60u, 0x7e676767u, 0x0000007cu, 0x00000000u, 0xe77e3c00u, 0x7e077fffu, 0x0000007cu,
    0x78780000u, 0x187e7e18u, 0x18181818u, 0x00000018u, 0x00000000u, 0x677e7e00u, 0x7e666767u, 0x3c7e606cu,
    0x06060000u, 0x667e7e06u, 0x66666666u, 0x00000066u, 0x10181800u, 0x181c1e00u, 0xfe181818u, 0x000000feu,
    0x10383000u, 0x383c3e00u, 0x38383838u, 0x0e1e3838u, 0x06060000u, 0x1e366606u, 0x66763e1eu, 0x000000c6u,
    0x1e1f0000u, 0x1c1c1c1cu, 0x781c1c1cu, 0x00000070u, 0x00000000u, 0xdbff7f00u, 0xdbdbdbdbu, 0x000000dbu,
    0x00000000u, 0x667e7e00u, 0x66666666u, 0x00000066u, 0x00000000u, 0x677e3c00u, 0x7e66e7e7u, 0x0000003cu,
    0x00000000u, 0xe67e7e00u, 0x7ee6e6e6u, 0x0606063eu, 0x00000000u, 0x677e7e00u, 0x7e676767u, 0x6060607cu,
    0x00000000u, 0x0cfcfc00u, 0x0c0c0c0cu, 0x0000000cu, 0x00000000u, 0x067e7c00u, 0x7e60783eu, 0x0000003eu,
    0x1c000000u, 0x1c7e7f1cu, 0x781c1c1cu, 0x00000078u, 0x00000000u, 0x66666600u, 0x7e666666u, 0x0000007cu,
    0x00000000u, 0x6666e700u, 0x3c3c3c66u, 0x00000018u, 0x00000000u, 0xdbc3c300u, 0x767e7fdbu, 0x00000066u,
    0x00000000u, 0x3c7e6600u, 0x663c3c18u, 0x000000e7u, 0x00000000u, 0x6666e700u, 0x383c3c7eu, 0x060e1818u,
    0x00000000u, 0x307e7e00u, 0x7e0e1c38u, 0x0000007eu, 0x38700000u, 0x1c181818u, 0x1818180eu, 0x00703818u,
    0x18180000u, 0x18181818u, 0x18181818u, 0x18181818u, 0x1c0e0000u, 0x38181818u, 0x18181870u, 0x000e1c18u,
    0x00000000u, 0x0e000000u, 0x000020ffu, 0x00000000u
)FONT");

    const vk::PushConstantRange push_constants{
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PushData),
    };
    const vk::PipelineLayoutCreateInfo layout_info{
        .setLayoutCount = 0,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constants,
    };
    pipeline_layout =
        Check<"create overlay pipeline layout">(device.createPipelineLayoutUnique(layout_info));

    const auto vs_module =
        Compile(HostShaders::FS_TRI_VERT, vk::ShaderStageFlagBits::eVertex, device);
    const auto fs_module = Compile(frag_source, vk::ShaderStageFlagBits::eFragment, device);
    ASSERT(vs_module && fs_module);

    const std::array shaders_ci{
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = vs_module,
            .pName = "main",
        },
        vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = fs_module,
            .pName = "main",
        },
    };
    const vk::PipelineRenderingCreateInfo rendering_ci{
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &format,
    };
    const vk::PipelineVertexInputStateCreateInfo vertex_input{};
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly{
        .topology = vk::PrimitiveTopology::eTriangleList,
    };
    const vk::Viewport viewport{.width = 1.0f, .height = 1.0f, .maxDepth = 1.0f};
    const vk::Rect2D scissor{.extent = {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info{
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    const vk::PipelineRasterizationStateCreateInfo raster_state{
        .polygonMode = vk::PolygonMode::eFill,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .lineWidth = 1.0f,
    };
    const vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
    };
    const vk::PipelineColorBlendAttachmentState blend{
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eZero,
        .dstAlphaBlendFactor = vk::BlendFactor::eOne,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };
    const vk::PipelineColorBlendStateCreateInfo color_blending{
        .attachmentCount = 1,
        .pAttachments = &blend,
    };
    const std::array dynamic_states{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    const vk::PipelineDynamicStateCreateInfo dynamic_info{
        .dynamicStateCount = dynamic_states.size(),
        .pDynamicStates = dynamic_states.data(),
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info{
        .pNext = &rendering_ci,
        .stageCount = shaders_ci.size(),
        .pStages = shaders_ci.data(),
        .pVertexInputState = &vertex_input,
        .pInputAssemblyState = &input_assembly,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_state,
        .pMultisampleState = &multisampling,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };
    pipeline = Check<"create overlay pipeline">(
        device.createGraphicsPipelineUnique(/*pipeline_cache*/ {}, pipeline_info));
    device.destroyShaderModule(vs_module);
    device.destroyShaderModule(fs_module);
}

void VrOverlayPass::Render(vk::CommandBuffer cmdbuf, Frame& frame, const Foveation& foveation,
                           std::span<const std::string> lines) {
    if (!pipeline || (!foveation.show && lines.empty())) {
        return;
    }
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    // After the post-processing pass's writes (it leaves the frame in the general layout).
    const vk::ImageMemoryBarrier2 before{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask =
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &before,
    });

    const vk::RenderingAttachmentInfo attachment{
        .imageView = frame.image_view,
        .imageLayout = vk::ImageLayout::eGeneral,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea{.extent{.width = frame.width, .height = frame.height}},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipeline);

    const auto draw = [&](const PushData& data) {
        const vk::Rect2D area{
            .offset{static_cast<s32>(data.rect[0]), static_cast<s32>(data.rect[1])},
            .extent{static_cast<u32>(data.rect[2]), static_cast<u32>(data.rect[3])},
        };
        cmdbuf.setViewport(0, vk::Viewport{
                                  .x = data.rect[0],
                                  .y = data.rect[1],
                                  .width = data.rect[2],
                                  .height = data.rect[3],
                                  .minDepth = 0.0f,
                                  .maxDepth = 1.0f,
                              });
        cmdbuf.setScissor(0, area);
        cmdbuf.pushConstants(*pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                             sizeof(PushData), &data);
        cmdbuf.draw(3, 1, 0, 0);
    };

    const float eye_width = static_cast<float>(frame.width / 2);
    const float eye_height = static_cast<float>(frame.height);
    for (u32 eye = 0; eye < 2; ++eye) {
        const float eye_x = eye * eye_width;
        if (foveation.show) {
            PushData data{};
            data.rect = {eye_x, 0.0f, eye_width, eye_height};
            data.fov = {foveation.center_x, foveation.center_y, foveation.inner, foveation.outer};
            data.extra = {0.22f, foveation.outer_4x4 ? 1.0f : 0.0f,
                          foveation.follows_eyes ? 1.0f : 0.0f, 0.0f};
            data.mode = 0;
            draw(data);
        }
        // The text block is centred on a point of each eye's picture, the same in both, so
        // that it looks far away: a little above the middle unless SHADPS4_VR_OVERLAY_POS=
        // <x>,<y> (0..1 across and down the picture) puts it elsewhere. Further out the lenses
        // blur it.
        const float line_height = static_cast<float>(GlyphHeight * scale + 4);
        u32 widest = 0;
        for (const std::string& line : lines) {
            widest = std::max(widest, std::min<u32>(static_cast<u32>(line.size()), MaxLine));
        }
        const float block_width = static_cast<float>((widest * GlyphWidth + 2) * scale);
        const float block_height = line_height * static_cast<float>(lines.size());
        float y = std::floor(eye_height * position_y - block_height * 0.5f);
        const float x = std::floor(eye_x + eye_width * position_x - block_width * 0.5f);
        for (const std::string& line : lines) {
            const u32 length = std::min<u32>(static_cast<u32>(line.size()), MaxLine);
            PushData data{};
            const float width = static_cast<float>((length * GlyphWidth + 2) * scale);
            data.rect = {std::clamp(x, eye_x, eye_x + eye_width - width),
                         std::clamp(y, 0.0f, eye_height - line_height), width,
                         static_cast<float>(GlyphHeight * scale)};
            data.mode = 1;
            data.count = length;
            data.scale = scale;
            std::memcpy(data.text.data(), line.data(), length);
            draw(data);
            y += line_height;
        }
    }
    cmdbuf.endRendering();

    const vk::ImageMemoryBarrier2 after{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame.image,
        .subresourceRange = range,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &after,
    });
}

} // namespace Vulkan::HostPasses
