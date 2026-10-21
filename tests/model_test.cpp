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

    std::cout << "tablist_test: PASS\n";
}

