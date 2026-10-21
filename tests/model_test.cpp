#include "tablist/config.hpp"
#include "tablist/model.hpp"

#include <cassert>
#include <iostream>
#include <vector>

int main() {
    assert(tablist::stripMinecraftFormatting("Steve") == "Steve");
    assert(tablist::stripMinecraftFormatting("\xC2\xA7" "aAlex") == "Alex");

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
      "text_color":"#FFEEDD", "corner_radius":12, "maximum_rows":20
    })", error);
    assert(error.empty());
    assert(!config.showFaces);
    assert(config.cornerRadius == 12.0f);
    assert(config.maximumRows == 20);
    std::string roundTripError;
    const auto roundTrip = tablist::parseConfig(tablist::configJson(config), roundTripError);
    assert(roundTripError.empty());
    assert(!roundTrip.showFaces && roundTrip.maximumRows == 20);

    std::cout << "tablist_test: PASS\n";
}

