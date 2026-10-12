#pragma once
#include <Windows.h>
#include <array>
#include <cmath>
#include <atomic>
#include <cstddef>
#include <limits>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Where the other players are, for Torpedo Homing (torpedo_homing.h). The multiplayer session
// publishes each shown player's world position every frame (client thread); the physics step of the
// local skater reads the nearest one (physics thread). Header-only, so the multiplayer tests that
// compile the session need nothing linked for it.
namespace dingosdk::torpedo_homing {
using Vec3 = std::array<float, 3>;

class Targets {
  public:
    static constexpr std::size_t capacity = 32;
    // A player not published for this long (ms) is gone: hidden, out of range or left.
    static constexpr ULONGLONG max_age_ms = 400;

    // Client thread: the local skater's position and one other player's, as of this frame.
    void publish(std::uint64_t id, std::string_view name, const Vec3 &other, const Vec3 &local) noexcept {
        if (!id || !finite(other) || !finite(local)) return;
        const auto now = GetTickCount64();
        AcquireSRWLockExclusive(&lock_);
        local_ = local;
        local_stamp_ = now;
        Slot *slot = nullptr, *oldest = &slots_[0];
        for (auto &s : slots_) {
            if (s.id == id) { slot = &s; break; }
            if (s.stamp < oldest->stamp) oldest = &s;
        }
        if (!slot) slot = oldest;
        slot->id = id;
        slot->name = {};
        for (std::size_t i = 0; i < name.size() && i + 1 < slot->name.size(); ++i)
            slot->name[i] = lower(name[i]);
        slot->position = other;
        slot->stamp = now;
        ReleaseSRWLockExclusive(&lock_);
    }
    void forget(std::uint64_t id) noexcept {
        AcquireSRWLockExclusive(&lock_);
        for (auto &s : slots_)
            if (s.id == id) s = {};
        ReleaseSRWLockExclusive(&lock_);
    }
    // Client thread: the player whose name is `query` (case-insensitive; else the one name that
    // contains it). 0 when none or more than one fits.
    std::uint64_t find(std::string_view query) noexcept {
        std::string q;
        for (const char c : query) q += lower(c);
        const auto now = GetTickCount64();
        std::uint64_t exact = 0, partial = 0;
        int partials = 0;
        AcquireSRWLockShared(&lock_);
        for (const auto &s : slots_) {
            if (!s.id || now - s.stamp > max_age_ms) continue;
            const std::string_view n(s.name.data());
            if (n == q) exact = s.id;
            else if (!q.empty() && n.find(q) != std::string_view::npos) { partial = s.id; ++partials; }
        }
        ReleaseSRWLockShared(&lock_);
        return exact ? exact : partials == 1 ? partial : 0;
    }
    // Client thread: the names of the players shown now (lower case).
    std::vector<std::string> names() {
        std::vector<std::string> result;
        const auto now = GetTickCount64();
        AcquireSRWLockShared(&lock_);
        for (const auto &s : slots_)
            if (s.id && now - s.stamp <= max_age_ms) result.emplace_back(s.name.data());
        ReleaseSRWLockShared(&lock_);
        return result;
    }
    // The player to home on: 0 for whoever is nearest. A chosen player is followed at any distance.
    void choose(std::uint64_t id) noexcept { wanted_.store(id, std::memory_order_release); }
    std::uint64_t chosen() const noexcept { return wanted_.load(std::memory_order_acquire); }
    std::string chosen_name() {
        const auto id = chosen();
        std::string result;
        AcquireSRWLockShared(&lock_);
        for (const auto &s : slots_)
            if (id && s.id == id) result = s.name.data();
        ReleaseSRWLockShared(&lock_);
        return result;
    }

    // Any thread: the local skater's last published position and the chosen player, or else the
    // nearest fresh one within `range` metres of it. False when either is missing or stale.
    bool nearest(float range, Vec3 &local, Vec3 &target, float *distance = nullptr) noexcept {
        const auto now = GetTickCount64();
        bool found = false;
        float best = range * range;
        const auto wanted = wanted_.load(std::memory_order_acquire);
        if (wanted) best = std::numeric_limits<float>::max();
        AcquireSRWLockShared(&lock_);
        if (now - local_stamp_ <= max_age_ms) {
            local = local_;
            for (const auto &s : slots_) {
                if (!s.id || now - s.stamp > max_age_ms || (wanted && s.id != wanted)) continue;
                const float dx = s.position[0] - local_[0], dy = s.position[1] - local_[1], dz = s.position[2] - local_[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < best) {
                    best = d2;
                    target = s.position;
                    found = true;
                }
            }
        }
        ReleaseSRWLockShared(&lock_);
        if (found && distance) *distance = std::sqrt(best);
        return found;
    }

  private:
    struct Slot {
        std::uint64_t id{};
        std::array<char, 32> name{}; // lower case, ends with 0
        Vec3 position{};
        ULONGLONG stamp{};
    };
    static bool finite(const Vec3 &v) noexcept {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::abs(v[0]) < 1e6f &&
               std::abs(v[1]) < 1e6f && std::abs(v[2]) < 1e6f;
    }
    static char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
    SRWLOCK lock_ = SRWLOCK_INIT;
    std::atomic<std::uint64_t> wanted_{};
    std::array<Slot, capacity> slots_{};
    Vec3 local_{};
    ULONGLONG local_stamp_{};
};
inline Targets &targets() noexcept {
    static Targets value;
    return value;
}
} // namespace dingosdk::torpedo_homing
