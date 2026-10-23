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
#include <bedrock/game_api.hpp>
#include <bedrock/game_hooks.hpp>
#include <bedrock/native_items.hpp>
#include <imgui.h>
#include <entt/entt.hpp>
#include "tablist/config.hpp"
#include "tablist/model.hpp"

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

namespace tablist_native {
struct EntityId {
    std::uint32_t rawId{};
    [[nodiscard]] constexpr bool operator==(const EntityId&) const = default;
    [[nodiscard]] constexpr operator std::uint32_t() const { return rawId; }
};
struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = std::uint32_t;
    using version_type = std::uint16_t;
    static constexpr entity_type entity_mask = 0x3FFFF;
    static constexpr version_type version_mask = 0x3FFF;
};
struct ActorEquipmentComponent {
    static constexpr std::uint32_t type_hash = 0xB06141A9;
    void* handContainer{};
    void* armorContainer{};
};
struct ActorGameTypeComponent {
    static constexpr std::uint32_t type_hash = 0x88D3EDDF;
    std::int32_t gameType{};
};
}

// These traits mirror the pinned Latite reference's Bedrock ECS component
// registration so GameApi::Actor::getHeldItem can use an Actor's replicated
// hand container without consulting PlayerSupplies or enumerating Inventory.
namespace entt {
template<> struct entt_traits<tablist_native::EntityId>
    : basic_entt_traits<tablist_native::EntityIdTraits> {
    static constexpr std::size_t page_size = 2048;
};
template<> struct component_traits<tablist_native::ActorEquipmentComponent,
                                   tablist_native::EntityId> {
    using element_type = tablist_native::ActorEquipmentComponent;
    using entity_type = tablist_native::EntityId;
    static constexpr bool in_place_delete = true;
    static constexpr std::size_t page_size = 128 * !std::is_empty_v<element_type>;
};
template<> struct storage_type<tablist_native::ActorEquipmentComponent,
                               tablist_native::EntityId> {
    using type = basic_storage<tablist_native::ActorEquipmentComponent,
                               tablist_native::EntityId>;
};
template<> struct type_hash<tablist_native::ActorEquipmentComponent> {
    [[nodiscard]] static consteval id_type value() noexcept {
        return tablist_native::ActorEquipmentComponent::type_hash;
    }
    [[nodiscard]] consteval operator id_type() const noexcept { return value(); }
};
template<> struct component_traits<tablist_native::ActorGameTypeComponent,
                                   tablist_native::EntityId> {
    using element_type = tablist_native::ActorGameTypeComponent;
    using entity_type = tablist_native::EntityId;
    static constexpr bool in_place_delete = true;
    static constexpr std::size_t page_size = 128 * !std::is_empty_v<element_type>;
};
template<> struct storage_type<tablist_native::ActorGameTypeComponent,
                               tablist_native::EntityId> {
    using type = basic_storage<tablist_native::ActorGameTypeComponent,
                               tablist_native::EntityId>;
};
template<> struct type_hash<tablist_native::ActorGameTypeComponent> {
    [[nodiscard]] static consteval id_type value() noexcept {
        return tablist_native::ActorGameTypeComponent::type_hash;
    }
    [[nodiscard]] consteval operator id_type() const noexcept { return value(); }
};
}

namespace {
enum class Status : unsigned { NotStarted, Loading, Running, Stopped, Failed };

using KeyboardFeedFn = bool(__fastcall*)(int, bool);
using CursorFn = void(__fastcall*)(void*);
using GetFovFn = float(__fastcall*)(void*, float, void*, void*);
using LevelSubTickFn = void(__fastcall*)(void*);
using ActorGetNameTagFn = const std::string&(__fastcall*)(void*);
using ItemHoverNameFn = std::string*(__fastcall*)(void*, std::string*);
// Exact 1.26.52.3 disassembly writes a std::vector<Actor*> hidden return area
// through RDX, then appends one Actor* per runtime entity.
using LevelRuntimeActorListFn = void(__fastcall*)(void*, std::vector<void*>*);

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
    std::string heldItemName;
    unsigned nameColorOrder = 28;
    bool hasNameColor = false;
    tablist::MurderRole murderRole = tablist::MurderRole::None;
    bool spectator = false;
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
std::atomic<void*> resolvedActorGetNameTag{nullptr};
BDS_HookToken actorNameTagHookToken{};
std::atomic<std::size_t> runtimeActorListSlot{std::numeric_limits<std::size_t>::max()};
std::atomic<void*> runtimeActorListTarget{nullptr};
std::atomic<void*> itemHoverNameTarget{nullptr};
std::mutex itemDisplayNameMutex;
std::unordered_map<std::string, std::string> itemDisplayNames;
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
    unsigned count{};
    bool hasItem{};
    bool hasGameType{};
    bool spectator{};
};
std::mutex murderQueueMutex;
std::condition_variable murderQueueReady;
std::deque<MurderSample> murderQueue;
std::thread murderWorker;
bool murderWorkerStopping{};
std::atomic<std::uint64_t> droppedMurderSamples{0};
// Raw handles are captured from GameApi's validated roster on the game thread.
// Re-validating each wrapper for every scan repeats several VirtualQuery calls.
struct MurderActorEntry {
    void* actor{};
    std::string playerName;
};
std::vector<MurderActorEntry> murderScanActors;
std::vector<std::string> murderPlayerNames;
std::unordered_map<void*, std::string> murderKnownPlayerNamesByActor;
std::size_t murderScanCursor = 0;
std::uintptr_t murderLastProcessedActor = 0;
void* murderScanLevel = nullptr;
std::uint64_t murderActorSnapshotTick = 0;
std::size_t lastLoggedMurderPlayerCount = std::numeric_limits<std::size_t>::max();
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
const std::vector<PlayerRow>* cachedFaceSource = nullptr;
std::mutex cachedTabRowsWriteMutex;
std::shared_ptr<const std::vector<PlayerRow>> cachedTabRows{
    std::make_shared<const std::vector<PlayerRow>>()};
std::atomic<bool> loggedTabRowsReady{false};
std::atomic<bool> loggedFirstNameTag{false};
std::atomic<std::uint64_t> actorNameTagSequence{0};
std::atomic<std::int64_t> lastActorNamePollMs{0};
std::atomic<std::size_t> actorNamePollCursor{0};
bool copyActorNameTag(void* actor, std::string* output);
std::optional<bedrock::NativeItemLayout> nativeItemLayout;
class TabListGameAccess final : public bedrock::GameAccess {
public:
    void setRuntimeActorList(std::size_t slot, void* target) noexcept {
        runtimeActorListTarget.store(target, std::memory_order_release);
        runtimeActorListSlot.store(slot, std::memory_order_release);
    }
    void setItemHoverName(void* target) noexcept {
        itemHoverNameTarget.store(target, std::memory_order_release);
    }
    bool isActorValid(void* actor) const noexcept override;
    void* localPlayer() const noexcept override;
    void* worldOf(void* actor) const noexcept override;
    std::vector<void*> actorHandles(void* world) const override;
    std::optional<std::string> nameTag(void* actor) const override {
        std::string result;
        return copyActorNameTag(actor, &result) ? std::optional<std::string>{std::move(result)}
                                               : std::nullopt;
    }
    std::optional<bedrock::ItemStack> equipmentItem(
        void* actor, bedrock::EquipmentSlot slot) const override;
    std::size_t localInventorySize(void* player) const override;
    std::optional<bedrock::ItemStack> localInventoryItem(
        void* player, std::size_t slot) const override;
    std::optional<std::size_t> selectedHotbarSlot(void* player) const override;
    std::vector<std::string> playerListNames(void* world) const override;
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
std::unique_ptr<bedrock::GameHooks> gameHooks;
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
std::mutex diagnosticMutex;
std::ofstream diagnosticLog;
std::atomic<bool> loggedRenderer{false};
std::atomic<bool> loggedListSuccess{false};

template <class... Args>
void diagnostic(Args&&... args) {
    std::lock_guard lock(diagnosticMutex);
    if (!diagnosticLog) return;
    (diagnosticLog << ... << std::forward<Args>(args));
    diagnosticLog << '\n';
    diagnosticLog.flush();
}

void publishHeldItemName(const std::string& playerName,
                         const std::string& displayName) noexcept {
    try {
        std::lock_guard writeLock(cachedTabRowsWriteMutex);
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
        std::string itemKey = sample.identifier.empty()
            ? sample.displayName : sample.identifier;
        if (itemKey.size() > 256) return;
        std::transform(itemKey.begin(), itemKey.end(), itemKey.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto role = sample.hasItem
            ? tablist::murderRoleForHeldItem(sample.identifier, sample.displayName)
            : tablist::MurderRole::None;

        bool changed = false;
        std::string previous;
        std::string currentDisplay;
        {
            std::lock_guard lock(murderDetectMutex);
            const auto now = std::chrono::steady_clock::now();
            const auto old = lastHeldItemByPlayer.find(sample.name);
            if (old != lastHeldItemByPlayer.end()) previous = old->second;
            // Failed/empty stack reads do not erase the last confirmed item.
            if (!itemKey.empty()) {
                changed = old == lastHeldItemByPlayer.end() || old->second != itemKey;
                lastHeldItemByPlayer.insert_or_assign(sample.name, itemKey);
            }
            if (sample.hasItem) {
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
        if (!currentDisplay.empty()) publishHeldItemName(sample.name, currentDisplay);
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
    bool processInline = false;
    try {
        {
            std::lock_guard lock(murderQueueMutex);
            if (!murderWorker.joinable() || murderWorkerStopping) {
                processInline = true;
            } else {
                constexpr std::size_t maxQueuedMurderSamples = 256;
                if (murderQueue.size() == maxQueuedMurderSamples) {
                    murderQueue.pop_front();
                    droppedMurderSamples.fetch_add(1, std::memory_order_relaxed);
                }
                murderQueue.push_back(std::move(sample));
            }
        }
    } catch (...) {
        // Queue allocation failure falls back to a bounded synchronous update.
        processInline = true;
    }
    if (processInline) {
        processMurderSample(sample);
        return;
    }
    murderQueueReady.notify_one();
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

void* callInventoryGetItem(void* inventory, std::size_t slot,
                           const bedrock::NativeItemLayout& layout) noexcept {
    if (!inventory || slot >= layout.localInventorySlots) return nullptr;
    void** vtable = nullptr;
    if (!safeRead(inventory, 0, vtable) || !vtable) return nullptr;
    static std::atomic<void**> cachedVtable{nullptr};
    static std::atomic<void*> cachedTarget{nullptr};
    void* target = nullptr;
    if (cachedVtable.load(std::memory_order_acquire) == vtable) {
        target = cachedTarget.load(std::memory_order_acquire);
    } else {
        if (!readable(vtable, sizeof(void*) * (layout.inventoryGetItemVtableSlot + 1)) ||
            !safeRead(vtable, sizeof(void*) * layout.inventoryGetItemVtableSlot, target) ||
            !executable(target)) return nullptr;
        cachedTarget.store(target, std::memory_order_release);
        cachedVtable.store(vtable, std::memory_order_release);
    }
    if (!target) return nullptr;
    using GetItemFn = void*(__fastcall*)(void*, int);
    __try {
        return reinterpret_cast<GetItemFn>(target)(inventory, static_cast<int>(slot));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
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

bool levelIsFresh() noexcept {
    const auto heartbeat = levelHeartbeatMs.load(std::memory_order_acquire);
    return heartbeat > 0 && steadyMilliseconds() - heartbeat <= 1000;
}

void recordLevel(void* level) noexcept {
    if (!level) return;
    void* previous = levelInstance.exchange(level, std::memory_order_acq_rel);
    levelHeartbeatMs.store(steadyMilliseconds(), std::memory_order_release);
    if (previous != level) {
        if (gameApi) gameApi->clearObservedActors();
        std::atomic_store_explicit(&cachedTabRows,
            std::shared_ptr<const std::vector<PlayerRow>>{}, std::memory_order_release);
        loggedTabRowsReady.store(false, std::memory_order_release);
        {
            std::lock_guard lock(actorNameTagMutex);
            observedPlayerColorOrders.clear();
        }
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

bool invokeLevelRuntimeActorList(void* level, void* target,
                                std::vector<void*>* actors) noexcept {
    if (!allowRuntimeActorListCall || !level || !target || !actors ||
        !readable(level, sizeof(void*)) || !executable(target)) return false;
    __try {
        reinterpret_cast<LevelRuntimeActorListFn>(target)(level, actors);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TabListGameAccess::isActorValid(void* actor) const noexcept {
    return looksLikeActor(actor);
}

std::vector<void*> TabListGameAccess::actorHandles(void* world) const {
    std::vector<void*> result;
    if (!allowRuntimeActorListCall || !world ||
        world != levelInstance.load(std::memory_order_acquire)) return result;
    const auto slot = runtimeActorListSlot.load(std::memory_order_acquire);
    auto* expectedTarget = runtimeActorListTarget.load(std::memory_order_acquire);
    if (slot == std::numeric_limits<std::size_t>::max() || slot > 0x400 ||
        !expectedTarget) return result;

    void** vtable = nullptr;
    void* target = nullptr;
    // Slot 0x145 is zero-based; reject derived/foreign vtables unless the
    // target is exactly the one resolved from this executable's profile.
    if (!safeRead(world, 0, vtable) || !vtable ||
        !safeRead(vtable, slot * sizeof(void*), target) ||
        target != expectedTarget || !executable(target)) return result;

    std::vector<void*> nativeActors;
    if (!invokeLevelRuntimeActorList(world, target, &nativeActors) ||
        nativeActors.size() > 4096) return result;
    result.reserve(nativeActors.size());
    std::unordered_set<void*> seen;
    seen.reserve(nativeActors.size());
    for (auto* actor : nativeActors) {
        if (actor && isActorValid(actor) && seen.insert(actor).second)
            result.push_back(actor);
    }
    if (!loggedActorListSuccess.exchange(true, std::memory_order_acq_rel))
        diagnostic("Level::getRuntimeActorList active: slot=0x", std::hex, slot,
                   " target=0x", reinterpret_cast<std::uintptr_t>(target), std::dec,
                   " native=", nativeActors.size(), " valid=", result.size());
    return result;
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
        std::lock_guard lock(itemDisplayNameMutex);
        const auto it = itemDisplayNames.find(item.identifier);
        if (it != itemDisplayNames.end()) {
            item.displayName = it->second;
            return;
        }
    }
    std::string displayName;
    auto* target = itemHoverNameTarget.load(std::memory_order_acquire);
    if (!invokeItemHoverName(stack, target, &displayName) ||
        displayName.empty() || displayName.size() > 256) return;
    item.displayName = displayName;
    try {
        std::lock_guard lock(itemDisplayNameMutex);
        if (itemDisplayNames.size() >= 512) itemDisplayNames.clear();
        itemDisplayNames.insert_or_assign(item.identifier, std::move(displayName));
    } catch (...) {
        // Display-name caching is optional; preserve the copied item snapshot.
    }
}

bool readActorEquipment(void* actor, const bedrock::NativeItemLayout& layout,
                        tablist_native::ActorEquipmentComponent** output) noexcept {
    if (output) *output = nullptr;
    if (!actor || !output) return false;
    void* registry = nullptr;
    std::uint32_t rawId = 0;
    const auto base = layout.actorEntityContextOffset;
    if (!safeRead(actor, base + layout.contextEnttRegistryOffset, registry) ||
        !safeRead(actor, base + layout.contextEntityIdOffset, rawId) || !registry ||
        !readable(registry, sizeof(void*))) return false;
    __try {
        auto* typed = static_cast<entt::basic_registry<tablist_native::EntityId>*>(registry);
        auto* storage = typed->storage(layout.equipmentComponentHash);
        if (!storage) return false;
        const auto entity = tablist_native::EntityId{rawId};
        if (!storage->contains(entity)) return false;
        *output = static_cast<tablist_native::ActorEquipmentComponent*>(storage->value(entity));
        return *output != nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        *output = nullptr;
        return false;
    }
}

bool readActorGameType(void* actor, std::int32_t& gameType) noexcept {
    if (!actor || !nativeItemLayout) return false;
    const auto& layout = *nativeItemLayout;
    void* registry = nullptr;
    std::uint32_t rawId = 0;
    if (!safeRead(actor, layout.actorEntityContextOffset + layout.contextEnttRegistryOffset,
                  registry) ||
        !safeRead(actor, layout.actorEntityContextOffset + layout.contextEntityIdOffset,
                  rawId) || !registry || !readable(registry, sizeof(void*))) return false;
    __try {
        auto* typed = static_cast<entt::basic_registry<tablist_native::EntityId>*>(registry);
        auto* storage = typed->storage(tablist_native::ActorGameTypeComponent::type_hash);
        if (!storage) return false;
        const auto entity = tablist_native::EntityId{rawId};
        if (!storage->contains(entity)) return false;
        const auto* component = static_cast<const tablist_native::ActorGameTypeComponent*>(
            storage->value(entity));
        if (!component) return false;
        gameType = component->gameType;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* TabListGameAccess::localPlayer() const noexcept {
    if (!nativeItemLayout) return nullptr;
    void* client = clientInstance.load(std::memory_order_acquire);
    void** vtable = nullptr;
    if (!client || !safeRead(client, 0, vtable) || !vtable ||
        !readable(vtable, sizeof(void*) * (nativeItemLayout->localPlayerVtableSlot + 1)))
        return nullptr;
    void* target = nullptr;
    if (!safeRead(vtable, sizeof(void*) * nativeItemLayout->localPlayerVtableSlot, target) ||
        !executable(target)) return nullptr;
    using GetLocalPlayerFn = void*(__fastcall*)(void*);
    __try {
        void* player = reinterpret_cast<GetLocalPlayerFn>(target)(client);
        return looksLikeActor(player) ? player : nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

std::optional<bedrock::ItemStack> TabListGameAccess::equipmentItem(
    void* actor, bedrock::EquipmentSlot slot) const {
    // Actor's public wrapper validates before dispatch. The murder sampler
    // calls this adapter directly for handles from the validated roster, and
    // every memory/native boundary below is guarded independently.
    if (!nativeItemLayout || !actor) return std::nullopt;
    const auto& layout = *nativeItemLayout;
    tablist_native::ActorEquipmentComponent* component = nullptr;
    if (!readActorEquipment(actor, layout, &component) || !component) return std::nullopt;
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
    if (!safeRead(component, containerOffset, container) || !container) return std::nullopt;
    void* stack = callInventoryGetItem(container, slotIndex, layout);
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
    if (!copyNativeString(id, item.identifier, layout, layout.maxStringBytes))
        return std::nullopt;
    if (!item.identifier.empty() && item.identifier.find(':') == std::string::npos)
        item.identifier.insert(0, "minecraft:");
    // The display name is cached by item identifier, so the native resolver
    // runs only for the first observation of each item type.
    populateItemDisplayName(stack, item);
    if (!item.valid()) return std::nullopt;
    return item;
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
    NativePlayerList* players = nullptr;
    if (!safeRead(world, levelPlayerListOffset, players) || !players ||
        !readable(players, sizeof(NativePlayerList)) || players->size() > 512) return names;
    names.reserve(players->size());
    for (const auto& item : *players) {
        const auto& name = item.second.name;
        if (name.empty() || name.size() > 128 || !readable(name.data(), name.size())) continue;
        names.push_back(name);
    }
    {
        std::lock_guard lock(playerNamesMutex_);
        cachedWorld_ = world;
        playerNamesCacheMs_ = now;
        playerNamesCache_ = names;
    }
    return names;
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

void captureQueriedActorNameTag(void* actor, const std::string& nameTag) noexcept {
    if (!actor || nameTag.size() > 512) return;
    try {
        const bool hasVisibleName = !tablist::stripMinecraftFormatting(nameTag).empty();
        const auto sequence = actorNameTagSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        bool logFirstName = false;
        {
            std::lock_guard lock(actorNameTagMutex);
            const auto existing = actorNameTags.find(actor);
            if (existing != actorNameTags.end() && existing->second.nameTag == nameTag) return;
            if (!actorNameTags.contains(actor) && actorNameTags.size() >= 4096)
                actorNameTags.clear();
            // Preserve an empty/reset nametag as a state change so stale colors
            // do not remain attached to an Actor after its formatting changes.
            const auto color = hasVisibleName ? tablist::minecraftNameColorOrder(nameTag) : 28u;
            actorNameTags.insert_or_assign(actor,
                ActorNameTagEntry{actor, nameTag, color, sequence});
            logFirstName = hasVisibleName &&
                !loggedFirstNameTag.exchange(true, std::memory_order_acq_rel);
        }
        if (logFirstName) diagnostic("Actor nameTag capture active; live color updates enabled");
    } catch (...) {
        // Never allow diagnostics/cache allocation failures to escape into
        // Minecraft's native Actor::getNameTag call.
    }
}

const std::string& __fastcall actorGetNameTagDetour(void* actor) {
    static const std::string unavailable;
    auto* rawFunction = originalActorGetNameTag.load(std::memory_order_acquire);
    if (!actor || !rawFunction) return unavailable;
    const auto& nameTag = reinterpret_cast<ActorGetNameTagFn>(rawFunction)(actor);
    captureQueriedActorNameTag(actor, nameTag);
    return nameTag;
}

bool copyActorNameTag(void* actor, std::string* output) {
    const char* data = nullptr;
    std::size_t size = 0;
    if (!output || !getActorNameTagView(actor, &data, &size) || size > 512 ||
        (size && !readable(data, size))) return false;
    output->assign(data, size);
    return true;
}

// Actor::getNameTag is normally called by the renderer, so a hook alone only
// refreshes actors whose labels happen to be queried that frame. Once an Actor
// has been observed, poll a bounded batch on the game tick (20 Hz maximum).
// This refreshes color/name changes even when its nametag is currently hidden.
// It intentionally does not guess a Level actor-list ABI or discover new actors.
void refreshObservedActorNameTags() noexcept {
    constexpr std::int64_t intervalMs = 50;
    constexpr std::size_t maximumBatch = 256;
    if (!originalActorGetNameTag.load(std::memory_order_acquire)) return;
    const auto now = steadyMilliseconds();
    auto previous = lastActorNamePollMs.load(std::memory_order_acquire);
    if (now - previous < intervalMs ||
        !lastActorNamePollMs.compare_exchange_strong(previous, now,
            std::memory_order_acq_rel, std::memory_order_acquire)) return;

    try {
        std::vector<void*> actors;
        {
            std::lock_guard lock(actorNameTagMutex);
            actors.reserve(actorNameTags.size());
            for (const auto& [actor, _] : actorNameTags) actors.push_back(actor);
        }
        if (actors.empty()) return;
        const auto start = actorNamePollCursor.fetch_add(maximumBatch,
            std::memory_order_acq_rel) % actors.size();
        const auto count = std::min(maximumBatch, actors.size());
        std::vector<void*> stale;
        for (std::size_t i = 0; i < count; ++i) {
            auto* actor = actors[(start + i) % actors.size()];
            if (!looksLikeActor(actor)) {
                stale.push_back(actor);
                continue;
            }
            std::string nameTag;
            if (copyActorNameTag(actor, &nameTag))
                captureQueriedActorNameTag(actor, nameTag);
        }
        if (!stale.empty()) {
            std::lock_guard lock(actorNameTagMutex);
            for (auto* actor : stale) actorNameTags.erase(actor);
        }
    } catch (...) {
        // Keep allocation/cache failures outside Minecraft's tick path.
    }
}

bool tryAcquireLevelPointerFromClient(void** output) noexcept {
    if (!output) return false;
    *output = nullptr;
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
    *output = level;
    return true;
}

bool tryAcquireLevelFromClient() noexcept {
    void* level = nullptr;
    if (!tryAcquireLevelPointerFromClient(&level)) return false;
    recordLevel(level);
    return true;
}

std::vector<ActorNameTagEntry> snapshotSdkActorNameTags() {
    std::vector<ActorNameTagEntry> result;
    // Read immutable SDK snapshots only; no GameApi::actors() walk, native
    // Actor calls, or PlayerList matching is done by the DXGI render callback.
    if (!gameApi) return result;
    std::unordered_map<void*, ActorNameTagEntry> byActor;
    const auto append = [&](bedrock::NameTagSource source) {
        const auto snapshot = gameApi->cachedActorNameTags(source);
        for (const auto& item : *snapshot) {
            if (!item.actor || item.nameTag.empty() || item.nameTag.size() > 512 ||
                tablist::stripMinecraftFormatting(item.nameTag).empty()) continue;
            const auto existing = byActor.find(item.actor);
            if (existing != byActor.end() && existing->second.sequence > item.sequence) continue;
            const auto color = tablist::minecraftNameColorOrder(item.nameTag);
            byActor.insert_or_assign(item.actor,
                ActorNameTagEntry{item.actor, item.nameTag, color, item.sequence});
        }
    };
    append(bedrock::NameTagSource::View);
    append(bedrock::NameTagSource::ActorGetter); // getter is newest/authoritative on duplicates
    result.reserve(byActor.size());
    for (auto& [_, entry] : byActor) result.push_back(std::move(entry));
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

bool collectRowsUnsafe(std::vector<PlayerRow>& rows, bool includeFaces,
                       bool sortByNameColor, bool gamemodeCheck,
                       bool murderDetect,
                       bool debug,
                       const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
                       const std::unordered_set<std::string>& spectators,
                       const std::vector<ActorNameTagEntry>& actorTags) {
    void* level = levelInstance.load(std::memory_order_acquire);
    if (!level) {
        if (!tryAcquireLevelFromClient()) return false;
        level = levelInstance.load(std::memory_order_acquire);
        if (!level) return false;
    }
    if (!gameApi) return false;
    const auto playerNames = gameApi->world(level).getPlayerNames();
    if (playerNames.size() > 512) return false;

    NativePlayerList* nativePlayers = nullptr;
    std::unordered_map<std::string, const NativePlayerListEntry*> faceEntries;
    if (includeFaces && safeRead(level, levelPlayerListOffset, nativePlayers) &&
        nativePlayers && readable(nativePlayers, sizeof(NativePlayerList)) &&
        nativePlayers->size() <= 512) {
        faceEntries.reserve(nativePlayers->size());
        for (const auto& item : *nativePlayers) {
            const auto& nativeName = item.second.name;
            if (nativeName.empty() || nativeName.size() > 128 ||
                !readable(nativeName.data(), nativeName.size())) continue;
            auto plain = tablist::stripMinecraftFormatting(nativeName);
            if (!plain.empty()) faceEntries.try_emplace(std::move(plain), &item.second);
        }
    }
    rows.clear();
    rows.reserve(playerNames.size());
    for (const auto& nativeName : playerNames) {
        const auto length = nativeName.size();
        if (length == 0 || length > 128 || !readable(nativeName.data(), length)) continue;
        PlayerRow row;
        row.name = tablist::stripMinecraftFormatting(nativeName);
        if (row.name.empty()) continue;
        if (murderDetect) {
            const auto role = murderRoles.find(row.name);
            if (role != murderRoles.end()) row.murderRole = role->second;
        }
        row.spectator = gamemodeCheck && spectatorPlayers.contains(row.name);
        if (sortByNameColor) {
            bool foundColor = false;
            bool bestWasExact = false;
            std::uint64_t bestSequence = 0;
            for (const auto& actorTag : actorTags) {
                const auto color = tablist::actorNameColorForPlayerName(row.name, actorTag.nameTag);
                if (!color) continue;
                const bool exact = true;
                if (foundColor && (exact < bestWasExact ||
                    (exact == bestWasExact && actorTag.sequence <= bestSequence))) continue;
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
        if (includeFaces) {
            const auto face = faceEntries.find(row.name);
            if (face != faceEntries.end()) extractFace(*face->second, row);
        }
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
                 bool sortByNameColor, bool gamemodeCheck,
                 bool murderDetect, bool debug,
                 const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
                 const std::unordered_set<std::string>& spectators,
                 const std::vector<ActorNameTagEntry>& actorTags) noexcept {
    // Keep SEH outside the function that owns C++ objects. A player joining or
    // leaving during the read cannot crash Minecraft; that frame is skipped.
    __try {
        return collectRowsUnsafe(rows, includeFaces, sortByNameColor, gamemodeCheck, murderDetect,
                                 debug, murderRoles, spectators, actorTags);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        rows.clear();
        return false;
    }
}

bool fillOneFaceUnsafe(void* level, PlayerRow* row) {
    if (!level || !row || row->name.empty()) return false;
    NativePlayerList* nativePlayers = nullptr;
    if (!safeRead(level, levelPlayerListOffset, nativePlayers) || !nativePlayers ||
        !readable(nativePlayers, sizeof(NativePlayerList)) || nativePlayers->size() > 512)
        return false;
    for (const auto& [_, entry] : *nativePlayers) {
        const auto& nativeName = entry.name;
        if (nativeName.empty() || nativeName.size() > 128 ||
            !readable(nativeName.data(), nativeName.size())) continue;
        if (tablist::stripMinecraftFormatting(nativeName) != row->name) continue;
        return extractFace(entry, *row);
    }
    return false;
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
    try {
        auto current = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
        if (!current || current->empty()) return;
        if (cachedFaceSource != current.get()) {
            cachedFaceSource = current.get();
            cachedFaceCursor = 0;
        }
        for (std::size_t scanned = 0; scanned < current->size(); ++scanned) {
            const auto index = cachedFaceCursor++ % current->size();
            if ((*current)[index].hasFace) continue;
            auto updated = std::make_shared<std::vector<PlayerRow>>(*current);
            if (!fillOneFaceSeh(levelInstance.load(std::memory_order_acquire),
                                &(*updated)[index])) return;
            std::lock_guard writeLock(cachedTabRowsWriteMutex);
            current = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
            if (!current || index >= current->size() || (*current)[index].name !=
                (*updated)[index].name) return;
            auto faceUpdated = std::make_shared<std::vector<PlayerRow>>(*current);
            (*faceUpdated)[index].face = (*updated)[index].face;
            (*faceUpdated)[index].hasFace = (*updated)[index].hasFace;
            std::atomic_store_explicit(&cachedTabRows,
                std::shared_ptr<const std::vector<PlayerRow>>(std::move(faceUpdated)),
                std::memory_order_release);
            return; // Exactly one native skin/face read per game tick.
        }
    } catch (...) {
        // Face extraction is optional; leave the cached row unchanged on error.
    }
}

void refreshTabRowsCache(const std::shared_ptr<const tablist::Config>& config) noexcept {
    if (!config || !config->enabled) return;
    try {
        std::vector<PlayerRow> rows;
        std::vector<ActorNameTagEntry> actorTags;
        std::unordered_map<std::string, tablist::MurderRole> murderRoles;
        std::unordered_set<std::string> spectators;
        std::unordered_map<std::string, std::string> heldItems;
        if (config->sortByNameColor) actorTags = snapshotSdkActorNameTags();
        if (config->murderDetect || config->gamemodeCheck) {
            std::lock_guard lock(murderDetectMutex);
            if (config->murderDetect) {
                pruneExpiredMurderRolesLocked(std::chrono::steady_clock::now());
                murderRoles = murderRolesByPlayer;
            }
            if (config->gamemodeCheck) spectators = spectatorPlayers;
        }
        if (!collectRows(rows, false, config->sortByNameColor, config->gamemodeCheck,
                         config->murderDetect, config->debug, murderRoles, spectators,
                         actorTags)) {
            return; // Keep the previous immutable snapshot on a transient read failure.
        }
        std::lock_guard writeLock(cachedTabRowsWriteMutex);
        if (config->murderDetect) {
            std::lock_guard stateLock(murderDetectMutex);
            heldItems = heldItemNamesByPlayer;
            for (auto& row : rows) {
                const auto item = heldItems.find(row.name);
                if (item != heldItems.end()) row.heldItemName = item->second;
            }
        }
        const auto previous = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
        if (config->showFaces && previous) {
            for (auto& row : rows) {
                const auto old = std::find_if(previous->begin(), previous->end(),
                    [&](const PlayerRow& candidate) { return candidate.name == row.name; });
                if (old != previous->end() && old->hasFace) {
                    row.face = old->face;
                    row.hasFace = true;
                }
            }
        }
        auto snapshot = std::make_shared<const std::vector<PlayerRow>>(std::move(rows));
        std::atomic_store_explicit(&cachedTabRows, std::move(snapshot), std::memory_order_release);
        if (!loggedTabRowsReady.exchange(true, std::memory_order_acq_rel))
            diagnostic("TabList row cache ready; renderer uses cached rows only");
    } catch (...) {
        // Cache construction is best-effort and never runs from the render callback.
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

struct ActorMurderSnapshot {
    bool isPlayer = false;
    std::string name;
    std::optional<bedrock::HeldItemInfo> item;
    bool hasGameType = false;
    bool spectator = false;
};

void readActorMurderSnapshot(void* actor,
                             std::string_view playerName,
                             ActorMurderSnapshot& snapshot,
                             bool readItem, bool readGameMode) {
    if (!actor) return;
    bool itemRead = false;
    if (!playerName.empty() && playerName.size() <= 128) {
        snapshot.name.assign(playerName);
        snapshot.isPlayer = true;
    } else if (readItem) {
        // Inspect one runtime Actor at a time. Check its equipment before doing
        // name/identity work so Actors with no held stack stay on the cheap path.
        snapshot.item = gameApi->actor(actor).getMainHand();
        itemRead = true;
        if (snapshot.item || readGameMode) {
            const auto identity = gameApi->actor(actor).getIdentity();
            snapshot.isPlayer = identity.isPlayer;
            if (snapshot.isPlayer) snapshot.name = identity.name;
        }
    } else {
        const auto identity = gameApi->actor(actor).getIdentity();
        snapshot.isPlayer = identity.isPlayer;
        if (snapshot.isPlayer) snapshot.name = identity.name;
    }
    if (readItem && !itemRead && snapshot.isPlayer)
        snapshot.item = gameApi->actor(actor).getMainHand();
    if (readGameMode) {
        std::int32_t gameType = -1;
        snapshot.hasGameType = snapshot.isPlayer && readActorGameType(actor, gameType);
        snapshot.spectator = snapshot.hasGameType && gameType == 3;
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
        // One bounded equipment read per tick across the exact Level runtime
        // list. Unresolved names are checked too; identity is requested only
        // when that Actor currently has an item (or gamemode checking needs it).
        constexpr std::size_t actorsPerTick = 1;
        const auto sampleCount = std::min(actorsPerTick, murderScanActors.size());
        for (std::size_t sample = 0; sample < sampleCount; ++sample) {
            if (murderScanCursor >= murderScanActors.size()) murderScanCursor = 0;
            const auto& tracked = murderScanActors[murderScanCursor];
            auto* actor = tracked.actor;
            const auto actorHandle = reinterpret_cast<std::uintptr_t>(actor);
            murderScanCursor = (murderScanCursor + 1) % murderScanActors.size();
            if (actorHandle) murderLastProcessedActor = actorHandle;
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
            if (!snapshot.name.empty()) {
                copied.name = std::move(snapshot.name);
            } else {
                std::ostringstream actorLabel;
                actorLabel << "unmatched@0x" << std::hex << actorHandle;
                copied.name = actorLabel.str();
            }
            copied.hasItem = item.has_value();
            copied.hasGameType = snapshot.hasGameType;
            copied.spectator = snapshot.spectator;
            if (item) {
                copied.identifier = item->identifier;
                copied.displayName = item->displayName;
                copied.count = item->count;
            }
            enqueueMurderSample(std::move(copied));
        }
    } catch (...) {
        // Item inspection is observational; never interfere with the game tick.
    }
}

void refreshTrackedMurderActors(void* level) noexcept {
    if (!gameApi || !level) return;
    try {
        if (murderScanLevel != level) {
            murderScanLevel = level;
            murderScanActors.clear();
            murderPlayerNames.clear();
            murderKnownPlayerNamesByActor.clear();
            murderScanCursor = 0;
            murderLastProcessedActor = 0;
            {
                std::lock_guard stateLock(murderDetectMutex);
                murderRolesByPlayer.clear();
                spectatorPlayers.clear();
                murderRoleExpiryByPlayer.clear();
                lastHeldItemByPlayer.clear();
                heldItemNamesByPlayer.clear();
            }
            murderActorSnapshotTick = 39; // First live roster is ready immediately.
            lastLoggedMurderPlayerCount = std::numeric_limits<std::size_t>::max();
            lastMurderRosterStatusLogMs = 0;
        }
        // Refresh the native Level actor list at most once every two seconds. Match
        // its handles against copied SDK NameTag snapshots; avoid calling the
        // native NameTag getter or SDK identity path for every Actor each tick.
        ++murderActorSnapshotTick;
        if (murderActorSnapshotTick < 40) return;
        murderActorSnapshotTick = 0;

        // Actor enumeration remains exclusively Level::getRuntimeActorList.
        auto actorHandles = tabListGameAccess.actorHandles(level);
        std::sort(actorHandles.begin(), actorHandles.end(),
            [](const void* left, const void* right) {
                return reinterpret_cast<std::uintptr_t>(left) <
                       reinterpret_cast<std::uintptr_t>(right);
            });
        actorHandles.erase(std::unique(actorHandles.begin(), actorHandles.end()),
                           actorHandles.end());

        std::vector<std::string> playerNames;
        for (const auto& name : gameApi->world(level).getPlayerNames()) {
            auto plainName = tablist::stripMinecraftFormatting(name);
            if (!plainName.empty() && plainName.size() <= 128)
                playerNames.push_back(std::move(plainName));
        }
        std::sort(playerNames.begin(), playerNames.end(),
            [](const std::string& left, const std::string& right) {
                return left.size() > right.size();
            });
        playerNames.erase(std::unique(playerNames.begin(), playerNames.end()),
                          playerNames.end());

        // These immutable SDK snapshots contain copied names and Actor handles
        // observed by the existing NameTag hook. Intersect them with the exact
        // runtime list instead of issuing native name reads for every entity.
        std::unordered_map<void*, std::string> playerNamesByActor;
        const auto actorTags = snapshotSdkActorNameTags();
        playerNamesByActor.reserve(actorTags.size());
        for (const auto& tag : actorTags) {
            if (!tag.actor || tag.nameTag.empty()) continue;
            auto playerName = matchPlayerNameFromTag(tag.nameTag, playerNames);
            if (!playerName.empty())
                playerNamesByActor.insert_or_assign(tag.actor, std::move(playerName));
        }

        std::vector<MurderActorEntry> trackedActors;
        trackedActors.reserve(actorHandles.size());
        std::unordered_map<void*, std::string> nextKnownPlayerNames;
        nextKnownPlayerNames.reserve(std::min(actorHandles.size(), playerNames.size()));
        std::size_t namedActorCount = 0;
        for (auto* actor : actorHandles) {
            auto player = playerNamesByActor.find(actor);
            if (player == playerNamesByActor.end()) {
                const auto known = murderKnownPlayerNamesByActor.find(actor);
                if (known != murderKnownPlayerNamesByActor.end())
                    player = playerNamesByActor.emplace(actor, known->second).first;
            }
            if (player != playerNamesByActor.end()) {
                trackedActors.push_back({actor, player->second});
                nextKnownPlayerNames.insert_or_assign(actor, player->second);
                ++namedActorCount;
            } else {
                trackedActors.push_back({actor, {}});
            }
        }
        murderKnownPlayerNamesByActor = std::move(nextKnownPlayerNames);
        if (!playerNames.empty() && !trackedActors.empty()) {
            std::vector<std::uintptr_t> handles;
            handles.reserve(trackedActors.size());
            for (const auto& actor : trackedActors)
                handles.push_back(reinterpret_cast<std::uintptr_t>(actor.actor));
            murderPlayerNames = std::move(playerNames);
            murderScanActors = std::move(trackedActors);
            murderScanCursor = tablist::nextActorScanIndex(handles, murderLastProcessedActor);
            const auto now = steadyMilliseconds();
            const bool playerCountChanged =
                murderPlayerNames.size() != lastLoggedMurderPlayerCount;
            if (playerCountChanged || lastMurderRosterStatusLogMs == 0 ||
                now - lastMurderRosterStatusLogMs >= 10000) {
                lastLoggedMurderPlayerCount = murderPlayerNames.size();
                lastMurderRosterStatusLogMs = now;
                diagnostic("MurderDetect roster updated: actors=", actorHandles.size(),
                           " namedActors=", namedActorCount,
                           " trackedActors=", murderScanActors.size(),
                           " players=", murderPlayerNames.size(),
                           " observedTags=", actorTags.size());
            }
        } else {
            const auto now = steadyMilliseconds();
            if (lastMurderRosterStatusLogMs == 0 ||
                now - lastMurderRosterStatusLogMs >= 5000) {
                diagnostic("MurderDetect waiting for live roster: actors=",
                           actorHandles.size(), " playerNames=", playerNames.size(),
                           " observedTags=", actorTags.size(),
                           " namedActors=", namedActorCount,
                           " trackedActors=", playerActors.size());
                lastMurderRosterStatusLogMs = now;
            }
        }
    } catch (...) {
        // Keep the last good roster if native enumeration transiently fails.
    }
}

void __fastcall onLevelSubTick(void* level) {
    RuntimeActorListTickScope actorListTickScope;
    recordLevel(level);
    if (auto original = originalLevelSubTick.load(std::memory_order_acquire)) original(level);
    // Poll at most one cached Actor per second as a fallback for hidden labels;
    // visible labels are captured immediately by the getter hook.
    static unsigned refreshTicks = 0;
    if (++refreshTicks >= 20) {
        refreshTicks = 0;
        if (gameApi) (void)gameApi->refreshCachedActorNameTags(1);
    }
    const auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    refreshOneCachedFace(config);
    static unsigned murderScanCadence = 0;
    if (config->murderDetect || config->gamemodeCheck) {
        refreshTrackedMurderActors(level);
        if (++murderScanCadence >= config->murderScanIntervalTicks) {
            murderScanCadence = 0;
            updateMurderDetection(config->murderDetect, config->gamemodeCheck);
        }
    } else {
        murderScanCadence = 0;
    }
    static unsigned rowsCacheTicks = 0;
    if (++rowsCacheTicks >= 20) {
        rowsCacheTicks = 0;
        refreshTabRowsCache(config);
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

void drawTabList(void*) {
    if (!loggedRenderer.exchange(true, std::memory_order_acq_rel))
        diagnostic("DXGI render callback active");
    // TabList is a passive HUD. Keep this invariant here as a safeguard when
    // built against an older overlay backend that defaults to a GUI cursor.
    auto& io = ImGui::GetIO();
    io.MouseDrawCursor = false;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    const auto config = std::atomic_load_explicit(&liveConfig, std::memory_order_acquire);
    if (!config->enabled) return;
    if (!tabHeld.load(std::memory_order_acquire)) return;
    if (!gameplayActive.load(std::memory_order_acquire)) return;
    const auto playersSnapshot = std::atomic_load_explicit(&cachedTabRows, std::memory_order_acquire);
    if (!playersSnapshot || playersSnapshot->empty()) return;
    const auto& players = *playersSnapshot;
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
            const float pixelSize = config->faceSize / 8.0f;
            for (std::size_t py = 0; py < 8; ++py) {
                std::size_t runStart = 0;
                while (runStart < 8) {
                    const auto value = players[i].face[py * 8 + runStart];
                    std::size_t runEnd = runStart + 1;
                    while (runEnd < 8 && players[i].face[py * 8 + runEnd] == value)
                        ++runEnd;
                    const ImU32 color = IM_COL32((value >> 24) & 0xFF,
                                                 (value >> 16) & 0xFF,
                                                 (value >> 8) & 0xFF, value & 0xFF);
                    draw->AddRectFilled(
                        {faceX + runStart * pixelSize, faceY + py * pixelSize},
                        {faceX + runEnd * pixelSize, faceY + (py + 1) * pixelSize}, color);
                    runStart = runEnd;
                }
            }
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
    stopMurderWorker();
    gameHooks.reset();
    gameApi.reset();
    originalKeyboardFeed.store(nullptr);
    originalGrabCursor.store(nullptr);
    originalReleaseCursor.store(nullptr);
    originalGetFov.store(nullptr);
    originalLevelSubTick.store(nullptr);
    originalActorGetNameTag.store(nullptr, std::memory_order_release);
    resolvedActorGetNameTag.store(nullptr, std::memory_order_release);
    runtimeActorListSlot.store(std::numeric_limits<std::size_t>::max(), std::memory_order_release);
    runtimeActorListTarget.store(nullptr, std::memory_order_release);
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
    const auto resolveActorNameTag = [&] {
        actorNameTag = resolver.resolve("Actor::getNameTag", true);
        if (actorNameTag &&
            (!actorNameTag->exact_build || actorNameTag->method != "exact-rva"))
            actorNameTag.reset();
    };
    const auto resolveRuntimeActorList = [](const bedrock::Resolver& source) {
        if (!source.exactBuild()) return false;
        const auto slot = source.resolveVirtualSlot("Level::getRuntimeActorList");
        const auto method = source.resolve("Level::getRuntimeActorList");
        if (!slot || !method || !method->exact_build || !method->verified ||
            method->method != "exact-rva") return false;
        tabListGameAccess.setRuntimeActorList(*slot,
            reinterpret_cast<void*>(method->address));
        return true;
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
    if (!actorNameTag || !runtimeActorListReady || !itemHoverNameReady) {
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
            if (!runtimeActorListReady)
                runtimeActorListReady = resolveRuntimeActorList(embeddedResolver);
            if (!itemHoverNameReady)
                itemHoverNameReady = resolveItemHoverName(embeddedResolver);
        } else {
            diagnostic("embedded exact-build profile unavailable: ", embeddedError);
        }
    }
    diagnostic("world Actor source: Level::getRuntimeActorList=",
               runtimeActorListReady ? "profile ready" : "unavailable",
               runtimeActorListReady ? " [exact vtable slot and ABI]" : "");
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

    if (startMurderWorker())
        diagnostic("MurderDetect worker ready [copied snapshots only; native reads stay on game thread]");
    else
        diagnostic("MurderDetect worker unavailable; copied-snapshot processing will use the game thread");

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

    gameApi = std::make_unique<bedrock::GameApi>(tabListGameAccess);
    gameHooks = std::make_unique<bedrock::GameHooks>(*gameApi);
    bool actorNameTagHookReady = false;
    if (actorNameTag) {
        diagnostic("Actor::getNameTag subscribe attempt: target=0x", std::hex,
                   actorNameTag->address, std::dec, " method=", actorNameTag->method,
                   " exact_build=", actorNameTag->exact_build,
                   " verified=", actorNameTag->verified,
                   " broker_owned=",
                   BDS_HookOwnsTarget(BDS_HOOK_ACTOR_GET_NAME_TAG,
                       reinterpret_cast<void*>(actorNameTag->address)));
        actorNameTagHookReady = gameHooks->installActorNameTag(resolver, error);
    }
    if (actorNameTagHookReady) {
        gameApi->Event.Enum.ActorNameTag.then([](const bedrock::ActorNameTagEvent& event) {
            if (!event.actor || event.nameTag.empty() || event.nameTag.size() > 512) return;
            try {
                captureQueriedActorNameTag(event.actor, std::string(event.nameTag));
            } catch (...) {
                // Cache updates are best-effort; never interfere with rendering.
            }
        });
        diagnostic("SDK GameHooks::ActorNameTag active; method=", actorNameTag->method,
                   " exact_build=", actorNameTag->exact_build,
                   " verified=", actorNameTag->verified);
    } else {
        diagnostic("SDK ActorNameTag hook unavailable; color sorting has no live Actor source",
                   error.empty() ? "" : ": ", error);
    }
    diagnostic("Actor NameTag color source: SDK hook ",
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

