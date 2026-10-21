#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tablist {

struct Layout {
    std::size_t columns = 1;
    std::size_t rows = 0;
};

std::string stripMinecraftFormatting(std::string_view text);
void sortPlayerNames(std::vector<std::string>& names);
Layout layoutFor(std::size_t playerCount, std::size_t maximumRows = 16) noexcept;

} // namespace tablist

