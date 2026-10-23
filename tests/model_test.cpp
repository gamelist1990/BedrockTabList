#include "tablist/config.hpp"
#include "tablist/model.hpp"

#include <algorithm>
#include <cassert>
#include <iostream>
#include <vector>

int main() {
    std::string defaultsError;
    const auto defaults = tablist::parseConfig("{}", defaultsError);
    assert(defaultsError.empty() && defaults.murderScanIntervalTicks == 3);
    std::string oldConfigError;
    const auto oldConfig = tablist::parseConfig(
        R"({"murder_scan_interval_ticks":20})", oldConfigError);
    assert(oldConfigError.empty() && oldConfig.murderScanIntervalTicks == 3);
    std::string customConfigError;
    const auto customConfig = tablist::parseConfig(
        R"({"murder_scan_interval_ticks":5})", customConfigError);
    assert(customConfigError.empty() && customConfig.murderScanIntervalTicks == 5);

    assert(tablist::stripMinecraftFormatting("Steve") == "Steve");
    assert(tablist::stripMinecraftFormatting("\xC2\xA7" "aAlex") == "Alex");
    assert(tablist::actorNameColorForPlayerName(
        "Alex", "\xC2\xA7" "7[VIP] \xC2\xA7" "cAlex\xC2\xA7" "8 [L]") == 12);
    // Extended nameTag codes and target-server overrides for §p/§s.
    assert(tablist::actorNameColorForPlayerName(
        "DamerDilly", "\xC2\xA7" "pDamerDilly") == 10);
    assert(tablist::actorNameColorForPlayerName(
        "Emerald", "\xC2\xA7" "qEmerald") == 23);
    assert(tablist::actorNameColorForPlayerName(
        "wiwphix94", "\xC2\xA7" "swiwphix94") == 6);
    assert(tablist::actorNameColorForPlayerName(
        "Biruplayz", "\xC2\xA7" "iBiruplayz\xC2\xA7" "8 [U]") == 18);
    assert(tablist::actorNameColorForPlayerName(
        "Copper", "\xC2\xA7" "nCopper") == 21);
    assert(tablist::actorNameColorForPlayerName(
        "Resin", "\xC2\xA7" "vResin") == 27);
    assert(tablist::actorNameColorForPlayerName(
        "kanatans77", "\xC2\xA7" "ikanatans77\xC2\xA7" "8 [\xC2\xA7" "iU\xC2\xA7" "8]") == 18);
    assert(!tablist::actorNameColorForPlayerName(
        "Alex", "\xC2\xA7" "cPrefix \xC2\xA7" "rAlex\xC2\xA7" "8 rank"));

    std::vector<std::string> names{"zed", "Alex", "bob"};
    tablist::sortPlayerNames(names);
    assert((names == std::vector<std::string>{"Alex", "bob", "zed"}));

    const auto empty = tablist::layoutFor(0);
    assert(empty.columns == 1 && empty.rows == 0);
    const auto small = tablist::layoutFor(8);
    assert(small.columns == 1 && small.rows == 8);
    const auto large = tablist::layoutFor(33);
    assert(large.columns == 3 && large.rows == 16);

    std::string error;
    const auto config = tablist::parseConfig(R"({
      "show_faces":false, "background_color":"#112233CC",
      "text_color":"#FFEEDD", "corner_radius":12, "maximum_rows":20,
      "murder_detect":true, "debug":true
    })", error);
    assert(error.empty());
    assert(!config.showFaces);
    assert(config.cornerRadius == 12.0f);
    assert(config.maximumRows == 20);
    assert(config.murderDetect && config.debug);
    std::string roundTripError;
    const auto roundTrip = tablist::parseConfig(tablist::configJson(config), roundTripError);
    assert(roundTripError.empty());
    assert(!roundTrip.showFaces && roundTrip.maximumRows == 20);
    assert(roundTrip.murderDetect && roundTrip.debug);
    assert(tablist::murderRoleForHeldItem("minecraft:bow", "Bow") ==
           tablist::MurderRole::Sheriff);
    assert(tablist::murderRoleForHeldItem("", "Bow") ==
           tablist::MurderRole::None);
    assert(tablist::murderRoleForHeldItem("minecraft:arrow", "Arrow") ==
           tablist::MurderRole::Sheriff);
    assert(tablist::murderRoleForHeldItem(
        "hivemurder:magnifying_glass_item", "Magnifying Glass") ==
           tablist::MurderRole::Sheriff);
    assert(tablist::murderRoleForHeldItem(
        "hivemurder:zapper_ray_gun", "Zapper Ray Gun") ==
           tablist::MurderRole::Murderer);
    assert(tablist::murderRoleForHeldItem("hivemurder:utility_compass", "Compass") ==
           tablist::MurderRole::None);
    assert(tablist::murderRoleForHeldItem(
        "hivemurder:murder_weapon_baseball_bat", "hivemurder:murder_weapon_baseball_bat") ==
           tablist::MurderRole::Murderer);
    assert(tablist::murderRoleForHeldItem("minecraft:golden_sword", "Golden Sword") ==
           tablist::MurderRole::Murderer);
    assert(tablist::murderRoleForHeldItem("minecraft:diamond_sword", "Diamond Sword") ==
           tablist::MurderRole::Murderer);
    assert(tablist::murderRoleForHeldItem("minecraft:crossbow", "Crossbow") ==
           tablist::MurderRole::None);
    constexpr std::string_view formattedJapaneseItem =
        "\302\247o\302\247f\302\247r\302\247a"
        "\343\203\257\343\203\263\343\202\267\343\203\247"
        "\343\203\203\343\203\210\345\205\211\347\267\232\351\212\203"
        "\302\247r";
    constexpr std::string_view plainJapaneseItem =
        "\343\203\257\343\203\263\343\202\267\343\203\247"
        "\343\203\203\343\203\210\345\205\211\347\267\232\351\212\203";
    assert(tablist::stripMinecraftFormatting(formattedJapaneseItem) == plainJapaneseItem);

    // Refreshing the Actor snapshot must resume after the last Actor already
    // sampled, rather than resetting to index zero and starving later players.
    std::vector<std::uintptr_t> actorHandles;
    for (std::uintptr_t handle = 1; handle <= 40; ++handle)
        actorHandles.push_back(handle);
    std::uintptr_t lastActor = 0;
    std::vector<std::uintptr_t> visitedActors;
    for (int refresh = 0; refresh < 3; ++refresh) {
        for (int sample = 0; sample < 25; ++sample) {
            const auto index = tablist::nextActorScanIndex(actorHandles, lastActor);
            lastActor = actorHandles[index];
            visitedActors.push_back(lastActor);
        }
    }
    for (const auto handle : actorHandles)
        assert(std::find(visitedActors.begin(), visitedActors.end(), handle) !=
               visitedActors.end());
    assert(tablist::nextActorScanIndex(actorHandles, 40) == 0);

    std::cout << "tablist_test: PASS\n";
}

