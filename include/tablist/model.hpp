#pragma once

#include <cstddef>
#include <optional>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tablist {

struct Layout {
    std::size_t columns = 1;
    std::size_t rows = 0;
};

enum class MurderRole { None, Sheriff, Murderer };
// Servers can replace the local player without replacing the Level allocation.
// Missing RuntimeID reads do not replace the last successful identity.
class WorldIdentityTracker {
public:
    bool observe(std::uintptr_t level, std::uintptr_t player,
                 std::optional<std::uint64_t> runtimeId) noexcept;
private:
    std::uintptr_t level_{};
    std::uintptr_t player_{};
    std::optional<std::uint64_t> runtimeId_;
};

[[nodiscard]] MurderRole murderRoleForHeldItem(std::string_view identifier,
                                                std::string_view displayName);
[[nodiscard]] bool isMurderLobbyItem(std::string_view identifier) noexcept;
// Actor handles are sorted before polling. Preserve round-robin progress when
// the live Actor snapshot is refreshed so large worlds cannot starve entries
// near the end of the list.
[[nodiscard]] std::size_t nextActorScanIndex(
    std::span<const std::uintptr_t> sortedHandles,
    std::uintptr_t lastProcessedHandle) noexcept;

std::string stripMinecraftFormatting(std::string_view text);
// Returns the first active legacy Minecraft name color (0-15), or 16 when
// the name has no color code. Formatting codes such as bold are ignored.
unsigned minecraftNameColorOrder(std::string_view text) noexcept;
std::optional<unsigned> actorNameColorForPlayerName(
    std::string_view playerListName, std::string_view actorNameTag);
void sortPlayerNames(std::vector<std::string>& names);
Layout layoutFor(std::size_t playerCount, std::size_t maximumRows = 16) noexcept;

} // namespace tablist

