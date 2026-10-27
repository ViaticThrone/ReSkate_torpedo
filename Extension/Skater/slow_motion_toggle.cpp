#include "slow_motion_toggle.h"
#include "no_bail.h"
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
// The skater's physics states 200-299 are the pop and the flight on the board (Extension/Trainer/trainer.cpp):
// 300 is a wipeout and the 500s are off the board, either of which ends a trick's slow motion.
constexpr std::uint32_t air_states_first = 200, air_states_end = 300;
constexpr std::uint32_t both_sticks = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;

struct State {
    std::atomic<bool> wanted{}, running{}, button{true}, trick_button{true}, trick{};
    std::atomic<float> speed{0.3f}, trick_speed{0.2f};
    // Client thread only.
    hall_of_meat::SlowMotion slow_motion;
    bool pad_down{};       // d-pad down was held last tick: one press, one toggle
    bool sticks_down{};    // both sticks were clicked in last tick
    bool refused_logged{}; // why it could not start is logged once a request
};
State &state() {
    static State value;
    return value;
}
float clamp_speed(float speed) noexcept { return std::clamp(speed, 0.05f, 0.95f); }
bool airborne() noexcept {
    const auto watch = watched_physics_state(); // the local skater's, as the trainer publishes it each tick
    return watch.valid && watch.state >= air_states_first && watch.state < air_states_end;
}
}

void set_speed(float speed) noexcept {
    if (std::isfinite(speed)) state().speed.store(clamp_speed(speed));
}
float speed() noexcept { return state().speed.load(); }
void set_trick_speed(float speed) noexcept {
    if (std::isfinite(speed)) state().trick_speed.store(clamp_speed(speed));
}
float trick_speed() noexcept { return state().trick_speed.load(); }
void set_button(bool enabled) noexcept { state().button.store(enabled); }
bool button() noexcept { return state().button.load(); }
void set_trick_button(bool enabled) noexcept {
    state().trick_button.store(enabled);
    if (!enabled) state().trick.store(false);
}
bool trick_button() noexcept { return state().trick_button.load(); }
void set_active(bool active) noexcept { state().wanted.store(active); }
bool active() noexcept { return state().running.load(); }
bool trick_active() noexcept { return state().trick.load() && state().running.load(); }

void on_client_tick(std::uintptr_t base) noexcept {
    auto &s = state();
    try {
        // The buttons, on their press only, while the game is being skated.
        bool down = false, sticks = false;
        if (s.button.load() || s.trick_button.load()) {
            ControllerInput pad;
            DingoSDKOverlayReadControllerInput(&pad); // nothing while ReSkate's menu or another window has the input
            if (pad.available && (pad.buttons & (XINPUT_GAMEPAD_DPAD_DOWN | both_sticks)) &&
                !multiplayer::sample_game_ui_state(base).in_menu) {
                down = s.button.load() && (pad.buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
                sticks = s.trick_button.load() && (pad.buttons & both_sticks) == both_sticks;
            }
        }
        if (down && !s.pad_down) {
            const bool next = !s.wanted.load();
            s.wanted.store(next);
            s.refused_logged = false;
            logging::log(logging::Level::info, logging::Channel::skater, "Slow motion {} (d-pad down).", next ? "on" : "off");
        }
        s.pad_down = down;

        // Nail the Trick: both sticks clicked in the air slow the game down until the skater lands,
        // bails or leaves the board, or both sticks are clicked again.
        const bool air = airborne();
        if (sticks && !s.sticks_down) {
            if (s.trick.load()) {
                s.trick.store(false);
                logging::log(logging::Level::info, logging::Channel::skater, "Trick slow motion off (both sticks).");
            } else if (air) {
                s.trick.store(true);
                s.refused_logged = false;
                logging::log(logging::Level::info, logging::Channel::skater, "Trick slow motion on (both sticks in the air).");
            }
        }
        s.sticks_down = sticks;
        if (s.trick.load() && !air) {
            s.trick.store(false);
            logging::log(logging::Level::info, logging::Channel::skater, "Trick slow motion off (landed or off the board).");
        }

        // A break's slow motion (Hall of Meat) writes over ours and, when it ends, writes ours back:
        // wait for it rather than take its change for someone else's.
        if (hall_of_meat::slow_motion_running()) return;
        const bool trick = s.trick.load();
        if (!s.wanted.load() && !trick) {
            if (!s.slow_motion.idle()) s.slow_motion.release();
            s.running.store(false);
            return;
        }
        if (!hall_of_meat::skater_available()) return; // the build's skater layout was not verified
        // During a trick its own speed, or the toggle's if that is slower still.
        const float speed = trick ? std::min(s.trick_speed.load(), s.wanted.load() ? s.speed.load() : 1.0f) : s.speed.load();
        const bool ok = s.slow_motion.set(speed);
        if (!ok && s.running.load()) {
            // It ran and was ended: a multiplayer session started, or the speed was changed elsewhere
            // (the trainer, the console). That change wins and both go back to off.
            s.wanted.store(false);
            s.trick.store(false);
            logging::log(logging::Level::info, logging::Channel::skater, "Slow motion ended ({}).",
                         multiplayer_session_active() ? "multiplayer session" : "game speed changed elsewhere");
        } else if (!ok && multiplayer_session_active()) {
            s.wanted.store(false);
            s.trick.store(false);
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
    const auto mode = s.running.load() ? (s.trick.load() ? "trick" : "running") : s.wanted.load() ? "waiting" : "off";
    return std::format("{}; toggle {:.2f}x (d-pad down {}), trick {:.2f}x (both sticks in the air {})", mode, s.speed.load(),
                       s.button.load() ? "on" : "off", s.trick_speed.load(), s.trick_button.load() ? "on" : "off");
}
}
