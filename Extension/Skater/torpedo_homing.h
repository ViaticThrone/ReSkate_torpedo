#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Torpedo Homing: while the local skater dives, torpedoes or glides (the air states that go through
// the game's ragdoll motion), the skater's velocity is turned towards the nearest other player,
// a little each physics step, so a torpedo guides itself into them. The speed is kept; only the
// direction turns, at most `turn` degrees a second, so it is a guide and not a snap.
//
// Where it reads and writes:
//  - the nearest player: the multiplayer session publishes every shown player's position each
//    frame (torpedo_homing_targets.h);
//  - the skater: No Bail's physics-step hook hands the step on (no_bail.h), where the velocity of
//    the skeleton's bodies is rotated and written back as the SDK's flight and boosts write it
//    (client_noclip.cpp: XYZ at +0x70 and dirty bit 8 at +0x60).
//
// Which air state is "the torpedo" is not named anywhere in the game data this SDK reads, so
// `torpedohoming learn 1` logs the state each change lands in; the gate (a ragdoll-motion
// off-board state, not a bail, fast enough) is the first guess.
namespace dingosdk::torpedo_homing {
// Requires the validated build and No Bail started.
bool start(std::uintptr_t image_base) noexcept;
bool available() noexcept;
bool enabled() noexcept;
void set_enabled(bool enabled) noexcept;
// Degrees a second the velocity may turn (clamped 10 to 720), metres the nearest player may be
// (5 to 500), and the speed (m/s) under which nothing steers (0 to 100).
void set_turn_rate(float degrees_per_second) noexcept;
void set_range(float metres) noexcept;
void set_min_speed(float metres_per_second) noexcept;
float turn_rate() noexcept;
float range() noexcept;
float min_speed() noexcept;
// Home on this player (by name, case-insensitive; part of a name is enough when only one player
// fits), at any distance; "" or "nearest" goes back to whoever is nearest. Returns the line to show.
// Client thread. A chosen player who is not shown (out of reach, left) is simply not steered at.
std::string set_target(std::string_view name);
std::string target_name();
// The names of the players that can be chosen now.
std::string players();
// Logs each off-board air state the skater changes into (state, substate, flags, speed).
void set_learning(bool enabled) noexcept;
bool learning() noexcept;
// Client thread, every tick: writes what the physics thread saw to the log.
void on_client_tick() noexcept;
// Any thread: one line for the console.
std::string status();
}
