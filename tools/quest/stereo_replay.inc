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

// The inverse of a 4x4 matrix (column-major), or false when it has none.
bool invert(const EyeMatrix &m, EyeMatrix &out) {
    EyeMatrix inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) return false;
    for (std::size_t i = 0; i < 16u; ++i) out[i] = inv[i] / det;
    return true;
}

} // namespace

// MSAA for the eyes ----------------------------------------------------------
//
// PURE_VR_MSAA=4 (the default), 2 or 0 (off). The eyes draw into multisampled colour and depth (lazily allocated
// where the GPU has such memory: on a tiler they stay in tile memory) and resolve into the eye image at the end of
// the pass. The game's pipelines are single-sampled, so each one the eyes use gets a multisampled twin, made from
// its key the first time (and kept in the pipeline cache for the next run).

bool VulkanRenderer::Impl::create_eye_msaa() {
    if (eye_msaa_pass != VK_NULL_HANDLE && eye_msaa[0].framebuffer != VK_NULL_HANDLE) return true;
    static const VkSampleCountFlagBits wanted = [] {
        const char *text = portablekit::env("VR_MSAA");
        const int samples = text != nullptr ? std::atoi(text) : 4;
        return samples >= 4 ? VK_SAMPLE_COUNT_4_BIT : samples >= 2 ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
    }();
    if (wanted == VK_SAMPLE_COUNT_1_BIT) return false;
    if (eye_msaa_pass == VK_NULL_HANDLE) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_device, &properties);
        const VkSampleCountFlags counts =
            properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts;
        eye_samples = (counts & wanted) != 0u ? wanted : (counts & VK_SAMPLE_COUNT_2_BIT) != 0u ? VK_SAMPLE_COUNT_2_BIT
                                                                                              : VK_SAMPLE_COUNT_1_BIT;
        if (eye_samples == VK_SAMPLE_COUNT_1_BIT) return false;
        std::array<VkAttachmentDescription, 3> attachments{};
        attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].samples = eye_samples;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[1] = attachments[0];
        attachments[1].format = depth_format;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments[2] = attachments[0];
        attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // The game's shader declares the target's colour as an input attachment (16-bit targets blend on it); the
        // eyes replay the shown 8888 frame, whose pipelines never read it, so the subpass leaves it unused.
        const VkAttachmentReference color_reference{0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference depth_reference{1u, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference resolve_reference{2u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        const VkAttachmentReference input_reference{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.inputAttachmentCount = 1u;
        subpass.pInputAttachments = &input_reference;
        subpass.colorAttachmentCount = 1u;
        subpass.pColorAttachments = &color_reference;
        subpass.pResolveAttachments = &resolve_reference;
        subpass.pDepthStencilAttachment = &depth_reference;
        constexpr VkPipelineStageFlags kStages =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        constexpr VkAccessFlags kAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                                          VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array<VkSubpassDependency, 2> dependencies{
            VkSubpassDependency{VK_SUBPASS_EXTERNAL, 0u, kStages, kStages, kAccess, kAccess, 0u},
            VkSubpassDependency{0u, VK_SUBPASS_EXTERNAL, kStages, kStages, kAccess, kAccess, 0u},
        };
        VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        info.pAttachments = attachments.data();
        info.subpassCount = 1u;
        info.pSubpasses = &subpass;
        info.dependencyCount = static_cast<std::uint32_t>(dependencies.size());
        info.pDependencies = dependencies.data();
        if (vkCreateRenderPass(device, &info, nullptr, &eye_msaa_pass) != VK_SUCCESS) {
            eye_msaa_pass = VK_NULL_HANDLE;
            std::cout << "[vr] no MSAA: the render pass failed\n";
            return false;
        }
    }
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
    bool lazy_used = false;
    const auto make = [&](VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage &image,
                          VkDeviceMemory &bound, VkImageView &view) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {eye_targets[0].extent.width, eye_targets[0].extent.height, 1u};
        info.mipLevels = 1u;
        info.arrayLayers = 1u;
        info.samples = eye_samples;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i)
            if ((requirements.memoryTypeBits & (1u << i)) != 0u &&
                (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0u)
                type = i, lazy_used = true;
        if (type == UINT32_MAX) type = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &allocate, nullptr, &bound) != VK_SUCCESS) return false;
        vkBindImageMemory(device, image, bound, 0u);
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = format;
        view_info.subresourceRange = {aspect, 0u, 1u, 0u, 1u};
        return vkCreateImageView(device, &view_info, nullptr, &view) == VK_SUCCESS;
    };
    for (std::uint32_t eye = 0; eye < 2u; ++eye) {
        EyeMsaa &m = eye_msaa[eye];
        if (!make(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, m.color,
                  m.color_memory, m.color_view) ||
            !make(depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, depth_aspect(), m.depth, m.depth_memory,
                  m.depth_view)) {
            std::cout << "[vr] no MSAA: its images failed\n";
            destroy_eye_msaa();
            return false;
        }
        const std::array<VkImageView, 3> views{m.color_view, m.depth_view, eye_targets[eye].color_view};
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = eye_msaa_pass;
        info.attachmentCount = static_cast<std::uint32_t>(views.size());
        info.pAttachments = views.data();
        info.width = eye_targets[eye].extent.width;
        info.height = eye_targets[eye].extent.height;
        info.layers = 1u;
        if (vkCreateFramebuffer(device, &info, nullptr, &m.framebuffer) != VK_SUCCESS) {
            std::cout << "[vr] no MSAA: its framebuffer failed\n";
            destroy_eye_msaa();
            return false;
        }
    }
    std::cout << "[vr] eyes with " << static_cast<int>(eye_samples) << "x MSAA (" << (lazy_used ? "tile memory" : "device memory")
              << ")\n";
    return true;
}

void VulkanRenderer::Impl::destroy_eye_msaa() {
    for (EyeMsaa &m : eye_msaa) {
        if (m.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, m.framebuffer, nullptr);
        if (m.color_view != VK_NULL_HANDLE) vkDestroyImageView(device, m.color_view, nullptr);
        if (m.depth_view != VK_NULL_HANDLE) vkDestroyImageView(device, m.depth_view, nullptr);
        if (m.color != VK_NULL_HANDLE) vkDestroyImage(device, m.color, nullptr);
        if (m.depth != VK_NULL_HANDLE) vkDestroyImage(device, m.depth, nullptr);
        if (m.color_memory != VK_NULL_HANDLE) vkFreeMemory(device, m.color_memory, nullptr);
        if (m.depth_memory != VK_NULL_HANDLE) vkFreeMemory(device, m.depth_memory, nullptr);
        m = EyeMsaa{};
    }
}

VkPipeline VulkanRenderer::Impl::msaa_pipeline(VkPipeline pipeline) {
    if (const auto found = msaa_pipelines.find(pipeline); found != msaa_pipelines.end()) return found->second;
    VkPipeline made = VK_NULL_HANDLE;
    for (const auto &[key, single] : pipelines) {
        if (single != pipeline) continue;
        msaa_build = true;
        made = create_pipeline(key);
        msaa_build = false;
        break;
    }
    msaa_pipelines.emplace(pipeline, made);
    return made;
}

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
// Before the frame's submit: whether its eyes are drawn (the headset located them and gave two images). The
// frame then leaves the screen's copy out: the headset shows the eyes.
bool VulkanRenderer::Impl::begin_stereo() {
    stereo_begun = false;
    if (!stereo_ready || !newer_frame.valid || !newer_frame.recorded) return false;
    stereo_ready = false;
    const VkExtent2D extent = xr::bridge()->stereo_extent();
    if (extent.width == 0u || extent.height == 0u) return false;
    // PURE_VR_EYE_COPY=unorm: eye images stored as UNORM, as before (a copy between formats).
    static const bool srgb_eyes = [] {
        const char *text = portablekit::env("VR_EYE_COPY");
        const bool srgb = text == nullptr || std::strcmp(text, "unorm") != 0;
        std::printf("[vr] eye images stored as %s\n", srgb ? "sRGB (viewed as UNORM)" : "UNORM");
        return srgb;
    }();
    for (Target &target : eye_targets) {
        if (target.color != VK_NULL_HANDLE) continue;
        destroy_eye_msaa();  // its framebuffers name the eye images
        target = Target{};
        target.extent = extent;
        std::string error;
        srgb_storage = srgb_eyes;
        const bool created = create_target(target, error);
        srgb_storage = false;
        if (!created) {
            std::cout << "[vr] no room for the eye images: " << error << "\n";
            destroy_target(target);
            return false;
        }
    }
    if (!xr::bridge()->stereo_begin(stereo_frame)) return false;
    create_eye_msaa();
    stereo_begun = true;
    return true;
}

// After the frame's submit: the eyes in a submit of their own, on one of the present slots (whose fences the
// renderer already waits for before it reuses a vertex region or frees a texture they may draw from). The frame's
// fence then covers the game's own work only, so the write-backs the next frame waits for do not wait for the eyes.
void VulkanRenderer::Impl::submit_stereo() {
    if (!stereo_begun) return;
    stereo_begun = false;
    const std::uint32_t p = present_slot;
    present_slot ^= 1u;
    VkCommandBuffer commands = present_commands[p];
    VkFence fence = present_fences[p];
    const perf::Clock::time_point wait_start = perf::Clock::now();
    wait_fence(fence, "the wait for the eyes before");
    perf::add_wait_time(perf::Clock::now() - wait_start, perf::Stall::Fence);
    if (eye_timer == VK_NULL_HANDLE && gpu_timer != VK_NULL_HANDLE) {
        VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 4u;
        if (vkCreateQueryPool(device, &info, nullptr, &eye_timer) != VK_SUCCESS) eye_timer = VK_NULL_HANDLE;
    }
    // This present slot's last pair has finished: its fence was just waited for.
    if (eye_timer != VK_NULL_HANDLE && eye_timed[p]) {
        std::array<std::uint64_t, 2> ticks{};
        if (vkGetQueryPoolResults(device, eye_timer, p * 2u, 2u, sizeof(ticks), ticks.data(), sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            eye_gpu_ms += static_cast<double>((ticks[1] - ticks[0]) & gpu_timer_mask) * gpu_timer_ns_per_tick / 1.0e6;
            ++eye_gpu_samples;
        }
        eye_timed[p] = false;
    }
    vkResetFences(device, 1u, &fence);
    vkResetCommandBuffer(commands, 0u);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commands, &begin);
    if (eye_timer != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(commands, eye_timer, p * 2u, 2u);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, eye_timer, p * 2u);
    }
    // The vertex regions the eyes read, so the frames after do not overwrite them before the eyes are drawn.
    std::uint32_t regions = 0u;
    for (const ReplayGroup &drawn : newer_frame.groups)
        if (drawn.vertex_base < static_cast<VkDeviceSize>(kFrameRegions) * kVertexBufferBytes)
            regions |= 1u << static_cast<std::uint32_t>(drawn.vertex_base / kVertexBufferBytes);
    present_regions[p] = regions;
    std::array<VkImage, 2> images{};
    for (std::uint32_t eye = 0; eye < 2u; ++eye) {
        replay_eye(commands, eye_targets[eye], stereo_frame, eye);
        transition(commands, eye_targets[eye].color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        images[eye] = eye_targets[eye].color;
    }
    if (eye_timer != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, eye_timer, p * 2u + 1u);
        eye_timed[p] = true;
    }
    const VkExtent2D extent = eye_targets[0].extent;
    xr::bridge()->record_stereo(commands, images, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, extent);
    for (const Target &target : eye_targets)
        transition(commands, target.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const perf::Clock::time_point submit_start = perf::Clock::now();
    vkEndCommandBuffer(commands);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &commands;
    VkSemaphore signal = xr::bridge()->frame_semaphore();
    if (signal != VK_NULL_HANDLE) {
        submit.signalSemaphoreCount = 1u;
        submit.pSignalSemaphores = &signal;
    }
    if (vkQueueSubmit(queue, 1u, &submit, fence) == VK_ERROR_DEVICE_LOST) device_lost("the eyes' submit");
    eye_submit_ms += std::chrono::duration<double, std::milli>(perf::Clock::now() - submit_start).count();
}

void VulkanRenderer::Impl::replay_eye(VkCommandBuffer commands, Target &target, const xr::StereoFrame &stereo,
                                      std::uint32_t eye) {
    const perf::SplitScope split(perf::Split::Replay);
    const FrameRecord &frame = newer_frame;
    const xr::EyeView &view = stereo.eyes[eye];
    breadcrumb(commands, "an eye of the stereo view, " + std::to_string(frame.groups.size()) + " draw calls");
    initialize_layouts(commands, target);
    const bool msaa = eye_msaa[eye].framebuffer != VK_NULL_HANDLE;
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = msaa ? eye_msaa_pass : render_pass;
    pass.framebuffer = msaa ? eye_msaa[eye].framebuffer : target.framebuffer;
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
    static const float hud_width = vr_setting("VR_HUD_WIDTH", 1.6f, 0.1f, 50.0f);
    const EyeMatrix from_reference = eye_from_reference(view, scale);
    const float width = static_cast<float>(target.extent.width);
    const float height = static_cast<float>(target.extent.height);

    // The interface: a plane fixed in the room (the reference space, like the screen in the menus), hud_distance
    // ahead of where the head was at the start, hud_height below it, hud_width wide and the PSP's shape. Each 2D
    // draw goes from the PSP's clip space onto that plane and from there into the eye. Its depth keeps the draw's
    // own, scaled by the plane's distance (an eye's w differs from it by a few percent at most).
    static const float hud_height = vr_setting("VR_HUD_HEIGHT", -0.15f, -5.0f, 5.0f);
    const EyeMatrix hud_view = eye_from_reference(view, 1.0f);
    EyeMatrix hud_projection{};  // OpenXR's eye space (x right, y up, -z ahead) to the eye image's clip space (y down)
    {
        const float sx = view.tan_right - view.tan_left, sy = view.tan_up - view.tan_down;
        hud_projection[0] = 2.0f / sx;
        hud_projection[8] = (view.tan_right + view.tan_left) / sx;
        hud_projection[5] = -2.0f / sy;
        hud_projection[9] = -(view.tan_up + view.tan_down) / sy;
        hud_projection[11] = -1.0f;
    }
    const float half_width = 0.5f * hud_width;
    const float half_height = half_width * static_cast<float>(kPspHeight) / static_cast<float>(kPspWidth);
    EyeMatrix hud_plane{};  // the PSP's clip x (right) and y (down) to the plane
    hud_plane[0] = half_width;
    hud_plane[5] = -half_height;
    hud_plane[13] = hud_height;
    hud_plane[14] = -hud_distance;
    hud_plane[15] = 1.0f;
    const EyeMatrix hud_base = interpolation::multiply(hud_projection, interpolation::multiply(hud_view, hud_plane));
    EyeMatrix through_clip{};  // a through draw's PSP pixels to its clip space
    through_clip[0] = 2.0f / static_cast<float>(kPspWidth);
    through_clip[5] = 2.0f / static_cast<float>(kPspHeight);
    through_clip[12] = -1.0f;
    through_clip[13] = -1.0f;
    through_clip[15] = 1.0f;
    const EyeMatrix hud_through = interpolation::multiply(hud_base, through_clip);
    // Where a point of the PSP's clip space lands in the eye image, in pixels.
    const auto hud_pixel = [&](float cx, float cy) {
        const float x = hud_base[0] * cx + hud_base[4] * cy + hud_base[12];
        const float y = hud_base[1] * cx + hud_base[5] * cy + hud_base[13];
        const float w = std::max(1e-3f, hud_base[3] * cx + hud_base[7] * cy + hud_base[15]);
        return std::array<float, 2>{(x / w + 1.0f) * 0.5f * width, (y / w + 1.0f) * 0.5f * height};
    };
    const std::array<float, 2> hud_top_left = hud_pixel(-1.0f, -1.0f), hud_bottom_right = hud_pixel(1.0f, 1.0f);
    const float hud_left = hud_top_left[0], hud_right = hud_bottom_right[0];
    const float hud_top = hud_top_left[1], hud_bottom = hud_bottom_right[1];
    VkExtent2D source = target_extent;
    if (const auto shown = targets.find(frame.displayed); shown != targets.end()) source = shown->second.extent;
    const VkRect2D all{{0, 0}, target.extent};

    DrawState set{};
    bool known = false;
    VkPipeline bound_single = VK_NULL_HANDLE;  // under MSAA: the single-sampled pipeline whose twin is bound
    EyeMatrix last_view{}, last_world{}, last_clip{}, eye_space{}, transform{};
    bool cached = false;
    std::uint32_t perspective = 0u, flat = 0u, sprites = 0u;
    float nearest = 0.0f;
    // The scene's projection, from the last 3D draw: a screen-space sprite the game projected itself (a light's
    // glow, a flare) is taken back into the scene through it and drawn where the eye sees that place.
    // PURE_VR_SPRITES=flat leaves them on the interface's rectangle; PURE_VR_TRACE_FLAT lists the flat draws.
    static const bool sprites_flat = [] {
        const char *text = portablekit::env("VR_SPRITES");
        return text != nullptr && std::strcmp(text, "flat") == 0;
    }();
    static const bool trace_flat = portablekit::env("VR_TRACE_FLAT") != nullptr;
    // A screen-space draw over (nearly) the whole screen is an effect on the picture (a glare, a tint, a flash):
    // drawn over the whole eye, not on the interface's rectangle. PURE_VR_FULLSCREEN=hud puts it there, =off leaves
    // it out.
    static const int fullscreen_mode = [] {
        const char *text = portablekit::env("VR_FULLSCREEN");
        if (text != nullptr && std::strcmp(text, "hud") == 0) return 1;
        if (text != nullptr && std::strcmp(text, "off") == 0) return 2;
        return 0;
    }();
    std::uint32_t fullscreen = 0u, glows = 0u;
    // A whole-screen draw textured from video memory is the game's glow: its own (flat, one-eyed) picture blurred and
    // added on top. Over the eyes it lands beside what it lights and blinds; it is left out unless PURE_VR_GLOW=on.
    static const bool glow_on = [] {
        const char *text = portablekit::env("VR_GLOW");
        return text != nullptr && std::strcmp(text, "on") == 0;
    }();
    const bool tracing = trace_flat && eye == 0u && stereo_frames % 360u == 0u;
    bool scene_valid = false;
    EyeMatrix scene_clip{}, sprite_transform{};
    float scene_z_scale = 0.0f, scene_z_offset = 0.0f;
    bool sprite_valid = false;
    const auto *vertices = static_cast<const std::uint8_t *>(vertex_mapped);
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
            if (!scene_valid || summary.projection != scene_clip || summary.z_scale != scene_z_scale ||
                summary.z_offset != scene_z_offset) {
                scene_clip = summary.projection;
                scene_z_scale = summary.z_scale;
                scene_z_offset = summary.z_offset;
                scene_valid = scene_z_scale != 0.0f;
                sprite_valid = false;
            }
            const auto &t = summary.eye_translation;
            const float distance = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
            if (nearest == 0.0f || distance < nearest) nearest = distance;
        } else if (summary.clear) {
            state.viewport = {0.0f, 0.0f, width, height, min_depth, max_depth};
            state.scissor = all;
        } else if (const int cover = [&] {
                       // 2: the game's glow, 1: an effect over the whole screen, or right across it (a hit's flash,
                       // a tint, strips of a wipe), 0: anything else. Screen-space draws by their vertices, transformed
                       // 2D draws (orthographic) by where their own matrices put them.
                       if (drawn.raw || drawn.vertex_count == 0u || drawn.vertex_count > 4096u ||
                           drawn.vertex_base + drawn.vertex_count * sizeof(GpuVertex) > kVertexBufferTotal)
                           return 0;
                       const auto *v = reinterpret_cast<const GpuVertex *>(vertices + drawn.vertex_base);
                       float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                       if (summary.through) {
                           for (std::uint32_t i = 0; i < drawn.vertex_count; ++i) {
                               x0 = std::min(x0, v[i].x), x1 = std::max(x1, v[i].x);
                               y0 = std::min(y0, v[i].y), y1 = std::max(y1, v[i].y);
                           }
                       } else {
                           const EyeMatrix m = interpolation::multiply(
                               summary.projection, interpolation::multiply(summary.view, summary.world));
                           for (std::uint32_t i = 0; i < drawn.vertex_count; ++i) {
                               const float cx = m[0] * v[i].x + m[4] * v[i].y + m[8] * v[i].z + m[12];
                               const float cy = m[1] * v[i].x + m[5] * v[i].y + m[9] * v[i].z + m[13];
                               const float cw = m[3] * v[i].x + m[7] * v[i].y + m[11] * v[i].z + m[15];
                               if (std::fabs(cw) < 1e-6f) return 0;
                               const float sx = (cx / cw + 1.0f) * 0.5f * static_cast<float>(kPspWidth);
                               const float sy = (cy / cw + 1.0f) * 0.5f * static_cast<float>(kPspHeight);
                               x0 = std::min(x0, sx), x1 = std::max(x1, sx);
                               y0 = std::min(y0, sy), y1 = std::max(y1, sy);
                           }
                       }
                       const float w = std::min(x1, static_cast<float>(kPspWidth)) - std::max(x0, 0.0f);
                       const float h = std::min(y1, static_cast<float>(kPspHeight)) - std::max(y0, 0.0f);
                       const bool whole = w >= 0.9f * static_cast<float>(kPspWidth) &&
                                          h >= 0.9f * static_cast<float>(kPspHeight);
                       const bool across = w >= 0.9f * static_cast<float>(kPspWidth) ||
                                           h >= 0.9f * static_cast<float>(kPspHeight);
                       const std::uint32_t texture = summary.texture_address & 0x0FFFFFFFu;
                       const bool from_vram = texture >= 0x04000000u && texture < 0x04800000u;
                       const int kind = whole && from_vram && summary.through && !glow_on ? 2
                                        : across && fullscreen_mode != 1                   ? 1
                                                                                           : 0;
                       // Each new kind of screen effect once (by texture and size), and every draw while tracing.
                       static std::vector<std::uint64_t> seen;
                       const std::uint64_t key = (static_cast<std::uint64_t>(summary.texture_address) << 24u) ^
                                                 (static_cast<std::uint64_t>(drawn.vertex_count) << 4u) ^
                                                 (summary.through ? 1u : 0u) ^ (whole ? 2u : 0u);
                       const bool fresh = kind != 0 && seen.size() < 256u &&
                                          std::find(seen.begin(), seen.end(), key) == seen.end();
                       if (fresh) seen.push_back(key);
                       if (fresh || (tracing && summary.through))
                           std::printf("[vr] %s draw: %u vertices, box %.0f,%.0f..%.0f,%.0f, texture 0x%x: %s\n",
                                       summary.through ? "screen-space" : "transformed 2D", drawn.vertex_count,
                                       static_cast<double>(x0), static_cast<double>(y0), static_cast<double>(x1),
                                       static_cast<double>(y1), summary.texture_address,
                                       kind == 2 ? "the glow, left out" : kind == 1 ? "over the whole eye" : "flat");
                       return kind;
                   }();
                   cover != 0) {
            if (cover == 2) {
                ++glows;
                continue;
            }
            if (fullscreen_mode == 2) continue;
            state.viewport = {0.0f, 0.0f, width, height, min_depth, max_depth};
            state.scissor = all;
            ++fullscreen;
        } else if (const bool sprite = [&] {
                       // A few screen-space vertices with a depth between the screen's nearest and farthest.
                       if (sprites_flat || !scene_valid || !summary.through || drawn.raw || drawn.vertex_count == 0u ||
                           drawn.vertex_count > 32u ||
                           drawn.vertex_base + drawn.vertex_count * sizeof(GpuVertex) > kVertexBufferTotal)
                           return false;
                       const auto *v = reinterpret_cast<const GpuVertex *>(vertices + drawn.vertex_base);
                       float low = 65535.0f, high = 0.0f;
                       for (std::uint32_t i = 0; i < drawn.vertex_count; ++i) {
                           low = std::min(low, v[i].z);
                           high = std::max(high, v[i].z);
                       }
                       const bool deep = low > 0.5f && high < 65534.5f;
                       if (tracing)
                           std::printf("[vr] flat draw: %u vertices at %.0f,%.0f, z %.0f..%.0f: %s\n",
                                       drawn.vertex_count, static_cast<double>(v[0].x), static_cast<double>(v[0].y),
                                       static_cast<double>(low), static_cast<double>(high), deep ? "sprite" : "flat");
                       return deep;
                   }();
                   sprite) {
            if (!sprite_valid) {
                // Screen pixels and depth to the game's clip space, back to its eye space, to this eye's clip space,
                // and its z to the device depth (times w) the GE's depth range gives.
                EyeMatrix to_clip{};
                to_clip[0] = 2.0f / static_cast<float>(kPspWidth);
                to_clip[5] = 2.0f / static_cast<float>(kPspHeight);
                to_clip[10] = 1.0f / scene_z_scale;
                to_clip[12] = -1.0f;
                to_clip[13] = -1.0f;
                to_clip[14] = -scene_z_offset / scene_z_scale;
                to_clip[15] = 1.0f;
                EyeMatrix from_clip{};
                if (invert(scene_clip, from_clip)) {
                    const EyeAxes axes = eye_axes(scene_clip);
                    const EyeMatrix flip{axes.x, 0.0f, 0.0f, 0.0f, 0.0f, axes.y, 0.0f, 0.0f,
                                         0.0f, 0.0f, axes.z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
                    EyeMatrix depth{};
                    depth[0] = depth[5] = depth[15] = 1.0f;
                    depth[10] = scene_z_scale / 65535.0f;
                    depth[14] = scene_z_offset / 65535.0f;
                    const EyeMatrix head = interpolation::multiply(flip, interpolation::multiply(from_reference, flip));
                    sprite_transform = interpolation::multiply(
                        depth, interpolation::multiply(
                                   eye_projection(scene_clip, axes, view),
                                   interpolation::multiply(head, interpolation::multiply(from_clip, to_clip))));
                    sprite_valid = true;
                }
            }
            state.push.transform = sprite_transform;
            state.push.viewport[2] = 2.0f;  // ge.vert: a through draw placed by the transform
            state.viewport = {0.0f, 0.0f, width, height, 0.0f, 1.0f};
            state.scissor = all;
            ++sprites;
            if (!sprite_valid) continue;
        } else {
            if (tracing && !summary.through) {
                // Where the game's own transform puts the draw's first vertex on the PSP's screen, and its depth.
                float sx = 0.0f, sy = 0.0f, sz = 0.0f;
                if (!drawn.raw && drawn.vertex_count != 0u &&
                    drawn.vertex_base + sizeof(GpuVertex) <= kVertexBufferTotal) {
                    const auto *v = reinterpret_cast<const GpuVertex *>(vertices + drawn.vertex_base);
                    const EyeMatrix m = interpolation::multiply(summary.projection,
                                                                interpolation::multiply(summary.view, summary.world));
                    const float cx = m[0] * v->x + m[4] * v->y + m[8] * v->z + m[12];
                    const float cy = m[1] * v->x + m[5] * v->y + m[9] * v->z + m[13];
                    const float cz = m[2] * v->x + m[6] * v->y + m[10] * v->z + m[14];
                    const float cw = m[3] * v->x + m[7] * v->y + m[11] * v->z + m[15];
                    if (cw != 0.0f) {
                        sx = (cx / cw + 1.0f) * 0.5f * static_cast<float>(kPspWidth);
                        sy = (cy / cw + 1.0f) * 0.5f * static_cast<float>(kPspHeight);
                        sz = cz / cw * summary.z_scale + summary.z_offset;
                    }
                }
                std::printf("[vr] flat draw: %u vertices, transformed (%s), first at %.0f,%.0f depth %.0f, "
                            "texture 0x%x\n",
                            drawn.vertex_count, summary.perspective ? "perspective" : "orthographic",
                            static_cast<double>(sx), static_cast<double>(sy), static_cast<double>(sz),
                            summary.texture_address);
            }
            // Onto the interface's plane: a through draw by its pixels (ge.vert's placed path), a transformed one by
            // its own clip space; z keeps the draw's own depth.
            EyeMatrix placed{};
            if (summary.through) {
                placed = hud_through;
                placed[2] = placed[6] = placed[10] = placed[14] = 0.0f;
                placed[10] = hud_distance / 65535.0f;
                state.push.viewport[2] = 2.0f;
            } else {
                const EyeMatrix &own = drawn.state.push.transform;
                placed = interpolation::multiply(hud_base, own);
                for (std::size_t column = 0; column < 4u; ++column)
                    placed[column * 4u + 2u] = hud_distance * own[column * 4u + 2u];
            }
            state.push.transform = placed;
            state.viewport = {0.0f, 0.0f, width, height, min_depth, max_depth};
            // The draw's scissor (in the flat frame's pixels), as the box its corners make on the plane.
            const VkRect2D &cut = drawn.state.scissor;
            const bool whole_cut = cut.offset.x <= 0 && cut.offset.y <= 0 &&
                                   cut.offset.x + static_cast<std::int32_t>(cut.extent.width) >=
                                       static_cast<std::int32_t>(source.width) &&
                                   cut.offset.y + static_cast<std::int32_t>(cut.extent.height) >=
                                       static_cast<std::int32_t>(source.height);
            if (whole_cut) {
                state.scissor = all;
            } else {
                const float cx0 = 2.0f * static_cast<float>(cut.offset.x) / static_cast<float>(source.width) - 1.0f;
                const float cy0 = 2.0f * static_cast<float>(cut.offset.y) / static_cast<float>(source.height) - 1.0f;
                const float cx1 = cx0 + 2.0f * static_cast<float>(cut.extent.width) / static_cast<float>(source.width);
                const float cy1 = cy0 + 2.0f * static_cast<float>(cut.extent.height) / static_cast<float>(source.height);
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const auto &corner : {hud_pixel(cx0, cy0), hud_pixel(cx1, cy0), hud_pixel(cx0, cy1), hud_pixel(cx1, cy1)}) {
                    x0 = std::min(x0, corner[0]), x1 = std::max(x1, corner[0]);
                    y0 = std::min(y0, corner[1]), y1 = std::max(y1, corner[1]);
                }
                x0 = std::clamp(std::floor(x0), 0.0f, width), x1 = std::clamp(std::ceil(x1), 0.0f, width);
                y0 = std::clamp(std::floor(y0), 0.0f, height), y1 = std::clamp(std::ceil(y1), 0.0f, height);
                state.scissor = {{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)},
                                 {static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0)}};
            }
            ++flat;
        }

        if (!known || std::memcmp(&state.viewport, &set.viewport, sizeof(VkViewport)) != 0)
            vkCmdSetViewport(commands, 0u, 1u, &state.viewport);
        if (!known || state.blend != set.blend) vkCmdSetBlendConstants(commands, state.blend.data());
        if (!known || std::memcmp(&state.scissor, &set.scissor, sizeof(VkRect2D)) != 0)
            vkCmdSetScissor(commands, 0u, 1u, &state.scissor);
        if (!msaa && (!known || state.pipeline != set.pipeline))
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
        if (msaa && state.pipeline != bound_single) {
            bound_single = state.pipeline;
            const VkPipeline twin = msaa_pipeline(state.pipeline);
            if (twin == VK_NULL_HANDLE) continue;
            vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, twin);
        } else if (msaa && msaa_pipeline(state.pipeline) == VK_NULL_HANDLE) {
            continue;
        }
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
        if (eye_gpu_samples != 0u)
            std::printf("[vr] GPU time of the two eyes: %.2f ms a frame (%u frames); CPU: the frame's end %.2f ms, "
                        "the eyes' end and submit %.2f ms; game frame %ux%u\n",
                        eye_gpu_ms / static_cast<double>(eye_gpu_samples), eye_gpu_samples,
                        frame_end_ms / static_cast<double>(eye_gpu_samples),
                        eye_submit_ms / static_cast<double>(eye_gpu_samples), target_extent.width,
                        target_extent.height);
        frame_end_ms = eye_submit_ms = 0.0;
        eye_gpu_ms = 0.0;
        eye_gpu_samples = 0u;
        std::printf("[vr] stereo: %u 3D, %u full-screen (%u glow left out), %u sprite and %u flat draw calls, eye %ux%u, nearest 3D object %.2f units away, "
                    "%.3g units a metre, interface %.0f..%.0f x %.0f..%.0f\n",
                    perspective, fullscreen, glows, sprites, flat, target.extent.width, target.extent.height, static_cast<double>(nearest),
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

