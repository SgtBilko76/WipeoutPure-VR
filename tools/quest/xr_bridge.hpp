#pragma once

// An OpenXR runtime that the Vulkan renderer presents into instead of its
// window: a standalone VR headset. A game that wants one sets it with
// set_bridge() before the renderer starts. Without a bridge, or when
// prepare() says no, the renderer presents to its window as before.
//
// What the renderer does with an active bridge:
// - It asks for the runtime's Vulkan instance and device extensions, renders
//   on the runtime's physical device, makes a second queue on its graphics
//   family when the family has one, and hands the device over (start()) with
//   the queue the runtime may use: index 1 then, which the renderer itself
//   never touches, so the runtime's frame loop can run on a thread of its own
//   without sharing a queue with the renderer.
// - Its "swapchain" is a ring of its own images at panel_extent(), in a plain
//   8-bit UNORM format, so the picture and the in-game menu are drawn into
//   them exactly as into a window. Each present calls begin_frame() (which
//   must not block: the runtime's pace is the bridge's business), records the
//   frame into one of them, then record_present() copies that image into the
//   runtime's swapchain in the same command buffer. The renderer's submit
//   signals frame_semaphore() when it is not null, and end_frame() follows
//   the submit.
// - It does not present to the window, frame interpolation is off, and the
//   display's refresh it reports (the game's display rate) is refresh_rate().
//
// begin_frame(), record_present(), frame_semaphore() and end_frame() come
// from the renderer's thread; button() and axis() from the game's input poll.

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <vector>

namespace portablekit::gpu::xr {

class Bridge {
public:
    virtual ~Bridge() = default;

    // Before vkCreateInstance. False: no headset, present to the window.
    virtual bool prepare() = 0;
    virtual void instance_extensions(std::vector<const char *> &extensions) = 0;
    // After vkCreateInstance: the device the runtime renders with, or null.
    virtual VkPhysicalDevice physical_device(VkInstance instance) = 0;
    virtual void device_extensions(VkPhysicalDevice physical_device, std::vector<const char *> &extensions) = 0;
    // After vkCreateDevice. `queue_index` is the runtime's queue: 1 when the
    // renderer made a second one, 0 when it shares the renderer's. False: the
    // session could not start; the renderer then presents to its window.
    virtual bool start(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                       std::uint32_t queue_family, std::uint32_t queue_index) = 0;

    // The size of the virtual screen the renderer draws its frames at.
    [[nodiscard]] virtual VkExtent2D panel_extent() const = 0;
    // The headset display's refresh in Hz.
    [[nodiscard]] virtual float refresh_rate() const = 0;

    // A present. begin_frame() false: the runtime shows nothing now (the
    // session is not visible); end_frame() is still called. Never blocks.
    virtual bool begin_frame() = 0;
    // `image` holds the frame in `layout` (TRANSFER_SRC_OPTIMAL), `extent`
    // big. Records its copy into the runtime's swapchain.
    virtual void record_present(VkCommandBuffer commands, VkImage image, VkImageLayout layout,
                                VkExtent2D extent) = 0;
    // A binary semaphore the frame's submit signals, or null.
    [[nodiscard]] virtual VkSemaphore frame_semaphore() = 0;
    // After the command buffer was submitted.
    virtual void end_frame() = 0;

    // The Touch controllers, as SDL3 numbers a gamepad's buttons and axes
    // (shell/gamepad.hpp): read by the pad on every poll.
    [[nodiscard]] virtual bool button(int button) const = 0;
    [[nodiscard]] virtual float axis(int axis) const = 0;
};

inline Bridge *&bridge_slot() noexcept {
    static Bridge *slot = nullptr;
    return slot;
}
inline void set_bridge(Bridge *bridge) noexcept { bridge_slot() = bridge; }
[[nodiscard]] inline Bridge *bridge() noexcept { return bridge_slot(); }

// Set by the renderer once start() has succeeded.
inline bool &active_slot() noexcept {
    static bool slot = false;
    return slot;
}
[[nodiscard]] inline bool active() noexcept { return active_slot() && bridge() != nullptr; }

} // namespace portablekit::gpu::xr
