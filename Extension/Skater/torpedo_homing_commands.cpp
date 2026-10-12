#include "torpedo_homing.h"
#include "torpedo_homing_targets.h"
#include "Extension/Console/commands.h"

// Torpedo Homing's console commands (see torpedo_homing.h).
namespace dingosdk::console {
void register_torpedo_homing_commands(Commands &registry) {
    const auto unavailable = "Torpedo Homing is unavailable for this game build.";

    auto homing = variable("torpedohoming", "While you dive, torpedo or glide, your velocity turns towards the nearest other player",
                           Group::gameplay, argument("0|1", Type::boolean));
    homing.inspect = [unavailable](const Model &) {
        return boolean_state(torpedo_homing::available(), torpedo_homing::enabled(), unavailable, torpedo_homing::status());
    };
    homing.run = [](const Model &, const Values &args, const Output &) { torpedo_homing::set_enabled(std::get<bool>(args[0])); };
    registry.add(std::move(homing));

    auto who = argument("player", Type::text, true);
    who.rest = true;
    who.complete = [](const Model &, auto) {
        auto names = torpedo_homing::targets().names();
        names.push_back("nearest");
        return names;
    };
    auto target = action("torpedohoming target", "Home on one player by name (part of it is enough); no name or `nearest` goes back to whoever is nearest",
                         Group::gameplay, {who});
    target.run = [](const Model &, const Values &args, const Output &out) {
        out(torpedo_homing::set_target(args.empty() ? std::string{} : std::get<std::string>(args[0])));
    };
    registry.add(std::move(target));

    auto players = action("torpedohoming players", "List the players that can be homed on now", Group::gameplay);
    players.run = [](const Model &, const Values &, const Output &out) { out(torpedo_homing::players()); };
    registry.add(std::move(players));

    auto turn = variable("torpedohoming turn", "Degrees a second the torpedo may turn towards the player (10 to 720)",
                         Group::gameplay, argument("degrees", Type::number));
    turn.inspect = [unavailable](const Model &) {
        return State{torpedo_homing::available(), std::to_string(static_cast<int>(torpedo_homing::turn_rate())), unavailable, {}, false};
    };
    turn.run = [](const Model &, const Values &args, const Output &) {
        torpedo_homing::set_turn_rate(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(turn));

    auto range = variable("torpedohoming range", "Metres: only a player this near is homed on (5 to 500)",
                          Group::gameplay, argument("metres", Type::number));
    range.inspect = [unavailable](const Model &) {
        return State{torpedo_homing::available(), std::to_string(static_cast<int>(torpedo_homing::range())), unavailable, {}, false};
    };
    range.run = [](const Model &, const Values &args, const Output &) {
        torpedo_homing::set_range(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(range));

    auto speed = variable("torpedohoming speed", "Metres a second: slower than this, nothing steers (0 to 100, default 4): the turning of the velocity you already have only, the push works at any speed",
                          Group::gameplay, argument("m/s", Type::number));
    speed.inspect = [unavailable](const Model &) {
        return State{torpedo_homing::available(), std::to_string(static_cast<int>(torpedo_homing::min_speed())), unavailable, {}, false};
    };
    speed.run = [](const Model &, const Values &args, const Output &) {
        torpedo_homing::set_min_speed(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(speed));

    auto thrust = variable("torpedohoming thrust", "How hard the torpedo is pushed at the target, in m/s per second (0 to 200, default 30): "
                           "what flies it with no thumbstick, up or down as the target lies",
                           Group::gameplay, argument("m/s2", Type::number));
    thrust.inspect = [unavailable](const Model &) {
        return State{torpedo_homing::available(), std::to_string(static_cast<int>(torpedo_homing::thrust())), unavailable, {}, false};
    };
    thrust.run = [](const Model &, const Values &args, const Output &) {
        torpedo_homing::set_thrust(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(thrust));

    auto top = variable("torpedohoming maxspeed", "The speed in m/s (5 to 300, default 45) above which the push stops adding speed",
                        Group::gameplay, argument("m/s", Type::number));
    top.inspect = [unavailable](const Model &) {
        return State{torpedo_homing::available(), std::to_string(static_cast<int>(torpedo_homing::max_speed())), unavailable, {}, false};
    };
    top.run = [](const Model &, const Values &args, const Output &) {
        torpedo_homing::set_max_speed(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(top));

    auto learn = variable("torpedohoming learn", "Log the air state each dive, torpedo or glide lands in, to tell them apart",
                          Group::gameplay, argument("0|1", Type::boolean));
    learn.inspect = [unavailable](const Model &) {
        return boolean_state(torpedo_homing::available(), torpedo_homing::learning(), unavailable);
    };
    learn.run = [](const Model &, const Values &args, const Output &) { torpedo_homing::set_learning(std::get<bool>(args[0])); };
    registry.add(std::move(learn));
}
} // namespace dingosdk::console
