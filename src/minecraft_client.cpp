#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <bedrock/hook_runtime.h>
#include <bedrock/gui_overlay.hpp>
#include <bedrock/logger.hpp>
#include <bedrock/remote_catalog.hpp>
#include <bedrock/resolver.hpp>
#include <bedrock/generated_symbols.hpp>
#include <imgui.h>
#include "tablist/config.hpp"
#include "tablist/model.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
enum class Status : unsigned { NotStarted, Loading, Running, Stopped, Failed };

using KeyboardFeedFn = bool(__fastcall*)(int, bool);
using CursorFn = void(__fastcall*)(void*);
using GetFovFn = float(__fastcall*)(void*, float, void*, void*);
using LevelSubTickFn = void(__fastcall*)(void*);
using ActorGetNameTagFn = const std::string&(__fastcall*)(void*);
using RuntimeActorListFn = void(__fastcall*)(void*, std::vector<void*>*);

// These layouts come from the matching Latite SDK revision and are used only
// after BedrockDynamicSDK has verified the exact 1.26.52.3 executable hash.
constexpr std::size_t clientMinecraftOffset = 0x1B0;
constexpr std::size_t minecraftGameSessionOffset = 0xC8;
constexpr std::size_t sessionReadyOffset = 0x28;
constexpr std::size_t sessionLevelControlOffset = 0x30;
constexpr std::size_t sessionLevelOffset = 0x40;
constexpr std::size_t levelPlayerListOffset = 0x4E0;
constexpr int tabInputKey = 0x09;
constexpr int tabInputKeyGlfw = 258;

struct Uuid {
    std::uint64_t low{};
    std::uint64_t high{};
    bool operator==(const Uuid&) const = default;
};
struct UuidHash {
    std::size_t operator()(const Uuid& value) const noexcept {
        return static_cast<std::size_t>(value.low ^ value.high);
    }
};
struct NativePlayerListEntry {
    std::byte beforeName[0x18];
    std::string name;
    std::byte beforeSkin[0x80 - 0x18 - sizeof(std::string)];
    struct {
        void* implementation;
        void* referenceCount;
    } skin;
    std::byte afterSkin[0xA8 - 0x80 - 0x10];
};
struct NativeSkinImage {
    std::int32_t format;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t depth;
    std::uint8_t usage;
    std::byte padding[7];
    void* deleter;
    const std::uint8_t* data;
    std::size_t size;
};
struct PlayerRow {
    std::string name;
    unsigned nameColorOrder = 16;
    std::array<std::uint32_t, 64> face{};
    bool hasFace = false;
};
static_assert(sizeof(std::string) == 0x20);
static_assert(sizeof(NativePlayerListEntry) == 0xA8);
static_assert(sizeof(NativeSkinImage) == 0x30);
using NativePlayerList = std::unordered_map<Uuid, NativePlayerListEntry, UuidHash>;

std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
std::atomic<bool> tabHeld{false};
std::atomic<bool> gameplayActive{false};
std::atomic<void*> clientInstance{nullptr};
std::atomic<void*> levelInstance{nullptr};
std::atomic<std::int64_t> levelHeartbeatMs{0};
std::atomic<KeyboardFeedFn> originalKeyboardFeed{nullptr};
std::atomic<CursorFn> originalGrabCursor{nullptr};
std::atomic<CursorFn> originalReleaseCursor{nullptr};
std::atomic<GetFovFn> originalGetFov{nullptr};
std::atomic<LevelSubTickFn> originalLevelSubTick{nullptr};
std::atomic<void*> originalActorGetNameTag{nullptr};
std::atomic<void*> runtimeActorListFunction{nullptr};
struct ActorNameTagEntry {
    std::string nameTag;
    unsigned colorOrder = 16;
    std::uint64_t sequence = 0;
};
std::mutex actorNameTagMutex;
std::unordered_map<void*, ActorNameTagEntry> actorNameTags;
std::unordered_set<std::string> loggedNameTags;
std::unordered_set<std::string> loggedPlayerColorMatches;
std::atomic<unsigned> loggedNameTagDetections{0};
std::atomic<std::uint64_t> actorNameTagSequence{0};
std::atomic<std::int64_t> lastActorListRefreshMs{0};
std::atomic<bool> loggedActorListFailure{false};
std::atomic<bool> loggedActorListSuccess{false};
std::atomic<std::shared_ptr<const tablist::Config>> liveConfig{
    std::make_shared<const tablist::Config>()};
std::array<BDS_HookToken, 5> hookTokens{};
struct NextFrame { BDS_HookNext next{}; void* frame{}; };
thread_local NextFrame keyFrame, cursorFrame, fovFrame, levelFrame;
thread_local NextFrame* activeCursorFrame{};
bool __fastcall callNextKeyboard(int key, bool down) {
    if (!keyFrame.next) return false;
    BDS_KeyboardArgs args{key, down ? 1 : 0, 0};
    keyFrame.next(keyFrame.frame, &args);
    return args.result != 0;
}
void __fastcall callNextCursor(void* self) {
    if (activeCursorFrame && activeCursorFrame->next) {
        BDS_CursorArgs args{self}; activeCursorFrame->next(activeCursorFrame->frame, &args);
    }
}
float __fastcall callNextFov(void* self, float fov, void* a3, void* a4) {
    if (!fovFrame.next) return fov;
    BDS_FovArgs args{self, fov, a3, a4, fov}; fovFrame.next(fovFrame.frame, &args);
    return args.result;
}
void __fastcall callNextLevel(void* level) {
    if (levelFrame.next) { BDS_LevelArgs args{level}; levelFrame.next(levelFrame.frame, &args); }
}
std::mutex diagnosticMutex;
std::ofstream diagnosticLog;
std::atomic<bool> loggedRenderer{false};
std::atomic<bool> loggedListFailure{false};
std::atomic<bool> loggedListSuccess{false};

template <class... Args>
void diagnostic(Args&&... args) {
    std::lock_guard lock(diagnosticMutex);
    if (!diagnosticLog) return;
    (diagnosticLog << ... << std::forward<Args>(args));
    diagnosticLog << '\n';
    diagnosticLog.flush();
}

bool readable(const void* pointer, std::size_t bytes) noexcept {
    if (!pointer || !bytes) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start > UINTPTR_MAX - bytes) return false;
    const auto wantedEnd = start + bytes;
    auto cursor = start;
    while (cursor < wantedEnd) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) ||
            region.State != MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD protection = region.Protect & 0xFF;
        if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
            protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
            protection != PAGE_EXECUTE_READWRITE &&
            protection != PAGE_EXECUTE_WRITECOPY) return false;
        const auto regionStart = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        const auto regionEnd = regionStart + region.RegionSize;
        if (regionEnd <= cursor) return false;
        cursor = regionEnd;
    }
    return true;
}

template <class T>
bool safeRead(const void* base, std::size_t offset, T& result) noexcept {
    if (!base) return false;
    const auto address = static_cast<const std::byte*>(base) + offset;
    if (!readable(address, sizeof(T))) return false;
    __try {
        result = *reinterpret_cast<const T*>(address);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool validateClient(void* client) noexcept {
    std::uintptr_t vtable = 0;
    return safeRead(client, 0, vtable) && vtable != 0 && readable(
        reinterpret_cast<const void*>(vtable), sizeof(void*));
}

bool extractFace(const NativePlayerListEntry& entry, PlayerRow& row) {
    if (!entry.skin.implementation) return false;
    NativeSkinImage image{};
    if (!safeRead(entry.skin.implementation, 0xA0, image) ||
        !image.data || image.width < 64 || image.height < 16 ||
        image.width > 4096 || image.height > 4096 || image.width % 8 != 0)
        return false;
    const std::size_t expected = static_cast<std::size_t>(image.width) * image.height * 4u;
    if (expected / 4u / image.width != image.height || image.size < expected ||
        !readable(image.data, expected)) return false;
    const auto sourceSize = image.width / 8;
    const auto faceX = sourceSize;
    const auto faceY = sourceSize;
    const auto overlayX = sourceSize * 5;
    if (faceX + sourceSize > image.width || overlayX + sourceSize > image.width ||
        faceY + sourceSize > image.height) return false;

    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return image.data + ((static_cast<std::size_t>(y) * image.width + x) * 4u);
    };
    for (std::uint32_t y = 0; y < 8; ++y) {
        for (std::uint32_t x = 0; x < 8; ++x) {
            const auto sourceX = x * sourceSize / 8;
            const auto sourceY = y * sourceSize / 8;
            const auto* base = pixel(faceX + sourceX, faceY + sourceY);
            const auto* overlay = pixel(overlayX + sourceX, faceY + sourceY);
            const unsigned alpha = overlay[3];
            const unsigned inverse = 255u - alpha;
            const auto blend = [&](unsigned channel) {
                return static_cast<std::uint8_t>(
                    (overlay[channel] * alpha + base[channel] * inverse) / 255u);
            };
            const auto r = blend(0);
            const auto g = blend(1);
            const auto b = blend(2);
            row.face[y * 8 + x] = (static_cast<std::uint32_t>(r) << 24) |
                (static_cast<std::uint32_t>(g) << 16) |
                (static_cast<std::uint32_t>(b) << 8) | 255u;
        }
    }
    row.hasFace = true;
    return true;
}

std::int64_t steadyMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void recordLevel(void* level) noexcept {
    if (!level) return;
    void* previous = levelInstance.exchange(level, std::memory_order_acq_rel);
    levelHeartbeatMs.store(steadyMilliseconds(), std::memory_order_release);
    if (previous != level) {
        {
            std::lock_guard lock(actorNameTagMutex);
            actorNameTags.clear();
            loggedNameTags.clear();
            loggedPlayerColorMatches.clear();
        }
        loggedNameTagDetections.store(0, std::memory_order_release);
        lastActorListRefreshMs.store(0, std::memory_order_release);
        diagnostic("level ready");
    }
}

bool executable(const void* pointer) noexcept {
    MEMORY_BASIC_INFORMATION info{};
    if (!pointer || !VirtualQuery(pointer, &info, sizeof(info)) ||
        info.State != MEM_COMMIT || info.Type != MEM_IMAGE) return false;
    const DWORD protection = info.Protect & 0xFFu;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

// Keep native dispatch inside a tiny SEH boundary with no owning C++ objects
// on its stack. The candidate signature is only usable on an exact build.
bool invokeRuntimeActorList(void* level, void* function,
                            std::vector<void*>* actors) noexcept {
    if (!level || !actors || !readable(level, sizeof(void*)) || !executable(function)) return false;
    __try {
        reinterpret_cast<RuntimeActorListFn>(function)(level, actors);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool getActorNameTagView(void* actor, const char** data, std::size_t* size) noexcept {
    auto* rawFunction = originalActorGetNameTag.load(std::memory_order_acquire);
    if (!actor || !data || !size || !rawFunction || !readable(actor, sizeof(void*)) ||
        !executable(rawFunction)) return false;
    __try {
        const auto& nameTag = reinterpret_cast<ActorGetNameTagFn>(rawFunction)(actor);
        *size = nameTag.size();
        *data = nameTag.data();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool copyActorNameTag(void* actor, std::string* output) {
    const char* data = nullptr;
    std::size_t size = 0;
    if (!output || !getActorNameTagView(actor, &data, &size) || size > 512 ||
        (size && !readable(data, size))) return false;
    output->assign(data, size);
    return true;
}

void updateRuntimeActorNameTags(void* level) {
    auto* function = runtimeActorListFunction.load(std::memory_order_acquire);
    if (!function || !originalActorGetNameTag.load(std::memory_order_acquire)) return;
    std::vector<void*> actors;
    if (!invokeRuntimeActorList(level, function, &actors) || actors.size() > 4096 ||
        (actors.size() && !readable(actors.data(), actors.size() * sizeof(void*)))) {
        runtimeActorListFunction.store(nullptr, std::memory_order_release);
        if (!loggedActorListFailure.exchange(true, std::memory_order_acq_rel))
            diagnostic("Level::getRuntimeActorList signature call failed; actor color sort inactive");
        return;
    }

    std::unordered_map<void*, ActorNameTagEntry> updated;
    updated.reserve(actors.size());
    std::vector<std::string> observedNames;
    observedNames.reserve(std::min<std::size_t>(actors.size(), 64));
    for (void* actor : actors) {
        if (!actor || !readable(actor, sizeof(void*))) continue;
        std::string nameTag;
        if (!copyActorNameTag(actor, &nameTag) || nameTag.empty() || nameTag.size() > 512)
            continue;
        if (tablist::stripMinecraftFormatting(nameTag).empty()) continue;
        observedNames.push_back(nameTag);
        const auto color = tablist::minecraftNameColorOrder(nameTag);
        const auto sequence = actorNameTagSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        updated.insert_or_assign(actor, ActorNameTagEntry{std::move(nameTag), color, sequence});
    }
    std::vector<std::string> namesToLog;
    const auto nameTagCount = updated.size();
    {
        std::lock_guard lock(actorNameTagMutex);
        actorNameTags.swap(updated);
        for (const auto& nameTag : observedNames) {
            if (loggedNameTags.size() >= 512 || !loggedNameTags.insert(nameTag).second) continue;
            const auto logCount = loggedNameTagDetections.fetch_add(1, std::memory_order_acq_rel);
            if (logCount < 64) namesToLog.push_back(nameTag);
        }
    }
    for (const auto& nameTag : namesToLog) diagnostic("World Actor NameTag: ", nameTag);
    if (!loggedActorListSuccess.exchange(true, std::memory_order_acq_rel))
        diagnostic("World Actor list active; ", actors.size(), " actor(s), ",
                   nameTagCount, " NameTag(s) read");
}

bool levelIsFresh() noexcept {
    const auto heartbeat = levelHeartbeatMs.load(std::memory_order_acquire);
    return heartbeat > 0 && steadyMilliseconds() - heartbeat <= 1500;
}

bool tryAcquireLevelFromClient() noexcept {
    void* client = clientInstance.load(std::memory_order_acquire);
    if (!validateClient(client)) return false;
    void* minecraft = nullptr;
    void* session = nullptr;
    void* control = nullptr;
    void* level = nullptr;
    std::uint8_t ready = 0;
    std::uint8_t controlReady = 0;
    if (!safeRead(client, clientMinecraftOffset, minecraft) || !minecraft ||
        !safeRead(minecraft, minecraftGameSessionOffset, session) || !session ||
        !safeRead(session, sessionReadyOffset, ready) || ready != 1 ||
        !safeRead(session, sessionLevelControlOffset, control) || !control ||
        !safeRead(control, 0, controlReady) || controlReady != 1 ||
        !safeRead(session, sessionLevelOffset, level) || !level) return false;
    recordLevel(level);
    return true;
}

bool collectRowsUnsafe(std::vector<PlayerRow>& rows, bool includeFaces,
                       bool sortByNameColor,
                       const std::vector<ActorNameTagEntry>& actorTags) {
    void* level = levelInstance.load(std::memory_order_acquire);
    if (!level) {
        if (!tryAcquireLevelFromClient()) return false;
        level = levelInstance.load(std::memory_order_acquire);
        if (!level) return false;
    }
    NativePlayerList* players = nullptr;
    if (!safeRead(level, levelPlayerListOffset, players) || !players ||
        !readable(players, sizeof(NativePlayerList))) return false;

    const auto count = players->size();
    if (count > 512) return false;
    rows.clear();
    rows.reserve(count);
    std::size_t visited = 0;
    for (const auto& item : *players) {
        if (++visited > 512) return false;
        const auto& nativeName = item.second.name;
        const auto length = nativeName.size();
        if (length == 0 || length > 128 || !readable(nativeName.data(), length)) continue;
        PlayerRow row;
        row.name = tablist::stripMinecraftFormatting(
            std::string_view(nativeName.data(), length));
        if (row.name.empty()) continue;
        if (sortByNameColor) {
            bool foundColor = false;
            bool bestWasExact = false;
            std::uint64_t bestSequence = 0;
            std::string matchedActorNameTag;
            for (const auto& actorTag : actorTags) {
                const auto color = tablist::actorNameColorForPlayerName(row.name, actorTag.nameTag);
                if (!color) continue;
                const auto rawActorName = tablist::stripMinecraftFormatting(actorTag.nameTag);
                const bool exact = rawActorName.size() == row.name.size() &&
                    std::equal(rawActorName.begin(), rawActorName.end(), row.name.begin(),
                        [](unsigned char a, unsigned char b) {
                            return std::tolower(a) == std::tolower(b);
                        });
                if (foundColor && (exact < bestWasExact ||
                    (exact == bestWasExact && actorTag.sequence <= bestSequence))) continue;
                row.nameColorOrder = *color;
                bestWasExact = exact;
                bestSequence = actorTag.sequence;
                matchedActorNameTag = actorTag.nameTag;
                foundColor = true;
            }
            if (foundColor) {
                bool firstMatch = false;
                {
                    std::lock_guard lock(actorNameTagMutex);
                    firstMatch = loggedPlayerColorMatches.insert(row.name).second;
                }
                if (firstMatch)
                    diagnostic("PlayerList color matched: player=", row.name,
                               " actor=", matchedActorNameTag,
                               " order=", row.nameColorOrder);
            }
        }
        if (includeFaces) extractFace(item.second, row);
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [sortByNameColor](const auto& left, const auto& right) {
        if (sortByNameColor && left.nameColorOrder != right.nameColorOrder)
            return left.nameColorOrder < right.nameColorOrder;
        return std::lexicographical_compare(
            left.name.begin(), left.name.end(), right.name.begin(), right.name.end(),
            [](unsigned char a, unsigned char b) {
                return std::tolower(a) < std::tolower(b);
            });
    });
    rows.erase(std::unique(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return left.name == right.name;
    }), rows.end());
    return true;
}

bool collectRows(std::vector<PlayerRow>& rows, bool includeFaces,
                 bool sortByNameColor,
                 const std::vector<ActorNameTagEntry>& actorTags) noexcept {
    // Keep SEH outside the function that owns C++ objects. A player joining or
    // leaving during the read cannot crash Minecraft; that frame is skipped.
    __try {
        return collectRowsUnsafe(rows, includeFaces, sortByNameColor, actorTags);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        rows.clear();
        return false;
    }
}

bool __fastcall onKeyboardFeed(int inputKey, bool down) {
    if (inputKey == tabInputKey || inputKey == tabInputKeyGlfw) {
        const bool previous = tabHeld.exchange(down, std::memory_order_acq_rel);
        if (previous != down) diagnostic("tab ", down ? 1 : 0);
    }
    if (auto original = originalKeyboardFeed.load(std::memory_order_acquire))
        return original(inputKey, down);
    return false;
}

void __fastcall onGrabCursor(void* self) {
    if (auto original = originalGrabCursor.load(std::memory_order_acquire)) original(self);
    if (validateClient(self)) clientInstance.store(self, std::memory_order_release);
    gameplayActive.store(true, std::memory_order_release);
}

void __fastcall onReleaseCursor(void* self) {
    if (validateClient(self)) clientInstance.store(self, std::memory_order_release);
    gameplayActive.store(false, std::memory_order_release);
    tabHeld.store(false, std::memory_order_release);
    if (auto original = originalReleaseCursor.load(std::memory_order_acquire)) original(self);
}

float __fastcall onGetFov(void* self, float inputFov, void* a3, void* a4) {
    // This callback is the reliable world-render heartbeat used by the prior
    // working build. Level freshness still prevents a menu from drawing a list.
    gameplayActive.store(true, std::memory_order_release);
    if (auto original = originalGetFov.load(std::memory_order_acquire))
        return original(self, inputFov, a3, a4);
    return inputFov;
}

void __fastcall onLevelSubTick(void* level) {
    recordLevel(level);
    if (auto original = originalLevelSubTick.load(std::memory_order_acquire)) original(level);
    const auto config = liveConfig.load(std::memory_order_acquire);
    if (!config->sortByNameColor || !tabHeld.load(std::memory_order_acquire)) return;
    const auto now = steadyMilliseconds();
    const auto last = lastActorListRefreshMs.load(std::memory_order_acquire);
    if (now - last < 250) return;
    lastActorListRefreshMs.store(now, std::memory_order_release);
    updateRuntimeActorNameTags(level);
}

void BDS_HOOK_CALL keyboardMiddleware(void*, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_KeyboardArgs*>(raw);
    const auto old = keyFrame; keyFrame = {next, frame};
    args->result = onKeyboardFeed(args->key, args->down != 0) ? 1 : 0;
    keyFrame = old;
}
void BDS_HOOK_CALL cursorMiddleware(void* user, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_CursorArgs*>(raw);
    const bool grabbing = reinterpret_cast<std::uintptr_t>(user) == 1;
    const auto oldFrame = cursorFrame; cursorFrame = {next, frame};
    const auto oldActive = activeCursorFrame; activeCursorFrame = &cursorFrame;
    if (grabbing) onGrabCursor(args->self); else onReleaseCursor(args->self);
    activeCursorFrame = oldActive; cursorFrame = oldFrame;
}
void BDS_HOOK_CALL fovMiddleware(void*, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_FovArgs*>(raw);
    const auto old = fovFrame; fovFrame = {next, frame};
    args->result = onGetFov(args->self, args->input_fov, args->a3, args->a4);
    fovFrame = old;
}
void BDS_HOOK_CALL levelMiddleware(void*, void* raw, BDS_HookNext next, void* frame) {
    auto* args = static_cast<BDS_LevelArgs*>(raw);
    const auto old = levelFrame; levelFrame = {next, frame};
    onLevelSubTick(args->level);
    levelFrame = old;
}

void drawTabList(void*) {
    if (!loggedRenderer.exchange(true, std::memory_order_acq_rel))
        diagnostic("DXGI render callback active");
    // TabList is a passive HUD. Keep this invariant here as a safeguard when
    // built against an older overlay backend that defaults to a GUI cursor.
    auto& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    const auto config = liveConfig.load(std::memory_order_acquire);
    if (!config->enabled) return;
    if (!tabHeld.load(std::memory_order_acquire)) return;
    if (!gameplayActive.load(std::memory_order_acquire)) return;
    if (!levelIsFresh() && !tryAcquireLevelFromClient()) return;

    std::vector<PlayerRow> players;
    std::vector<ActorNameTagEntry> actorTags;
    if (config->sortByNameColor) {
        std::lock_guard lock(actorNameTagMutex);
        actorTags.reserve(actorNameTags.size());
        for (const auto& [actor, entry] : actorNameTags) {
            (void)actor;
            actorTags.push_back(entry);
        }
    }
    if (!collectRows(players, config->showFaces, config->sortByNameColor, actorTags) || players.empty()) {
        if (!loggedListFailure.exchange(true, std::memory_order_acq_rel))
            diagnostic("Tab detected but native player list is unavailable");
        return;
    }
    if (!loggedListSuccess.exchange(true, std::memory_order_acq_rel))
        diagnostic("Native player list active: ", players.size(), " player(s)");
    const auto layout = tablist::layoutFor(players.size(), config->maximumRows);
    auto* draw = ImGui::GetForegroundDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    constexpr float horizontalPadding = 10.0f;
    const float faceArea = config->showFaces ? config->faceSize + 7.0f : 8.0f;
    const auto packed = [](tablist::Color color) {
        return ImGui::ColorConvertFloat4ToU32({color.r, color.g, color.b, color.a});
    };

    std::vector<float> widths(layout.columns, config->minimumColumnWidth);
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto column = i / config->maximumRows;
        widths[column] = std::max(widths[column],
            ImGui::CalcTextSize(players[i].name.c_str()).x +
            horizontalPadding * 2.0f + faceArea);
    }
    float totalWidth = 0.0f;
    for (float width : widths) totalWidth += width;
    totalWidth += config->columnGap * static_cast<float>(layout.columns - 1);
    const float totalHeight = config->headerHeight +
        config->rowHeight * static_cast<float>(layout.rows);
    const ImVec2 topLeft{(display.x - totalWidth) * 0.5f, config->topOffset};
    const ImVec2 bottomRight{topLeft.x + totalWidth, topLeft.y + totalHeight};

    draw->AddRectFilled(topLeft, bottomRight, packed(config->background), config->cornerRadius);
    draw->AddRect(topLeft, bottomRight, packed(config->border), config->cornerRadius,
                  ImDrawFlags_None, 1.2f);
    const std::string title = "Players  " + std::to_string(players.size());
    const auto titleSize = ImGui::CalcTextSize(title.c_str());
    draw->AddText({topLeft.x + (totalWidth - titleSize.x) * 0.5f,
                   topLeft.y + (config->headerHeight - titleSize.y) * 0.5f},
                  packed(config->text), title.c_str());
    auto separator = config->border;
    separator.a *= 0.55f;
    draw->AddLine({topLeft.x + 8.0f, topLeft.y + config->headerHeight},
                  {bottomRight.x - 8.0f, topLeft.y + config->headerHeight},
                  packed(separator));

    std::vector<float> columnX(layout.columns, topLeft.x);
    for (std::size_t column = 1; column < layout.columns; ++column)
        columnX[column] = columnX[column - 1] + widths[column - 1] + config->columnGap;
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto column = i / config->maximumRows;
        const auto row = i % config->maximumRows;
        const float y = topLeft.y + config->headerHeight +
            config->rowHeight * static_cast<float>(row);
        if ((row & 1U) != 0)
            draw->AddRectFilled({columnX[column], y},
                                {columnX[column] + widths[column], y + config->rowHeight},
                                IM_COL32(255, 255, 255, 10));
        float textX = columnX[column] + horizontalPadding;
        if (config->showFaces && players[i].hasFace) {
            const float faceX = textX;
            const float faceY = y + (config->rowHeight - config->faceSize) * 0.5f;
            const float pixelSize = config->faceSize / 8.0f;
            for (std::size_t py = 0; py < 8; ++py) for (std::size_t px = 0; px < 8; ++px) {
                const auto value = players[i].face[py * 8 + px];
                const ImU32 color = IM_COL32((value >> 24) & 0xFF, (value >> 16) & 0xFF,
                                             (value >> 8) & 0xFF, value & 0xFF);
                draw->AddRectFilled({faceX + px * pixelSize, faceY + py * pixelSize},
                                    {faceX + (px + 1) * pixelSize,
                                     faceY + (py + 1) * pixelSize}, color);
            }
            textX += config->faceSize + 7.0f;
        } else {
            draw->AddCircleFilled({textX + 2.5f, y + config->rowHeight * 0.5f},
                                  2.5f, packed(config->accent));
            textX += 9.0f;
        }
        const auto textSize = ImGui::CalcTextSize(players[i].name.c_str());
        draw->AddText({textX, y + (config->rowHeight - textSize.y) * 0.5f},
                      packed(config->text), players[i].name.c_str());
    }
}

std::filesystem::path configPath() {
    PWSTR documents = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documents)))
        return {};
    std::filesystem::path result{documents};
    CoTaskMemFree(documents);
    return result / L"PEXData" / L"TabList" / L"config.json";
}

bool writeDefaultConfig(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "could not create config.json";
        return false;
    }
    output << tablist::configJson(tablist::Config{});
    return static_cast<bool>(output);
}

bool loadConfig(const std::filesystem::path& path, bedrock::Logger& logger) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::ostringstream text;
    text << input.rdbuf();
    std::string error;
    auto parsed = tablist::parseConfig(text.str(), error);
    if (!error.empty()) {
        logger.error("Ignoring invalid config.json: ", error);
        return false;
    }
    liveConfig.store(std::make_shared<const tablist::Config>(parsed),
                     std::memory_order_release);
    return true;
}

void removeNativeHooks() noexcept {
    for (auto& token : hookTokens) {
        if (token) BDS_HookUnsubscribe(token);
        token = nullptr;
    }
    originalKeyboardFeed.store(nullptr);
    originalGrabCursor.store(nullptr);
    originalReleaseCursor.store(nullptr);
    originalGetFov.store(nullptr);
    originalLevelSubTick.store(nullptr);
    originalActorGetNameTag.store(nullptr, std::memory_order_release);
    runtimeActorListFunction.store(nullptr, std::memory_order_release);
    gameplayActive.store(false, std::memory_order_release);
    clientInstance.store(nullptr);
    levelInstance.store(nullptr);
    levelHeartbeatMs.store(0, std::memory_order_release);
}

DWORD WINAPI startClient(void*) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&startClient), &pinned)) {
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    status.store(Status::Loading, std::memory_order_release);
    bedrock::Logger logger{"TabList"};
    const auto path = configPath();
    std::string error;
    if (path.empty() || (!std::filesystem::exists(path) &&
                         !writeDefaultConfig(path, error)) || !loadConfig(path, logger)) {
        logger.error("Config initialization failed: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnosticLog.open(path.parent_path() / L"TabList.log",
                       std::ios::binary | std::ios::trunc);
    diagnostic("start");
    bedrock::ProfileChoice version;
    std::string profile;
    if (!bedrock::fetchSupportedProfile(profile, version, error,
            [](std::string_view stage) { diagnostic("profile ", stage); })) {
        logger.error("Unsupported Minecraft build or profile download failed: ", error);
        diagnostic("Version profile unavailable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    bedrock::Resolver resolver;
    diagnostic("resolver load");
    if (!resolver.loadJson(profile, error)) {
        diagnostic("resolver JSON failed: ", error);
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("resolver attach");
    if (!resolver.attachWindowsMainModule(version.identity, error)) {
        diagnostic("resolver attach failed: ", error);
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    if (!resolver.exactBuild()) {
        diagnostic("resolver build mismatch");
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("resolver ready");
    std::optional<bedrock::Resolution> keyboard, grab, release, fov, levelTick;
    const auto resolveRequired = [&] {
        keyboard = bedrock::resolveSharedHook(resolver, "Keyboard::feed", BDS_HOOK_KEYBOARD_FEED);
        grab = bedrock::resolveSharedHook(resolver, "ClientInstance::grabCursor", BDS_HOOK_GRAB_CURSOR);
        release = bedrock::resolveSharedHook(resolver, "ClientInstance::releaseCursor", BDS_HOOK_RELEASE_CURSOR);
        fov = bedrock::resolveSharedHook(resolver, "LevelRendererPlayer::getFov", BDS_HOOK_GET_FOV);
        levelTick = bedrock::resolveSharedHook(resolver, "MultiPlayerLevel::_subTick", BDS_HOOK_LEVEL_SUB_TICK);
    };
    const auto valid = [](const auto& symbol) {
        return symbol && bedrock::isUsableSharedHookResolution(*symbol);
    };
    std::optional<bedrock::Resolution> actorNameTag;
    std::optional<bedrock::Resolution> actorListFunction;
    const auto resolveActorNameTag = [&] {
        actorNameTag = resolver.resolve("Actor::getNameTag", true);
        if (actorNameTag &&
            (!actorNameTag->exact_build || actorNameTag->method != "exact-rva"))
            actorNameTag.reset();
        actorListFunction = resolver.resolve("Level::getRuntimeActorList", true);
    };
    resolveActorNameTag();
    const auto requirementsMet = [&] {
        return valid(keyboard) && valid(grab) && valid(release) && valid(fov) && valid(levelTick);
    };
    resolveRequired();
    if ((!requirementsMet() || !actorNameTag) &&
        version.source != bedrock::ProfileSource::Remote) {
        diagnostic("required symbols missing in cached profile; refreshing exact-build GitHub definition");
        std::string refreshedProfile;
        bedrock::ProfileChoice refreshedVersion;
        if (bedrock::refreshSupportedProfile(version.identity, refreshedProfile, refreshedVersion, error,
                [](std::string_view stage) { diagnostic("profile ", stage); })) {
            if (resolver.loadJson(refreshedProfile, error) &&
                resolver.attachWindowsMainModule(refreshedVersion.identity, error) &&
                resolver.exactBuild()) {
                profile = std::move(refreshedProfile);
                version = std::move(refreshedVersion);
                resolveRequired();
                resolveActorNameTag();
                diagnostic("latest exact-build profile loaded [",
                           bedrock::profileSourceName(version.source), "]");
            } else {
                diagnostic("refreshed profile attach failed: ", error);
            }
        } else {
            diagnostic("exact-build profile refresh failed: ", error);
        }
    }

    // A fresh runtime-profile cache can predate virtual-method metadata. The
    // bundled profile is still accepted only for this exact executable hash.
    if (!actorNameTag || !actorListFunction) {
        bedrock::Resolver embeddedResolver;
        std::string embeddedError;
        if (embeddedResolver.loadJson(bedrock::generated::kSymbolDatabase, embeddedError) &&
            embeddedResolver.attachWindowsMainModule(version.identity, embeddedError) &&
            embeddedResolver.exactBuild()) {
            if (!actorNameTag) {
                actorNameTag = embeddedResolver.resolve("Actor::getNameTag", true);
                if (actorNameTag &&
                    (!actorNameTag->exact_build || actorNameTag->method != "exact-rva"))
                    actorNameTag.reset();
            }
            if (!actorListFunction) {
                actorListFunction = embeddedResolver.resolve("Level::getRuntimeActorList", true);
            }
        } else {
            diagnostic("embedded exact-build profile unavailable: ", embeddedError);
        }
    }

    if (!requirementsMet()) {
        diagnostic("required symbols unavailable keyboard=", bool(keyboard),
                   " grab=", bool(grab), " release=", bool(release),
                   " fov=", bool(fov), " level=", bool(levelTick));
        logger.error("Required native input/render symbols are unavailable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    originalKeyboardFeed.store(&callNextKeyboard);
    originalGrabCursor.store(&callNextCursor);
    originalReleaseCursor.store(&callNextCursor);
    originalGetFov.store(&callNextFov);
    originalLevelSubTick.store(&callNextLevel);
    diagnostic("shared hooks register");
    const bool created =
        BDS_HookSubscribe(BDS_HOOK_KEYBOARD_FEED, reinterpret_cast<void*>(keyboard->address), keyboardMiddleware, nullptr, &hookTokens[0]) &&
        BDS_HookSubscribe(BDS_HOOK_GRAB_CURSOR, reinterpret_cast<void*>(grab->address), cursorMiddleware, reinterpret_cast<void*>(1), &hookTokens[1]) &&
        BDS_HookSubscribe(BDS_HOOK_RELEASE_CURSOR, reinterpret_cast<void*>(release->address), cursorMiddleware, nullptr, &hookTokens[2]) &&
        BDS_HookSubscribe(BDS_HOOK_GET_FOV, reinterpret_cast<void*>(fov->address), fovMiddleware, nullptr, &hookTokens[3]) &&
        BDS_HookSubscribe(BDS_HOOK_LEVEL_SUB_TICK, reinterpret_cast<void*>(levelTick->address), levelMiddleware, nullptr, &hookTokens[4]);
    if (!created) {
        diagnostic("shared hook registration failed");
        removeNativeHooks();
        logger.error("Could not subscribe TabList to the shared hook runtime");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    diagnostic("shared hooks registered");

    if (actorNameTag)
        originalActorGetNameTag.store(reinterpret_cast<void*>(actorNameTag->address),
                                      std::memory_order_release);
    else
        originalActorGetNameTag.store(nullptr, std::memory_order_release);
    if (actorListFunction && actorNameTag) {
        runtimeActorListFunction.store(reinterpret_cast<void*>(actorListFunction->address),
                                       std::memory_order_release);
    } else {
        runtimeActorListFunction.store(nullptr, std::memory_order_release);
    }
    diagnostic("world Actor source: runtimeActorList=",
               actorListFunction && actorNameTag ? "available" : "unavailable",
               " Actor::getNameTag=", actorNameTag ? "available" : "missing");
    if (runtimeActorListFunction.load(std::memory_order_acquire))
        diagnostic("world Actor list ready [method=", actorListFunction->method,
                   " exact_build=", actorListFunction->exact_build,
                   " verified=", actorListFunction->verified, "]");
    else
        diagnostic("world Actor list unavailable; color sorting cannot collect Actor names");

    bedrock::gui::OverlayOptions options;
    options.toggleVirtualKey = 0;
    options.initiallyVisible = true;
    options.captureInput = false;
    options.nativeCursor = false;
    diagnostic("overlay install");
    if (!bedrock::gui::Overlay::install(&drawTabList, nullptr, error, options)) {
        diagnostic("overlay install failed: ", error);
        removeNativeHooks();
        logger.error("Could not install in-game renderer: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    logger.info("Minecraft native TabList ready; hold Tab in a world. Config: ",
                path.string());
    diagnostic("hooks and overlay ready");
    diagnostic("ready ", version.version, " ", bedrock::profileSourceName(version.source));
    status.store(Status::Running, std::memory_order_release);
    auto lastWrite = std::filesystem::last_write_time(path);
    while (!stopRequested.load(std::memory_order_acquire)) {
        const auto delay = liveConfig.load(std::memory_order_acquire)->reloadIntervalMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        if (!levelIsFresh()) tryAcquireLevelFromClient();
        std::error_code ec;
        const auto currentWrite = std::filesystem::last_write_time(path, ec);
        if (!ec && currentWrite != lastWrite && loadConfig(path, logger)) {
            lastWrite = currentWrite;
            logger.info("TabList config applied");
        }
    }

    bedrock::gui::Overlay::shutdown();
    removeNativeHooks();
    status.store(Status::Stopped, std::memory_order_release);
    return 0;
}
} // namespace

extern "C" __declspec(dllexport) unsigned TabList_GetStatus() {
    return static_cast<unsigned>(status.load(std::memory_order_acquire));
}

extern "C" __declspec(dllexport) void TabList_Stop() {
    stopRequested.store(true, std::memory_order_release);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, &startClient, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
        else status.store(Status::Failed, std::memory_order_release);
    } else if (reason == DLL_PROCESS_DETACH) {
        stopRequested.store(true, std::memory_order_release);
    }
    return TRUE;
}
#endif

