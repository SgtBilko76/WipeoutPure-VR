// Wipeout Pure VR: the headset side of the Quest build. An OpenXR session on
// the renderer's own Vulkan device (PortableKit's host/gpu/xr_bridge.hpp):
// the renderer draws each frame, the port's menu included, into a panel
// image, which is copied into an OpenXR swapchain and shown as a large
// curved screen in front of the player. The Touch controllers become the
// gamepad the port already understands, so every button stays rebindable in
// its Controls tab.
//
// Two threads. The game's (the renderer's) copies each frame it makes into
// the screen swapchain and releases the image, never waiting for the
// headset. The headset's own thread runs the OpenXR frame loop at the
// display's rate (xrWaitFrame, xrBeginFrame, xrEndFrame with the screen
// layer, which shows the image released last) and reads the controllers.
// The runtime has a Vulkan queue of its own (index 1, which the renderer
// never uses); the frame's copy is ordered before the runtime's use of the
// image by a semaphore the renderer's submit signals and an empty submit on
// the runtime's queue waits for. Every call that may touch the runtime's
// queue holds xr_mutex_.
//
// Stage 1 of the port (README-QUEST.md): the whole game on the screen; the
// stereo race view follows.
//
// Default mapping (the gamepad buttons in SDL's names, then the PSP button
// the port's gamepad mapping makes of them):
//   left stick          steer                     left stick
//   right trigger       thrust / confirm          south      -> cross
//   left trigger        use pickup (fire)         west       -> square
//   grips               airbrakes                 shoulders  -> L / R
//   A                   confirm                   south      -> cross
//   B                   back / look behind        north      -> triangle
//   X                   absorb pickup             east       -> circle
//   Y                   change view               back       -> select
//   left stick click    pause                     start
//   menu button         pause                     start
//   right stick         menu cursor               D-pad
//   left stick (menus)  menu cursor               D-pad (outside a race)
//   both stick clicks   the port's own menu       L3 + R3

#include "gpu/xr_bridge.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#include <android/log.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// The panel the renderer draws at: the PSP's 480x272 four times over.
constexpr std::uint32_t kPanelWidth = 1920u;
constexpr std::uint32_t kPanelHeight = 1088u;
// The screen: part of a cylinder around the player, its axis through the
// head at start. 2.2 m away and 1.3 rad (75 degrees) wide.
constexpr float kScreenRadius = 2.2f;
constexpr float kScreenAngle = 1.3f;
// Frames in flight on the renderer's side, at most: one semaphore each.
constexpr std::size_t kSemaphores = 4;

void vlog(const char *format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    __android_log_vprint(ANDROID_LOG_INFO, "PureVR", format, args);
    std::printf("[vr] ");
    std::vprintf(format, copy);
    std::printf("\n");
    std::fflush(stdout);
    va_end(copy);
}

void log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vlog(format, args);
    va_end(args);
}

bool ok(XrResult result, const char *what) {
    if (XR_SUCCEEDED(result)) return true;
    log("%s failed: %d", what, static_cast<int>(result));
    return false;
}

std::vector<std::string> split(const std::string &text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find(' ', at);
        const std::string word = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (!word.empty()) out.push_back(word);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return out;
}

// A digital button made from an analog value, with hysteresis so a half
// pulled trigger does not flicker.
struct Latch {
    bool on{};
    bool update(float value, float press = 0.45f, float release = 0.25f) {
        on = on ? value > release : value > press;
        return on;
    }
};

class QuestXr final : public portablekit::gpu::xr::Bridge {
public:
    bool prepare() override;
    void instance_extensions(std::vector<const char *> &extensions) override;
    VkPhysicalDevice physical_device(VkInstance instance) override;
    void device_extensions(VkPhysicalDevice physical_device, std::vector<const char *> &extensions) override;
    bool start(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device, std::uint32_t queue_family,
               std::uint32_t queue_index) override;
    [[nodiscard]] VkExtent2D panel_extent() const override { return {kPanelWidth, kPanelHeight}; }
    [[nodiscard]] float refresh_rate() const override { return refresh_.load(std::memory_order_relaxed); }
    bool begin_frame() override;
    void record_present(VkCommandBuffer commands, VkImage image, VkImageLayout layout, VkExtent2D extent) override;
    [[nodiscard]] VkSemaphore frame_semaphore() override {
        return acquired_ || stereo_acquired_ ? semaphores_[next_semaphore_] : VK_NULL_HANDLE;
    }
    void end_frame() override;
    [[nodiscard]] VkExtent2D stereo_extent() const override { return eye_extent_; }
    bool stereo_begin(portablekit::gpu::xr::StereoFrame &frame) override;
    void record_stereo(VkCommandBuffer commands, const std::array<VkImage, 2> &images, VkImageLayout layout,
                       VkExtent2D extent) override;
    [[nodiscard]] bool button(int button) const override {
        return button >= 0 && button < 32 && (buttons_.load(std::memory_order_relaxed) & (1u << button)) != 0u;
    }
    [[nodiscard]] float axis(int axis) const override {
        return axis >= 0 && axis < static_cast<int>(axes_.size()) ? axes_[static_cast<std::size_t>(axis)].load(
                                                                        std::memory_order_relaxed)
                                                                  : 0.0f;
    }

private:
    bool has_extension(const char *name) const {
        return std::find(available_.begin(), available_.end(), name) != available_.end();
    }
    void frame_loop();
    void record_copy(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkImage target,
                     VkExtent2D extent) const;
    bool create_eye_swapchains();
    void poll_events();
    bool create_actions();
    void read_input();
    XrAction make_action(const char *name, XrActionType type);
    XrPath path(const char *text) const {
        XrPath result = XR_NULL_PATH;
        xrStringToPath(instance_, text, &result);
        return result;
    }
    bool held(XrAction action) const;
    float value(XrAction action) const;
    XrVector2f stick(XrAction vector, XrAction x, XrAction y) const;

    XrInstance instance_{XR_NULL_HANDLE};
    XrSystemId system_{XR_NULL_SYSTEM_ID};
    XrSession session_{XR_NULL_HANDLE};
    XrSpace local_{XR_NULL_HANDLE};
    XrSwapchain swapchain_{XR_NULL_HANDLE};
    std::vector<XrSwapchainImageVulkanKHR> images_;
    std::int64_t swapchain_format_{};
    std::vector<std::string> available_;
    std::vector<std::string> instance_extensions_;
    std::vector<std::string> device_extensions_;
    bool cylinder_{};
    bool pico_{};
    bool refresh_ext_{};
    bool perf_ext_{};
    PFN_xrPerfSettingsSetPerformanceLevelEXT set_performance_level_{};

    PFN_xrGetVulkanInstanceExtensionsKHR get_instance_extensions_{};
    PFN_xrGetVulkanDeviceExtensionsKHR get_device_extensions_{};
    PFN_xrGetVulkanGraphicsDeviceKHR get_graphics_device_{};
    PFN_xrGetVulkanGraphicsRequirementsKHR get_graphics_requirements_{};
    PFN_xrGetDisplayRefreshRateFB get_refresh_rate_{};
    PFN_xrEnumerateDisplayRefreshRatesFB enumerate_refresh_rates_{};
    PFN_xrRequestDisplayRefreshRateFB request_refresh_rate_{};

    // Vulkan: the runtime's queue and the semaphores ordering the copies.
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue xr_queue_{VK_NULL_HANDLE};
    bool shared_queue_{};
    std::array<VkSemaphore, kSemaphores> semaphores_{};
    std::size_t next_semaphore_{};

    // Shared by the two threads.
    std::mutex xr_mutex_;
    std::atomic<bool> running_{};    // between xrBeginSession and xrEndSession
    std::atomic<bool> visible_{};    // VISIBLE or FOCUSED: frames are shown
    std::atomic<bool> has_image_{};  // the swapchain has had an image released
    std::atomic<float> refresh_{72.0f};
    std::thread thread_;
    XrSessionState state_{XR_SESSION_STATE_UNKNOWN};  // the frame thread's

    // The renderer's frame.
    bool acquired_{};
    bool stereo_acquired_{};
    std::array<XrPosef, 2> eye_pose_{};
    std::array<XrFovf, 2> eye_fov_{};
    std::array<VkImage, 2> eye_target_{};

    // The eyes' swapchains (stereo while the game shows a race).
    std::array<XrSwapchain, 2> eye_swapchains_{XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::array<std::vector<XrSwapchainImageVulkanKHR>, 2> eye_images_;
    VkExtent2D eye_extent_{};
    std::atomic<bool> stereo_ready_{};  // both eye swapchains have had an image released
    // What the frame thread shows: written by end_frame() under xr_mutex_.
    bool shown_stereo_{};
    std::array<XrPosef, 2> shown_pose_{};
    std::array<XrFovf, 2> shown_fov_{};
    // The display time of the headset's next frame, for locating the eyes.
    std::atomic<XrTime> next_display_time_{0};
    std::uint64_t stereo_presents_{};
    // The renderer's time in the bridge (ms, summed until logged): locating the eyes and waiting for their images,
    // the screen's image, and the release.
    struct Spent {
        double stereo{}, panel{}, release{}, lock{};
        std::uint32_t presents{};
    } spent_;

    XrActionSet actions_{XR_NULL_HANDLE};
    XrAction a_{}, b_{}, x_{}, y_{}, menu_{}, left_click_{}, right_click_{};
    XrAction left_trigger_{}, right_trigger_{}, left_grip_{}, right_grip_{};
    XrAction left_stick_{}, right_stick_{}, left_x_{}, left_y_{}, right_x_{}, right_y_{};
    Latch thrust_, fire_, brake_left_, brake_right_;
    std::array<Latch, 4> dpad_{};
    std::uint64_t input_frames_{};

    std::atomic<std::uint32_t> buttons_{};
    std::array<std::atomic<float>, SDL_GAMEPAD_AXIS_COUNT> axes_{};
};

bool QuestXr::prepare() {
    PFN_xrInitializeLoaderKHR initialize = nullptr;
    if (!ok(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                  reinterpret_cast<PFN_xrVoidFunction *>(&initialize)),
            "xrGetInstanceProcAddr(xrInitializeLoaderKHR)") ||
        initialize == nullptr)
        return false;
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity_local = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity_local == nullptr) {
        log("no JNI environment or activity: %s", SDL_GetError());
        return false;
    }
    JavaVM *vm = nullptr;
    env->GetJavaVM(&vm);
    jobject activity = env->NewGlobalRef(activity_local);
    env->DeleteLocalRef(activity_local);

    XrLoaderInitInfoAndroidKHR loader{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader.applicationVM = vm;
    loader.applicationContext = activity;
    if (!ok(initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader)), "xrInitializeLoaderKHR"))
        return false;

    std::uint32_t count = 0;
    if (!ok(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data());
    for (const XrExtensionProperties &p : properties) available_.emplace_back(p.extensionName);
    if (!has_extension(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME)) {
        log("the OpenXR runtime has no %s", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        return false;
    }
    std::vector<const char *> enabled{XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                      XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    cylinder_ = has_extension(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    if (cylinder_) enabled.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
    pico_ = has_extension(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    if (pico_) enabled.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
    refresh_ext_ = has_extension(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (refresh_ext_) enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    perf_ext_ = has_extension(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if (perf_ext_) enabled.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android.applicationVM = vm;
    android.applicationActivity = activity;
    XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
    create.next = &android;
    std::strncpy(create.applicationInfo.applicationName, "Wipeout Pure VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(create.applicationInfo.engineName, "PortableKit", XR_MAX_ENGINE_NAME_SIZE - 1);
    create.applicationInfo.applicationVersion = 1;
    create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    create.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    create.enabledExtensionNames = enabled.data();
    if (!ok(xrCreateInstance(&create, &instance_), "xrCreateInstance")) return false;

    XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};
    system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xrGetSystem(instance_, &system, &system_), "xrGetSystem")) return false;
    const auto get = [this](const char *name, auto &function) {
        return ok(xrGetInstanceProcAddr(instance_, name, reinterpret_cast<PFN_xrVoidFunction *>(&function)), name) &&
               function != nullptr;
    };
    if (!get("xrGetVulkanInstanceExtensionsKHR", get_instance_extensions_) ||
        !get("xrGetVulkanDeviceExtensionsKHR", get_device_extensions_) ||
        !get("xrGetVulkanGraphicsDeviceKHR", get_graphics_device_) ||
        !get("xrGetVulkanGraphicsRequirementsKHR", get_graphics_requirements_))
        return false;
    if (refresh_ext_ && (!get("xrGetDisplayRefreshRateFB", get_refresh_rate_) ||
                         !get("xrEnumerateDisplayRefreshRatesFB", enumerate_refresh_rates_) ||
                         !get("xrRequestDisplayRefreshRateFB", request_refresh_rate_)))
        refresh_ext_ = false;
    if (perf_ext_ && !get("xrPerfSettingsSetPerformanceLevelEXT", set_performance_level_)) perf_ext_ = false;
    XrInstanceProperties instance_properties{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance_, &instance_properties);
    log("OpenXR runtime %s %u.%u.%u, cylinder layer %s, PICO controllers %s", instance_properties.runtimeName,
        XR_VERSION_MAJOR(instance_properties.runtimeVersion), XR_VERSION_MINOR(instance_properties.runtimeVersion),
        XR_VERSION_PATCH(instance_properties.runtimeVersion), cylinder_ ? "yes" : "no", pico_ ? "yes" : "no");
    return true;
}

void QuestXr::instance_extensions(std::vector<const char *> &extensions) {
    std::uint32_t size = 0;
    get_instance_extensions_(instance_, system_, 0, &size, nullptr);
    std::string text(size, '\0');
    get_instance_extensions_(instance_, system_, size, &size, text.data());
    text.resize(std::strlen(text.c_str()));
    instance_extensions_ = split(text);
    for (const std::string &name : instance_extensions_) extensions.push_back(name.c_str());
    log("Vulkan instance extensions for the runtime: %s", text.c_str());
}

VkPhysicalDevice QuestXr::physical_device(VkInstance instance) {
    XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    get_graphics_requirements_(instance_, system_, &requirements);
    VkPhysicalDevice device = VK_NULL_HANDLE;
    if (!ok(get_graphics_device_(instance_, system_, instance, &device), "xrGetVulkanGraphicsDeviceKHR"))
        return VK_NULL_HANDLE;
    return device;
}

void QuestXr::device_extensions(VkPhysicalDevice, std::vector<const char *> &extensions) {
    std::uint32_t size = 0;
    get_device_extensions_(instance_, system_, 0, &size, nullptr);
    std::string text(size, '\0');
    get_device_extensions_(instance_, system_, size, &size, text.data());
    text.resize(std::strlen(text.c_str()));
    device_extensions_ = split(text);
    for (const std::string &name : device_extensions_) extensions.push_back(name.c_str());
    log("Vulkan device extensions for the runtime: %s", text.c_str());
}

bool QuestXr::start(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
                    std::uint32_t queue_family, std::uint32_t queue_index) {
    device_ = device;
    vkGetDeviceQueue(device, queue_family, queue_index, &xr_queue_);
    shared_queue_ = queue_index == 0u;
    for (VkSemaphore &semaphore : semaphores_) {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device, &info, nullptr, &semaphore) != VK_SUCCESS) {
            log("vkCreateSemaphore failed");
            return false;
        }
    }
    log("the runtime's queue: family %u index %u%s", queue_family, queue_index,
        shared_queue_ ? " (shared with the renderer: frames may stall)" : "");

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = instance;
    binding.physicalDevice = physical_device;
    binding.device = device;
    binding.queueFamilyIndex = queue_family;
    binding.queueIndex = queue_index;
    XrSessionCreateInfo session{XR_TYPE_SESSION_CREATE_INFO};
    session.next = &binding;
    session.systemId = system_;
    if (!ok(xrCreateSession(instance_, &session, &session_), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    space.poseInReferenceSpace.orientation.w = 1.0f;
    if (!ok(xrCreateReferenceSpace(session_, &space, &local_), "xrCreateReferenceSpace(LOCAL)")) return false;

    // The game's colours are gamma-encoded already: an sRGB swapchain takes
    // them byte for byte (the copy does not convert), and the compositor
    // decodes them as it should.
    std::uint32_t count = 0;
    xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
    std::vector<std::int64_t> formats(count);
    xrEnumerateSwapchainFormats(session_, count, &count, formats.data());
    for (std::int64_t format : formats)
        if (format == VK_FORMAT_R8G8B8A8_SRGB) swapchain_format_ = format;
    if (swapchain_format_ == 0)
        for (std::int64_t format : formats)
            if (format == VK_FORMAT_R8G8B8A8_UNORM) swapchain_format_ = format;
    if (swapchain_format_ == 0) {
        log("the runtime offers neither R8G8B8A8_SRGB nor _UNORM swapchains");
        return false;
    }
    XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    create.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    create.format = swapchain_format_;
    create.sampleCount = 1;
    create.width = kPanelWidth;
    create.height = kPanelHeight;
    create.faceCount = 1;
    create.arraySize = 1;
    create.mipCount = 1;
    if (!ok(xrCreateSwapchain(session_, &create, &swapchain_), "xrCreateSwapchain")) return false;
    xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr);
    images_.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(swapchain_, count, &count,
                               reinterpret_cast<XrSwapchainImageBaseHeader *>(images_.data()));
    log("screen swapchain %ux%u, format %lld, %u images", kPanelWidth, kPanelHeight,
        static_cast<long long>(swapchain_format_), count);
    if (refresh_ext_) {
        // The rates the headset offers, and the one PURE_VR_REFRESH asks for (120 by default: each frame of a 60-fps
        // race shown twice, evenly, where 72 repeats every fifth; PURE_VR_REFRESH=72 keeps the system's).
        std::uint32_t count = 0;
        enumerate_refresh_rates_(session_, 0, &count, nullptr);
        std::vector<float> rates(count);
        enumerate_refresh_rates_(session_, count, &count, rates.data());
        std::string offered;
        for (float r : rates) offered += " " + std::to_string(static_cast<int>(std::lround(r)));
        log("display refresh rates offered:%s", offered.c_str());
        const char *named = std::getenv("PURE_VR_REFRESH");
        if (const char *text = named != nullptr ? named : "120"; text != nullptr) {
            const float wanted = std::strtof(text, nullptr);
            float best = 0.0f;
            for (float r : rates)
                if (std::fabs(r - wanted) < 0.5f) best = r;
            if (best > 0.0f) ok(request_refresh_rate_(session_, best), "xrRequestDisplayRefreshRateFB");
            else log("PURE_VR_REFRESH=%s is not offered", text);
        }
        float rate = 0.0f;
        if (XR_SUCCEEDED(get_refresh_rate_(session_, &rate)) && rate > 0.0f) refresh_.store(rate);
    }
    log("display refresh %.0f Hz", static_cast<double>(refresh_.load()));
    // The headset's frames never come late (the frame loop has a thread of its own), so the system would keep
    // the clocks low while the game's thread works hard: the levels are asked for instead. PURE_VR_PERF_LEVEL:
    // boost, high (the default), low or powersave.
    if (perf_ext_) {
        XrPerfSettingsLevelEXT level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
        const char *named = std::getenv("PURE_VR_PERF_LEVEL");
        if (named != nullptr && std::strcmp(named, "boost") == 0) level = XR_PERF_SETTINGS_LEVEL_BOOST_EXT;
        if (named != nullptr && std::strcmp(named, "low") == 0) level = XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT;
        if (named != nullptr && std::strcmp(named, "powersave") == 0) level = XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT;
        const bool cpu = ok(set_performance_level_(session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level), "CPU level");
        const bool gpu = ok(set_performance_level_(session_, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, level), "GPU level");
        log("performance level %d for the CPU (%s) and the GPU (%s)", static_cast<int>(level), cpu ? "set" : "refused",
            gpu ? "set" : "refused");
    } else {
        log("no %s: the system picks the clocks", XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    }
    if (!create_eye_swapchains()) {
        eye_extent_ = {};
        log("no stereo: the race stays on the screen");
    }
    if (!create_actions()) return false;
    thread_ = std::thread([this] { frame_loop(); });
    return true;
}

// Two swapchains at the runtime's recommended eye size, scaled by
// PURE_VR_EYE_SCALE (default 0.9: smoother edges than 0.8 at 60 fps; 1.1 made the GPU the bottleneck).
bool QuestXr::create_eye_swapchains() {
    std::uint32_t count = 0;
    if (!ok(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
                                              &count, nullptr),
            "xrEnumerateViewConfigurationViews") ||
        count != 2u)
        return false;
    std::array<XrViewConfigurationView, 2> views{};
    for (XrViewConfigurationView &view : views) view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count,
                                      views.data());
    float scale = 0.9f;
    if (const char *text = std::getenv("PURE_VR_EYE_SCALE"); text != nullptr) {
        const float named = std::strtof(text, nullptr);
        if (named >= 0.3f && named <= 1.5f) scale = named;
    }
    const auto size = [scale](std::uint32_t recommended, std::uint32_t most) {
        const auto scaled = static_cast<std::uint32_t>(static_cast<float>(recommended) * scale) & ~7u;
        return std::clamp<std::uint32_t>(scaled, 256u, most);
    };
    eye_extent_ = {size(views[0].recommendedImageRectWidth, views[0].maxImageRectWidth),
                   size(views[0].recommendedImageRectHeight, views[0].maxImageRectHeight)};
    for (std::size_t eye = 0; eye < 2u; ++eye) {
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        create.format = swapchain_format_;
        create.sampleCount = 1;
        create.width = eye_extent_.width;
        create.height = eye_extent_.height;
        create.faceCount = 1;
        create.arraySize = 1;
        create.mipCount = 1;
        if (!ok(xrCreateSwapchain(session_, &create, &eye_swapchains_[eye]), "xrCreateSwapchain (eye)")) return false;
        xrEnumerateSwapchainImages(eye_swapchains_[eye], 0, &count, nullptr);
        eye_images_[eye].assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xrEnumerateSwapchainImages(eye_swapchains_[eye], count, &count,
                                   reinterpret_cast<XrSwapchainImageBaseHeader *>(eye_images_[eye].data()));
    }
    log("eye swapchains %ux%u (recommended %ux%u, scale %.2f)", eye_extent_.width, eye_extent_.height,
        views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight, static_cast<double>(scale));
    return true;
}

XrAction QuestXr::make_action(const char *name, XrActionType type) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    info.actionType = type;
    std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    std::string localized(name);
    std::replace(localized.begin(), localized.end(), '_', ' ');
    std::strncpy(info.localizedActionName, localized.c_str(), XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    XrAction action = XR_NULL_HANDLE;
    ok(xrCreateAction(actions_, &info, &action), name);
    return action;
}

bool QuestXr::create_actions() {
    XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(set.actionSetName, "racing");
    std::strcpy(set.localizedActionSetName, "Racing");
    if (!ok(xrCreateActionSet(instance_, &set, &actions_), "xrCreateActionSet")) return false;
    a_ = make_action("button_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    b_ = make_action("button_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    x_ = make_action("button_x", XR_ACTION_TYPE_BOOLEAN_INPUT);
    y_ = make_action("button_y", XR_ACTION_TYPE_BOOLEAN_INPUT);
    menu_ = make_action("menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    left_click_ = make_action("left_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    right_click_ = make_action("right_stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    left_trigger_ = make_action("left_trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    right_trigger_ = make_action("right_trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    left_grip_ = make_action("left_grip", XR_ACTION_TYPE_FLOAT_INPUT);
    right_grip_ = make_action("right_grip", XR_ACTION_TYPE_FLOAT_INPUT);
    left_stick_ = make_action("left_stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    right_stick_ = make_action("right_stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    // The stick's components as well: on Quest the vector binding alone can
    // stay at 0,0.
    left_x_ = make_action("left_stick_x", XR_ACTION_TYPE_FLOAT_INPUT);
    left_y_ = make_action("left_stick_y", XR_ACTION_TYPE_FLOAT_INPUT);
    right_x_ = make_action("right_stick_x", XR_ACTION_TYPE_FLOAT_INPUT);
    right_y_ = make_action("right_stick_y", XR_ACTION_TYPE_FLOAT_INPUT);

    const auto suggest = [this](const char *profile) {
        const std::vector<std::pair<XrAction, const char *>> pairs{
            {a_, "/user/hand/right/input/a/click"},
            {b_, "/user/hand/right/input/b/click"},
            {x_, "/user/hand/left/input/x/click"},
            {y_, "/user/hand/left/input/y/click"},
            {menu_, "/user/hand/left/input/menu/click"},
            {left_click_, "/user/hand/left/input/thumbstick/click"},
            {right_click_, "/user/hand/right/input/thumbstick/click"},
            {left_trigger_, "/user/hand/left/input/trigger/value"},
            {right_trigger_, "/user/hand/right/input/trigger/value"},
            {left_grip_, "/user/hand/left/input/squeeze/value"},
            {right_grip_, "/user/hand/right/input/squeeze/value"},
            {left_stick_, "/user/hand/left/input/thumbstick"},
            {right_stick_, "/user/hand/right/input/thumbstick"},
            {left_x_, "/user/hand/left/input/thumbstick/x"},
            {left_y_, "/user/hand/left/input/thumbstick/y"},
            {right_x_, "/user/hand/right/input/thumbstick/x"},
            {right_y_, "/user/hand/right/input/thumbstick/y"},
        };
        std::vector<XrActionSuggestedBinding> bindings;
        for (const auto &[action, where] : pairs) bindings.push_back({action, path(where)});
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = path(profile);
        suggested.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        return ok(xrSuggestInteractionProfileBindings(instance_, &suggested), profile);
    };
    if (!suggest("/interaction_profiles/oculus/touch_controller")) return false;
    if (pico_) suggest("/interaction_profiles/bytedance/pico4_controller");

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actions_;
    return ok(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets");
}

bool QuestXr::held(XrAction action) const {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(session_, &info, &state)) && state.isActive && state.currentState;
}

float QuestXr::value(XrAction action) const {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = action;
    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(session_, &info, &state)) || !state.isActive) return 0.0f;
    return state.currentState;
}

XrVector2f QuestXr::stick(XrAction vector, XrAction x, XrAction y) const {
    XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
    info.action = vector;
    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
    XrVector2f result{};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(session_, &info, &state)) && state.isActive)
        result = state.currentState;
    if (result.x == 0.0f && result.y == 0.0f) result = {value(x), value(y)};
    return result;
}

// The frame thread's: once a headset frame.
void QuestXr::read_input() {
    std::uint32_t bits = 0u;
    std::array<float, SDL_GAMEPAD_AXIS_COUNT> axes{};
    if (state_ == XR_SESSION_STATE_FOCUSED) {
        XrActiveActionSet active{actions_, XR_NULL_PATH};
        XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (XR_SUCCEEDED(xrSyncActions(session_, &sync))) {
            const auto set = [&bits](int button, bool on) {
                if (on) bits |= 1u << button;
            };
            const bool left_click = held(left_click_);
            const bool right_click = held(right_click_);
            set(SDL_GAMEPAD_BUTTON_SOUTH, held(a_) || thrust_.update(value(right_trigger_)));
            set(SDL_GAMEPAD_BUTTON_WEST, fire_.update(value(left_trigger_)));
            set(SDL_GAMEPAD_BUTTON_NORTH, held(b_));
            set(SDL_GAMEPAD_BUTTON_EAST, held(x_));
            set(SDL_GAMEPAD_BUTTON_BACK, held(y_));
            set(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, brake_left_.update(value(left_grip_), 0.5f, 0.3f));
            set(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, brake_right_.update(value(right_grip_), 0.5f, 0.3f));
            // Both stick clicks open the port's own menu (L3 + R3); the left
            // one alone pauses, as does the menu button where the system
            // leaves it to the app.
            if (left_click && right_click) {
                set(SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
                set(SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
            } else {
                set(SDL_GAMEPAD_BUTTON_START, left_click);
            }
            if (held(menu_)) set(SDL_GAMEPAD_BUTTON_START, true);
            const XrVector2f left = stick(left_stick_, left_x_, left_y_);
            const XrVector2f right = stick(right_stick_, right_x_, right_y_);
            axes[SDL_GAMEPAD_AXIS_LEFTX] = std::clamp(left.x, -1.0f, 1.0f);
            axes[SDL_GAMEPAD_AXIS_LEFTY] = std::clamp(-left.y, -1.0f, 1.0f);  // SDL: down is positive
            // The menus move with the D-pad: the right stick, and the left one too outside a race, and in a race
            // (the pause menu) its up and down while no thrust is held. Steering with thrust held never presses
            // the D-pad, which the game would read as digital steering on top of the stick; left and right stay
            // off in a race for the same reason. PURE_VR_PAUSE_STICK=0 leaves the left stick out of races.
            static const bool pause_stick = [] {
                const char *text = std::getenv("PURE_VR_PAUSE_STICK");
                return text == nullptr || std::strcmp(text, "0") != 0;
            }();
            XrVector2f menu = right;
            const bool racing = portablekit::gpu::xr::scene_3d();
            if (!racing) {
                if (std::fabs(left.x) > std::fabs(menu.x)) menu.x = left.x;
                if (std::fabs(left.y) > std::fabs(menu.y)) menu.y = left.y;
            } else if (pause_stick && !thrust_.on && std::fabs(left.y) > std::fabs(menu.y) &&
                       std::fabs(left.y) > 1.5f * std::fabs(left.x)) {
                menu.y = left.y;
            }
            set(SDL_GAMEPAD_BUTTON_DPAD_UP, dpad_[0].update(menu.y, 0.6f, 0.4f));
            set(SDL_GAMEPAD_BUTTON_DPAD_DOWN, dpad_[1].update(-menu.y, 0.6f, 0.4f));
            set(SDL_GAMEPAD_BUTTON_DPAD_LEFT, dpad_[2].update(-menu.x, 0.6f, 0.4f));
            set(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, dpad_[3].update(menu.x, 0.6f, 0.4f));
            // Every two seconds while something is held: what the controllers send.
            if ((++input_frames_ % 144u) == 0u && (bits != 0u || left.x != 0.0f || left.y != 0.0f))
                log("input: buttons 0x%x, left stick %.2f %.2f, right stick %.2f %.2f", bits, left.x, left.y,
                    right.x, right.y);
        }
    }
    buttons_.store(bits, std::memory_order_relaxed);
    for (std::size_t i = 0; i < axes.size(); ++i) axes_[i].store(axes[i], std::memory_order_relaxed);
}

// The frame thread's.
void QuestXr::poll_events() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto &changed = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
            state_ = changed.state;
            log("session state %d", static_cast<int>(state_));
            visible_.store(state_ == XR_SESSION_STATE_VISIBLE || state_ == XR_SESSION_STATE_FOCUSED);
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                std::lock_guard lock(xr_mutex_);
                running_.store(ok(xrBeginSession(session_, &begin), "xrBeginSession"));
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                std::lock_guard lock(xr_mutex_);
                running_.store(false);
                ok(xrEndSession(session_), "xrEndSession");
            } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                running_.store(false);
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            log("interaction profile changed");
        } else if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const auto &changed = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB &>(event);
            refresh_.store(changed.toDisplayRefreshRate);
            log("display refresh %.0f Hz", static_cast<double>(changed.toDisplayRefreshRate));
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            running_.store(false);
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// The headset's frame loop, on its own thread, at the display's rate.
void QuestXr::frame_loop() {
    std::uint64_t frames = 0;
    auto window_start = std::chrono::steady_clock::now();
    std::uint64_t window_frames = 0;
    while (state_ != XR_SESSION_STATE_EXITING && state_ != XR_SESSION_STATE_LOSS_PENDING) {
        poll_events();
        if (!running_.load()) {
            buttons_.store(0u);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
        if (!ok(xrWaitFrame(session_, &wait, &frame), "xrWaitFrame")) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        read_input();
        // The renderer's next eye images are shown from the headset's next frame on.
        next_display_time_.store(frame.predictedDisplayTime + frame.predictedDisplayPeriod);
        std::lock_guard lock(xr_mutex_);
        XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
        if (!ok(xrBeginFrame(session_, &begin), "xrBeginFrame")) continue;
        XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::array<XrCompositionLayerProjectionView, 2> eye_views{};
        const XrCompositionLayerBaseHeader *layers[1]{};
        std::uint32_t layer_count = 0;
        if (frame.shouldRender == XR_TRUE && shown_stereo_ && stereo_ready_.load()) {
            // The race in stereo: the eye images, with the poses they were drawn from (the
            // compositor turns them to where the head is now).
            for (std::size_t eye = 0; eye < 2u; ++eye) {
                eye_views[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                eye_views[eye].pose = shown_pose_[eye];
                eye_views[eye].fov = shown_fov_[eye];
                eye_views[eye].subImage = {eye_swapchains_[eye],
                                           {{0, 0},
                                            {static_cast<std::int32_t>(eye_extent_.width),
                                             static_cast<std::int32_t>(eye_extent_.height)}},
                                           0};
            }
            projection.space = local_;
            projection.viewCount = 2;
            projection.views = eye_views.data();
            layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection);
        } else if (frame.shouldRender == XR_TRUE && has_image_.load()) {
            const XrSwapchainSubImage sub{swapchain_, {{0, 0}, {static_cast<std::int32_t>(kPanelWidth),
                                                                static_cast<std::int32_t>(kPanelHeight)}}, 0};
            const float aspect = static_cast<float>(kPanelWidth) / static_cast<float>(kPanelHeight);
            if (cylinder_) {
                cylinder.space = local_;
                cylinder.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                cylinder.subImage = sub;
                cylinder.pose.orientation.w = 1.0f;
                cylinder.radius = kScreenRadius;
                cylinder.centralAngle = kScreenAngle;
                cylinder.aspectRatio = aspect;
                layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&cylinder);
            } else {
                const float width = kScreenRadius * kScreenAngle;
                quad.space = local_;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage = sub;
                quad.pose.orientation.w = 1.0f;
                quad.pose.position.z = -kScreenRadius;
                quad.size = {width, width / aspect};
                layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad);
            }
        }
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
        end.displayTime = frame.predictedDisplayTime;
        end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        end.layerCount = layer_count;
        end.layers = layers;
        ok(xrEndFrame(session_, &end), "xrEndFrame");
        ++frames;
        ++window_frames;
        const auto now = std::chrono::steady_clock::now();
        if (now - window_start >= std::chrono::seconds(10)) {
            const double seconds = std::chrono::duration<double>(now - window_start).count();
            log("headset frames: %.1f a second (%llu in all)", static_cast<double>(window_frames) / seconds,
                static_cast<unsigned long long>(frames));
            window_start = now;
            window_frames = 0;
        }
    }
    log("frame loop ended");
}

// The renderer's side. Never blocks for the headset.
bool QuestXr::begin_frame() {
    acquired_ = false;
    stereo_acquired_ = false;
    return running_.load() && visible_.load();
}

// Records the copy of `source` (in `layout`) into a swapchain image, leaving
// that in COLOR_ATTACHMENT_OPTIMAL as the runtime expects it.
void QuestXr::record_copy(VkCommandBuffer commands, VkImage source, VkImageLayout layout, VkImage target,
                          VkExtent2D extent) const {
    VkImageMemoryBarrier barriers[2]{};
    barriers[0] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = layout;
    barriers[0].newLayout = layout;
    barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barriers[0].image = source;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
    barriers[1] = barriers[0];
    barriers[1].srcAccessMask = 0u;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image = target;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr, 0u, nullptr, 2u, barriers);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, 0u, 1u};
    copy.dstSubresource = copy.srcSubresource;
    copy.extent = {extent.width, extent.height, 1u};
    vkCmdCopyImage(commands, source, layout, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &copy);
    VkImageMemoryBarrier done = barriers[1];
    done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    done.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    done.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    done.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0u,
                         0u, nullptr, 0u, nullptr, 1u, &done);
}

void QuestXr::record_present(VkCommandBuffer commands, VkImage image, VkImageLayout layout, VkExtent2D extent) {
    if (!running_.load()) return;
    const auto started = std::chrono::steady_clock::now();
    struct Add {
        double &to;
        std::chrono::steady_clock::time_point from;
        ~Add() { to += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count(); }
    } add{spent_.panel, started};
    std::uint32_t index = 0;
    {
        std::lock_guard lock(xr_mutex_);
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!ok(xrAcquireSwapchainImage(swapchain_, &acquire, &index), "xrAcquireSwapchainImage")) return;
    }
    // The compositor is done with the image it hands out at once, as a rule:
    // three images and one present between two of its frames at most.
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = 100'000'000;  // 100 ms
    if (!ok(xrWaitSwapchainImage(swapchain_, &wait), "xrWaitSwapchainImage")) {
        std::lock_guard lock(xr_mutex_);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchain_, &release);
        return;
    }
    acquired_ = true;
    record_copy(commands, image, layout, images_[index].image,
                {std::min(extent.width, kPanelWidth), std::min(extent.height, kPanelHeight)});
}

// Where the eyes are when the frame is shown, and an image of each eye's
// swapchain to copy their pictures into.
bool QuestXr::stereo_begin(portablekit::gpu::xr::StereoFrame &stereo) {
    if (!running_.load() || !visible_.load() || eye_extent_.width == 0u) return false;
    const auto started = std::chrono::steady_clock::now();
    struct Add {
        double &to;
        std::chrono::steady_clock::time_point from;
        ~Add() { to += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count(); }
    } add{spent_.stereo, started};
    const XrTime when = next_display_time_.load();
    if (when == 0) return false;
    std::array<XrView, 2> views{};
    for (XrView &view : views) view.type = XR_TYPE_VIEW;
    std::array<std::uint32_t, 2> index{};
    {
        const auto lock_start = std::chrono::steady_clock::now();
        std::lock_guard lock(xr_mutex_);
        spent_.lock += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - lock_start).count();
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = when;
        locate.space = local_;
        XrViewState state{XR_TYPE_VIEW_STATE};
        std::uint32_t count = 0;
        if (!ok(xrLocateViews(session_, &locate, &state, 2u, &count, views.data()), "xrLocateViews") || count != 2u)
            return false;
        constexpr XrViewStateFlags kTracked =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
        if ((state.viewStateFlags & kTracked) != kTracked) return false;
        for (std::size_t eye = 0; eye < 2u; ++eye) {
            XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (!ok(xrAcquireSwapchainImage(eye_swapchains_[eye], &acquire, &index[eye]), "xrAcquireSwapchainImage (eye)")) {
                if (eye == 1u) {
                    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(eye_swapchains_[0], &release);
                }
                return false;
            }
        }
    }
    bool waited = true;
    for (std::size_t eye = 0; eye < 2u; ++eye) {
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = 100'000'000;  // 100 ms
        waited = ok(xrWaitSwapchainImage(eye_swapchains_[eye], &wait), "xrWaitSwapchainImage (eye)") && waited;
    }
    if (!waited) {
        std::lock_guard lock(xr_mutex_);
        for (XrSwapchain swapchain : eye_swapchains_) {
            XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(swapchain, &release);
        }
        return false;
    }
    stereo.extent = eye_extent_;
    for (std::size_t eye = 0; eye < 2u; ++eye) {
        const XrView &view = views[eye];
        eye_pose_[eye] = view.pose;
        eye_fov_[eye] = view.fov;
        eye_target_[eye] = eye_images_[eye][index[eye]].image;
        auto &out = stereo.eyes[eye];
        out.position = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
        out.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                           view.pose.orientation.w};
        out.tan_left = std::tan(view.fov.angleLeft);
        out.tan_right = std::tan(view.fov.angleRight);
        out.tan_up = std::tan(view.fov.angleUp);
        out.tan_down = std::tan(view.fov.angleDown);
    }
    stereo_acquired_ = true;
    if (stereo_presents_++ % 720u == 0u)
        log("stereo frame %llu: eyes %.3f m apart, fov %.0f..%.0f x %.0f..%.0f degrees",
            static_cast<unsigned long long>(stereo_presents_),
            static_cast<double>(std::hypot(views[0].pose.position.x - views[1].pose.position.x,
                                           views[0].pose.position.y - views[1].pose.position.y,
                                           views[0].pose.position.z - views[1].pose.position.z)),
            static_cast<double>(views[0].fov.angleLeft * 57.2958f), static_cast<double>(views[0].fov.angleRight * 57.2958f),
            static_cast<double>(views[0].fov.angleDown * 57.2958f), static_cast<double>(views[0].fov.angleUp * 57.2958f));
    return true;
}

void QuestXr::record_stereo(VkCommandBuffer commands, const std::array<VkImage, 2> &images, VkImageLayout layout,
                            VkExtent2D extent) {
    if (!stereo_acquired_) return;
    for (std::size_t eye = 0; eye < 2u; ++eye)
        record_copy(commands, images[eye], layout, eye_target_[eye],
                    {std::min(extent.width, eye_extent_.width), std::min(extent.height, eye_extent_.height)});
}

void QuestXr::end_frame() {
    if (!acquired_ && !stereo_acquired_) return;
    const auto started = std::chrono::steady_clock::now();
    struct Add {
        double &to;
        std::chrono::steady_clock::time_point from;
        ~Add() { to += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from).count(); }
    } add{spent_.release, started};
    if (++spent_.presents == 300u) {
        log("bridge time a present: eyes %.2f ms (lock %.2f), screen %.2f ms, release %.2f ms",
            spent_.stereo / 300.0, spent_.lock / 300.0, spent_.panel / 300.0, spent_.release / 300.0);
        spent_ = {};
    }
    const bool panel = acquired_;
    const bool stereo = stereo_acquired_;
    acquired_ = false;
    stereo_acquired_ = false;
    VkSemaphore semaphore = semaphores_[next_semaphore_];
    next_semaphore_ = (next_semaphore_ + 1u) % semaphores_.size();
    std::lock_guard lock(xr_mutex_);
    // The runtime's queue waits for the renderer's copies before the runtime
    // uses the images it is about to be given.
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1u;
    submit.pWaitSemaphores = &semaphore;
    submit.pWaitDstStageMask = &stage;
    if (vkQueueSubmit(xr_queue_, 1u, &submit, VK_NULL_HANDLE) != VK_SUCCESS) log("the runtime queue's wait failed");
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (panel && ok(xrReleaseSwapchainImage(swapchain_, &release), "xrReleaseSwapchainImage")) has_image_.store(true);
    if (stereo) {
        bool released = true;
        for (XrSwapchain swapchain : eye_swapchains_)
            released = ok(xrReleaseSwapchainImage(swapchain, &release), "xrReleaseSwapchainImage (eye)") && released;
        if (released) {
            stereo_ready_.store(true);
            shown_pose_ = eye_pose_;
            shown_fov_ = eye_fov_;
        }
    }
    // A present without the eyes' pictures (the frontend, the port's menu) shows the screen.
    shown_stereo_ = stereo;
}

QuestXr &quest_xr() {
    static QuestXr bridge;
    return bridge;
}

// Registered when libmain.so loads, before the renderer starts.
const bool kRegistered = [] {
    portablekit::gpu::xr::set_bridge(&quest_xr());
    // A 60-fps race leaves 16.7 ms a frame: the performance hint asks for the clocks that finish the game's thread's
    // work in about 12 (env.txt, read later, can set PURE_PERFORMANCE_HINT_MS=25 for 30-fps races).
    setenv("PURE_PERFORMANCE_HINT_MS", "12", 0);
    return true;
}();

} // namespace
