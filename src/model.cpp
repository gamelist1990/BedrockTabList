#include "tablist/model.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace tablist {

bool WorldIdentityTracker::observe(std::uintptr_t level, std::uintptr_t player,
                                   std::optional<std::uint64_t> runtimeId) noexcept {
    const bool changed = level != level_ || player != player_ ||
        (runtimeId && runtimeId_ && runtimeId != runtimeId_);
    if (changed) runtimeId_.reset();
    level_ = level;
    player_ = player;
    if (runtimeId) runtimeId_ = runtimeId;
    return changed;
}

bool isMurderLobbyItem(std::string_view identifier) noexcept {
    // Stable IDs observed in the hub and pre-game lobby, never display names.
    return identifier.starts_with("hivehub:") || identifier == "hivecommon:settings";
}

MurderRole murderRoleForHeldItem(std::string_view identifier,
                                 std::string_view displayName) {
    (void)displayName; // Role identity is based only on the stable Item ID.
    const auto equalsIgnoreCase = [](std::string_view left, std::string_view right) {
        if (left.size() != right.size()) return false;
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(left[i])) !=
                std::tolower(static_cast<unsigned char>(right[i]))) return false;
        }
        return true;
    };
    const auto idIs = [&](std::string_view id) {
        return equalsIgnoreCase(identifier, id) ||
            (identifier.size() > id.size() && identifier[identifier.size() - id.size() - 1] == ':' &&
             equalsIgnoreCase(identifier.substr(identifier.size() - id.size()), id));
    };
    if (idIs("bow") || idIs("arrow") || idIs("magnifying_glass_item"))
        return MurderRole::Sheriff;
    constexpr std::string_view murderWeaponPrefix = "hivemurder:murder_weapon_";
    if (identifier.size() >= murderWeaponPrefix.size() &&
        equalsIgnoreCase(identifier.substr(0, murderWeaponPrefix.size()), murderWeaponPrefix))
        return MurderRole::Murderer;
    constexpr std::array<std::string_view, 7> meleeWeapons{
        "wooden_sword", "stone_sword", "iron_sword", "golden_sword",
        "diamond_sword", "netherite_sword", "mace"};
    if (std::any_of(meleeWeapons.begin(), meleeWeapons.end(), idIs))
        return MurderRole::Murderer;
    return MurderRole::None;
}

std::size_t nextActorScanIndex(std::span<const std::uintptr_t> sortedHandles,
                               std::uintptr_t lastProcessedHandle) noexcept {
    if (sortedHandles.empty()) return 0;
    const auto next = std::upper_bound(sortedHandles.begin(), sortedHandles.end(),
                                       lastProcessedHandle);
    return next == sortedHandles.end()
        ? 0
        : static_cast<std::size_t>(next - sortedHandles.begin());
}

namespace {

struct FormattedText {
    std::string plain;
    std::vector<unsigned> colorAtByte;
};

bool isUtf8ContinuationByte(std::string_view text, std::size_t index) noexcept {
    if (index == 0 || index >= text.size()) return false;
    const auto current = static_cast<unsigned char>(text[index]);
    if ((current & 0xC0) != 0x80) return false;

    auto leadIndex = index - 1;
    while (leadIndex > 0 &&
           (static_cast<unsigned char>(text[leadIndex]) & 0xC0) == 0x80)
        --leadIndex;
    const auto lead = static_cast<unsigned char>(text[leadIndex]);
    const std::size_t width = lead >= 0xC2 && lead <= 0xDF ? 2
        : lead >= 0xE0 && lead <= 0xEF ? 3
        : lead >= 0xF0 && lead <= 0xF4 ? 4 : 1;
    return width > 1 && index < leadIndex + width;
}

FormattedText parseFormatting(std::string_view text) {
    FormattedText result;
    result.plain.reserve(text.size());
    result.colorAtByte.reserve(text.size());
    // Bedrock adds material colors after the shared Java/Bedrock palette.
    // §k/§l/§o remain formatting codes, while §m/§n are Bedrock colors.
    constexpr std::string_view colorCodes = "0123456789abcdefghijmnpqstuv";
    constexpr unsigned noColor = 28;
    unsigned activeColor = noColor;
    for (std::size_t i = 0; i < text.size();) {
        const auto current = static_cast<unsigned char>(text[i]);
        std::size_t codeIndex = std::string_view::npos;
        std::size_t sequenceLength = 0;
        if (current == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            codeIndex = i + 2;
            sequenceLength = 3;
        } else if (current == 0xA7 && i + 1 < text.size() &&
                   !isUtf8ContinuationByte(text, i)) {
            codeIndex = i + 1;
            sequenceLength = 2;
        }
        if (sequenceLength != 0) {
            const auto code = static_cast<char>(std::tolower(
                static_cast<unsigned char>(text[codeIndex])));
            const auto color = colorCodes.find(code);
            // Target-server formatting differs from the vanilla extended
            // palette for these live nameTag codes: §p is green and §s orange.
            if (code == 'p')
                activeColor = 10;
            else if (code == 's')
                activeColor = 6;
            else if (color != std::string_view::npos)
                activeColor = static_cast<unsigned>(color);
            else if (code == 'r')
                activeColor = noColor;
            i += sequenceLength;
            continue;
        }
        result.plain.push_back(text[i]);
        result.colorAtByte.push_back(activeColor);
        ++i;
    }
    return result;
}

bool equalsIgnoreAsciiCase(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

} // namespace

std::string stripMinecraftFormatting(std::string_view text) {
    return parseFormatting(text).plain;
}

unsigned minecraftNameColorOrder(std::string_view text) noexcept {
    try {
        const auto parsed = parseFormatting(text);
        for (const auto color : parsed.colorAtByte)
            if (color < 28) return color;
    } catch (...) {
        // Keep the legacy lookup noexcept; allocation failure means no color.
    }
    return 28;
}

std::optional<unsigned> actorNameColorForPlayerName(
    std::string_view playerListName, std::string_view actorNameTag) {
    const auto parsed = parseFormatting(actorNameTag);
    const auto& actorRawName = parsed.plain;
    if (playerListName.empty() || actorRawName.empty() ||
        playerListName.size() > actorRawName.size()) return std::nullopt;
    for (std::size_t offset = 0;
         offset + playerListName.size() <= actorRawName.size(); ++offset) {
        if (equalsIgnoreAsciiCase(playerListName,
                                  std::string_view(actorRawName).substr(offset, playerListName.size()))) {
            const auto color = parsed.colorAtByte[offset];
            if (color < 28) return color;
        }
    }
    return std::nullopt;
}

void sortPlayerNames(std::vector<std::string>& names) {
    std::stable_sort(names.begin(), names.end(), [](const auto& left, const auto& right) {
        return std::lexicographical_compare(
            left.begin(), left.end(), right.begin(), right.end(),
            [](unsigned char a, unsigned char b) {
                return std::tolower(a) < std::tolower(b);
            });
    });
}

Layout layoutFor(std::size_t playerCount, std::size_t maximumRows) noexcept {
    if (maximumRows == 0) maximumRows = 1;
    if (playerCount == 0) return {1, 0};
    const auto columns = (playerCount + maximumRows - 1) / maximumRows;
    return {columns, std::min(playerCount, maximumRows)};
}

} // namespace tablist

