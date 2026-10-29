// Stereo for a headset (xr_bridge.hpp) ---------------------------------------
//
// While the game says its scene is 3D (xr::scene_3d), each frame is recorded
// as frame interpolation records it, and at its present drawn again for each
// eye into an eye image: the perspective draws into the displayed framebuffer
// from the eye's pose and with the eye's field of view, clears over the whole
// image, and everything else (the interface, 2D) flat on a rectangle a little
// way in front of the eyes. The eye's projection keeps the game's clip z and w,
// so depth comes out as the game wrote it; its clip x and y are the eye's.
//
// Test switches (env.txt on the Quest): PURE_VR_FLAT=1 (no stereo),
// PURE_VR_UNITS_PER_METRE (the game's world units in a metre: the scale of the
// eyes' distance and the head's movement), PURE_VR_HUD_DISTANCE and
// PURE_VR_HUD_WIDTH (metres).

namespace {

using EyeMatrix = interpolation::Matrix;

float vr_setting(const char *name, float fallback, float low, float high) {
    const char *text = portablekit::env(name);
    if (text == nullptr) return fallback;
    char *end = nullptr;
    const float value = std::strtof(text, &end);
    return end != text && value >= low && value <= high ? value : fallback;
}

// From the reference space the eye's pose is given in to the eye's own: the
// inverse of the pose, its position scaled from metres to the game's units.
EyeMatrix eye_from_reference(const xr::EyeView &eye, float scale) {
    const float x = eye.orientation[0], y = eye.orientation[1], z = eye.orientation[2], w = eye.orientation[3];
    const float r00 = 1.0f - 2.0f * (y * y + z * z), r01 = 2.0f * (x * y - z * w), r02 = 2.0f * (x * z + y * w);
    const float r10 = 2.0f * (x * y + z * w), r11 = 1.0f - 2.0f * (x * x + z * z), r12 = 2.0f * (y * z - x * w);
    const float r20 = 2.0f * (x * z - y * w), r21 = 2.0f * (y * z + x * w), r22 = 1.0f - 2.0f * (x * x + y * y);
    const float px = eye.position[0] * scale, py = eye.position[1] * scale, pz = eye.position[2] * scale;
    EyeMatrix m{};  // column-major: the transposed rotation, and minus it times the position
    m[0] = r00, m[4] = r10, m[8] = r20;
    m[1] = r01, m[5] = r11, m[9] = r21;
    m[2] = r02, m[6] = r12, m[10] = r22;
    m[12] = -(r00 * px + r10 * py + r20 * pz);
    m[13] = -(r01 * px + r11 * py + r21 * pz);
    m[14] = -(r02 * px + r12 * py + r22 * pz);
    m[15] = 1.0f;
    return m;
}

// The game's eye axes against OpenXR's (x right, y up, -z forward), read off
// the signs of its projection into clip space (x right, y down, w the depth),
// and the scale of its w.
struct EyeAxes {
    float x{1.0f}, y{1.0f}, z{1.0f}, w_scale{1.0f};
};

EyeAxes eye_axes(const EyeMatrix &clip) {
    EyeAxes axes;
    axes.x = clip[0] < 0.0f ? -1.0f : 1.0f;
    axes.y = clip[5] > 0.0f ? -1.0f : 1.0f;
    axes.z = clip[11] > 0.0f ? -1.0f : 1.0f;
    axes.w_scale = std::fabs(clip[11]);
    return axes;
}

// The eye's projection, applied to the game's eye space: x and y from the
// eye's field of view, z and w the game's own (`clip`: the draw's projection
// with its viewport folded in).
EyeMatrix eye_projection(const EyeMatrix &clip, const EyeAxes &axes, const xr::EyeView &eye) {
    const float ax = 2.0f / (eye.tan_right - eye.tan_left);
    const float bx = (eye.tan_right + eye.tan_left) / (eye.tan_right - eye.tan_left);
    const float ay = 2.0f / (eye.tan_up - eye.tan_down);
    const float by = (eye.tan_up + eye.tan_down) / (eye.tan_up - eye.tan_down);
    const float k = axes.w_scale;
    EyeMatrix p{};
    p[0] = k * ax * axes.x;
    p[8] = k * bx * axes.z;
    p[5] = -k * ay * axes.y;
    p[9] = -k * by * axes.z;
    for (std::size_t column = 0; column < 4u; ++column) {
        p[column * 4u + 2u] = clip[column * 4u + 2u];
        p[column * 4u + 3u] = clip[column * 4u + 3u];
    }
    return p;
}

} // namespace

bool VulkanRenderer::Impl::stereo_wanted() const {
    if (!xr) return false;
    static const bool flat = portablekit::env("VR_FLAT") != nullptr;
    const VkExtent2D extent = xr::bridge()->stereo_extent();
    return !flat && xr::scene_3d() && extent.width != 0u && extent.height != 0u;
}

// The frame the game just flipped becomes the one the eyes draw again.
void VulkanRenderer::Impl::finish_stereo_frame(std::uint32_t displayed) {
    FrameRecord &frame = recording_frame;
    frame.recorded = frame.recorded && interpolating;
    interpolation::mark_eligible(frame.summaries, displayed, false);
    frame.displayed = displayed;
    frame.valid = true;
    std::swap(newer_frame, recording_frame);
    recording_frame.clear();
    next_region = (frame_region + 1u) % kFrameRegions;
    stereo_ready = newer_frame.recorded;
}

// In the frame's own command buffer, outside a render pass: the two eye
// images, handed to the headset.
void VulkanRenderer::Impl::record_stereo(VkCommandBuffer commands) {
    if (!stereo_ready || !newer_frame.valid || !newer_frame.recorded) return;
    stereo_ready = false;
    const VkExtent2D extent = xr::bridge()->stereo_extent();
    if (extent.width == 0u || extent.height == 0u) return;
    for (Target &target : eye_targets) {
        if (target.color != VK_NULL_HANDLE) continue;
        target = Target{};
        target.extent = extent;
        std::string error;
        if (!create_target(target, error)) {
            std::cout << "[vr] no room for the eye images: " << error << "\n";
            destroy_target(target);
            return;
        }
    }
    xr::StereoFrame stereo{};
    if (!xr::bridge()->stereo_begin(stereo)) return;
    std::array<VkImage, 2> images{};
    for (std::uint32_t eye = 0; eye < 2u; ++eye) {
        replay_eye(commands, eye_targets[eye], stereo, eye);
        transition(commands, eye_targets[eye].color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        images[eye] = eye_targets[eye].color;
    }
    xr::bridge()->record_stereo(commands, images, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, extent);
    for (const Target &target : eye_targets)
        transition(commands, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

void VulkanRenderer::Impl::replay_eye(VkCommandBuffer commands, Target &target, const xr::StereoFrame &stereo,
                                      std::uint32_t eye) {
    const perf::SplitScope split(perf::Split::Replay);
    const FrameRecord &frame = newer_frame;
    const xr::EyeView &view = stereo.eyes[eye];
    breadcrumb(commands, "an eye of the stereo view, " + std::to_string(frame.groups.size()) + " draw calls");
    initialize_layouts(commands, target);
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = render_pass;
    pass.framebuffer = target.framebuffer;
    pass.renderArea = {{0, 0}, target.extent};
    vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    perf::count_render_pass();
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 2u, 1u,
                            &target.input_descriptor, 0u, nullptr);
    std::array<VkClearAttachment, 2> clears{};
    clears[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clears[0].colorAttachment = 0u;
    clears[0].clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    clears[1].aspectMask = depth_aspect();
    clears[1].clearValue.depthStencil = {0.0f, 0u};
    const VkClearRect whole_rect{{{0, 0}, target.extent}, 0u, 1u};
    vkCmdClearAttachments(commands, static_cast<std::uint32_t>(clears.size()), clears.data(), 1u, &whole_rect);

    static const float scale = vr_setting("VR_UNITS_PER_METRE", 1.0f, 0.001f, 100000.0f);
    static const float hud_distance = vr_setting("VR_HUD_DISTANCE", 2.0f, 0.3f, 50.0f);
    static const float hud_width = vr_setting("VR_HUD_WIDTH", 1.8f, 0.1f, 50.0f);
    const EyeMatrix from_reference = eye_from_reference(view, scale);
    const float width = static_cast<float>(target.extent.width);
    const float height = static_cast<float>(target.extent.height);

    // The interface's rectangle: a plane hud_distance ahead of the head,
    // centred between the eyes, hud_width wide, as tall as the PSP's screen.
    const xr::EyeView &other = stereo.eyes[eye ^ 1u];
    const float dx = view.position[0] - other.position[0];
    const float dy = view.position[1] - other.position[1];
    const float dz = view.position[2] - other.position[2];
    const float half_ipd = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    const float eye_x = eye == 0u ? -half_ipd : half_ipd;
    const auto pixel_x = [&](float metres) {
        const float ndc = (2.0f * metres / hud_distance - (view.tan_right + view.tan_left)) /
                          (view.tan_right - view.tan_left);
        return (ndc + 1.0f) * 0.5f * width;
    };
    const auto pixel_y = [&](float metres) {
        const float ndc = (2.0f * metres / hud_distance - (view.tan_up + view.tan_down)) / (view.tan_up - view.tan_down);
        return (1.0f - ndc) * 0.5f * height;
    };
    const float half_width = 0.5f * hud_width;
    const float half_height = half_width * static_cast<float>(kPspHeight) / static_cast<float>(kPspWidth);
    const float hud_left = pixel_x(-half_width - eye_x), hud_right = pixel_x(half_width - eye_x);
    const float hud_top = pixel_y(half_height), hud_bottom = pixel_y(-half_height);
    VkExtent2D source = target_extent;
    if (const auto shown = targets.find(frame.displayed); shown != targets.end()) source = shown->second.extent;
    const float scissor_x = (hud_right - hud_left) / static_cast<float>(std::max(1u, source.width));
    const float scissor_y = (hud_bottom - hud_top) / static_cast<float>(std::max(1u, source.height));
    const VkRect2D all{{0, 0}, target.extent};

    DrawState set{};
    bool known = false;
    EyeMatrix last_view{}, last_world{}, last_clip{}, eye_space{}, transform{};
    bool cached = false;
    std::uint32_t perspective = 0u, flat = 0u;
    float nearest = 0.0f;
    for (const ReplayGroup &drawn : frame.groups) {
        if (drawn.target != frame.displayed) continue;
        DrawState state = drawn.state;
        const interpolation::DrawSummary &summary = frame.summaries[drawn.first_draw];
        const float min_depth = drawn.state.viewport.minDepth, max_depth = drawn.state.viewport.maxDepth;
        if (summary.eligible) {
            if (!cached || summary.view != last_view || summary.world != last_world ||
                summary.projection != last_clip) {
                const EyeAxes axes = eye_axes(summary.projection);
                const EyeMatrix flip{axes.x, 0.0f, 0.0f, 0.0f, 0.0f, axes.y, 0.0f, 0.0f,
                                     0.0f, 0.0f, axes.z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
                eye_space = interpolation::multiply(
                    flip, interpolation::multiply(
                              from_reference, interpolation::multiply(
                                                  flip, interpolation::multiply(summary.view, summary.world))));
                transform = interpolation::multiply(eye_projection(summary.projection, axes, view), eye_space);
                last_view = summary.view;
                last_world = summary.world;
                last_clip = summary.projection;
                cached = true;
            }
            state.push.transform = transform;
            state.push.view_z = {eye_space[2], eye_space[6], eye_space[10], eye_space[14]};
            state.viewport = {0.0f, 0.0f, width, height, min_depth, max_depth};
            state.scissor = all;
            ++perspective;
            const auto &t = summary.eye_translation;
            const float distance = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (nearest == 0.0f || distance < nearest) nearest = distance;
        } else if (summary.clear) {
            state.viewport = {0.0f, 0.0f, width, height, min_depth, max_depth};
            state.scissor = all;
        } else {
            state.viewport = {hud_left, hud_top, hud_right - hud_left, hud_bottom - hud_top, min_depth, max_depth};
            const float x0 = std::clamp(hud_left + static_cast<float>(drawn.state.scissor.offset.x) * scissor_x, 0.0f, width);
            const float y0 = std::clamp(hud_top + static_cast<float>(drawn.state.scissor.offset.y) * scissor_y, 0.0f, height);
            const float x1 = std::clamp(hud_left + static_cast<float>(drawn.state.scissor.offset.x +
                                                                      static_cast<std::int32_t>(drawn.state.scissor.extent.width)) *
                                                       scissor_x,
                                        0.0f, width);
            const float y1 = std::clamp(hud_top + static_cast<float>(drawn.state.scissor.offset.y +
                                                                     static_cast<std::int32_t>(drawn.state.scissor.extent.height)) *
                                                      scissor_y,
                                        0.0f, height);
            state.scissor = {{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)},
                             {static_cast<std::uint32_t>(std::max(0.0f, x1 - x0)),
                              static_cast<std::uint32_t>(std::max(0.0f, y1 - y0))}};
            ++flat;
        }

        if (!known || std::memcmp(&state.viewport, &set.viewport, sizeof(VkViewport)) != 0)
            vkCmdSetViewport(commands, 0u, 1u, &state.viewport);
        if (!known || state.blend != set.blend) vkCmdSetBlendConstants(commands, state.blend.data());
        if (!known || std::memcmp(&state.scissor, &set.scissor, sizeof(VkRect2D)) != 0)
            vkCmdSetScissor(commands, 0u, 1u, &state.scissor);
        if (!known || state.pipeline != set.pipeline)
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, state.pipeline);
        if (!known || state.texture != set.texture)
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0u, 1u,
                                    &state.texture, 0u, nullptr);
        if (!known || state.lighting != set.lighting)
            vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1u, 1u,
                                    &lighting_descriptor, static_cast<std::uint32_t>(state.lighting.size()),
                                    state.lighting.data());
        if (!known || std::memcmp(&state.push, &set.push, sizeof(PushConstants)) != 0)
            vkCmdPushConstants(commands, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0u, sizeof(PushConstants), &state.push);
        set = state;
        known = true;
        const VkDeviceSize vertex_base = drawn.vertex_base;
        vkCmdBindVertexBuffers(commands, 0u, 1u, &vertex_buffer, &vertex_base);
        if (drawn.index_buffer != VK_NULL_HANDLE) {
            vkCmdBindIndexBuffer(commands, drawn.index_buffer, drawn.index_base, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(commands, drawn.count, 1u, 0u, 0, drawn.raw ? static_cast<std::uint32_t>(vertex_base) : 0u);
        } else {
            vkCmdDraw(commands, drawn.count, 1u, 0u, 0u);
        }
    }
    vkCmdEndRenderPass(commands);

    // Every five seconds or so: what the eyes drew, and the numbers that say
    // whether the game's eye space was read right and how big its units are.
    if (eye == 0u && stereo_frames++ % 360u == 0u) {
        std::printf("[vr] stereo: %u 3D and %u flat draw calls, eye %ux%u, nearest 3D object %.2f units away, "
                    "%.3g units a metre, interface %.0f..%.0f x %.0f..%.0f\n",
                    perspective, flat, target.extent.width, target.extent.height, static_cast<double>(nearest),
                    static_cast<double>(scale), static_cast<double>(hud_left), static_cast<double>(hud_right),
                    static_cast<double>(hud_top), static_cast<double>(hud_bottom));
        for (const interpolation::DrawSummary &summary : frame.summaries) {
            if (!summary.eligible) continue;
            const EyeMatrix &c = summary.projection;
            std::printf("[vr] the game's projection: x %.3f y %.3f z %.4f %.4f w %.3f %.3f\n",
                        static_cast<double>(c[0]), static_cast<double>(c[5]), static_cast<double>(c[10]),
                        static_cast<double>(c[14]), static_cast<double>(c[11]), static_cast<double>(c[15]));
            break;
        }
        std::fflush(stdout);
    }
}

