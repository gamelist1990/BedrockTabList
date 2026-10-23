#pragma once

#include <string>
#include <string_view>

namespace tablist {

struct Color {
    float r{};
    float g{};
    float b{};
    float a{1.0f};
};

struct Config {
    bool enabled = true;
    bool showFaces = true;
    bool sortByNameColor = false;
    bool murderDetect = true;
    bool debug = false;
    unsigned murderScanIntervalTicks = 20;
    Color background{0.039f, 0.051f, 0.078f, 0.80f};
    Color text{0.949f, 0.961f, 0.984f, 1.0f};
    Color accent{0.392f, 0.863f, 0.608f, 1.0f};
    Color border{0.431f, 0.569f, 0.863f, 0.666667f};
    float cornerRadius = 7.0f;
    float rowHeight = 24.0f;
    float headerHeight = 32.0f;
    float faceSize = 18.0f;
    float topOffset = 22.0f;
    float columnGap = 10.0f;
    float minimumColumnWidth = 150.0f;
    unsigned maximumRows = 16;
    unsigned reloadIntervalMs = 250;
};

Config parseConfig(std::string_view jsonText, std::string& error) noexcept;
std::string configJson(const Config& config);

} // namespace tablist

