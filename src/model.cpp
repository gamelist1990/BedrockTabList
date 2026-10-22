#include "tablist/model.hpp"

#include <algorithm>
#include <cctype>

namespace tablist {

std::string stripMinecraftFormatting(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto current = static_cast<unsigned char>(text[i]);
        if (current == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            i += 2;
            continue;
        }
        if (current == 0xA7 && i + 1 < text.size()) {
            ++i;
            continue;
        }
        result.push_back(text[i]);
    }
    return result;
}

unsigned minecraftNameColorOrder(std::string_view text) noexcept {
    constexpr std::string_view colorCodes = "0123456789abcdef";
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        std::size_t codeIndex = i;
        const auto current = static_cast<unsigned char>(text[i]);
        if (current == 0xC2 && i + 2 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            codeIndex = i + 2;
            i += 2;
        } else if (current == 0xA7) {
            codeIndex = i + 1;
            ++i;
        } else {
            continue;
        }
        if (codeIndex >= text.size()) break;
        const auto code = static_cast<char>(std::tolower(
            static_cast<unsigned char>(text[codeIndex])));
        const auto found = colorCodes.find(code);
        if (found != std::string_view::npos) return static_cast<unsigned>(found);
    }
    return 16;
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

