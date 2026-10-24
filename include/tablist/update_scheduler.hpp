#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace tablist {

// A sweep target, distributed over game ticks. Actual work is also constrained
// by the cooperative time budget in the native reader; never catch up in bursts.
inline std::size_t equipmentReadsPerTick(std::size_t players,unsigned intervalMs) noexcept {
    if(!players) return 0;
    const auto ticks=std::max<std::size_t>(1,(intervalMs+49)/50);
    return std::min<std::size_t>(12,(players+ticks-1)/ticks);
}

enum class NativeUpdate : std::size_t { Item, Players, Roster, Face };

// Select one native operation per callback, without catch-up bursts. Round-robin
// arbitration prevents a large pending roster from starving equipment or faces.
class NativeUpdateScheduler {
public:
    void reset() noexcept { due_.fill(0); cursor_ = 0; nextWorkMs_ = 0; }

    [[nodiscard]] std::optional<NativeUpdate> choose(
        std::int64_t now, const std::array<bool, 4>& ready) noexcept {
        if (now < nextWorkMs_) return std::nullopt;
        constexpr std::array<std::int64_t, 4> intervals{150, 1000, 0, 200};
        for (std::size_t n = 0; n < ready.size(); ++n) {
            const auto i = (cursor_ + n) % ready.size();
            if (!ready[i] || now < due_[i]) continue;
            cursor_ = (i + 1) % ready.size();
            due_[i] = now + intervals[i];
            nextWorkMs_ = now + 25;
            return static_cast<NativeUpdate>(i);
        }
        return std::nullopt;
    }

    void defer(NativeUpdate work, std::int64_t until) noexcept {
        due_[static_cast<std::size_t>(work)] = until;
    }

private:
    std::array<std::int64_t, 4> due_{};
    std::size_t cursor_{};
    std::int64_t nextWorkMs_{};
};

} // namespace tablist
