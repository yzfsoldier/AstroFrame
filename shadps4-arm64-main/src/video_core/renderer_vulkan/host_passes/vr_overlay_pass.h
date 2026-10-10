//  SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
//  SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
struct Frame;
} // namespace Vulkan

namespace Vulkan::HostPasses {

/// A debug overlay drawn over the frame that goes to the machine's own headset, eye by eye:
/// lines of text (frame rate, the size the scene is drawn at, how busy the GPU is ...) and, for
/// foveated rendering, the rings of its shading rates in colour (green: every pixel, yellow:
/// one colour for 2x2, red: for 4x4) with a mark where the eyes look. Drawn with a small
/// built-in font (DejaVu Sans Mono Bold, 8x16), nothing but push constants.
class VrOverlayPass {
public:
    void Create(const Instance& instance, vk::Format format);

    struct Foveation {
        bool show{};
        /// The fine middle, -1..1 across the eye's picture (+y down).
        float center_x{};
        float center_y{};
        /// The rings, as a share of the way from the middle of the picture to its edge.
        float inner{};
        float outer{};
        bool outer_4x4{};
        bool follows_eyes{};
    };

    /// Draws over `frame`, whose left and right halves are the two eyes. The frame is in the
    /// general layout, as the post-processing pass leaves it, and is left so.
    void Render(vk::CommandBuffer cmdbuf, Frame& frame, const Foveation& foveation,
                std::span<const std::string> lines);

private:
    vk::Device device{};
    vk::UniquePipeline pipeline{};
    vk::UniquePipelineLayout pipeline_layout{};
    u32 scale = 2;
    /// Where the middle of the text block is, 0..1 across and down each eye's picture.
    float position_x = 0.5f;
    float position_y = 0.38f;
};

} // namespace Vulkan::HostPasses
