#include "torpedo_homing.h"
#include "torpedo_homing_targets.h"
#include "no_bail.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/addresses.h"
#include "Engine/Game/Build/20260929/no_bail.h"
#include "Engine/Game/Build/20260929/offboard_flight.h"
#include "Engine/Game/Build/20260929/skater_state.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>

namespace dingosdk::torpedo_homing {
namespace {
namespace ofb = addr::offboard_flight;
namespace state = addr::skater_state;

constexpr float pi = 3.14159265358979f;
constexpr float aim_height = 1.0f;        // aim at the chest, not the feet
constexpr float arrive_distance = 1.5f;   // close enough: stop steering and let the hit happen
constexpr float cone = 100.0f * pi / 180; // nothing steers towards a player further behind than this
// The ragdoll substate the torpedo trick runs in, as `torpedohoming learn 1` logged it: the third
// ragdoll state (substate 12), in the air. After the landing or a hit the skater moves to substate 10
// (and 11), the bail itself, which is never steered.
constexpr int torpedo_substate = 12;

struct Config {
    std::atomic<bool> on{}, learn{};
    std::atomic<float> turn{120.0f}, range{60.0f}, min_speed{4.0f};
};
Config &config() {
    static Config value;
    return value;
}
std::atomic<bool> &ready() {
    static std::atomic<bool> value{};
    return value;
}

// What the physics thread saw, for the client tick to log and the console to show.
struct Seen {
    std::uint32_t physics_state{};
    int substate{-1}; // 0..2 the flight states, 10..12 the ragdoll ones, -1 none known
    bool ragdoll{}, in_the_air{}, wipeout{};
    float speed{};
    bool operator==(const Seen &o) const {
        return physics_state == o.physics_state && substate == o.substate && ragdoll == o.ragdoll &&
               in_the_air == o.in_the_air && wipeout == o.wipeout;
    }
};
struct Live {
    SRWLOCK lock = SRWLOCK_INIT;
    Seen seen;
    std::uint64_t seq{}, logged{};
};
Live &live() {
    static Live value;
    return value;
}
std::atomic<std::uint64_t> &steered_steps() {
    static std::atomic<std::uint64_t> value{};
    return value;
}
std::atomic<float> &last_distance() {
    static std::atomic<float> value{-1.0f};
    return value;
}

using Vec3 = std::array<float, 3>;
float dot(const Vec3 &a, const Vec3 &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
bool finite(const Vec3 &v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::abs(v[0]) < 1000 &&
           std::abs(v[1]) < 1000 && std::abs(v[2]) < 1000;
}
// Rodrigues: `v` turned by `angle` about the unit `axis`.
Vec3 rotate(const Vec3 &v, const Vec3 &axis, float angle) {
    const float c = std::cos(angle), s = std::sin(angle), d = dot(axis, v) * (1 - c);
    const Vec3 x = cross(axis, v);
    return {v[0] * c + x[0] * s + axis[0] * d, v[1] * c + x[1] * s + axis[1] * d, v[2] * c + x[2] * s + axis[2] * d};
}

// A write the game may refuse (an unmapped or read-only page): false instead of a crash.
bool guarded_copy(std::uintptr_t address, const void *source, std::size_t size) noexcept {
    __try {
        std::memcpy(reinterpret_cast<void *>(address), source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The skeleton's physics bodies 1 to 23, as the SDK's boosts write them (client_noclip.cpp).
constexpr std::size_t first_body = 1, last_body = 23;
using Bodies = std::array<std::uintptr_t, last_body + 1>;
bool rig_bodies(const NoBailSkater &skater, Bodies &bodies) noexcept {
    std::uintptr_t physics{}, type{}, list{};
    std::uint32_t count{};
    if (!memory::peek(skater.rig + state::rig_physics_offset, physics) || !memory::peek(physics, type) ||
        type != skater.base + state::rig_physics_vtable ||
        !memory::peek(physics + state::physics_bodies_offset, list) || !memory::peek(list, count) ||
        count != state::rig_body_count)
        return false;
    for (std::size_t i = first_body; i <= last_body; ++i) {
        const auto body = list + i * state::physics_body_size;
        std::uintptr_t owner{};
        if (!memory::peek(body + state::body_owner_offset, owner) || owner != physics) return false;
        bodies[i] = body;
    }
    return true;
}
bool read_velocity(std::uintptr_t body, Vec3 &velocity) noexcept {
    return memory::peek(body + state::body_velocity_offset, velocity) && finite(velocity);
}
bool write_velocity(std::uintptr_t body, const Vec3 &velocity) noexcept {
    std::uint32_t flags{};
    if (!memory::peek(body + 0x60, flags)) return false;
    flags |= 8u; // the dirty bit: the physics world takes the velocity at +0x70
    return guarded_copy(body + state::body_velocity_offset, &velocity, sizeof(velocity)) &&
           guarded_copy(body + 0x60, &flags, sizeof(flags));
}

// Which off-board substate is active: 0..2 FollowGround, FollowGroundSlide, Falling; 10..12 the three
// ragdoll ones (dives and glides move through those: offboard_flight.h); -1 when none is known.
int active_substate(const NoBailSkater &skater) noexcept {
    std::uintptr_t parent{}, type{}, active{};
    if (!memory::peek(skater.core + 0x3b0, parent) || !memory::peek(parent, type) ||
        type != skater.base + ofb::offboard_flight_vtable || !memory::peek(parent + 0x48, active))
        return -1;
    for (std::size_t i = 0; i < ofb::offboard_flight_states.size(); ++i)
        if (active == parent + ofb::offboard_flight_states[i].offset) return static_cast<int>(i);
    for (std::size_t i = 0; i < ofb::offboard_ragdoll_states.size(); ++i)
        if (active == parent + ofb::offboard_ragdoll_states[i][0]) return 10 + static_cast<int>(i);
    return -1;
}
struct Flags {
    bool ragdoll{}, in_the_air{};
};
bool read_flags(const NoBailSkater &skater, Flags &flags) noexcept {
    std::uintptr_t trick_state{}, offboard{};
    std::uint8_t ragdoll{}, in_the_air{};
    if (!memory::peek(skater.core + state::trick_state_offset, trick_state) ||
        !memory::peek(trick_state + state::offboard_state_offset, offboard) ||
        !memory::peek(offboard + state::ragdoll_offset, ragdoll) ||
        !memory::peek(offboard + state::in_the_air_offset, in_the_air))
        return false;
    flags = {ragdoll != 0, in_the_air != 0};
    return true;
}

// Physics thread, every step of the local skater, after No Bail's own filtering.
void on_step(const NoBailSkater &skater, float seconds, bool wipeout) noexcept {
    auto &cfg = config();
    const bool on = cfg.on.load(std::memory_order_relaxed), learn = cfg.learn.load(std::memory_order_relaxed);
    if (!on && !learn) return;
    std::uint32_t physics{};
    if (!memory::peek(skater.context + state::physics_state_offset, physics)) return;
    const bool offboard = physics == addr::no_bail::offboard_physics_state;
    const int substate = offboard ? active_substate(skater) : -1;
    Flags flags;
    if (offboard) (void)read_flags(skater, flags);

    Bodies bodies{};
    const bool have_bodies = (offboard && substate >= 10) && rig_bodies(skater, bodies);
    Vec3 mean{};
    if (have_bodies) {
        for (std::size_t i = first_body; i <= last_body; ++i) {
            Vec3 v{};
            if (!read_velocity(bodies[i], v)) return;
            for (std::size_t a = 0; a < 3; ++a) mean[a] += v[a];
        }
        for (auto &a : mean) a /= static_cast<float>(last_body - first_body + 1);
    }
    const float speed = length(mean);

    if (learn && offboard) {
        Seen now{physics, substate, flags.ragdoll, flags.in_the_air, wipeout, speed};
        auto &l = live();
        AcquireSRWLockExclusive(&l.lock);
        if (!(now == l.seen)) {
            l.seen = now;
            ++l.seq;
        } else {
            l.seen.speed = speed;
        }
        ReleaseSRWLockExclusive(&l.lock);
    }
    if (!on || !have_bodies || wipeout || substate != torpedo_substate || !flags.in_the_air ||
        !std::isfinite(seconds) || seconds <= 0 || seconds > 0.1f)
        return;
    if (speed < cfg.min_speed.load(std::memory_order_relaxed)) return;

    Vec3 me{}, target{};
    float distance{};
    if (!targets().nearest(cfg.range.load(std::memory_order_relaxed), me, target, &distance)) {
        last_distance().store(-1.0f, std::memory_order_relaxed);
        return;
    }
    last_distance().store(distance, std::memory_order_relaxed);
    target[1] += aim_height;
    Vec3 to{target[0] - me[0], target[1] - me[1], target[2] - me[2]};
    const float gap = length(to); // not `far`: Windows.h defines that
    if (!std::isfinite(gap) || gap < arrive_distance) return;
    for (auto &a : to) a /= gap;
    Vec3 heading{mean[0] / speed, mean[1] / speed, mean[2] / speed};
    const float cosine = std::clamp(dot(heading, to), -1.0f, 1.0f);
    const float angle = std::acos(cosine);
    if (angle < 0.002f || angle > cone) return;
    Vec3 axis = cross(heading, to);
    const float axis_length = length(axis);
    if (axis_length < 1e-5f) return;
    for (auto &a : axis) a /= axis_length;
    const float turn = std::min(angle, cfg.turn.load(std::memory_order_relaxed) * pi / 180.0f * seconds);

    // Every body turns by the same rotation, so the body keeps its shape and its speed.
    std::array<Vec3, last_body + 1> turned{};
    for (std::size_t i = first_body; i <= last_body; ++i) {
        Vec3 v{};
        if (!read_velocity(bodies[i], v)) return;
        turned[i] = rotate(v, axis, turn);
        if (!finite(turned[i])) return;
    }
    bool wrote = false;
    for (std::size_t i = first_body; i <= last_body; ++i)
        if (write_velocity(bodies[i], turned[i])) wrote = true;
    if (wrote) steered_steps().fetch_add(1, std::memory_order_relaxed);
}
}

bool start(std::uintptr_t) noexcept {
    if (ready().load(std::memory_order_acquire)) return true;
    if (!no_bail_available()) {
        logging::write(logging::Level::warning, logging::Channel::skater,
                       "Torpedo Homing cannot follow the skater: its physics steps come through No Bail's hooks, which did not start.");
        return false;
    }
    set_no_bail_motion_observer(&on_step);
    ready().store(true, std::memory_order_release);
    logging::log(logging::Level::info, logging::Channel::skater, "Torpedo Homing ready (off until `torpedohoming 1`).");
    return true;
}
bool available() noexcept { return ready().load(std::memory_order_acquire); }
bool enabled() noexcept { return available() && config().on.load(std::memory_order_relaxed); }
void set_enabled(bool enabled) noexcept { config().on.store(enabled, std::memory_order_relaxed); }
void set_turn_rate(float v) noexcept {
    if (std::isfinite(v)) config().turn.store(std::clamp(v, 10.0f, 720.0f), std::memory_order_relaxed);
}
void set_range(float v) noexcept {
    if (std::isfinite(v)) config().range.store(std::clamp(v, 5.0f, 500.0f), std::memory_order_relaxed);
}
void set_min_speed(float v) noexcept {
    if (std::isfinite(v)) config().min_speed.store(std::clamp(v, 0.0f, 100.0f), std::memory_order_relaxed);
}
float turn_rate() noexcept { return config().turn.load(std::memory_order_relaxed); }
float range() noexcept { return config().range.load(std::memory_order_relaxed); }
float min_speed() noexcept { return config().min_speed.load(std::memory_order_relaxed); }
std::string set_target(std::string_view name) {
    std::string text(name);
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (text.empty() || text == "nearest" || text == "off") {
        targets().choose(0);
        return "Homing on whoever is nearest.";
    }
    const auto id = targets().find(text);
    if (!id) return std::format("No single player shown matches \"{}\". Shown: {}", text, players());
    targets().choose(id);
    return std::format("Homing on {}.", targets().chosen_name());
}
std::string target_name() {
    auto name = targets().chosen_name();
    return targets().chosen() ? (name.empty() ? std::string("a player not shown now") : name) : std::string("nearest");
}
std::string players() {
    std::string result;
    for (const auto &n : targets().names()) result += (result.empty() ? "" : ", ") + n;
    return result.empty() ? std::string("none") : result;
}
void set_learning(bool enabled) noexcept { config().learn.store(enabled, std::memory_order_relaxed); }
bool learning() noexcept { return config().learn.load(std::memory_order_relaxed); }

void on_client_tick() noexcept {
    if (!available() || !learning()) return;
    try {
        auto &l = live();
        Seen seen;
        std::uint64_t seq{};
        AcquireSRWLockExclusive(&l.lock);
        seq = l.seq;
        seen = l.seen;
        const bool fresh = seq != l.logged;
        l.logged = seq;
        ReleaseSRWLockExclusive(&l.lock);
        if (fresh)
            logging::log(logging::Level::info, logging::Channel::skater,
                         "Torpedo Homing learn: physics state {}, substate {} ({}), ragdoll {}, in the air {}, bail {}, speed {:.1f} m/s",
                         seen.physics_state, seen.substate,
                         seen.substate >= 10 ? "ragdoll motion" : seen.substate >= 0 ? "flight/ground" : "none",
                         seen.ragdoll, seen.in_the_air, seen.wipeout, static_cast<double>(seen.speed));
    } catch (...) { /* A lost log line is never worth the client tick. */ }
}

std::string status() {
    const float d = last_distance().load(std::memory_order_relaxed);
    return std::format("{}; target {}; turn {:.0f} deg/s, range {:.0f} m, min speed {:.0f} m/s; steered {} steps; distance {}",
                       enabled() ? "on" : "off", target_name(), static_cast<double>(turn_rate()), static_cast<double>(range()),
                       static_cast<double>(min_speed()), steered_steps().load(std::memory_order_relaxed),
                       d >= 0 ? std::format("{:.1f} m", static_cast<double>(d)) : std::string("none in range"));
}
}
