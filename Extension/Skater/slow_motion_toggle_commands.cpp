#include "slow_motion_toggle.h"
#include "Extension/Console/commands.h"
#include <format>

// Slow Motion's console commands (see slow_motion_toggle.h).
namespace dingosdk::console {
void register_slow_motion_commands(Commands &registry) {
    auto slowmo = variable("slowmo", "Slow motion on or off (single player only); d-pad down toggles it too",
                           Group::gameplay, argument("0|1", Type::boolean));
    slowmo.inspect = [](const Model &) { return boolean_state(true, slow_motion_toggle::active(), {}, slow_motion_toggle::status()); };
    slowmo.run = [](const Model &, const Values &args, const Output &) { slow_motion_toggle::set_active(std::get<bool>(args[0])); };
    registry.add(std::move(slowmo));

    auto speed = variable("slowmo speed", "How slow slow motion runs, as a fraction of normal speed (0.05 to 0.95, default 0.3)",
                          Group::gameplay, argument("speed", Type::number));
    speed.inspect = [](const Model &) { return State{true, std::format("{:.2f}", slow_motion_toggle::speed()), {}, {}, false}; };
    speed.run = [](const Model &, const Values &args, const Output &) {
        slow_motion_toggle::set_speed(static_cast<float>(std::get<double>(args[0])));
    };
    registry.add(std::move(speed));

    auto pad = variable("slowmo button", "Whether d-pad down toggles slow motion (default 1)",
                        Group::gameplay, argument("0|1", Type::boolean));
    pad.inspect = [](const Model &) { return boolean_state(true, slow_motion_toggle::button()); };
    pad.run = [](const Model &, const Values &args, const Output &) { slow_motion_toggle::set_button(std::get<bool>(args[0])); };
    registry.add(std::move(pad));
}
} // namespace dingosdk::console
