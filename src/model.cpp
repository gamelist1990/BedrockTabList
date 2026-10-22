#include "tablist/model.hpp"

#include <algorithm>
#include <cctype>

namespace tablist {

MurderRole murderRoleForHeldItem(std::string_view identifier,
                                 std::string_view displayName) {
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
    if (idIs("iron_sword") || equalsIgnoreCase(displayName, "iron sword"))
        return MurderRole::Murderer;
    if (idIs("bow") || idIs("arrow") || equalsIgnoreCase(displayName, "bow") ||
        equalsIgnoreCase(displayName, "arrow"))
        return MurderRole::Sheriff;
    return MurderRole::None;
}

namespace {

struct FormattedText {
    std::string plain;
    std::vector<unsigned> colorAtByte;
};

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
        } else if (current == 0xA7 && i + 1 < text.size()) {
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

