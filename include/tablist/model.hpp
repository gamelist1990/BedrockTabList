#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tablist {

struct Layout {
    std::size_t columns = 1;
    std::size_t rows = 0;
};

enum class MurderRole { None, Sheriff, Murderer };
[[nodiscard]] MurderRole murderRoleForHeldItem(std::string_view identifier,
                                                std::string_view displayName);

std::string stripMinecraftFormatting(std::string_view text);
// Returns the first active legacy Minecraft name color (0-15), or 16 when
// the name has no color code. Formatting codes such as bold are ignored.
unsigned minecraftNameColorOrder(std::string_view text) noexcept;
std::optional<unsigned> actorNameColorForPlayerName(
    std::string_view playerListName, std::string_view actorNameTag);
void sortPlayerNames(std::vector<std::string>& names);
Layout layoutFor(std::size_t playerCount, std::size_t maximumRows = 16) noexcept;

} // namespace tablist

