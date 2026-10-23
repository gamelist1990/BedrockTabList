#include "tablist/config.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace tablist {
namespace {
template <class T>
T numberOr(const nlohmann::json& root, const char* key, T fallback) {
    const auto it = root.find(key);
    return it != root.end() && it->is_number() ? it->get<T>() : fallback;
}

int hexDigit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

Color parseColor(const nlohmann::json& root, const char* key, Color fallback) {
    const auto it = root.find(key);
    if (it == root.end() || !it->is_string()) return fallback;
    const auto value = it->get<std::string>();
    if ((value.size() != 7 && value.size() != 9) || value.front() != '#') return fallback;
    unsigned channels[4]{0, 0, 0, 255};
    for (std::size_t channel = 0; channel < (value.size() - 1) / 2; ++channel) {
        const int high = hexDigit(value[1 + channel * 2]);
        const int low = hexDigit(value[2 + channel * 2]);
        if (high < 0 || low < 0) return fallback;
        channels[channel] = static_cast<unsigned>(high * 16 + low);
    }
    return {channels[0] / 255.0f, channels[1] / 255.0f,
            channels[2] / 255.0f, channels[3] / 255.0f};
}

std::string colorString(Color color) {
    const auto channel = [](float value) {
        return static_cast<unsigned>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    };
    std::ostringstream result;
    result << '#' << std::uppercase << std::hex << std::setfill('0')
           << std::setw(2) << channel(color.r) << std::setw(2) << channel(color.g)
           << std::setw(2) << channel(color.b) << std::setw(2) << channel(color.a);
    return result.str();
}
} // namespace

Config parseConfig(std::string_view jsonText, std::string& error) noexcept {
    Config config;
    error.clear();
    try {
        const auto root = nlohmann::json::parse(jsonText);
        if (!root.is_object()) throw std::runtime_error("config root must be an object");
        config.enabled = root.value("enabled", config.enabled);
        config.showFaces = root.value("show_faces", config.showFaces);
        config.sortByNameColor = root.value("sort_by_name_color", config.sortByNameColor);
        config.murderDetect = root.value("murder_detect", config.murderDetect);
        config.debug = root.value("debug", config.debug);
        const auto scanInterval = numberOr<unsigned>(
            root, "murder_scan_interval_ticks", config.murderScanIntervalTicks);
        // 20 was the previous built-in default. Treat it as the new default
        // for existing generated config files while preserving custom values.
        config.murderScanIntervalTicks = scanInterval == 20
            ? 1u : std::clamp(scanInterval, 1u, 40u);
        config.background = parseColor(root, "background_color", config.background);
        config.text = parseColor(root, "text_color", config.text);
        config.accent = parseColor(root, "accent_color", config.accent);
        config.border = parseColor(root, "border_color", config.border);
        config.cornerRadius = std::clamp(numberOr<float>(root, "corner_radius", config.cornerRadius), 0.0f, 24.0f);
        config.rowHeight = std::clamp(numberOr<float>(root, "row_height", config.rowHeight), 16.0f, 64.0f);
        config.headerHeight = std::clamp(numberOr<float>(root, "header_height", config.headerHeight), 20.0f, 80.0f);
        config.faceSize = std::clamp(numberOr<float>(root, "face_size", config.faceSize), 8.0f, 48.0f);
        config.faceSize = std::min(config.faceSize, config.rowHeight - 2.0f);
        config.topOffset = std::clamp(numberOr<float>(root, "top_offset", config.topOffset), 0.0f, 500.0f);
        config.columnGap = std::clamp(numberOr<float>(root, "column_gap", config.columnGap), 0.0f, 40.0f);
        config.minimumColumnWidth = std::clamp(numberOr<float>(root, "minimum_column_width", config.minimumColumnWidth), 80.0f, 500.0f);
        config.maximumRows = std::clamp(numberOr<unsigned>(root, "maximum_rows", config.maximumRows), 1u, 64u);
        config.reloadIntervalMs = std::clamp(numberOr<unsigned>(root, "reload_interval_ms", config.reloadIntervalMs), 50u, 5000u);
        if (!std::isfinite(config.cornerRadius) || !std::isfinite(config.rowHeight) ||
            !std::isfinite(config.headerHeight) || !std::isfinite(config.faceSize) ||
            !std::isfinite(config.topOffset) || !std::isfinite(config.columnGap) ||
            !std::isfinite(config.minimumColumnWidth))
            throw std::runtime_error("style values must be finite");
    } catch (const std::exception& exception) {
        error = exception.what();
    }
    return config;
}

std::string configJson(const Config& config) {
    nlohmann::ordered_json root;
    root["enabled"] = config.enabled;
    root["show_faces"] = config.showFaces;
    root["sort_by_name_color"] = config.sortByNameColor;
    root["murder_detect"] = config.murderDetect;
    root["debug"] = config.debug;
    root["murder_scan_interval_ticks"] = config.murderScanIntervalTicks;
    root["background_color"] = colorString(config.background);
    root["text_color"] = colorString(config.text);
    root["accent_color"] = colorString(config.accent);
    root["border_color"] = colorString(config.border);
    root["corner_radius"] = config.cornerRadius;
    root["row_height"] = config.rowHeight;
    root["header_height"] = config.headerHeight;
    root["face_size"] = config.faceSize;
    root["top_offset"] = config.topOffset;
    root["column_gap"] = config.columnGap;
    root["minimum_column_width"] = config.minimumColumnWidth;
    root["maximum_rows"] = config.maximumRows;
    root["reload_interval_ms"] = config.reloadIntervalMs;
    return root.dump(2) + '\n';
}

} // namespace tablist

