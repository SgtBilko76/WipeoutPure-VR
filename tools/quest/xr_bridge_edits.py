#!/usr/bin/env python3
"""Adds the OpenXR bridge hooks (host/gpu/xr_bridge.hpp) to PortableKit's Vulkan
renderer, the SDL pad and the ImGui layer. Idempotent: run it on a tree that has
it already and nothing changes. Usage: xr_bridge_edits.py <portablekit dir>"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
changed = []


def edit(rel, old, new, count=1):
    path = root / rel
    text = path.read_text()
    if new in text and (old not in text or new not in old):
        return  # already applied
    found = text.count(old)
    assert found == count, f"{rel}: anchor found {found}x, want {count}: {old[:80]!r}"
    text = text.replace(old, new)
    path.write_text(text)
    changed.append(rel)


R = "host/gpu/vulkan_renderer.cpp"

edit(R, '#include "frame_interpolation.hpp"\n',
     '#include "frame_interpolation.hpp"\n#include "xr_bridge.hpp"\n')

# State.
edit(R, "    bool quit{};\n    bool ready{};\n",
     "    bool quit{};\n    bool ready{};\n"
     "    // Presenting to an OpenXR runtime (xr_bridge.hpp) instead of the window:\n"
     "    // the swapchain images are the renderer's own, xr_memory their memory.\n"
     "    bool xr{};\n"
     "    std::uint32_t xr_next_image{};\n"
     "    std::vector<VkDeviceMemory> xr_memory;\n")
edit(R, "    bool create_swapchain(std::string &error);\n",
     "    bool create_swapchain(std::string &error);\n"
     "    bool create_xr_images(std::string &error);\n")

# Instance extensions.
edit(R, "    if (portability_enumeration) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);\n",
     "    if (portability_enumeration) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);\n"
     "    // A headset (xr_bridge.hpp): the runtime's instance extensions too, each once.\n"
     "    xr::Bridge *const xr_bridge = xr::bridge();\n"
     "    const bool xr_wanted = xr_bridge != nullptr && xr_bridge->prepare();\n"
     "    const auto add_unique = [](std::vector<const char *> &list, const std::vector<const char *> &more) {\n"
     "        for (const char *name : more) {\n"
     "            bool have = false;\n"
     "            for (const char *existing : list) have = have || std::strcmp(existing, name) == 0;\n"
     "            if (!have) list.push_back(name);\n"
     "        }\n"
     "    };\n"
     "    if (xr_wanted) {\n"
     "        std::vector<const char *> wanted;\n"
     "        xr_bridge->instance_extensions(wanted);\n"
     "        add_unique(extensions, wanted);\n"
     "    }\n")

# Physical device.
edit(R, "    std::uint32_t family_count = 0u;\n    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical_device, &family_count, nullptr);\n",
     "    if (xr_wanted) {\n"
     "        if (VkPhysicalDevice chosen = xr_bridge->physical_device(impl.instance)) impl.physical_device = chosen;\n"
     "    }\n"
     "    std::uint32_t family_count = 0u;\n    vkGetPhysicalDeviceQueueFamilyProperties(impl.physical_device, &family_count, nullptr);\n")

# Device extensions.
edit(R, "    if (want_breadcrumbs && !impl.breadcrumbs.available)\n",
     "    if (xr_wanted) {\n"
     "        std::vector<const char *> wanted;\n"
     "        xr_bridge->device_extensions(impl.physical_device, wanted);\n"
     "        add_unique(enabled_device_extensions, wanted);\n"
     "    }\n"
     "    if (want_breadcrumbs && !impl.breadcrumbs.available)\n")

# Session.
edit(R, "    vkGetDeviceQueue(impl.device, impl.queue_family, 0u, &impl.queue);\n",
     "    vkGetDeviceQueue(impl.device, impl.queue_family, 0u, &impl.queue);\n"
     "    if (xr_wanted) {\n"
     "        impl.xr = xr_bridge->start(impl.instance, impl.physical_device, impl.device, impl.queue_family, 0u);\n"
     "        xr::active_slot() = impl.xr;\n"
     "        std::cout << \"[render] OpenXR \"\n"
     "                  << (impl.xr ? \"session started: frames go to the headset\" : \"failed: frames go to the window\")\n"
     "                  << std::endl;\n"
     "    }\n")

# Format: plain UNORM, the GE's colours are gamma-encoded already.
edit(R, "    impl.swapchain_format = impl.surface_format.format;\n",
     "    impl.swapchain_format = impl.surface_format.format;\n"
     "    if (impl.xr) impl.swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;\n")

# The swapchain is the renderer's own ring of images.
edit(R, "bool VulkanRenderer::Impl::create_swapchain(std::string &error) {\n    VkSurfaceCapabilitiesKHR capabilities{};\n",
     "bool VulkanRenderer::Impl::create_swapchain(std::string &error) {\n"
     "    if (xr) return create_xr_images(error);\n"
     "    VkSurfaceCapabilitiesKHR capabilities{};\n")
edit(R, "void VulkanRenderer::Impl::destroy_swapchain_views() {\n",
     "// The headset's frames (xr_bridge.hpp): three images of the panel's size,\n"
     "// made once, which the bridge copies into the runtime's swapchain.\n"
     "bool VulkanRenderer::Impl::create_xr_images(std::string &error) {\n"
     "    const VkExtent2D extent = xr::bridge()->panel_extent();\n"
     "    swapchain_quarter_turns = 0;\n"
     "    swapchain_image_extent = extent;\n"
     "    swapchain_extent = extent;\n"
     "    update_content_rect();\n"
     "    swapchain_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |\n"
     "                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT;\n"
     "    constexpr std::uint32_t kImages = 3u;\n"
     "    while (swapchain_images.size() < kImages) {\n"
     "        VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};\n"
     "        image_info.imageType = VK_IMAGE_TYPE_2D;\n"
     "        image_info.format = swapchain_format;\n"
     "        image_info.extent = {extent.width, extent.height, 1u};\n"
     "        image_info.mipLevels = 1u;\n"
     "        image_info.arrayLayers = 1u;\n"
     "        image_info.samples = VK_SAMPLE_COUNT_1_BIT;\n"
     "        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;\n"
     "        image_info.usage = swapchain_usage;\n"
     "        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;\n"
     "        VkImage image{};\n"
     "        if (!check(vkCreateImage(device, &image_info, nullptr, &image), \"vkCreateImage\", error)) return false;\n"
     "        VkMemoryRequirements requirements{};\n"
     "        vkGetImageMemoryRequirements(device, image, &requirements);\n"
     "        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};\n"
     "        allocate.allocationSize = requirements.size;\n"
     "        allocate.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);\n"
     "        VkDeviceMemory memory{};\n"
     "        if (!check(vkAllocateMemory(device, &allocate, nullptr, &memory), \"vkAllocateMemory\", error)) return false;\n"
     "        vkBindImageMemory(device, image, memory, 0u);\n"
     "        swapchain_images.push_back(image);\n"
     "        xr_memory.push_back(memory);\n"
     "    }\n"
     "    for (VkImage image : swapchain_images) {\n"
     "        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};\n"
     "        view_info.image = image;\n"
     "        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;\n"
     "        view_info.format = swapchain_format;\n"
     "        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};\n"
     "        VkImageView view{};\n"
     "        if (!check(vkCreateImageView(device, &view_info, nullptr, &view), \"vkCreateImageView\", error)) return false;\n"
     "        swapchain_views.push_back(view);\n"
     "    }\n"
     "    swapchain_min_images = kImages;\n"
     "    if (ui_render_pass != VK_NULL_HANDLE && !create_ui_framebuffers(error)) return false;\n"
     "    swapchain_dirty = false;\n"
     "    update_display_info();\n"
     "    std::cout << \"[render] headset panel \" << extent.width << \"x\" << extent.height << \", \" << kImages\n"
     "              << \" images\" << std::endl;\n"
     "    return true;\n"
     "}\n\n"
     "void VulkanRenderer::Impl::destroy_swapchain_views() {\n")
edit(R, "void VulkanRenderer::Impl::recreate_swapchain() {\n",
     "void VulkanRenderer::Impl::recreate_swapchain() {\n"
     "    if (xr) return;  // the panel keeps its size\n")
edit(R, "    void check_swapchain() {\n        if (!std::exchange(swapchain_check, false) || swapchain == VK_NULL_HANDLE) return;\n",
     "    void check_swapchain() {\n        if (xr) return;\n"
     "        if (!std::exchange(swapchain_check, false) || swapchain == VK_NULL_HANDLE) return;\n")

# submit_and_present: no window, the bridge's frame instead.
edit(R, "                                              VkExtent2D source_extent, bool game_frame, bool main_frame) {\n"
        "#if defined(__ANDROID__)\n"
        "    check_native_window();\n"
        "    if (surface_returned) reset_surface();\n"
        "    // No window to show it in: the frame is recorded and finished, not shown\n"
        "    // (an image acquired before the window went is still given back).\n"
        "    if (!surface_lost)\n"
        "#endif\n"
        "    if (!acquired_image) {\n",
     "                                              VkExtent2D source_extent, bool game_frame, bool main_frame) {\n"
     "#if defined(__ANDROID__)\n"
     "    if (!xr) {\n"
     "        check_native_window();\n"
     "        if (surface_returned) reset_surface();\n"
     "    }\n"
     "    // No window to show it in: the frame is recorded and finished, not shown\n"
     "    // (an image acquired before the window went is still given back).\n"
     "    if (!surface_lost && !xr)\n"
     "#else\n"
     "    if (!xr)\n"
     "#endif\n"
     "    if (!acquired_image) {\n")
edit(R, "    VkResult acquired = VK_ERROR_OUT_OF_DATE_KHR;\n    if (acquired_image) {\n        image_index = *acquired_image;\n",
     "    VkResult acquired = VK_ERROR_OUT_OF_DATE_KHR;\n"
     "    if (xr) {\n"
     "        // The runtime paces the frame (xrWaitFrame); one of the panel images takes it.\n"
     "        if (xr::bridge()->begin_frame() && has_content && !swapchain_images.empty()) {\n"
     "            image_index = xr_next_image;\n"
     "            xr_next_image = (xr_next_image + 1u) % static_cast<std::uint32_t>(swapchain_images.size());\n"
     "            acquired = VK_SUCCESS;\n"
     "        }\n"
     "        acquired_image.reset();\n"
     "    } else if (acquired_image) {\n        image_index = *acquired_image;\n")
edit(R, "#if defined(__ANDROID__)\n"
        "        if (prerotated() && image_index < upright_images.size()) record_rotation(commands, image_index, layout);\n"
        "        else\n"
        "#endif\n"
        "            transition(commands, target, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);\n",
     "        if (xr) {\n"
     "            transition(commands, target, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);\n"
     "            xr::bridge()->record_present(commands, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_extent);\n"
     "        } else\n"
     "#if defined(__ANDROID__)\n"
     "        if (prerotated() && image_index < upright_images.size()) record_rotation(commands, image_index, layout);\n"
     "        else\n"
     "#endif\n"
     "            transition(commands, target, layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);\n")
edit(R, "    if (can_present) {\n        submit.waitSemaphoreCount = 1u;\n",
     "    if (can_present && !xr) {\n        submit.waitSemaphoreCount = 1u;\n")
edit(R, "    if (can_present) {\n        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};\n",
     "    if (xr) xr::bridge()->end_frame();\n"
     "    if (can_present && !xr) {\n        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};\n")

# No frame interpolation into a headset: the runtime paces and reprojects.
edit(R, "bool VulkanRenderer::Impl::interpolation_wanted() const {\n",
     "bool VulkanRenderer::Impl::interpolation_wanted() const {\n    if (xr) return false;\n")

# The pad: the Touch controllers as one more gamepad.
P = "host/shell/sdl_pad.cpp"
edit(P, '#include "settings/settings.hpp"\n',
     '#include "settings/settings.hpp"\n#include "gpu/xr_bridge.hpp"\n')
edit(P, "    if (gamepad != nullptr) read_gamepad(gamepad, next, analog_x, analog_y);\n",
     "    if (gamepad != nullptr) read_gamepad(gamepad, next, analog_x, analog_y);\n"
     "    // A headset's controllers (gpu/xr_bridge.hpp), through the same mapping.\n"
     "    if (const gpu::xr::Bridge *xr = gpu::xr::active() ? gpu::xr::bridge() : nullptr) {\n"
     "        read_gamepad([xr](GamepadButton button) { return xr->button(static_cast<int>(button)); },\n"
     "                     [xr](GamepadAxis id) { return xr->axis(static_cast<int>(id)); }, next, analog_x, analog_y);\n"
     "    }\n")

# The interface is laid out on the headset's panel, not the window.
L = "host/ui/layer.cpp"
edit(L, '#include "shell/shell.hpp"\n',
     '#include "shell/shell.hpp"\n#include "gpu/xr_bridge.hpp"\n')
edit(L, "    ImGui_ImplSDL3_NewFrame();\n    ImGuiIO &io = ImGui::GetIO();\n",
     "    ImGui_ImplSDL3_NewFrame();\n    ImGuiIO &io = ImGui::GetIO();\n"
     "    if (gpu::xr::active()) {\n"
     "        const VkExtent2D panel = gpu::xr::bridge()->panel_extent();\n"
     "        io.DisplaySize = {static_cast<float>(panel.width), static_cast<float>(panel.height)};\n"
     "        io.DisplayFramebufferScale = {1.0f, 1.0f};\n"
     "    }\n")

# Android apps without the system's document picker (a VR headset): with
# PORTABLEKIT_ANDROID_EXTERNAL_DATA the data directory is the app's folder on
# shared storage, its game/ is the game directory once it holds the game, and
# tar archives dropped into its import/ are unpacked there at start (folders
# made over adb or USB in Android/data are not the app's own).
A = "host/platform/android_app.cpp"
edit(A, "#include <pthread.h>\n",
     "#include <pthread.h>\n#include <cstdlib>\n#include <cstring>\n#include <vector>\n")
edit(A, "} // namespace\n\nint main(int argc, char *argv[]) {\n    redirect_output();\n",
     "#if defined(PORTABLEKIT_ANDROID_EXTERNAL_DATA)\n"
     "// Unpacks a ustar archive (regular files and directories) into `into`.\n"
     "bool unpack_tar(const std::filesystem::path &tar, const std::filesystem::path &into) {\n"
     "    std::ifstream in(tar, std::ios::binary);\n"
     "    if (!in) return false;\n"
     "    std::vector<char> buffer(1u << 20);\n"
     "    char header[512];\n"
     "    std::error_code ec;\n"
     "    std::size_t files = 0;\n"
     "    while (in.read(header, sizeof(header))) {\n"
     "        if (header[0] == '\\0') break;  // the end: zero blocks\n"
     "        const auto field = [&](std::size_t at, std::size_t length) {\n"
     "            return std::string(header + at, strnlen(header + at, length));\n"
     "        };\n"
     "        std::string name = field(0, 100);\n"
     "        if (std::memcmp(header + 257, \"ustar\", 5) == 0 && header[345] != '\\0') name = field(345, 155) + \"/\" + name;\n"
     "        const std::uint64_t size = std::strtoull(field(124, 12).c_str(), nullptr, 8);\n"
     "        const char type = header[156];\n"
     "        const std::filesystem::path relative = std::filesystem::path(name).lexically_normal();\n"
     "        const bool safe = !relative.empty() && relative.is_relative() && *relative.begin() != \"..\";\n"
     "        const std::filesystem::path target = into / relative;\n"
     "        if (safe && type == '5') {\n"
     "            std::filesystem::create_directories(target, ec);\n"
     "        } else if (safe && (type == '0' || type == '\\0')) {\n"
     "            std::filesystem::create_directories(target.parent_path(), ec);\n"
     "            std::ofstream out(target, std::ios::binary | std::ios::trunc);\n"
     "            std::uint64_t left = size;\n"
     "            while (left > 0 && in) {\n"
     "                const auto chunk = static_cast<std::streamsize>(std::min<std::uint64_t>(left, buffer.size()));\n"
     "                in.read(buffer.data(), chunk);\n"
     "                out.write(buffer.data(), in.gcount());\n"
     "                left -= static_cast<std::uint64_t>(in.gcount());\n"
     "            }\n"
     "            if (left != 0) return false;\n"
     "            ++files;\n"
     "            in.seekg(static_cast<std::streamoff>((512u - size % 512u) % 512u), std::ios::cur);\n"
     "            continue;\n"
     "        }\n"
     "        in.seekg(static_cast<std::streamoff>((size + 511u) / 512u * 512u), std::ios::cur);\n"
     "    }\n"
     "    std::printf(\"[android] unpacked %zu files from %s\\n\", files, tar.filename().c_str());\n"
     "    return true;\n"
     "}\n"
     "\n"
     "void use_external_storage() {\n"
     "    const char *external = SDL_GetAndroidExternalStoragePath();\n"
     "    if (external == nullptr) return;\n"
     "    const std::filesystem::path root(external);\n"
     "    std::error_code ec;\n"
     "    std::filesystem::create_directories(root / \"import\", ec);\n"
     "    std::filesystem::create_directories(root / \"game\" / \"ms0\" / \"PSP\" / \"SAVEDATA\", ec);\n"
     "    // Every step logged and nothing thrown out of here: this runs before the game, on SDL's thread.\n"
     "    try {\n"
     "        std::vector<std::filesystem::path> archives;\n"
     "        std::filesystem::directory_iterator it(root / \"import\", ec), end;\n"
     "        for (; !ec && it != end; it.increment(ec))\n"
     "            if (it->path().extension() == \".tar\") archives.push_back(it->path());\n"
     "        if (ec) std::printf(\"[android] cannot list %s: %s\\n\", (root / \"import\").c_str(), ec.message().c_str());\n"
     "        for (const std::filesystem::path &archive : archives) {\n"
     "            std::printf(\"[android] unpacking %s\\n\", archive.c_str());\n"
     "            std::fflush(stdout);\n"
     "            if (unpack_tar(archive, root)) std::filesystem::remove(archive, ec);\n"
     "            else std::printf(\"[android] cannot unpack %s\\n\", archive.c_str());\n"
     "        }\n"
     "    } catch (const std::exception &e) {\n"
     "        std::printf(\"[android] unpacking the imports failed: %s\\n\", e.what());\n"
     "    }\n"
     "    setenv(portablekit::env_name(\"DATA_DIR\").c_str(), root.c_str(), 0);\n"
     "    const std::filesystem::path game = root / \"game\";\n"
     "    if (std::filesystem::is_regular_file(game / \"disc.iso\", ec) &&\n"
     "        std::filesystem::is_regular_file(game / \"EBOOT.ELF\", ec))\n"
     "        setenv(portablekit::env_name(\"GAME_DIR\").c_str(), game.c_str(), 0);\n"
     "    std::printf(\"[android] data in %s, game %s\\n\", root.c_str(),\n"
     "                std::getenv(portablekit::env_name(\"GAME_DIR\").c_str()) != nullptr ? \"found\" : \"not there yet\");\n"
     "    std::fflush(stdout);\n"
     "}\n"
     "#endif\n"
     "\n"
     "} // namespace\n\nint main(int argc, char *argv[]) {\n    redirect_output();\n"
     "#if defined(PORTABLEKIT_ANDROID_EXTERNAL_DATA)\n"
     "    use_external_storage();\n"
     "#endif\n")

print("changed:", sorted(set(changed)) or "nothing (already applied)")
