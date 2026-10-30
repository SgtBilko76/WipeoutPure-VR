cmake_minimum_required(VERSION 3.20)
project(PureRecomp VERSION 0.2.0 LANGUAGES C CXX)

# The framework (TeamGDB/PortableKit, MIT) is vendored at the commit in vendor/PIN by tools/vendor_setup.sh.
set(PSPRECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/portablekit portablekit)

# The Metal GE backend, the Metal shell and ImGui's Metal renderer backend are macOS only (docs/windows.md); on
# Windows and Linux the framework's Vulkan renderer and shell are the path, and none of src/gpu/ is built.
if(APPLE)
    # The Metal GE backend's files, by concern (docs/metal-backend.md, "Files"; task 049).
    set(metal_backend_sources src/gpu/metal_backend.mm src/gpu/metal_targets.mm src/gpu/metal_textures.mm
        src/gpu/metal_texture_pack.mm src/gpu/metal_pipelines.mm src/gpu/metal_effects.mm src/gpu/metal_trace.mm
        src/gpu/metal_draw.mm)
    set(platform_sources ${metal_backend_sources} src/gpu/sprite_upscale.cpp src/gpu/metal_device.mm
        src/host/metal_shell.mm third_party/imgui_metal/imgui_impl_metal.mm)
else()
    set(platform_sources)
endif()

# Our profile. PROFILE_DIR is the repo root, so the corpus lives in generated/ and the player's files in game/
# (both git-ignored), as docs/PROFILE_GUIDE.md in the framework lays out.
# Our own main() (src/host/pure_main.cpp) runs ge_replay for `--replay` and the framework's main, compiled as
# portablekit_main, otherwise (task 022). The record backend (PURE_GE_BACKEND=record, which writes what ge_replay
# reads) is built on every platform, as the framework built it before task 095 moved it here, and so is the models
# backend (PURE_GE_BACKEND=models, the default since task 102: figures drawn as models, src/gpu/models_backend.cpp).
portablekit_add_game(PureNative
    PROFILE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    HOST_MAIN_NAME portablekit_main
    SOURCES src/profile/pure_profile.cpp src/profile/enhancements.cpp src/profile/test_hooks.cpp
            src/profile/aspect.cpp src/profile/shield_step.cpp src/hle/audio_capture.cpp
            src/host/pure_main.cpp src/tools/ge_replay.cpp src/tools/ge_extract.cpp src/tools/record_backend.cpp
            src/gpu/models_backend.cpp
            ${platform_sources})

# The models features (D-027): the Video tab's Crowds and people and Skies rows (D-025, D-026) and the models backend as
# the default backend. ON in a developer build, OFF in a release build (PORTABLEKIT_RELEASE), so a release runs on the
# platform's backend alone with neither row, as 0.2.0 did, until the features are measured on hardware below the
# Mac's and the rig's. The models backend is compiled either way (PURE_GE_BACKEND=models names it by hand). Only the
# two files that read the switch get its definition, so flipping it recompiles them and nothing else.
if(PORTABLEKIT_RELEASE)
    set(pure_models_default OFF)
else()
    set(pure_models_default ON)
endif()
option(PURE_MODELS "The Crowds and people and Skies rows, and the models backend as the default (D-027)" ${pure_models_default})
if(PURE_MODELS)
    set(pure_models_value 1)
else()
    set(pure_models_value 0)
endif()
set_property(SOURCE src/host/pure_main.cpp src/profile/enhancements.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    PURE_MODELS=${pure_models_value})
message(STATUS "PURE_MODELS: ${PURE_MODELS} (the crowd and sky rows and the models backend as the default, D-027)")

# Ad hoc play on a desktop build (the Network tab, the master server as the default address), as the Quest build has.
option(PURE_ADHOC "Ad hoc play through the master server on a desktop build" OFF)
if(PURE_ADHOC)
    target_compile_definitions(PureNative PRIVATE PURE_ADHOC_PLAY=1)
endif()

# Zstandard's decoder for the ZIM images of HD texture packs (D-024, task 099): the framework compiles
# host/gpu/texture_pack.cpp into PureNative itself, so linking the library here gives it zstd.h and the symbols.
include(cmake/Zstd.cmake)
target_link_libraries(PureNative PRIVATE pure_zstd_decoder)

# Wipeout Pure VR (README-QUEST.md): the Android app for Meta Quest and PICO headsets, presenting through OpenXR
# (src/vr/quest_xr.cpp and the framework's host/gpu/xr_bridge.hpp). PURE_OPENXR_DIR is the unpacked
# openxr_loader_for_android AAR from Maven Central (packaging/quest/build_quest.sh fetches it).
option(PURE_QUEST_VR "Build the Quest/PICO VR app (Android, OpenXR)" OFF)
if(PURE_QUEST_VR)
    if(NOT ANDROID OR NOT PORTABLEKIT_ANDROID_APP)
        message(FATAL_ERROR "PURE_QUEST_VR needs the Android NDK toolchain and PORTABLEKIT_ANDROID_APP=ON")
    endif()
    set(PURE_OPENXR_DIR "" CACHE PATH "The unpacked openxr_loader_for_android AAR")
    set(pure_openxr_loader "${PURE_OPENXR_DIR}/prefab/modules/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so")
    if(NOT EXISTS "${pure_openxr_loader}")
        message(FATAL_ERROR "PURE_QUEST_VR: no OpenXR loader at ${pure_openxr_loader} (set PURE_OPENXR_DIR)")
    endif()
    target_sources(PureNative PRIVATE src/vr/quest_xr.cpp)
    target_include_directories(PureNative PRIVATE "${PURE_OPENXR_DIR}/prefab/modules/headers/include")
    target_link_libraries(PureNative PRIVATE "${pure_openxr_loader}" log)
    target_compile_definitions(PureNative PRIVATE PURE_QUEST_VR=1 PORTABLEKIT_ANDROID_EXTERNAL_DATA=1)
    message(STATUS "PURE_QUEST_VR: OpenXR loader ${pure_openxr_loader}")
endif()

# The Windows release (task 061, docs/release.md "Windows"): the windows subsystem, so a player who starts the program
# from Explorer gets no console window; the entry stays the C runtime's main(). The developer build keeps the console
# subsystem, whose stdout every tool reads.
if(WIN32 AND PORTABLEKIT_RELEASE)
    set_target_properties(PureNative PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(PureNative PRIVATE /ENTRY:mainCRTStartup)
endif()

# The executable's icon and version resource (task 071), in both Windows presets: packaging/windows/pure.rc, compiled
# by llvm-rc (CMAKE_RC_COMPILER), with the project version; the icon is packaging/windows/pure.ico (make_icon.py).
if(WIN32)
    target_sources(PureNative PRIVATE packaging/windows/pure.rc)
    set_source_files_properties(packaging/windows/pure.rc PROPERTIES
        COMPILE_DEFINITIONS "PURE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR};PURE_VERSION_MINOR=${PROJECT_VERSION_MINOR};PURE_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
        OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/pure.ico")
endif()

if(APPLE)
    # The Metal GE backend (PURE_GE_BACKEND=metal, docs/metal-backend.md), the Metal shell (PURE_SHELL=metal) and Dear
    # ImGui's Metal renderer backend (1.92.9b, the vendored ImGui's release, MIT): Objective-C++ with ARC, compiled by the
    # same clang as everything else. Their shaders are MSL source compiled at run time, so no Xcode is needed. The backend
    # and the shell share one device and command queue (src/gpu/metal_device.mm, task 031).
    set(objc_sources ${metal_backend_sources} src/gpu/metal_device.mm src/host/metal_shell.mm
        third_party/imgui_metal/imgui_impl_metal.mm)
    list(TRANSFORM objc_sources PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")
    set_source_files_properties(${objc_sources}
        PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
    target_include_directories(PureNative PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui_metal")
    # The backend's shaders are src/gpu/shaders/ge.metal, a plain Metal source file (task 049), turned at build time into a
    # generated header that holds its text as a raw string (pure::metal::kShaderSource), so the app carries them and no
    # build reads a path at run time; the backend compiles them when it starts, as before.
    set(metal_shader_header "${CMAKE_CURRENT_BINARY_DIR}/generated_metal/ge_metal.hpp")
    add_custom_command(
        OUTPUT "${metal_shader_header}"
        COMMAND ${CMAKE_COMMAND} "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/ge.metal"
                "-DOUTPUT=${metal_shader_header}" -DNAME=kShaderSource
                -P "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/embed.cmake"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/ge.metal" "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/embed.cmake"
        VERBATIM
        COMMENT "Embedding the Metal shaders")
    target_sources(PureNative PRIVATE "${metal_shader_header}")
    target_include_directories(PureNative PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated_metal")
    # CoreGraphics for the Metal shell's sRGB colour space.
    target_link_libraries(PureNative PRIVATE "-framework Metal" "-framework Foundation" "-framework QuartzCore"
        "-framework CoreGraphics")
endif()
