#include "slow_motion_toggle.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Game/Input/controller_bindings.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Extension/HallOfMeat/hall_of_meat.h"
#include "Extension/HallOfMeat/hall_of_meat_skater.h"
#include "Extension/HallOfMeat/hall_of_meat_slow_motion.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/UI/Overlay/overlay.h"
#include <Windows.h>
#include <Xinput.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>

namespace dingosdk::slow_motion_toggle {
namespace {
struct State {
    std::atomic<bool> wanted{}, running{}, button{true};
    std::atomic<float> speed{0.3f};
    // Client thread only.
    hall_of_meat::SlowMotion slow_motion;
    bool pad_down{};       // d-pad down was held last tick: one press, one toggle
    bool refused_logged{}; // why it could not start is logged once a request
};
State &state() {
    static State value;
    return value;
}
}

void set_speed(float speed) noexcept {
    if (std::isfinite(speed)) state().speed.store(std::clamp(speed, 0.05f, 0.95f));
}
float speed() noexcept { return state().speed.load(); }
void set_button(bool enabled) noexcept { state().button.store(enabled); }
bool button() noexcept { return state().button.load(); }
void set_active(bool active) noexcept { state().wanted.store(active); }
bool active() noexcept { return state().running.load(); }

void on_client_tick(std::uintptr_t base) noexcept {
    auto &s = state();
    try {
        // D-pad down, on its press only, while the game is being skated.
        bool down = false;
        if (s.button.load()) {
            ControllerInput pad;
            DingoSDKOverlayReadControllerInput(&pad); // nothing while ReSkate's menu or another window has the input
            down = pad.available && (pad.buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 &&
                !multiplayer::sample_game_ui_state(base).in_menu;
        }
        if (down && !s.pad_down) {
            const bool next = !s.wanted.load();
            s.wanted.store(next);
            s.refused_logged = false;
            logging::log(logging::Level::info, logging::Channel::skater, "Slow motion {} (d-pad down).", next ? "on" : "off");
        }
        s.pad_down = down;

        // A break's slow motion (Hall of Meat) writes over ours and, when it ends, writes ours back:
        // wait for it rather than take its change for someone else's.
        if (hall_of_meat::slow_motion_running()) return;
        const bool wanted = s.wanted.load();
        if (!wanted) {
            if (!s.slow_motion.idle()) s.slow_motion.release();
            s.running.store(false);
            return;
        }
        if (!hall_of_meat::skater_available()) return; // the build's skater layout was not verified
        const bool ok = s.slow_motion.set(s.speed.load());
        if (!ok && s.running.load()) {
            // It ran and was ended: a multiplayer session started, or the speed was changed elsewhere
            // (the trainer, the console). That change wins and the toggle goes back to off.
            s.wanted.store(false);
            logging::log(logging::Level::info, logging::Channel::skater, "Slow motion ended ({}).",
                         multiplayer_session_active() ? "multiplayer session" : "game speed changed elsewhere");
        } else if (!ok && multiplayer_session_active()) {
            s.wanted.store(false);
            logging::log(logging::Level::info, logging::Channel::skater, "Slow motion is single player only.");
        } else if (!ok && !s.refused_logged) {
            // No local skater yet (loading), or the game already runs this slowly: keep asking.
            s.refused_logged = true;
            logging::log(logging::Level::debug, logging::Channel::skater, "Slow motion waiting (no skater, or the game already runs as slowly).");
        }
        s.running.store(ok);
    } catch (...) { /* A missed toggle is never worth the client tick. */ }
}

std::string status() {
    auto &s = state();
    return std::format("{} at {:.2f}x, d-pad down {}", s.running.load() ? "running" : s.wanted.load() ? "waiting" : "off",
                       s.speed.load(), s.button.load() ? "on" : "off");
}
}
