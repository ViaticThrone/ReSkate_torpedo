#pragma once
#include <cstdint>
#include <string>

// Slow Motion: d-pad down toggles the game between its own speed and slow motion (0.3x unless set
// otherwise). It runs on Hall of Meat's slow motion (Extension/HallOfMeat/hall_of_meat_slow_motion.h),
// which keeps the simulation stepping smoothly at low speed, carries the local skater's step along and
// gives everything back when it ends; so, like that one, it never runs in a multiplayer session (the
// local simulation must keep the others' pace) and a session that starts while it runs ends it.
//
// The button is read only while skating: not while the game's own menus are up (the pause menu takes
// d-pad down for itself), nor while ReSkate's menu has the input. The console's `slowmo 0|1` works anywhere.
namespace dingosdk::slow_motion_toggle {
// The speed slow motion runs at, 0.05 to 0.95 (default 0.3).
void set_speed(float speed) noexcept;
float speed() noexcept;
// Whether d-pad down toggles it (default on).
void set_button(bool enabled) noexcept;
bool button() noexcept;
// Asks for slow motion on or off; the client tick applies it.
void set_active(bool active) noexcept;
bool active() noexcept;
// Client thread, every tick: reads the button and holds or gives back the game's speed.
void on_client_tick(std::uintptr_t image_base) noexcept;
// Any thread: one line for the console.
std::string status();
}
