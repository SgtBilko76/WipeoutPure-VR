// E2, the 3D view at the window's shape (aspect.hpp says what is here; D-020 holds the audit).
//
// Wipeout Pure builds its projection in FUN_08a31148(fovy, aspect, near, far, y scale, frame): m00 = cot(fovy/2) /
// aspect, m11 = cot(fovy/2). Its callers pass the aspect in f13 as a constant compiled into their code: the race
// camera's apply FUN_0883ff34 (the call at 0x088402b8) 1.7647059, the frontend's three 3D views 0x3fe1e1e2 (both
// 480/272 to 1e-6), FUN_08869290 1.0 (a square view, left alone). So the hook is a function override: registered at
// the function's entry, it replaces an aspect of 480/272 by the picture's and runs the generated body. A registration
// whose name is not a generated unit's takes the function's 16 KiB unit out of cross-unit chaining, so every caller
// (all six lie in other units) reaches it through the runtime's function table.
//
// The camera also keeps the tangent of half its horizontal angle at 0x08b1fb14, which every writer sets as the
// vertical one (0x08b1fb18) times 480/272; at each flip under another shape it is set to the vertical one times that
// shape, so what reads it follows. At the PSP's shape nothing is registered and nothing in guest memory is written
// until another shape has been seen (PURE_TRACE_ASPECT puts the override in from the first flip).
#include "aspect.hpp"

#include "../helpers.hpp"
#include "psprecomp/corpus_abi.hpp"
#include "psprecomp/runtime.hpp"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

#if defined(PURE_QUEST_VR)
#include "gpu/xr_bridge.hpp"
#endif

namespace portablekit::aspect {
namespace {

constexpr std::uint32_t kPerspective = 0x08A31148u;      // FUN_08a31148, the projection builder
constexpr std::uint32_t kRaceCameraReturn = 0x088402C0u;  // ra of FUN_0883ff34's call at 0x088402b8 (the race camera)
constexpr std::uint32_t kTanHalfX = 0x08B1FB14u;         // the camera's horizontal half-angle tangent (float)
constexpr std::uint32_t kTanHalfY = 0x08B1FB18u;         // the vertical one (float)
constexpr float kPspAspect = 480.0f / 272.0f;
constexpr float kSame = 1e-4f;

[[nodiscard]] bool is_psp(float aspect) { return std::fabs(aspect - kPspAspect) < kSame; }

// The generated FUN_08a31148, the override's tail.
psprecomp::CorpusFunction original = nullptr;
// The last view aspect the flip hook received; 480/272 leaves the argument alone.
float view_aspect = kPspAspect;
// 0x08b1fb14 holds a value of the flip hook's, not the game's.
bool tangent_changed = false;

// The override's calls over the run: [0] from the race camera, [1] from the others (the frontend's views and the
// square one); each the calls and those whose aspect was replaced.
struct Count {
    std::uint64_t calls = 0u;
    std::uint64_t widened = 0u;
};
Count counts[2];

#if defined(PURE_QUEST_VR)
// The headset: the eyes see far more than the game's 65 degrees (and turn with the head), so the race camera gets a
// wide field of view and the game draws what lies around it, not only ahead. The eyes take their own field of view
// from the headset and only the projection's depth from the game, so the picture itself does not widen; the game's
// own frame (not shown in a race) and its screen-space effects stay consistent with it. PURE_VR_FOV names the
// vertical angle in degrees (default 110; 0 or the game's own leaves it alone).
float vr_fov_radians() {
    static const float radians = [] {
        const char *text = std::getenv("PURE_VR_FOV");
        const float degrees = text != nullptr ? std::strtof(text, nullptr) : 110.0f;
        return degrees >= 30.0f && degrees <= 160.0f ? degrees * 3.14159265f / 180.0f : 0.0f;
    }();
    return radians;
}
#endif

void perspective(psprecomp::CorpusRuntime &rt, psprecomp::AllegrexContext &ctx) {
    Count &count = counts[ctx.gpr[31] == kRaceCameraReturn ? 0 : 1];
    ++count.calls;
#if defined(PURE_QUEST_VR)
    if (ctx.gpr[31] == kRaceCameraReturn && vr_fov_radians() > ctx.fpr[12]) ctx.fpr[12] = vr_fov_radians();
#endif
    if (is_psp(ctx.fpr[13]) && !is_psp(view_aspect)) {
        ctx.fpr[13] = view_aspect;
        ++count.widened;
    }
    original(rt, ctx);
}

// The function the corpus registers at an address. Runtime keeps its lookup private, so the corpus's registration
// (register_generated_functions, the one this executable links: the game has a single edition and no overlays) is
// replayed into a runtime of its own that only records it. CorpusRuntime is the corpus ABI's base, built by a derived
// runtime with a table of host functions; registration calls no other entry of the table.
class Registrations final : public psprecomp::CorpusRuntime {
public:
    explicit Registrations(std::uint32_t address) : CorpusRuntime(&kApi, &kProcess), wanted_(address) {
        psprecomp::register_generated_functions(*this);
    }
    [[nodiscard]] psprecomp::CorpusFunction found() const { return found_; }

private:
    static void on_function(psprecomp::CorpusRuntime &runtime, std::uint32_t address,
                            psprecomp::CorpusFunction function, const char *) {
        auto &self = static_cast<Registrations &>(runtime);
        if (address == self.wanted_ && self.found_ == nullptr) self.found_ = function;
    }
    static void on_unit(psprecomp::CorpusRuntime &, std::uint32_t, std::uint32_t, std::uint32_t,
                        psprecomp::CorpusFunction, psprecomp::CorpusEntryFunction) {}
    static constexpr psprecomp::CorpusHostApi kApi{.abi_version = psprecomp::kCorpusAbiVersion,
                                                   .register_function = &on_function,
                                                   .register_unit = &on_unit};
    static constexpr psprecomp::CorpusProcessState kProcess{};

    std::uint32_t wanted_;
    psprecomp::CorpusFunction found_ = nullptr;
};

// PURE_TRACE_ASPECT=1 (test-only): the override's counts once a second, and the override in from the first flip.
bool trace_on() {
    static const bool on = pure::env_on("TRACE_ASPECT");
    return on;
}

// The override in, once, at the first flip under a shape other than the PSP's (the corpus is registered by then), or
// at the first flip under PURE_TRACE_ASPECT, so that a check's lanes at both shapes run the same dispatches. Not
// before: a registration sends every call of the function through the runtime's function table instead of chaining
// it, which moves where the scheduler's dispatch counts fall and so the run's timing (the headless suite's tt_hud,
// fp_hud and zone_change scenes moved with it registered from the first flip, task 084), so the PSP's shape keeps the
// run as it was. False when the generated function is not there: an executable this build does not know runs under
// the interpreter.
bool install(psprecomp::Runtime &rt) {
    if (rt.has_function(kPerspective)) original = Registrations(kPerspective).found();
    if (original == nullptr) {
        std::cout << "[aspect] FUN_08a31148 not found among the recompiled functions: the 3D view keeps the PSP's "
                     "shape\n";
        return false;
    }
    rt.register_function(kPerspective, &perspective, "pure::perspective");
    std::cout << "[aspect] FUN_08a31148 overridden: the 3D view takes the picture's shape\n";
    return true;
}

// PURE_TRACE_ASPECT=1: the counts over the run and the view aspect, once a second at a flip.
void trace() {
    if (!trace_on()) return;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next = Clock::now();
    const Clock::time_point now = Clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    std::cout << "[aspect] calls: race camera " << counts[0].calls << " (" << counts[0].widened << " widened), other "
              << counts[1].calls << " (" << counts[1].widened << " widened); view aspect " << view_aspect << "\n";
}

} // namespace

void view_aspect_frame(psprecomp::Runtime &rt, float aspect) {
    enum class State { Waiting, In, Off };
    static State state = State::Waiting;
#if defined(PURE_QUEST_VR)
    const bool vr = vr_fov_radians() > 0.0f;
#else
    const bool vr = false;
#endif
    if (state == State::Waiting) {
        if (is_psp(aspect) && !trace_on() && !vr) return;
        state = install(rt) ? State::In : State::Off;
    }
    if (state != State::In) return;
    view_aspect = is_psp(aspect) ? kPspAspect : aspect;
    if (!is_psp(aspect)) {
        const float tan_y = pure::load_float(rt.memory(), kTanHalfY);
        if (std::isfinite(tan_y) && tan_y > 0.0f) {
            rt.memory().store32(kTanHalfX, std::bit_cast<std::uint32_t>(tan_y * aspect));
            tangent_changed = true;
        }
    } else if (tangent_changed) {
        // Back at the PSP's shape: the PSP's value, once.
        rt.memory().store32(kTanHalfX, std::bit_cast<std::uint32_t>(pure::load_float(rt.memory(), kTanHalfY) * kPspAspect));
        tangent_changed = false;
    }
#if defined(PURE_QUEST_VR)
    // In a race the camera's tangents follow the wide angle, so the game's culling keeps what the eyes can see.
    if (vr && portablekit::gpu::xr::scene_3d()) {
        const float wide_y = std::tan(0.5f * vr_fov_radians());
        const float tan_y = pure::load_float(rt.memory(), kTanHalfY);
        if (std::isfinite(tan_y) && tan_y > 0.0f && tan_y < wide_y) {
            rt.memory().store32(kTanHalfY, std::bit_cast<std::uint32_t>(wide_y));
            rt.memory().store32(kTanHalfX, std::bit_cast<std::uint32_t>(wide_y * view_aspect));
            static bool announced = false;
            if (!announced) {
                announced = true;
                std::cout << "[aspect] VR: the race camera at " << vr_fov_radians() * 180.0f / 3.14159265f
                          << " degrees (tangents " << tan_y << " -> " << wide_y << ")\n";
            }
        }
    }
#endif
    trace();
}

} // namespace portablekit::aspect
