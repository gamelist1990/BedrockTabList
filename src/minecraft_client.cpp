#include <bedrock/profiles/profile_json.hpp>
#ifdef _WIN32
#include <bedrock/native/local_player.hpp>
#include <bedrock/native/player_list.hpp>
#include <bedrock/native/runtime_actors.hpp>
#include <bedrock/packets/packet_api.hpp>
#include <bedrock/packets/player_roster_service.hpp>
#include <bedrock/runtime/module_identity.hpp>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <bedrock/runtime/hook_runtime.h>
#include <bedrock/render/gui_overlay.hpp>
#include <bedrock/logging/logger.hpp>
#include <bedrock/profiles/remote_catalog.hpp>
#include <bedrock/profiles/resolver.hpp>
#include <bedrock/generated/symbols.hpp>
#include <bedrock/api/game_api.hpp>
#include <bedrock/native/items.hpp>
#include <bedrock/native/actors.hpp>
#include <bedrock/native/actor_components.hpp>
#include <bedrock/enums/game_type.hpp>
#include <imgui.h>
#include <entt/entt.hpp>
#include "tablist/config.hpp"
#include "tablist/model.hpp"
#include "tablist/update_scheduler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sstream>
#include <thread>
#include <type_traits>
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
using ItemHoverNameFn = std::string*(__fastcall*)(void*, std::string*);
// Exact 1.26.52.3 disassembly writes a std::vector<Actor*> hidden return area
// through RDX, then appends one Actor* per runtime entity.


bedrock::NativePlayerList playerRoster; // Legacy builds without the common receive bridge.
bedrock::NativeRuntimeActors runtimeActors;
bedrock::NativeActorComponents actorComponents;
bedrock::PacketApi packets;
bedrock::PlayerRosterService packetPlayers;
bool packetRosterMode{};
bedrock::NativeLocalPlayerGetter localPlayerGetter;
constexpr int tabInputKey = 0x09;
constexpr int tabInputKeyGlfw = 258;

struct PlayerRow {
    std::string name;
    std::string heldItemName;
    unsigned nameColorOrder = 28;
    bool hasNameColor = false;
    tablist::MurderRole murderRole = tablist::MurderRole::None;
    bool spectator = false;
    std::array<std::uint32_t, 64> face{};
    bool hasFace = false;
};

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
BDS_HookToken actorNameTagHookToken{};
std::atomic<void*> itemHoverNameTarget{nullptr};
std::unordered_map<std::string, std::string> itemDisplayNames;
std::unordered_map<void*, std::string> nativeItemIdentifiers; // Game thread, cleared on world change.
std::chrono::steady_clock::time_point lastItemDisplayNameResolve{};
thread_local bool allowRuntimeActorListCall = false;
struct RuntimeActorListTickScope {
    bool previous{allowRuntimeActorListCall};
    RuntimeActorListTickScope() noexcept { allowRuntimeActorListCall = true; }
    ~RuntimeActorListTickScope() { allowRuntimeActorListCall = previous; }
};
struct ActorNameTagEntry {
    void* actor{};
    std::string nameTag;
    unsigned colorOrder = 28;
    std::uint64_t sequence = 0;
    std::string playerName;
};
// Keep only the latest observed color per player for concise transition logs.
// A value of 28 means the current Actor nameTag has no supported color.
std::mutex actorNameTagMutex;
std::unordered_map<std::string, unsigned> observedPlayerColorOrders;
std::unordered_map<void*, ActorNameTagEntry> actorNameTags;
std::mutex murderDetectMutex;
std::unordered_map<std::string, tablist::MurderRole> murderRolesByPlayer;
std::unordered_set<std::string> spectatorPlayers;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> murderRoleExpiryByPlayer;
std::unordered_map<std::string, std::string> lastHeldItemByPlayer;
std::unordered_map<std::string, std::string> heldItemNamesByPlayer;
struct MurderSample {
    std::string name;
    std::string identifier;
    std::string displayName;
    std::uint64_t worldEpoch{};
    unsigned count{};
    bool hasItem{};
    bool handEmpty{};
    bool hasGameType{};
    bool spectator{};
};
std::mutex murderQueueMutex;
std::condition_variable murderQueueReady;
std::deque<MurderSample> murderQueue;
std::thread murderWorker;
bool murderWorkerStopping{};
std::atomic<std::uint64_t> droppedMurderSamples{0};
std::atomic<std::uint64_t> murderWorldEpoch{0};
bool murderRoundHasRole = false; // Game thread only.
// Raw handles come exclusively from Level::getRuntimeActorList on the game thread.
// Each handle is validated incrementally before use, rather than in one sweep.
struct MurderActorEntry {
    void* actor{};
    std::uint64_t runtimeId{};
    std::uintptr_t scanKey{};
    std::string playerName;
};
std::vector<MurderActorEntry> murderScanActors;
std::vector<std::string> murderPlayerNames;
std::unordered_map<std::uint64_t, std::string> murderKnownPlayerNamesByRuntimeId;
std::unordered_map<void*, std::string> murderKnownPlayerNamesByActor;
std::size_t murderScanCursor = 0;
std::uintptr_t murderLastProcessedScanKey = 0;
void* murderScanLevel = nullptr;
struct ActorRosterBuild {
    std::vector<void*> handles;
    std::size_t cursor{};
    std::vector<MurderActorEntry> tracked;
    std::unordered_set<std::uint64_t> runtimeIds;
    std::unordered_map<std::uint64_t, std::string> namesById;
    std::unordered_map<void*, std::string> namesByActor;
};
std::optional<ActorRosterBuild> pendingActorRoster;
std::int64_t nextActorRosterMs{};
std::int64_t lastMurderRosterStatusLogMs{};
std::atomic<bool> loggedMurderActorReadFault{false};

void pruneExpiredMurderRolesLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = murderRoleExpiryByPlayer.begin(); it != murderRoleExpiryByPlayer.end();) {
        if (it->second > now) {
            ++it;
            continue;
        }
        murderRolesByPlayer.erase(it->first);
        it = murderRoleExpiryByPlayer.erase(it);
    }
}
std::size_t cachedFaceCursor = 0;
std::mutex cachedTabRowsWriteMutex;
std::shared_ptr<const std::vector<PlayerRow>> cachedTabRows{
    std::make_shared<const std::vector<PlayerRow>>()};
struct TabRowsBuild {
    bool failed{};
    std::uint64_t worldEpoch{};
    std::vector<std::string> names;
    std::vector<PlayerRow> rows;
    std::shared_ptr<const tablist::Config> config;
    std::unordered_map<std::string, tablist::MurderRole> roles;
    std::unordered_set<std::string> spectators;
    std::vector<ActorNameTagEntry> actorTags;
};
std::mutex tabRowsWorkerMutex;
std::thread tabRowsWorker;
std::condition_variable tabRowsReady;
std::shared_ptr<TabRowsBuild> pendingTabRowsBuild;
struct FaceUpdate {
    std::uint64_t worldEpoch{};
    PlayerRow row;
};
std::optional<FaceUpdate> pendingFaceUpdate;
bool tabRowsStopping{};
std::unordered_map<std::string, bedrock::PlayerUuid> playerFaceIds; // Game thread only.
std::int64_t nextFaceReadMs{};
std::int64_t tabRowsLastStartedMs{};
std::atomic<bool> loggedTabRowsReady{false};
std::atomic<std::uint64_t> actorNameTagSequence{0};
std::optional<std::uint64_t> readActorRuntimeId(void* actor) noexcept;
std::optional<bedrock::NativeItemLayout> nativeItemLayout;
std::optional<bedrock::NativeActorLayout> nativeActorLayout;
class TabListGameAccess final : public bedrock::GameAccess {
public:
    void setItemHoverName(void* target) noexcept {
        itemHoverNameTarget.store(target, std::memory_order_release);
    }
    bool isActorValid(void* actor) const noexcept override;
    void* localPlayer() const noexcept override;
    void* worldOf(void* actor) const noexcept override;
    std::vector<void*> actorHandles(void* world) const override;
    std::optional<std::uint64_t> actorRuntimeId(void* actor) const override;
    std::optional<std::string> nameTag(void* actor) const override {
        std::unique_lock lock(actorNameTagMutex, std::try_to_lock);
        if (!lock) return std::nullopt;
        const auto found = actorNameTags.find(actor);
        return found == actorNameTags.end() ? std::nullopt
            : std::optional<std::string>{found->second.nameTag};
    }
    std::optional<bedrock::ItemStack> equipmentItem(
        void* actor, bedrock::EquipmentSlot slot) const override;
    std::size_t localInventorySize(void* player) const override;
    std::optional<bedrock::ItemStack> localInventoryItem(
        void* player, std::size_t slot) const override;
    std::optional<std::size_t> selectedHotbarSlot(void* player) const override;
    std::vector<std::string> playerListNames(void* world) const override;
    void clearPlayerNameCache() const noexcept {
        std::lock_guard lock(playerNamesMutex_);
        cachedWorld_ = nullptr;
        playerNamesCacheMs_ = 0;
        playerNamesCache_.clear();
    }
    std::optional<bedrock::ServerAddress> serverAddress(void* player) const override {
        (void)player;
        return std::nullopt;
    }
private:
    mutable std::mutex playerNamesMutex_;
    mutable void* cachedWorld_{};
    mutable std::int64_t playerNamesCacheMs_{};
    mutable std::vector<std::string> playerNamesCache_;
};
TabListGameAccess tabListGameAccess;
std::unique_ptr<bedrock::GameApi> gameApi;
std::atomic<bool> loggedActorListSuccess{false};
std::shared_ptr<const tablist::Config> liveConfig{
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
// Callbacks enqueue text; only this writer thread opens/writes/flushes the file.
class DiagnosticWriter {
public:
    bool start(const std::filesystem::path& path) noexcept {
        try {
            writer_ = std::thread([this, path] {
                try {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    std::ofstream output(path, std::ios::binary | std::ios::trunc);
                    for (;;) {
                        std::deque<std::string> batch;
                        bool done;
                        {
                            std::unique_lock lock(mutex_);
                            ready_.wait_for(lock, std::chrono::milliseconds(250),
                                [this] { return stopping_; });
                            batch.swap(lines_);
                            done = stopping_;
                        }
                        for (const auto& line : batch) output << line << '\n';
                        if (!batch.empty()) output.flush();
                        if (done) return;
                    }
                } catch (...) {}
            });
            return true;
        } catch (...) { return false; }
    }
    void enqueue(std::string line) noexcept {
        try {
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (lock && lines_.size() < 512) lines_.push_back(std::move(line));
        } catch (...) {}
    }
    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        ready_.notify_one();
        if (writer_.joinable()) writer_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> lines_;
    std::thread writer_;
    bool stopping_{};
};
DiagnosticWriter diagnosticWriter;
std::atomic<bool> loggedRenderer{false};
std::atomic<bool> loggedListSuccess{false};

template <class... Args>
void diagnostic(Args&&... args) noexcept {
    try {
        std::ostringstream line;
        (line << ... << std::forward<Args>(args));
        diagnosticWriter.enqueue(std::move(line).str());
    } catch (...) {}
}

void publishHeldItemName(const std::string& playerName,
                         const std::string& displayName,
                         std::uint64_t worldEpoch) noexcept {
    try {
        std::lock_guard writeLock(cachedTabRowsWriteMutex);
        if (worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        const auto current = std::atomic_load_explicit(&cachedTabRows,
                                                       std::memory_order_acquire);
        if (!current || current->empty()) return;
        const auto found = std::find_if(current->begin(), current->end(),
            [&](const PlayerRow& row) { return row.name == playerName; });
        if (found == current->end() || found->heldItemName == displayName) return;
        auto updated = std::make_shared<std::vector<PlayerRow>>(*current);
        (*updated)[static_cast<std::size_t>(std::distance(current->begin(), found))]
            .heldItemName = displayName;
        std::atomic_store_explicit(&cachedTabRows,
            std::shared_ptr<const std::vector<PlayerRow>>(std::move(updated)),
            std::memory_order_release);
    } catch (...) {
        // Rendering remains on the last immutable row snapshot on allocation failure.
    }
}

std::string normalizeItemDisplayName(std::string_view raw,
                                     std::string_view fallback) {
    auto name = tablist::stripMinecraftFormatting(raw);
    const auto actionHint = name.find('[');
    if (actionHint != std::string::npos) name.resize(actionHint);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
        name.pop_back();
    const auto first = std::find_if_not(name.begin(), name.end(),
        [](unsigned char c) { return std::isspace(c) != 0; });
    name.erase(name.begin(), first);
    if (name.empty()) name.assign(fallback);

    // Keep complete UTF-8 code points in the narrow TabList row.
    std::size_t offset = 0;
    std::size_t codepoints = 0;
    while (offset < name.size() && codepoints < 40) {
        const auto lead = static_cast<unsigned char>(name[offset]);
        const std::size_t width = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2
            : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        offset += std::min(width, name.size() - offset);
        ++codepoints;
    }
    if (offset < name.size()) {
        name.resize(offset);
        name += "...";
    }
    return name;
}

void processMurderSample(const MurderSample& sample) noexcept {
    try {
        if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        std::string itemKey = sample.identifier.empty()
            ? sample.displayName : sample.identifier;
        if (itemKey.size() > 256) return;
        std::transform(itemKey.begin(), itemKey.end(), itemKey.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto role = sample.hasItem
            ? tablist::murderRoleForHeldItem(sample.identifier, sample.displayName)
            : tablist::MurderRole::None;

        const auto sampleConfig = std::atomic_load_explicit(&liveConfig,
                                                            std::memory_order_acquire);
        bool changed = false;
        std::string previous;
        std::string currentDisplay;
        bool clearedHeldItem = false;
        {
            std::lock_guard lock(murderDetectMutex);
            if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire))
                return;
            const auto now = std::chrono::steady_clock::now();
            const auto old = lastHeldItemByPlayer.find(sample.name);
            if (old != lastHeldItemByPlayer.end()) previous = old->second;
            // A confirmed empty slot clears stale item state; failed reads
            // leave the prior snapshot untouched.
            if (sample.handEmpty) {
                clearedHeldItem = old != lastHeldItemByPlayer.end() ||
                                  heldItemNamesByPlayer.contains(sample.name);
                lastHeldItemByPlayer.erase(sample.name);
                heldItemNamesByPlayer.erase(sample.name);
            } else if (!itemKey.empty()) {
                changed = old == lastHeldItemByPlayer.end() || old->second != itemKey;
                lastHeldItemByPlayer.insert_or_assign(sample.name, itemKey);
            }
            if (sample.hasItem && sampleConfig && sampleConfig->showHeldItems) {
                std::string display = normalizeItemDisplayName(
                    sample.displayName, sample.identifier);
                if (!display.empty()) {
                    currentDisplay = std::move(display);
                    heldItemNamesByPlayer.insert_or_assign(sample.name, currentDisplay);
                }
            }
            if (role != tablist::MurderRole::None) {
                murderRolesByPlayer.insert_or_assign(sample.name, role);
                murderRoleExpiryByPlayer.insert_or_assign(
                    sample.name, now + std::chrono::minutes(5));
            }
            if (sample.hasGameType) {
                if (sample.spectator) spectatorPlayers.insert(sample.name);
                else spectatorPlayers.erase(sample.name);
            }
        }
        if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        if (!currentDisplay.empty() || clearedHeldItem)
            publishHeldItemName(sample.name, currentDisplay, sample.worldEpoch);
        if (changed && sample.hasItem) {
            const char* roleName = role == tablist::MurderRole::Sheriff ? "Sheriff"
                : role == tablist::MurderRole::Murderer ? "Murderer" : "none";
            const bool resolvedPlayer = sample.name.rfind("unmatched@0x", 0) != 0;
            diagnostic("Item held: ", resolvedPlayer ? "player=" : "actor=",
                       sample.name,
                       " name=", currentDisplay.empty() ? itemKey : currentDisplay,
                       " id=", itemKey, " count=", sample.count,
                       " role=", roleName,
                       previous.empty() ? "" : " previous=", previous);
        }
    } catch (...) {
        // Background reporting must not terminate the worker or affect the game.
    }
}

void murderWorkerLoop() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        MurderSample sample;
        {
            std::unique_lock lock(murderQueueMutex);
            murderQueueReady.wait(lock, [] {
                return murderWorkerStopping || !murderQueue.empty();
            });
            if (murderWorkerStopping && murderQueue.empty()) return;
            sample = std::move(murderQueue.front());
            murderQueue.pop_front();
        }
        processMurderSample(sample);
        const auto dropped = droppedMurderSamples.exchange(0, std::memory_order_relaxed);
        if (dropped) diagnostic("MurderDetect worker queue dropped ", dropped,
                                " older sample(s)");
    }
}

bool startMurderWorker() noexcept {
    try {
        std::lock_guard lock(murderQueueMutex);
        if (murderWorker.joinable()) return true;
        murderWorkerStopping = false;
        murderWorker = std::thread(murderWorkerLoop);
        return true;
    } catch (...) {
        return false;
    }
}

void stopMurderWorker() noexcept {
    {
        std::lock_guard lock(murderQueueMutex);
        if (!murderWorker.joinable()) return;
        murderWorkerStopping = true;
    }
    murderQueueReady.notify_all();
    murderWorker.join();
    std::lock_guard lock(murderQueueMutex);
    murderWorkerStopping = false;
}

void enqueueMurderSample(MurderSample sample) noexcept {
    try {
        if (sample.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
        std::unique_lock lock(murderQueueMutex, std::try_to_lock);
        if (!lock || !murderWorker.joinable() || murderWorkerStopping) {
            droppedMurderSamples.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        constexpr std::size_t maxQueuedMurderSamples = 256;
        if (murderQueue.size() == maxQueuedMurderSamples) {
            murderQueue.pop_front();
            droppedMurderSamples.fetch_add(1, std::memory_order_relaxed);
        }
        murderQueue.push_back(std::move(sample));
        lock.unlock();
        murderQueueReady.notify_one();
    } catch (...) {
        // Never run role processing, row copies or logging inline on queue failure.
        droppedMurderSamples.fetch_add(1, std::memory_order_relaxed);
    }
}

struct ReadRegion { std::uintptr_t begin{},end{}; };
thread_local std::array<ReadRegion,32> nativeReadRegions{};
thread_local std::size_t nativeReadRegionCursor{};
thread_local bool nativeReadRegionsActive=false;
struct NativeReadRegionScope {
    bool previous=nativeReadRegionsActive;
    NativeReadRegionScope() noexcept {
        if(!previous) { nativeReadRegions.fill({}); nativeReadRegionCursor=0; }
        nativeReadRegionsActive=true;
    }
    ~NativeReadRegionScope() { nativeReadRegionsActive=previous; }
};

bool readable(const void* pointer, std::size_t bytes) noexcept {
    if (!pointer || !bytes) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    if (start > UINTPTR_MAX - bytes) return false;
    const auto wantedEnd = start + bytes;
    if(nativeReadRegionsActive) for(const auto& region:nativeReadRegions)
        if(start>=region.begin && wantedEnd<=region.end) return true;
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
        if(nativeReadRegionsActive)
            nativeReadRegions[nativeReadRegionCursor++%nativeReadRegions.size()]={regionStart,regionEnd};
        cursor = regionEnd;
    }
    return true;
}

bool executable(const void* pointer) noexcept;

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

bool safeCopyBytes(const char* source, char* destination, std::size_t size) noexcept {
    if (!source || !destination || !size || !readable(source, size)) return false;
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool copyNativeString(const void* object, std::string& output,
                      const bedrock::NativeItemLayout& layout,
                      std::size_t maximum) noexcept {
    if (!object) return false;
    std::uintptr_t data = 0;
    std::size_t size = 0;
    std::size_t capacity = 0;
    if (!safeRead(object, 0, data) ||
        !safeRead(object, layout.itemStringSizeOffset, size) ||
        !safeRead(object, layout.itemStringCapacityOffset, capacity) ||
        size > maximum || capacity < size)
        return false;
    if (size == 0) { output.clear(); return true; }
    const auto* bytes = capacity <= 15
        ? static_cast<const char*>(object)
        : reinterpret_cast<const char*>(data);
    if (!readable(bytes, size)) return false;
    try {
        std::string result(size, '\0');
        if (!safeCopyBytes(bytes, result.data(), size)) return false;
        output = std::move(result);
        return true;
    } catch (...) {
        return false;
    }
}

bool tryCallInventoryGetItem(void* inventory, std::size_t slot,
                             const bedrock::NativeItemLayout& layout,
                             void** output) noexcept {
    if (output) *output = nullptr;
    if (!inventory || !output || slot >= layout.localInventorySlots) return false;
    void** vtable = nullptr;
    if (!safeRead(inventory, 0, vtable) || !vtable) return false;
    static std::atomic<void**> cachedVtable{nullptr};
    static std::atomic<void*> cachedTarget{nullptr};
    void* target = nullptr;
    if (cachedVtable.load(std::memory_order_acquire) == vtable) {
        target = cachedTarget.load(std::memory_order_acquire);
    } else {
        if (!readable(vtable, sizeof(void*) * (layout.inventoryGetItemVtableSlot + 1)) ||
            !safeRead(vtable, sizeof(void*) * layout.inventoryGetItemVtableSlot, target) ||
            !executable(target)) return false;
        cachedTarget.store(target, std::memory_order_release);
        cachedVtable.store(vtable, std::memory_order_release);
    }
    if (!target) return false;
    using GetItemFn = void*(__fastcall*)(void*, int);
    __try {
        *output = reinterpret_cast<GetItemFn>(target)(inventory, static_cast<int>(slot));
        // A successfully invoked getter returning nullptr is a confirmed empty slot.
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

void* callInventoryGetItem(void* inventory, std::size_t slot,
                           const bedrock::NativeItemLayout& layout) noexcept {
    void* stack = nullptr;
    return tryCallInventoryGetItem(inventory, slot, layout, &stack) ? stack : nullptr;
}

bool validateClient(void* client) noexcept {
    std::uintptr_t vtable = 0;
    return safeRead(client, 0, vtable) && vtable != 0 && readable(
        reinterpret_cast<const void*>(vtable), sizeof(void*));
}


std::int64_t steadyMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool levelIsFresh() noexcept {
    const auto heartbeat = levelHeartbeatMs.load(std::memory_order_acquire);
    return heartbeat > 0 && steadyMilliseconds() - heartbeat <= 1000;
}

void recordLevel(void* level, bool lobbyReturn = false) noexcept {
    if (!level) return;
    static tablist::WorldIdentityTracker identity;
    auto* player = tabListGameAccess.localPlayer();
    const bool changed = identity.observe(reinterpret_cast<std::uintptr_t>(level),
        reinterpret_cast<std::uintptr_t>(player), readActorRuntimeId(player));
    levelInstance.store(level, std::memory_order_release);
    levelHeartbeatMs.store(steadyMilliseconds(), std::memory_order_release);
    if (changed || lobbyReturn) {
        murderWorldEpoch.fetch_add(1, std::memory_order_acq_rel);
        murderRoundHasRole = false;
        tabListGameAccess.clearPlayerNameCache();
        tabRowsLastStartedMs = 0;
        {
            std::lock_guard rowsLock(tabRowsWorkerMutex);
            pendingTabRowsBuild.reset();
            pendingFaceUpdate.reset();
        }
        {
            std::lock_guard queueLock(murderQueueMutex);
            murderQueue.clear();
        }
        {
            std::lock_guard detectLock(murderDetectMutex);
            murderRolesByPlayer.clear();
            spectatorPlayers.clear();
            murderRoleExpiryByPlayer.clear();
            lastHeldItemByPlayer.clear();
            heldItemNamesByPlayer.clear();
        }
        playerFaceIds.clear();
        nativeItemIdentifiers.clear();
        murderScanActors.clear();
        murderPlayerNames.clear();
        murderKnownPlayerNamesByRuntimeId.clear();
        murderKnownPlayerNamesByActor.clear();
        pendingActorRoster.reset();
        murderScanLevel = nullptr;
        murderScanCursor = 0;
        murderLastProcessedScanKey = 0;
        nextActorRosterMs = 0;
        nextFaceReadMs = 0;
        cachedFaceCursor = 0;
        {
            std::lock_guard rowsLock(cachedTabRowsWriteMutex);
            std::atomic_store_explicit(&cachedTabRows,
                std::shared_ptr<const std::vector<PlayerRow>>{}, std::memory_order_release);
        }
        loggedTabRowsReady.store(false, std::memory_order_release);
        {
            std::lock_guard lock(actorNameTagMutex);
            observedPlayerColorOrders.clear();
            actorNameTags.clear();
        }
        loggedMurderActorReadFault.store(false, std::memory_order_release);
        droppedMurderSamples.store(0, std::memory_order_release);
        diagnostic("level ready [", lobbyReturn ? "returned to lobby" : "world identity changed",
                   "; roles and item caches reset]");
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

bool looksLikeActor(void* actor) noexcept {
    if (!actor || !readable(actor, sizeof(void*))) return false;
    void** vtable = nullptr;
    if (!safeRead(actor, 0, vtable)) return false;
    if (!vtable || !readable(vtable, sizeof(void*) * 3)) return false;
    unsigned executableSlots = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        void* entry = nullptr;
        std::memcpy(&entry, vtable + i, sizeof(entry));
        if (executable(entry)) ++executableSlots;
    }
    return executableSlots >= 2;
}

bool TabListGameAccess::isActorValid(void* actor) const noexcept {
    return looksLikeActor(actor);
}

std::vector<void*> TabListGameAccess::actorHandles(void* world) const {
    std::vector<void*> result;
    if (!allowRuntimeActorListCall || !world ||
        world != levelInstance.load(std::memory_order_acquire)) return result;
    auto captured=runtimeActors.snapshot(world);
    if(!captured)return result;
    if(!loggedActorListSuccess.exchange(true,std::memory_order_acq_rel))
        diagnostic("SDK Level::getRuntimeActorList active: native=",captured->size()," [owning tick; incremental validation]");
    return std::move(*captured);
}

bool invokeItemHoverName(void* stack, void* target, std::string* output) noexcept {
    if (!stack || !target || !output || !executable(target)) return false;
    __try {
        return reinterpret_cast<ItemHoverNameFn>(target)(stack, output) == output;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void populateItemDisplayName(void* stack, bedrock::ItemStack& item) {
    if (item.identifier.empty()) return;
    {
        const auto it = itemDisplayNames.find(item.identifier);
        if (it != itemDisplayNames.end()) {
            item.displayName = it->second;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        constexpr auto minimumResolveInterval = std::chrono::milliseconds(500);
        if (lastItemDisplayNameResolve != std::chrono::steady_clock::time_point{} &&
            now - lastItemDisplayNameResolve < minimumResolveInterval) return;
        lastItemDisplayNameResolve = now;
    }
    std::string displayName;
    auto* target = itemHoverNameTarget.load(std::memory_order_acquire);
    if (!invokeItemHoverName(stack, target, &displayName) ||
        displayName.empty() || displayName.size() > 256) return;
    item.displayName = displayName;
    try {
        if (itemDisplayNames.size() >= 512) itemDisplayNames.clear();
        itemDisplayNames.insert_or_assign(item.identifier, std::move(displayName));
    } catch (...) {
        // Display-name caching is optional; preserve the copied item snapshot.
    }
}

bool readActorEquipment(void* actor,const bedrock::NativeItemLayout& layout,void** output) noexcept {
    if(!output)return false;*output=actorComponents.component(actor,layout.equipmentComponentHash);return *output!=nullptr;
}
bool readActorRuntimeIdValue(void* actor,std::uint64_t* output) noexcept {
    if(!output)return false;const auto value=actorComponents.runtimeId(actor);*output=value.value_or(0);return value.has_value();
}
std::optional<std::uint64_t> readActorRuntimeId(void* actor) noexcept {
    std::uint64_t runtimeId = 0;
    return readActorRuntimeIdValue(actor, &runtimeId)
        ? std::optional<std::uint64_t>{runtimeId} : std::nullopt;
}

std::optional<std::uint64_t> TabListGameAccess::actorRuntimeId(void* actor) const {
    return readActorRuntimeId(actor);
}

bool readActorGameType(void* actor,std::int32_t& gameType) noexcept {
    const auto value=actorComponents.gameType(actor);if(!value)return false;gameType=*value;return true;
}

void* TabListGameAccess::localPlayer() const noexcept {
    if (!nativeItemLayout) return nullptr;
    void* client = clientInstance.load(std::memory_order_acquire);
    void* player=localPlayerGetter.get(client);
    return looksLikeActor(player)?player:nullptr;
}

enum class EquipmentReadStatus { Failed, Empty, Item };

EquipmentReadStatus readEquipmentItem(void* actor, bedrock::EquipmentSlot slot,
                                     bedrock::ItemStack* output) {
    if (output) *output = {};
    if (!nativeItemLayout || !actor || !output) return EquipmentReadStatus::Failed;
    const auto& layout = *nativeItemLayout;
    void* component = nullptr;
    if (!readActorEquipment(actor, layout, &component) || !component)
        return EquipmentReadStatus::Failed;
    std::size_t slotIndex = 0;
    std::size_t containerOffset = layout.handContainerOffset;
    switch (slot) {
    case bedrock::EquipmentSlot::MainHand: slotIndex = layout.handSlots[0]; break;
    case bedrock::EquipmentSlot::Offhand: slotIndex = layout.handSlots[1]; break;
    case bedrock::EquipmentSlot::Head:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[0]; break;
    case bedrock::EquipmentSlot::Chest:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[1]; break;
    case bedrock::EquipmentSlot::Legs:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[2]; break;
    case bedrock::EquipmentSlot::Feet:
        containerOffset = layout.armorContainerOffset; slotIndex = layout.armorSlots[3]; break;
    }
    void* container = nullptr;
    if (!safeRead(component, containerOffset, container) || !container)
        return EquipmentReadStatus::Failed;
    void* stack = nullptr;
    if (!tryCallInventoryGetItem(container, slotIndex, layout, &stack))
        return EquipmentReadStatus::Failed;
    if (!stack) return EquipmentReadStatus::Empty;
    bedrock::ItemStack item;
    std::uint8_t count = 0;
    void** itemReference = nullptr;
    void* nativeItem = nullptr;
    if (!safeRead(stack, layout.itemCountOffset, count))
        return EquipmentReadStatus::Failed;
    if (count == 0) return EquipmentReadStatus::Empty;
    if (!safeRead(stack, layout.itemReferenceOffset, itemReference) || !itemReference ||
        !safeRead(itemReference, 0, nativeItem) || !nativeItem)
        return EquipmentReadStatus::Failed;
    item.count = count;
    const auto cachedId = nativeItemIdentifiers.find(nativeItem);
    if (cachedId != nativeItemIdentifiers.end()) {
        item.identifier = cachedId->second;
    } else {
        const auto* id = static_cast<const std::byte*>(nativeItem) + layout.itemNamespacedIdOffset;
        if (!copyNativeString(id, item.identifier, layout, layout.maxStringBytes))
            return EquipmentReadStatus::Failed;
        if (!item.identifier.empty() && item.identifier.find(':') == std::string::npos)
            item.identifier.insert(0, "minecraft:");
        if (!item.identifier.empty() && nativeItemIdentifiers.size() < 2048)
            nativeItemIdentifiers.emplace(nativeItem, item.identifier);
    }
    // The display name is cached by item identifier, so the native resolver
    // runs only for the first observation of each item type.
    const auto itemConfig = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (itemConfig && itemConfig->showHeldItems) populateItemDisplayName(stack, item);
    if (!item.valid()) return EquipmentReadStatus::Failed;
    *output = std::move(item);
    return EquipmentReadStatus::Item;
}

std::optional<bedrock::ItemStack> TabListGameAccess::equipmentItem(
    void* actor, bedrock::EquipmentSlot slot) const {
    bedrock::ItemStack item;
    return readEquipmentItem(actor, slot, &item) == EquipmentReadStatus::Item
        ? std::optional<bedrock::ItemStack>{std::move(item)} : std::nullopt;
}

std::size_t TabListGameAccess::localInventorySize(void* player) const {
    return nativeItemLayout && player && player == localPlayer()
        ? nativeItemLayout->localInventorySlots : 0;
}

std::optional<bedrock::ItemStack> TabListGameAccess::localInventoryItem(
    void* player, std::size_t slot) const {
    if (!nativeItemLayout || !player || player != localPlayer() ||
        slot >= nativeItemLayout->localInventorySlots) return std::nullopt;
    const auto& layout = *nativeItemLayout;
    void* supplies = nullptr;
    void* inventory = nullptr;
    if (!safeRead(player, layout.localPlayerSuppliesOffset, supplies) || !supplies ||
        !safeRead(supplies, layout.suppliesInventoryOffset, inventory) || !inventory)
        return std::nullopt;
    void* stack = callInventoryGetItem(inventory, slot, layout);
    if (!stack) return std::nullopt;
    bedrock::ItemStack item;
    std::uint8_t count = 0;
    void** itemReference = nullptr;
    void* nativeItem = nullptr;
    if (!safeRead(stack, layout.itemCountOffset, count) || count == 0 ||
        !safeRead(stack, layout.itemReferenceOffset, itemReference) || !itemReference ||
        !safeRead(itemReference, 0, nativeItem) || !nativeItem) return std::nullopt;
    item.count = count;
    const auto* id = static_cast<const std::byte*>(nativeItem) + layout.itemNamespacedIdOffset;
    const auto* translation = static_cast<const std::byte*>(nativeItem) +
                              layout.itemTranslationKeyOffset;
    if (!copyNativeString(id, item.identifier, layout, layout.maxStringBytes))
        return std::nullopt;
    (void)copyNativeString(translation, item.translationKey, layout, layout.maxStringBytes);
    if (!item.identifier.empty() && item.identifier.find(':') == std::string::npos)
        item.identifier.insert(0, "minecraft:");
    return item.valid() ? std::optional<bedrock::ItemStack>{std::move(item)} : std::nullopt;
}

std::optional<std::size_t> TabListGameAccess::selectedHotbarSlot(void* player) const {
    if (!nativeItemLayout || !player || player != localPlayer()) return std::nullopt;
    void* supplies = nullptr;
    std::uint32_t selected = 0;
    if (!safeRead(player, nativeItemLayout->localPlayerSuppliesOffset, supplies) || !supplies ||
        !safeRead(supplies, nativeItemLayout->suppliesSelectedSlotOffset, selected) || selected > 8)
        return std::nullopt;
    return static_cast<std::size_t>(selected);
}

void* TabListGameAccess::worldOf(void* actor) const noexcept {
    return isActorValid(actor) ? levelInstance.load(std::memory_order_acquire) : nullptr;
}

std::vector<std::string> TabListGameAccess::playerListNames(void* world) const {
    std::vector<std::string> names;
    if (!world || world != levelInstance.load(std::memory_order_acquire)) return names;
    const auto now = steadyMilliseconds();
    {
        std::lock_guard lock(playerNamesMutex_);
        if (cachedWorld_ == world && now - playerNamesCacheMs_ < 50)
            return playerNamesCache_;
    }
    if(packetRosterMode){
        const auto players=packetPlayers.snapshotShared();names.reserve(players->size());
        for(const auto& player:*players)names.push_back(player.name);
    }else{
    const auto players=playerRoster.snapshot(world);if(!players)return names;
    names.reserve(players->size());playerFaceIds.clear();
    for(const auto& player:*players){names.push_back(player.name);
        playerFaceIds.insert_or_assign(tablist::stripMinecraftFormatting(player.name),player.uuid);}
    }
    {
        std::lock_guard lock(playerNamesMutex_);
        cachedWorld_ = world;
        playerNamesCacheMs_ = now;
        playerNamesCache_ = names;
    }
    return names;
}

void captureQueriedActorNameTag(void* actor, const std::string& nameTag) noexcept {
    if (!actor || nameTag.size() > 512) return;
    try {
        // Called by the renderer's native getter: never wait for a snapshot copy.
        std::unique_lock lock(actorNameTagMutex, std::try_to_lock);
        if (!lock) return;
        const auto existing = actorNameTags.find(actor);
        if (existing != actorNameTags.end() && existing->second.nameTag == nameTag) return;
        if (existing == actorNameTags.end() && actorNameTags.size() >= 4096)
            actorNameTags.erase(actorNameTags.begin());
        const auto sequence = actorNameTagSequence.fetch_add(1, std::memory_order_relaxed) + 1;
        actorNameTags.insert_or_assign(actor,
            ActorNameTagEntry{actor, nameTag, 28, sequence});
    } catch (...) {}
}

void BDS_HOOK_CALL actorNameTagMiddleware(void*, void* raw,
                                        BDS_HookNext next, void* frame) noexcept {
    next(frame, raw);
    const auto& args = *static_cast<BDS_ActorNameTagArgs*>(raw);
    if (args.actor && args.name_tag)
        captureQueriedActorNameTag(args.actor,
            *static_cast<const std::string*>(args.name_tag));
}

// Only copied names cross to the worker. Handles are opaque comparison keys there.
std::vector<ActorNameTagEntry> snapshotActorNameTags() {
    std::vector<ActorNameTagEntry> result;
    std::lock_guard lock(actorNameTagMutex);
    result.reserve(actorNameTags.size());
    for (const auto& [_, tag] : actorNameTags) result.push_back(tag);
    return result;
}

std::string matchPlayerNameFromTag(
    std::string_view nameTag, const std::vector<std::string>& playerNames) {
    const auto plainTag = tablist::stripMinecraftFormatting(nameTag);
    for (const auto& playerName : playerNames) {
        if (playerName.size() > plainTag.size()) continue;
        for (std::size_t offset = 0; offset + playerName.size() <= plainTag.size(); ++offset) {
            bool matches = true;
            for (std::size_t i = 0; i < playerName.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(plainTag[offset + i])) !=
                    std::tolower(static_cast<unsigned char>(playerName[i]))) {
                    matches = false;
                    break;
                }
            }
            if (matches) return playerName;
        }
    }
    return {};
}

bool collectRowsUnsafe(std::vector<PlayerRow>& rows,
                       const std::vector<std::string>& playerNames,
                       std::size_t begin, std::size_t end,
                       bool sortByNameColor, bool gamemodeCheck,
                       bool murderDetect,
                       bool debug,
                       const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
                       const std::unordered_set<std::string>& spectators,
                       const std::vector<ActorNameTagEntry>& actorTags) {
    if (begin > end || end > playerNames.size()) return false;
    for (std::size_t index = begin; index < end; ++index) {
        const auto& nativeName = playerNames[index];
        const auto length = nativeName.size();
        if (length == 0 || length > 128) continue;
        PlayerRow row;
        row.name = tablist::stripMinecraftFormatting(nativeName);
        if (row.name.empty()) continue;
        if (murderDetect) {
            const auto role = murderRoles.find(row.name);
            if (role != murderRoles.end()) row.murderRole = role->second;
        }
        row.spectator = gamemodeCheck && spectators.contains(row.name);
        if (sortByNameColor) {
            bool foundColor = false;
            bool bestWasExact = false;
            std::uint64_t bestSequence = 0;
            for (const auto& actorTag : actorTags) {
                const auto color = tablist::actorNameColorForPlayerName(row.name, actorTag.nameTag);
                if (!color) continue;
                const bool exact = true;
                if (foundColor && actorTag.sequence <= bestSequence) continue;
                row.nameColorOrder = *color;
                row.hasNameColor = true;
                bestWasExact = exact;
                bestSequence = actorTag.sequence;
                foundColor = true;
            }
            // Record every rendered row, including loss of a color. This makes
            // subsequent Actor::getNameTag changes visible without reopening
            // the TabList, while avoiding per-frame/per-Actor log spam.
            bool colorChanged = false;
            unsigned oldOrder = 28;
            unsigned newOrder = foundColor ? row.nameColorOrder : 28u;
            {
                std::lock_guard lock(actorNameTagMutex);
                const auto it = observedPlayerColorOrders.find(row.name);
                if (it == observedPlayerColorOrders.end()) {
                    observedPlayerColorOrders.emplace(row.name, newOrder);
                    colorChanged = foundColor;
                } else if (it->second != newOrder) {
                    oldOrder = it->second;
                    it->second = newOrder;
                    colorChanged = true;
                }
            }
            if (colorChanged && debug) {
                const auto colorLabel = [](unsigned order) {
                    constexpr std::string_view codes = "0123456789abcdefghijmnpqstuv";
                    if (order == 10) return 'p'; // target server's §p green
                    return order < codes.size() ? codes[order] : '-';
                };
                const auto colorName = [](unsigned order) -> std::string_view {
                    static constexpr std::array<std::string_view, 28> names{
                        "black", "dark_blue", "dark_green", "dark_aqua",
                        "dark_red", "dark_purple", "orange", "gray", "dark_gray",
                        "blue", "green", "aqua", "red", "light_purple", "yellow",
                        "white", "minecoin_gold", "material_quartz", "server_purple",
                        "material_netherite", "material_redstone", "material_copper",
                        "material_gold", "material_emerald", "material_diamond",
                        "material_lapis", "material_amethyst", "material_resin"};
                    return order < names.size() ? names[order] : "none";
                };
                diagnostic("PlayerList name color updated: player=", row.name,
                           " ", colorLabel(oldOrder), "->", colorLabel(newOrder),
                           " (", colorName(newOrder), "; match=",
                           bestWasExact ? "exact" : "none", ")");
            }
        }
        rows.push_back(std::move(row));
    }
    return true;
}

bool copyPlayerNamesUnsafe(std::vector<std::string>& names) {
    void* level = levelInstance.load(std::memory_order_acquire);
    if (!level || !gameApi) return false;
    names = tabListGameAccess.playerListNames(level);
    return names.size() <= 512;
}

bool copyPlayerNames(std::vector<std::string>* names) noexcept {
    if (!names) return false;
    __try {
        return copyPlayerNamesUnsafe(*names);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        names->clear();
        return false;
    }
}

bool collectRowsChunk(std::vector<PlayerRow>& rows,
                 const std::vector<std::string>& playerNames,
                 std::size_t begin, std::size_t end,
                 bool sortByNameColor, bool gamemodeCheck,
                 bool murderDetect, bool debug,
                 const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
                 const std::unordered_set<std::string>& spectators,
                 const std::vector<ActorNameTagEntry>& actorTags) noexcept {
    // Keep SEH outside the function that owns C++ objects. A player joining or
    // leaving during the read cannot crash Minecraft; that frame is skipped.
    __try {
        return collectRowsUnsafe(rows, playerNames, begin, end, sortByNameColor, gamemodeCheck, murderDetect,
                                 debug, murderRoles, spectators, actorTags);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        rows.clear();
        return false;
    }
}

void buildTabRowsInBackground(const std::shared_ptr<TabRowsBuild>& build) noexcept {
    try {
        if (build->config->sortByNameColor) build->actorTags = snapshotActorNameTags();
        {
            std::lock_guard lock(murderDetectMutex);
            pruneExpiredMurderRolesLocked(std::chrono::steady_clock::now());
            build->roles = murderRolesByPlayer;
            build->spectators = spectatorPlayers;
        }
        constexpr std::size_t batchSize = 8;
        build->rows.reserve(build->names.size());
        for (std::size_t begin = 0; begin < build->names.size(); begin += batchSize) {
            const auto end = std::min(begin + batchSize, build->names.size());
            if (!collectRowsChunk(build->rows, build->names, begin, end,
                    build->config->sortByNameColor, build->config->gamemodeCheck,
                    build->config->murderDetect, build->config->debug,
                    build->roles, build->spectators, build->actorTags)) {
                build->failed = true;
                break;
            }
            // Let other client work run between small batches.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!build->failed) {
            const auto config = build->config;
            std::stable_sort(build->rows.begin(), build->rows.end(),
                [config](const auto& left, const auto& right) {
                    if (config->sortByNameColor &&
                        left.nameColorOrder != right.nameColorOrder)
                        return left.nameColorOrder < right.nameColorOrder;
                    return std::lexicographical_compare(
                        left.name.begin(), left.name.end(),
                        right.name.begin(), right.name.end(),
                        [](unsigned char a, unsigned char b) {
                            return std::tolower(a) < std::tolower(b);
                        });
                });
            build->rows.erase(std::unique(build->rows.begin(), build->rows.end(),
                [](const auto& left, const auto& right) {
                    return left.name == right.name;
                }), build->rows.end());
        }
    } catch (...) {
        build->failed = true;
    }
    if (build->failed) return;
    try {
        const auto config = build->config;
        std::lock_guard writeLock(cachedTabRowsWriteMutex);
        if (build->worldEpoch != murderWorldEpoch.load(std::memory_order_acquire) ||
            config != std::atomic_load_explicit(&liveConfig, std::memory_order_acquire)) return;
        {
            std::lock_guard stateLock(murderDetectMutex);
            for (auto& row : build->rows) {
                if (config->murderDetect) {
                    const auto role = murderRolesByPlayer.find(row.name);
                    if (role != murderRolesByPlayer.end()) row.murderRole = role->second;
                }
                row.spectator = config->gamemodeCheck && spectatorPlayers.contains(row.name);
                if (config->showHeldItems && config->murderDetect) {
                    const auto item = heldItemNamesByPlayer.find(row.name);
                    if (item != heldItemNamesByPlayer.end()) row.heldItemName = item->second;
                }
            }
        }
        if(packetRosterMode&&config->showFaces){
            const auto players=packetPlayers.snapshotShared();
            for(auto& row:build->rows)for(const auto& player:*players)
                if(tablist::stripMinecraftFormatting(player.name)==row.name){row.face=player.face;row.hasFace=player.hasFace;break;}
        }
        const auto previous = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
        std::unordered_map<std::string_view, const PlayerRow*> oldRows;
        if (config->showFaces && previous && !packetRosterMode) {
            oldRows.reserve(previous->size());
            for (const auto& row : *previous) if (row.hasFace) oldRows.emplace(row.name, &row);
            for (auto& row : build->rows) {
                const auto old = oldRows.find(row.name);
                if (old != oldRows.end()) {
                    row.face = old->second->face;
                    row.hasFace = true;
                }
            }
        }
        const auto publishedCount=build->rows.size();
        std::atomic_store_explicit(&cachedTabRows,
            std::make_shared<const std::vector<PlayerRow>>(std::move(build->rows)),
            std::memory_order_release);
        if (!loggedTabRowsReady.exchange(true, std::memory_order_acq_rel))
            diagnostic("TabList row cache ready; rows=",publishedCount," source_names=",build->names.size()," [copied worker rows]");
    } catch (...) {}
}

void publishFaceUpdate(const FaceUpdate& update) {
    std::lock_guard lock(cachedTabRowsWriteMutex);
    if (update.worldEpoch != murderWorldEpoch.load(std::memory_order_acquire)) return;
    const auto previous = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
    if (!previous) return;
    const auto found = std::find_if(previous->begin(), previous->end(),
        [&](const PlayerRow& row) { return row.name == update.row.name; });
    if (found == previous->end() || found->hasFace) return;
    auto rows = std::make_shared<std::vector<PlayerRow>>(*previous);
    auto& row = (*rows)[static_cast<std::size_t>(found - previous->begin())];
    row.face = update.row.face;
    row.hasFace = true;
    std::atomic_store_explicit(&cachedTabRows,
        std::shared_ptr<const std::vector<PlayerRow>>(std::move(rows)), std::memory_order_release);
}

void tabRowsWorkerLoop() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::shared_ptr<TabRowsBuild> build;
        std::optional<FaceUpdate> face;
        {
            std::unique_lock lock(tabRowsWorkerMutex);
            tabRowsReady.wait(lock, [] {
                return tabRowsStopping || pendingTabRowsBuild || pendingFaceUpdate;
            });
            if (tabRowsStopping) return;
            build = std::move(pendingTabRowsBuild);
            face = std::move(pendingFaceUpdate);
            pendingFaceUpdate.reset();
        }
        if (build) buildTabRowsInBackground(build);
        try { if (face) publishFaceUpdate(*face); } catch (...) {}
    }
}

bool startTabRowsWorker() noexcept {
    try {
        tabRowsWorker = std::thread(tabRowsWorkerLoop);
        return true;
    } catch (...) { return false; }
}

void stopTabRowsWorker() noexcept {
    {
        std::lock_guard lock(tabRowsWorkerMutex);
        tabRowsStopping = true;
        pendingTabRowsBuild.reset();
        pendingFaceUpdate.reset();
    }
    tabRowsReady.notify_one();
    if (tabRowsWorker.joinable()) tabRowsWorker.join();
}

bool fillOneFaceUnsafe(void* level, PlayerRow* row) {
    if(!level||!row||row->name.empty())return false;
    if(packetRosterMode){const auto players=packetPlayers.snapshotShared();for(const auto& player:*players)
        if(tablist::stripMinecraftFormatting(player.name)==row->name){row->face=player.face;row->hasFace=player.hasFace;return row->hasFace;}
        return false;}
    const auto uuid=playerFaceIds.find(row->name);if(uuid==playerFaceIds.end())return false;
    row->hasFace=playerRoster.face(level,uuid->second,row->face);return row->hasFace;
}

bool fillOneFaceSeh(void* level, PlayerRow* row) noexcept {
    __try {
        return fillOneFaceUnsafe(level, row);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void refreshOneCachedFace(const std::shared_ptr<const tablist::Config>& config) noexcept {
    if (!config || !config->showFaces) return;
    nextFaceReadMs = steadyMilliseconds() + 200;
    try {
        const auto current = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
        if (!current || current->empty()) return;
        // Retain progress when unrelated item/row publications replace the snapshot.
        for (std::size_t scanned = 0; scanned < current->size(); ++scanned) {
            const auto index = cachedFaceCursor++ % current->size();
            if ((*current)[index].hasFace) continue;
            FaceUpdate update;
            update.worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
            update.row.name = (*current)[index].name;
            if (!fillOneFaceSeh(levelInstance.load(std::memory_order_acquire), &update.row)) return;
            std::unique_lock lock(tabRowsWorkerMutex, std::try_to_lock);
            if (!lock || tabRowsStopping) return;
            pendingFaceUpdate = std::move(update);
            lock.unlock();
            tabRowsReady.notify_one();
            return;
        }
    } catch (...) {}
}

void refreshTabRowsCache(const std::shared_ptr<const tablist::Config>& config) noexcept {
    if (!config || !config->enabled) return;
    try {
        auto next = std::make_shared<TabRowsBuild>();
        next->worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
        next->config = config;
        if (!copyPlayerNames(&next->names)) return;
        std::unique_lock lock(tabRowsWorkerMutex, std::try_to_lock);
        if (!lock || tabRowsStopping) return;
        // Only the latest captured input matters. No game callback starts or joins a thread.
        // Match Actor names against the same capture used to render the PlayerList.
        murderPlayerNames = next->names;
        for (auto& name : murderPlayerNames) name = tablist::stripMinecraftFormatting(name);
        std::sort(murderPlayerNames.begin(), murderPlayerNames.end(),
            [](const auto& a, const auto& b) {
                return a.size() != b.size() ? a.size() > b.size() : a < b;
            });
        murderPlayerNames.erase(std::remove(murderPlayerNames.begin(), murderPlayerNames.end(), ""),
                                murderPlayerNames.end());
        murderPlayerNames.erase(std::unique(murderPlayerNames.begin(), murderPlayerNames.end()),
                                murderPlayerNames.end());
        if (murderPlayerNames.empty()) {
            murderScanActors.clear();
            pendingActorRoster.reset();
            murderKnownPlayerNamesByRuntimeId.clear();
            murderKnownPlayerNamesByActor.clear();
            nextActorRosterMs = 0;
        }
        pendingTabRowsBuild = std::move(next);
        tabRowsLastStartedMs = steadyMilliseconds();
        lock.unlock();
        tabRowsReady.notify_one();
    } catch (...) {}
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

struct ActorMurderSnapshot {
    bool isPlayer = false;
    std::string name;
    std::optional<bedrock::HeldItemInfo> item;
    bool handEmpty = false;
    bool hasGameType = false;
    bool spectator = false;
};

void readActorMurderSnapshot(void* actor,
                             std::string_view playerName,
                             ActorMurderSnapshot& snapshot,
                             bool readItem, bool readGameMode) {
    if (!actor) return;
    if (playerName.empty() || playerName.size() > 128) return;
    snapshot.name.assign(playerName);
    snapshot.isPlayer = true;
    if (readItem) {
        bedrock::ItemStack heldItem;
        const auto handStatus = readEquipmentItem(actor, bedrock::EquipmentSlot::MainHand, &heldItem);
        if (handStatus == EquipmentReadStatus::Item) snapshot.item = std::move(heldItem);
        snapshot.handEmpty = handStatus == EquipmentReadStatus::Empty;
    }
    if (readGameMode) {
        std::int32_t gameType = -1;
        snapshot.hasGameType = snapshot.isPlayer && readActorGameType(actor, gameType);
        snapshot.spectator = snapshot.hasGameType && gameType == static_cast<int>(bedrock::GameType::Spectator);
    }
}

// Keep native Actor/inventory access behind a per-actor SEH boundary. One stale
// cached Actor or invalid inventory layout must not take down Minecraft.
bool readActorMurderSnapshotSeh(void* actor,
                                const std::string* playerName,
                                ActorMurderSnapshot* snapshot,
                                bool readItem, bool readGameMode) noexcept {
    if (!actor || !playerName || !snapshot) return false;
    __try {
        readActorMurderSnapshot(actor, *playerName, *snapshot, readItem, readGameMode);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void updateMurderDetection(bool readItems, bool readGameMode) noexcept {
    // Keep native Actor/inventory reads thread-affine, but sample a small
    // bounded batch each tick so a brief held-item change is less likely to
    // fall between two samples. Opening the TabList never triggers a scan burst.
    if (!gameApi) return;
    try {
        void* level = levelInstance.load(std::memory_order_acquire);
        if (!level) return;
        if (murderScanActors.empty() || murderPlayerNames.empty()) return;
        const auto config=std::atomic_load_explicit(&liveConfig,std::memory_order_acquire);
        const auto sampleCount=tablist::equipmentReadsPerTick(murderScanActors.size(),config->murderScanIntervalMs);
        const auto begin=std::chrono::steady_clock::now();
        std::size_t attempted=0;
        for (std::size_t sample = 0; sample < sampleCount; ++sample) {
            // A single native call cannot be preempted. Continue from this cursor
            // on the next tick instead of doing overdue work in a catch-up burst.
            if(sample && std::chrono::steady_clock::now()-begin>=std::chrono::microseconds(750)) break;
            ++attempted;
            if (murderScanCursor >= murderScanActors.size()) murderScanCursor = 0;
            const auto& tracked = murderScanActors[murderScanCursor];
            auto* actor = tracked.actor;
            murderScanCursor = (murderScanCursor + 1) % murderScanActors.size();
            const auto runtimeId = tracked.runtimeId;
            // A handle captured across ticks may have been destroyed/reused.
            // Check its identity before reading equipment or trusting the cached name.
            if (runtimeId != 0 && readActorRuntimeId(actor) != std::optional{runtimeId}) continue;
            if (std::find(murderPlayerNames.begin(), murderPlayerNames.end(), tracked.playerName) ==
                murderPlayerNames.end()) continue;
            murderLastProcessedScanKey = tracked.scanKey;
            ActorMurderSnapshot snapshot;
            if (!readActorMurderSnapshotSeh(actor, &tracked.playerName, &snapshot,
                                            readItems, readGameMode)) {
                if (!loggedMurderActorReadFault.exchange(true, std::memory_order_acq_rel))
                    diagnostic("MurderDetect skipped an Actor whose native read raised an exception");
                continue;
            }
            const auto& item = snapshot.item;
            if ((!snapshot.isPlayer || snapshot.name.empty() || snapshot.name.size() > 128) &&
                !item) continue;
            MurderSample copied;
            copied.worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
            if (!snapshot.name.empty()) {
                copied.name = std::move(snapshot.name);
            } else {
                std::ostringstream actorLabel;
                actorLabel << "runtimeId=" << std::dec << runtimeId;
                copied.name = actorLabel.str();
            }
            copied.hasItem = item.has_value();
            copied.handEmpty = readItems && snapshot.handEmpty;
            copied.hasGameType = snapshot.hasGameType;
            copied.spectator = snapshot.spectator;
            if (item) {
                copied.identifier = item->identifier;
                copied.displayName = item->displayName;
                copied.count = item->count;
            }
            // Hive can return to its lobby inside the same Level and player.
            // Reset once on that boundary, retaining roles during ordinary swaps.
            if (murderRoundHasRole && tablist::isMurderLobbyItem(copied.identifier)) {
                recordLevel(level, true);
                copied.worldEpoch = murderWorldEpoch.load(std::memory_order_acquire);
                enqueueMurderSample(std::move(copied));
                return; // recordLevel invalidated the Actor roster.
            }
            if (copied.hasItem && tablist::murderRoleForHeldItem(
                    copied.identifier, copied.displayName) != tablist::MurderRole::None)
                murderRoundHasRole = true;
            enqueueMurderSample(std::move(copied));
        }
        static std::int64_t nextScanLog=0;
        const auto now=steadyMilliseconds();
        if(now>=nextScanLog) {
            nextScanLog=now+5000;
            diagnostic("MurderDetect equipment sweep: tracked=",murderScanActors.size(),
                " target_ms=",config->murderScanIntervalMs," planned_per_tick=",sampleCount,
                " attempted=",attempted," budget_limited=",attempted<sampleCount,
                " native_us=",std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-begin).count(),
                " [750us cooperative budget; maximum 12 Actors/tick; copied snapshots to worker]");
        }
    } catch (...) {
        // Item inspection is observational; never interfere with the game tick.
    }
}

void refreshTrackedMurderActors(void* level) noexcept {
    if (!level) return;
    try {
        if (murderScanLevel != level) {
            murderScanLevel = level;
            murderScanActors.clear();
            murderKnownPlayerNamesByRuntimeId.clear();
            murderKnownPlayerNamesByActor.clear();
            pendingActorRoster.reset();
            nextActorRosterMs = 0;
            lastMurderRosterStatusLogMs = 0;
            murderScanCursor = 0;
            murderLastProcessedScanKey = 0;
        }
        if (!pendingActorRoster) {
            if (steadyMilliseconds() < nextActorRosterMs) return;
            ActorRosterBuild build;
            build.handles = tabListGameAccess.actorHandles(level);
            // Sorting/deduplication touches opaque handles only; native validation is deferred.
            std::sort(build.handles.begin(), build.handles.end(), std::less<void*>{});
            build.handles.erase(std::unique(build.handles.begin(), build.handles.end()),
                                build.handles.end());
            build.tracked.reserve(std::min(build.handles.size(), murderPlayerNames.size()));
            pendingActorRoster = std::move(build);
            nextActorRosterMs = steadyMilliseconds() + 2000;
            return; // Do not combine native enumeration and Actor reads on one tick.
        }
        auto& build = *pendingActorRoster;
        if (build.cursor < build.handles.size()) {
            auto* actor = build.handles[build.cursor++];
            if (!actor) return;
            std::string nameTag;
            {
                std::unique_lock lock(actorNameTagMutex, std::try_to_lock);
                if (!lock) { --build.cursor; return; }
                const auto tag = actorNameTags.find(actor);
                if (tag != actorNameTags.end()) nameTag = tag->second.nameTag;
            }
            auto playerName = matchPlayerNameFromTag(nameTag, murderPlayerNames);
            // A handle with no PlayerList match needs an ID read only if a
            // prior player ID exists that it could reattach to.
            const auto knownPointer = murderKnownPlayerNamesByActor.find(actor);
            if (playerName.empty() && knownPointer == murderKnownPlayerNamesByActor.end() &&
                murderKnownPlayerNamesByRuntimeId.empty()) return;
            // This reader has its own SEH boundary; one native ID read per step.
            const auto runtimeId = readActorRuntimeId(actor);
            if (runtimeId && !build.runtimeIds.insert(*runtimeId).second) return;
            if (playerName.empty() && runtimeId) {
                const auto known = murderKnownPlayerNamesByRuntimeId.find(*runtimeId);
                if (known != murderKnownPlayerNamesByRuntimeId.end()) playerName = known->second;
            }
            if (playerName.empty() && !runtimeId) {
                const auto known = murderKnownPlayerNamesByActor.find(actor);
                if (known != murderKnownPlayerNamesByActor.end()) playerName = known->second;
            }
            if (std::find(murderPlayerNames.begin(), murderPlayerNames.end(), playerName) ==
                murderPlayerNames.end()) return;
            const auto scanKey = runtimeId ? static_cast<std::uintptr_t>(*runtimeId)
                : reinterpret_cast<std::uintptr_t>(actor);
            build.tracked.push_back({actor, runtimeId.value_or(0), scanKey, playerName});
            const auto active = std::find_if(murderScanActors.begin(), murderScanActors.end(),
                [&](const auto& entry) { return entry.scanKey == scanKey; });
            if (active == murderScanActors.end()) murderScanActors.push_back(build.tracked.back());
            else *active = build.tracked.back();
            if (runtimeId) build.namesById.insert_or_assign(*runtimeId, std::move(playerName));
            else build.namesByActor.insert_or_assign(actor, std::move(playerName));
            return;
        }
        std::sort(build.tracked.begin(), build.tracked.end(),
            [](const auto& a, const auto& b) { return a.scanKey < b.scanKey; });
        murderScanActors = std::move(build.tracked);
        murderKnownPlayerNamesByRuntimeId = std::move(build.namesById);
        murderKnownPlayerNamesByActor = std::move(build.namesByActor);
        const auto next = std::upper_bound(murderScanActors.begin(), murderScanActors.end(),
            murderLastProcessedScanKey,
            [](std::uintptr_t key, const MurderActorEntry& actor) { return key < actor.scanKey; });
        murderScanCursor = next == murderScanActors.end() ? 0 :
            static_cast<std::size_t>(next - murderScanActors.begin());
        const auto now = steadyMilliseconds();
        if (now - lastMurderRosterStatusLogMs >= 10000 || lastMurderRosterStatusLogMs == 0) {
            diagnostic("MurderDetect roster: actors=", build.handles.size(),
                       " trackedPlayers=", murderScanActors.size(),
                       " players=", murderPlayerNames.size(), " [incremental RuntimeID reads]");
            lastMurderRosterStatusLogMs = now;
        }
        pendingActorRoster.reset();
        nextActorRosterMs = steadyMilliseconds() + 2000;
    } catch (...) {
        pendingActorRoster.reset();
        nextActorRosterMs = steadyMilliseconds() + 2000;
    }
}

void __fastcall onLevelSubTick(void* level) {
    RuntimeActorListTickScope actorListTickScope;
    recordLevel(level);
    if (auto original = originalLevelSubTick.load(std::memory_order_acquire)) original(level);
    NativeReadRegionScope nativeReadScope;
    const auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (!config->enabled || stopRequested.load(std::memory_order_acquire)) return;
    static tablist::NativeUpdateScheduler scheduler;
    static std::uint64_t epoch = 0;
    const auto currentEpoch = murderWorldEpoch.load(std::memory_order_acquire);
    if (epoch != currentEpoch) {
        epoch = currentEpoch;
        scheduler.reset();
        pendingActorRoster.reset();
        murderScanLevel = nullptr;
        murderScanActors.clear();
        murderPlayerNames.clear();
    }
    const auto now = steadyMilliseconds();
    const bool detect = config->murderDetect || config->gamemodeCheck;
    const auto rows = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
    static std::array<std::int64_t, 4> maximumUs{};
    static std::int64_t nextReport = 0;
    const auto start = std::chrono::steady_clock::now();
    if(detect) updateMurderDetection(config->murderDetect,config->gamemodeCheck);
    const auto itemUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
    maximumUs[0]=std::max(maximumUs[0],static_cast<std::int64_t>(itemUs));
    // Maintenance keeps a guaranteed turn at least every three callbacks even
    // if a single native equipment getter exceeds the cooperative time budget.
    static unsigned maintenanceTurn=0;
    const bool maintenanceDue=(++maintenanceTurn%3)==0;
    if(!maintenanceDue && std::chrono::steady_clock::now()-start>=std::chrono::microseconds(750)) return;
    const auto work = scheduler.choose(now, {
        false, // Equipment is distributed on every tick, independently of maintenance.
        tabRowsLastStartedMs == 0 || now - tabRowsLastStartedMs >= 1000,
        detect && !murderPlayerNames.empty() &&
            (pendingActorRoster.has_value() || now >= nextActorRosterMs),
        config->showFaces && now >= nextFaceReadMs && rows &&
            std::any_of(rows->begin(), rows->end(), [](const auto& row) { return !row.hasFace; })
    });
    if (!work) return;
    const auto stageStart=std::chrono::steady_clock::now();
    try {
        switch (*work) {
        case tablist::NativeUpdate::Item:
            break; // Equipment runs above; never schedule another sweep here.
        case tablist::NativeUpdate::Players:
            refreshTabRowsCache(config);
            break;
        case tablist::NativeUpdate::Roster:
            refreshTrackedMurderActors(level);
            break;
        case tablist::NativeUpdate::Face:
            refreshOneCachedFace(config);
            break;
        }
    } catch (...) {
        // Allocation/cache failure must not escape the native update callback.
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - stageStart).count();
    const auto index = static_cast<std::size_t>(*work);
    maximumUs[index] = std::max(maximumUs[index], static_cast<std::int64_t>(elapsed));
    if ((config->debug || elapsed >= 2000 || maximumUs[0]>=2000) && now >= nextReport) {
        nextReport = now + 10000;
        diagnostic("TabList native max us: item=", maximumUs[0], " players=", maximumUs[1],
                   " roster=", maximumUs[2], " face=", maximumUs[3],
                   " [bounded item slice + at most one maintenance stage per callback]");
        maximumUs.fill(0);
    }
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

void drawFacePixels(ImDrawList* draw, const std::array<std::uint32_t, 64>& face,
                    float x, float y, float size) {
    if (!draw) return;
    std::size_t visiblePixels = 0;
    for (const auto color : face)
        if ((color & 0xFFU) != 0) ++visiblePixels;
    if (visiblePixels == 0) return;

    draw->PrimReserve(static_cast<int>(visiblePixels * 6),
                      static_cast<int>(visiblePixels * 4));
    const float pixelSize = size / 8.0f;
    for (std::size_t py = 0; py < 8; ++py) {
        for (std::size_t px = 0; px < 8; ++px) {
            const auto value = face[py * 8 + px];
            if ((value & 0xFFU) == 0) continue;
            const auto color = IM_COL32((value >> 24) & 0xFF,
                                        (value >> 16) & 0xFF,
                                        (value >> 8) & 0xFF,
                                        value & 0xFF);
            const ImVec2 topLeft{x + static_cast<float>(px) * pixelSize,
                                 y + static_cast<float>(py) * pixelSize};
            draw->PrimRect(topLeft,
                           {topLeft.x + pixelSize, topLeft.y + pixelSize}, color);
        }
    }
}

void drawTabList(void*) {
    if (!loggedRenderer.exchange(true, std::memory_order_acq_rel))
        diagnostic("DXGI render callback active");
    // TabList is a passive HUD. Keep this invariant here as a safeguard when
    // built against an older overlay backend that defaults to a GUI cursor.
    auto& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    const auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (!tabHeld.load(std::memory_order_acquire)) return;
    const bool fresh=levelIsFresh();const bool playing=gameplayActive.load(std::memory_order_acquire);
    const auto playersSnapshot = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
    static auto nextReport=std::chrono::steady_clock::time_point{};
    const auto now=std::chrono::steady_clock::now();
    if(now>=nextReport){
        nextReport=now+std::chrono::seconds(5);const auto net=packetPlayers.statistics();const auto receive=packets.receiveStatistics();
        diagnostic("TabList display: enabled=",config->enabled," gameplay=",playing," fresh_level=",fresh,
            " rows=",playersSnapshot?playersSnapshot->size():0," packet_source=",packetRosterMode,
            " receive_ready=",packets.receiveReady()," receive_targets=",packets.receiveTargetCount(),
            " native_calls=",receive.calls," copied=",receive.copied," stream_failed=",receive.streamFailures,
            " game_decode_failed=",receive.decodeFailures," published=",receive.published," queue_dropped=",packetPlayers.dropped()," roster_packets=",net.packets," added=",net.added,
            " removed=",net.removed," rejected=",net.rejected," error=",net.error,
            " viewport=",io.DisplaySize.x,"x",io.DisplaySize.y);
    }
    if(!config->enabled||!playing||!fresh)return;
    if(!playersSnapshot||playersSnapshot->empty()){
        auto* draw=ImGui::GetForegroundDrawList();const char* title="Players  0";
        const char* message=packetRosterMode?"Waiting for player list. Rejoin the world if needed.":"Waiting for player list.";
        const float width=std::max(280.0f,ImGui::CalcTextSize(message).x+24.0f);
        const ImVec2 start{(io.DisplaySize.x-width)*0.5f,config->topOffset};
        draw->AddRectFilled(start,{start.x+width,start.y+60},IM_COL32(20,24,32,230),6);
        draw->AddText({start.x+12,start.y+8},IM_COL32(240,240,245,255),title);
        draw->AddText({start.x+12,start.y+34},IM_COL32(180,185,195,255),message);
        return;
    }
    const auto& players = *playersSnapshot;
    if (!loggedListSuccess.exchange(true, std::memory_order_acq_rel))
        diagnostic("Player list draw submitted: ", players.size(), " player(s)");
    const auto layout = tablist::layoutFor(players.size(), config->maximumRows);
    auto* draw = ImGui::GetForegroundDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    constexpr float horizontalPadding = 10.0f;
    const float faceArea = config->showFaces ? config->faceSize + 7.0f : 8.0f;
    const auto packed = [](tablist::Color color) {
        return ImGui::ColorConvertFloat4ToU32({color.r, color.g, color.b, color.a});
    };
    static constexpr std::array<ImU32, 28> minecraftNameColors{
        IM_COL32(0, 0, 0, 255),       // §0 black
        IM_COL32(0, 0, 170, 255),     // §1 dark blue
        IM_COL32(0, 170, 0, 255),     // §2 dark green
        IM_COL32(0, 170, 170, 255),   // §3 dark aqua
        IM_COL32(170, 0, 0, 255),     // §4 dark red
        IM_COL32(170, 0, 170, 255),   // §5 dark purple
        IM_COL32(255, 170, 0, 255),   // §6 gold
        IM_COL32(170, 170, 170, 255), // §7 gray
        IM_COL32(85, 85, 85, 255),    // §8 dark gray
        IM_COL32(85, 85, 255, 255),   // §9 blue
        IM_COL32(85, 255, 85, 255),   // §a green
        IM_COL32(85, 255, 255, 255),  // §b aqua
        IM_COL32(255, 85, 85, 255),   // §c red
        IM_COL32(255, 85, 255, 255),  // §d light purple
        IM_COL32(255, 255, 85, 255),  // §e yellow
        IM_COL32(255, 255, 255, 255), // §f white
        IM_COL32(221, 214, 5, 255),   // §g minecoin gold
        IM_COL32(227, 212, 209, 255), // §h material quartz
        IM_COL32(154, 92, 198, 255),  // §i rendered purple on the observed server
        IM_COL32(68, 58, 59, 255),    // §j material netherite
        IM_COL32(151, 22, 7, 255),    // §m material redstone
        IM_COL32(180, 104, 77, 255),  // §n material copper
        IM_COL32(222, 177, 45, 255),  // §p material gold
        IM_COL32(17, 159, 54, 255),   // §q material emerald
        IM_COL32(44, 186, 168, 255),  // §s material diamond
        IM_COL32(33, 73, 123, 255),   // §t material lapis
        IM_COL32(154, 92, 198, 255),  // §u material amethyst
        IM_COL32(235, 113, 20, 255),  // §v material resin
    };

    // Player snapshots are capped at 512 in collectRows. Fixed scratch arrays
    // avoid heap allocations on every DXGI frame while Tab is held.
    std::array<float, 512> widths{};
    std::array<float, 512> columnX{};
    struct RowTextMetrics { ImVec2 name{}, role{}, spectator{}, item{}; };
    std::array<RowTextMetrics, 512> textMetrics{};
    std::fill_n(widths.begin(), layout.columns, config->minimumColumnWidth);
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto column = i / config->maximumRows;
        const auto roleText = players[i].murderRole == tablist::MurderRole::Sheriff
            ? " [Sheriff]" : players[i].murderRole == tablist::MurderRole::Murderer
                ? " [Murderer]" : "";
        auto& metrics = textMetrics[i];
        metrics.name = ImGui::CalcTextSize(players[i].name.c_str());
        metrics.role = ImGui::CalcTextSize(roleText);
        if (players[i].spectator)
            metrics.spectator = ImGui::CalcTextSize(" [Spectator]");
        if (!players[i].heldItemName.empty())
            metrics.item = ImGui::CalcTextSize(players[i].heldItemName.c_str());
        widths[column] = std::max(widths[column],
            metrics.name.x + metrics.role.x + metrics.spectator.x +
            (metrics.item.x > 0.0f ? metrics.item.x + 8.0f : 0.0f) +
            horizontalPadding * 2.0f + faceArea);
    }
    float totalWidth = 0.0f;
    for (std::size_t column = 0; column < layout.columns; ++column)
        totalWidth += widths[column];
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

    std::fill_n(columnX.begin(), layout.columns, topLeft.x);
    for (std::size_t column = 1; column < layout.columns; ++column)
        columnX[column] = columnX[column - 1] + widths[column - 1] + config->columnGap;
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto column = i / config->maximumRows;
        const auto row = i % config->maximumRows;
        const auto roleText = players[i].murderRole == tablist::MurderRole::Sheriff
            ? " [Sheriff]" : players[i].murderRole == tablist::MurderRole::Murderer
                ? " [Murderer]" : "";
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
            drawFacePixels(draw, players[i].face, faceX, faceY, config->faceSize);
            textX += config->faceSize + 7.0f;
        } else {
            draw->AddCircleFilled({textX + 2.5f, y + config->rowHeight * 0.5f},
                                  2.5f, packed(config->accent));
            textX += 9.0f;
        }
        const auto textSize = textMetrics[i].name;
        const ImU32 nameColor = players[i].spectator
            ? IM_COL32(145, 150, 165, 210)
            : players[i].hasNameColor && players[i].nameColorOrder <
            minecraftNameColors.size()
            ? minecraftNameColors[players[i].nameColorOrder]
            : packed(config->text);
        draw->AddText({textX, y + (config->rowHeight - textSize.y) * 0.5f},
                      nameColor, players[i].name.c_str());
        if (players[i].murderRole != tablist::MurderRole::None) {
            const char* roleLabel = players[i].murderRole == tablist::MurderRole::Sheriff
                ? " [Sheriff]" : " [Murderer]";
            const ImU32 roleColor = players[i].murderRole == tablist::MurderRole::Sheriff
                ? IM_COL32(85, 220, 255, 255) : IM_COL32(255, 85, 85, 255);
            draw->AddText({textX + textSize.x,
                           y + (config->rowHeight - textSize.y) * 0.5f},
                          roleColor, roleLabel);
        }
        if (players[i].spectator) {
            const float suffixX = textX + textSize.x + textMetrics[i].role.x;
            draw->AddText({suffixX, y + (config->rowHeight - textSize.y) * 0.5f},
                          IM_COL32(165, 170, 185, 230), " [Spectator]");
        }
        if (!players[i].heldItemName.empty()) {
            const float itemX = textX + textSize.x + textMetrics[i].role.x +
                textMetrics[i].spectator.x + 8.0f;
            draw->AddText({itemX, y + (config->rowHeight - textMetrics[i].item.y) * 0.5f},
                          IM_COL32(190, 198, 210, 220), players[i].heldItemName.c_str());
        }
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
    std::atomic_store_explicit(&liveConfig,
        std::make_shared<const tablist::Config>(parsed), std::memory_order_release);
    return true;
}

void removeNativeHooks() noexcept {
    for (auto& token : hookTokens) {
        if (token) BDS_HookUnsubscribe(token);
        token = nullptr;
    }
    stopTabRowsWorker();
    stopMurderWorker();
    if (actorNameTagHookToken) BDS_HookUnsubscribe(actorNameTagHookToken);
    actorNameTagHookToken = nullptr;
    gameApi.reset();
    originalKeyboardFeed.store(nullptr);
    originalGrabCursor.store(nullptr);
    originalReleaseCursor.store(nullptr);
    originalGetFov.store(nullptr);
    originalLevelSubTick.store(nullptr);
    packets.stop();
    packetPlayers.stop(packets);
    itemHoverNameTarget.store(nullptr, std::memory_order_release);
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
    if (!diagnosticWriter.start(path.parent_path() / L"TabList.log")) {
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    struct DiagnosticStop { ~DiagnosticStop() { diagnosticWriter.stop(); } } diagnosticStop;
    diagnostic("start ",bedrock::moduleIdentity(reinterpret_cast<const void*>(&startClient)));
    diagnostic("TabList roster revision=4 [packet PlayerList / SDK runtime Actors]");
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
    diagnostic("resolver ready; executable SHA256=",version.identity.fileSha256," profile=",version.path);
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
    const auto resolveActorNameTag = [&] {
        actorNameTag = bedrock::resolveSharedHook(
            resolver, "Actor::getNameTag", BDS_HOOK_ACTOR_GET_NAME_TAG);
        if (actorNameTag && !bedrock::isUsableSharedHookResolution(*actorNameTag))
            actorNameTag.reset();
    };
    const auto resolveRuntimeActorList = [&](const bedrock::Resolver&) {
        std::string failure;
        const bool loaded=runtimeActors.load(profile,bedrock::generated::kSymbolDatabase,failure);
        if(!loaded)diagnostic("SDK runtime Actor list unavailable: ",failure);
        return loaded;
    };
    const auto resolveItemHoverName = [](const bedrock::Resolver& source) {
        if (!source.exactBuild()) return false;
        const auto method = source.resolve("ItemStackBase::getHoverName");
        if (!method || !method->exact_build || !method->verified ||
            method->method != "exact-rva") return false;
        tabListGameAccess.setItemHoverName(reinterpret_cast<void*>(method->address));
        return true;
    };
    bedrock::NativeItemLayout cachedItemLayout;
    std::string cachedItemLayoutError;
    bool itemLayoutAvailable = bedrock::parseNativeItemLayout(
        profile, cachedItemLayout, cachedItemLayoutError);
    bedrock::NativeActorLayout cachedActorLayout;
    std::string cachedActorLayoutError;
    bool actorLayoutAvailable = bedrock::parseNativeActorLayout(
        profile, cachedActorLayout, cachedActorLayoutError);
    resolveActorNameTag();
    bool runtimeActorListReady = resolveRuntimeActorList(resolver);
    bool itemHoverNameReady = resolveItemHoverName(resolver);
    const auto requirementsMet = [&] {
        return valid(keyboard) && valid(grab) && valid(release) && valid(fov) && valid(levelTick);
    };
    resolveRequired();
    if ((!requirementsMet() || !actorNameTag || !itemLayoutAvailable ||
         !itemHoverNameReady) &&
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
                runtimeActorListReady = resolveRuntimeActorList(resolver);
                itemHoverNameReady = resolveItemHoverName(resolver);
                itemLayoutAvailable = bedrock::parseNativeItemLayout(
                    profile, cachedItemLayout, cachedItemLayoutError);
                actorLayoutAvailable = bedrock::parseNativeActorLayout(
                    profile, cachedActorLayout, cachedActorLayoutError);
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
    if (!actorNameTag || !runtimeActorListReady || !itemHoverNameReady ||
        !actorLayoutAvailable) {
        bedrock::Resolver embeddedResolver;
        std::string embeddedError;
        if (embeddedResolver.loadJson(bedrock::generated::kSymbolDatabase, embeddedError) &&
            embeddedResolver.attachWindowsMainModule(version.identity, embeddedError) &&
            embeddedResolver.exactBuild()) {
            if (!actorNameTag) {
                actorNameTag = bedrock::resolveSharedHook(
                    embeddedResolver, "Actor::getNameTag", BDS_HOOK_ACTOR_GET_NAME_TAG);
                if (actorNameTag && !bedrock::isUsableSharedHookResolution(*actorNameTag))
                    actorNameTag.reset();
            }
            if (!runtimeActorListReady)
                runtimeActorListReady = resolveRuntimeActorList(embeddedResolver);
            if (!itemHoverNameReady)
                itemHoverNameReady = resolveItemHoverName(embeddedResolver);
            if (!actorLayoutAvailable) {
                actorLayoutAvailable = bedrock::parseNativeActorLayout(
                    bedrock::generated::kSymbolDatabase, cachedActorLayout,
                    cachedActorLayoutError);
            }
        } else {
            diagnostic("embedded exact-build profile unavailable: ", embeddedError);
        }
    }
    diagnostic("world Actor source: Level::getRuntimeActorList=",
               runtimeActorListReady ? "profile ready" : "unavailable",
               runtimeActorListReady ? " [SDK exact function / hidden vector return]" : "");
    diagnostic("ItemStackBase::getHoverName=",
               itemHoverNameReady ? "verified exact-build display name active"
                                   : "unavailable; item ID fallback");

    if (!requirementsMet()) {
        diagnostic("required symbols unavailable keyboard=", bool(keyboard),
                   " grab=", bool(grab), " release=", bool(release),
                   " fov=", bool(fov), " level=", bool(levelTick));
        logger.error("Required native input/render symbols are unavailable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    if (itemLayoutAvailable) {
        nativeItemLayout = std::move(cachedItemLayout);
        diagnostic("SDK item API layout loaded [versioned candidate; actor equipment + local inventory]");
    } else {
        bedrock::NativeItemLayout embeddedItemLayout;
        std::string embeddedItemError;
        if (bedrock::parseNativeItemLayout(bedrock::generated::kSymbolDatabase,
                                           embeddedItemLayout, embeddedItemError)) {
            nativeItemLayout = std::move(embeddedItemLayout);
            diagnostic("SDK item API layout loaded [embedded exact-build candidate]");
        } else {
            nativeItemLayout.reset();
            diagnostic("SDK item API unavailable: no versioned native item layout");
        }
    }

    if (actorLayoutAvailable) {
        nativeActorLayout = std::move(cachedActorLayout);
        diagnostic("Actor RuntimeID component reader ready [versioned candidate layout; 64-bit value]");
    } else {
        nativeActorLayout.reset();
        diagnostic("Actor RuntimeID component reader unavailable: ", cachedActorLayoutError);
    }

    if(nativeActorLayout)actorComponents.configure(*nativeActorLayout,bedrock::parseNativeProfile(profile));
    packetRosterMode=bedrock::PlayerListPackets::supports(profile,bedrock::generated::kSymbolDatabase);
    if(!packetRosterMode){
        try{playerRoster.load(profile,bedrock::generated::kSymbolDatabase);
            diagnostic("SDK PlayerList API ready [legacy native roster]");}
        catch(const std::exception& e){diagnostic("SDK PlayerList API unavailable: ",e.what());}
    }
    try{localPlayerGetter.load(profile);diagnostic("SDK local player getter ready; exact=",localPlayerGetter.exactGetter());}
    catch(const std::exception& e){diagnostic("SDK local player getter unavailable: ",e.what());}

    if (!startTabRowsWorker()) {
        diagnostic("TabList row worker unavailable; startup stopped");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    if (startMurderWorker())
        diagnostic("MurderDetect worker ready [copied snapshots only; native reads stay on game thread]");
    else
        diagnostic("MurderDetect worker unavailable; samples are skipped");

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
    if(packetRosterMode){
        packetPlayers.start(packets);
        if(!packets.installReceive(profile,bedrock::generated::kSymbolDatabase,error))
            diagnostic("SDK packet PlayerList unavailable: ",error);
        else diagnostic("SDK packet PlayerList ready [per-ID virtual decoders; copied-data worker; rejoin after loading]");
    }


    gameApi = std::make_unique<bedrock::GameApi>(tabListGameAccess);

    bool actorNameTagHookReady = false;
    if (actorNameTag) {
        diagnostic("Actor::getNameTag subscribe attempt: target=0x", std::hex,
                   actorNameTag->address, std::dec, " method=", actorNameTag->method,
                   " exact_build=", actorNameTag->exact_build,
                   " verified=", actorNameTag->verified,
                   " broker_owned=",
                   BDS_HookOwnsTarget(BDS_HOOK_ACTOR_GET_NAME_TAG,
                       reinterpret_cast<void*>(actorNameTag->address)));
        actorNameTagHookReady = BDS_HookSubscribe(BDS_HOOK_ACTOR_GET_NAME_TAG,
            reinterpret_cast<void*>(actorNameTag->address), actorNameTagMiddleware,
            nullptr, &actorNameTagHookToken);
    }
    if (actorNameTagHookReady) {
        diagnostic("Shared ActorNameTag capture active [no per-call SDK validation]; method=", actorNameTag->method,
                   " exact_build=", actorNameTag->exact_build,
                   " verified=", actorNameTag->verified);
    } else {
        diagnostic("SDK ActorNameTag hook unavailable; color sorting has no live Actor source",
                   error.empty() ? "" : ": ", error);
    }
    diagnostic("Actor NameTag color source: shared capture ",
               actorNameTagHookReady ? "active" : "unavailable");

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
        const auto delay = std::atomic_load_explicit(&liveConfig,
            std::memory_order_acquire)->reloadIntervalMs;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));

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

