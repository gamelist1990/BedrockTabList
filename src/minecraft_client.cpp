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
#include <cstring>
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
using ItemStackHoverNameFn = std::string*(__fastcall*)(void*, std::string*);
struct RuntimeActorRecord { std::array<std::byte, 0x40> bytes{}; };
static_assert(sizeof(RuntimeActorRecord) == 0x40);
using RuntimeActorListFn = void*(__fastcall*)(void*, std::vector<RuntimeActorRecord>*);

// These layouts come from the matching Latite SDK revision and are used only
// after BedrockDynamicSDK has verified the exact 1.26.52.3 executable hash.
constexpr std::size_t clientMinecraftOffset = 0x1B0;
constexpr std::size_t minecraftGameSessionOffset = 0xC8;
constexpr std::size_t sessionReadyOffset = 0x28;
constexpr std::size_t sessionLevelControlOffset = 0x30;
constexpr std::size_t sessionLevelOffset = 0x40;
constexpr std::size_t levelPlayerListOffset = 0x4E0;
// Bedrock Lens/Latite layout hints for the current client family. These are
// guarded by readable-range checks and SEH; if validation fails, item scanning
// simply reports unavailable rather than dereferencing an untrusted pointer.
constexpr std::size_t playerSuppliesOffset = 0x5B8;
constexpr std::size_t suppliesSelectedSlotOffset = 0x10;
constexpr std::size_t suppliesInventoryOffset = 0xB8;
constexpr std::size_t inventoryGetItemVtableSlot = 7;
constexpr std::size_t itemStackItemPointerOffset = 0x08;
constexpr std::size_t itemStackCountOffset = 0x22;
constexpr std::size_t itemNamespacedIdStringOffset = 0x128;
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
    unsigned nameColorOrder = 28;
    bool hasNameColor = false;
    tablist::MurderRole murderRole = tablist::MurderRole::None;
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
std::atomic<ItemStackHoverNameFn> itemStackHoverName{nullptr};
std::atomic<void*> originalActorGetNameTag{nullptr};
std::atomic<void*> resolvedActorGetNameTag{nullptr};
BDS_HookToken actorNameTagHookToken{};
std::atomic<void*> runtimeActorListFunction{nullptr};
std::atomic<void*> resolvedRuntimeActorListFunction{nullptr};
std::atomic<int> runtimeActorPointerOffset{-1};
std::atomic<int> runtimeActorReceiverAdjustment{std::numeric_limits<int>::min()};
std::atomic<void*> runtimeActorReceiverLevel{nullptr};
struct ActorNameTagEntry {
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
std::unordered_map<std::string, std::string> lastHeldItemByPlayer;
std::unordered_set<std::string> loggedHeldItemIdentifiers;
std::vector<bedrock::Actor> murderScanActors;
std::size_t murderScanCursor = 0;
void* murderScanLevel = nullptr;
std::atomic<bool> loggedMurderActorReadFault{false};
std::atomic<bool> loggedFirstNameTag{false};
std::atomic<std::uint64_t> actorNameTagSequence{0};
std::atomic<std::int64_t> lastActorNamePollMs{0};
std::atomic<std::size_t> actorNamePollCursor{0};
bool copyActorNameTag(void* actor, std::string* output);
class TabListGameAccess final : public bedrock::GameAccess {
public:
    bool isActorValid(void* actor) const noexcept override;
    void* localPlayer() const noexcept override { return nullptr; }
    void* worldOf(void* actor) const noexcept override;
    std::optional<std::string> nameTag(void* actor) const override {
        std::string result;
        return copyActorNameTag(actor, &result) ? std::optional<std::string>{std::move(result)}
                                               : std::nullopt;
    }
    std::optional<bedrock::HeldItemInfo> heldItem(void* actor) const override;
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
std::atomic<bool> loggedActorListFailure{false};
std::atomic<bool> loggedActorListSuccess{false};
std::atomic<bool> loggedActorReceiverAdjustment{false};
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
                      std::size_t maximum = 256) noexcept {
    if (!object) return false;
    std::uintptr_t data = 0;
    std::size_t size = 0;
    std::size_t capacity = 0;
    if (!safeRead(object, 0, data) || !safeRead(object, 0x10, size) ||
        !safeRead(object, 0x18, capacity) || size > maximum || capacity < size)
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

void* callInventoryGetItem(void* inventory, int slot) noexcept {
    if (!inventory || slot < 0 || slot > 8) return nullptr;
    void** vtable = nullptr;
    if (!safeRead(inventory, 0, vtable) || !vtable ||
        !readable(vtable, sizeof(void*) * (inventoryGetItemVtableSlot + 1))) return nullptr;
    void* target = nullptr;
    if (!safeRead(vtable, sizeof(void*) * inventoryGetItemVtableSlot, target) ||
        !executable(target)) return nullptr;
    using GetItemFn = void*(__fastcall*)(void*, int);
    __try {
        return reinterpret_cast<GetItemFn>(target)(inventory, slot);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool callItemStackHoverName(void* itemStack, std::string* output) noexcept {
    const auto function = itemStackHoverName.load(std::memory_order_acquire);
    if (!itemStack || !output || !function || !executable(reinterpret_cast<void*>(function)))
        return false;
    __try {
        function(itemStack, output);
        return output->size() <= 256;
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
        if (gameApi) gameApi->clearObservedActors();
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

struct RuntimeActorReceiverProbe {
    void* receiver{};
    std::ptrdiff_t adjustment{};
    std::uintptr_t begin{};
    std::uintptr_t end{};
    std::uintptr_t capacity{};
    std::size_t count{};
    unsigned score{};
};

std::optional<RuntimeActorReceiverProbe> probeRuntimeActorReceiver(
    void* level, std::ptrdiff_t adjustment) noexcept {
    const auto levelAddress = reinterpret_cast<std::uintptr_t>(level);
    const auto receiverAddress = adjustment < 0
        ? levelAddress - static_cast<std::uintptr_t>(-adjustment)
        : levelAddress + static_cast<std::uintptr_t>(adjustment);
    if ((adjustment < 0 && receiverAddress > levelAddress) ||
        (adjustment >= 0 && receiverAddress < levelAddress)) return std::nullopt;
    auto* receiver = reinterpret_cast<void*>(receiverAddress);
    const auto managerAddress = receiverAddress + 0x2E8;
    if (managerAddress < receiverAddress) return std::nullopt;

    RuntimeActorReceiverProbe probe{};
    probe.receiver = receiver;
    probe.adjustment = adjustment;
    if (!safeRead(reinterpret_cast<void*>(managerAddress), 0x1A0, probe.begin) ||
        !safeRead(reinterpret_cast<void*>(managerAddress), 0x1A8, probe.end) ||
        !safeRead(reinterpret_cast<void*>(managerAddress), 0x1B0, probe.capacity))
        return std::nullopt;
    if (!probe.begin || probe.end < probe.begin || probe.capacity < probe.end ||
        (probe.begin & (alignof(void*) - 1)) != 0) return std::nullopt;
    const auto bytes = probe.end - probe.begin;
    const auto capacityBytes = probe.capacity - probe.begin;
    if (bytes == 0 || bytes % sizeof(RuntimeActorRecord) != 0 ||
        capacityBytes % sizeof(RuntimeActorRecord) != 0 ||
        bytes > 4096 * sizeof(RuntimeActorRecord) ||
        capacityBytes > 4096 * sizeof(RuntimeActorRecord) ||
        !readable(reinterpret_cast<const void*>(probe.begin), bytes)) return std::nullopt;
    probe.count = bytes / sizeof(RuntimeActorRecord);

    // Ghidra shows the helper entering a Windows critical section at
    // receiver + 0x370. Reject offsets whose adjacent bytes do not resemble
    // an initialized CRITICAL_SECTION before allowing the native call.
    const auto lockAddress = managerAddress + 0x88;
    std::uintptr_t debugInfo = 0, owner = 0, semaphore = 0, spinCount = 0;
    std::int32_t lockCount = 0, recursionCount = 0;
    if (lockAddress < managerAddress ||
        !safeRead(reinterpret_cast<void*>(lockAddress), 0, debugInfo) ||
        !safeRead(reinterpret_cast<void*>(lockAddress), 8, lockCount) ||
        !safeRead(reinterpret_cast<void*>(lockAddress), 12, recursionCount) ||
        !safeRead(reinterpret_cast<void*>(lockAddress), 16, owner) ||
        !safeRead(reinterpret_cast<void*>(lockAddress), 24, semaphore) ||
        !safeRead(reinterpret_cast<void*>(lockAddress), 32, spinCount)) return std::nullopt;
    if (lockCount < -1 || lockCount > 4096 || recursionCount < 0 ||
        recursionCount > 1024 || owner > UINT32_MAX || spinCount > 0x7FFFFFFFu)
        return std::nullopt;
    probe.score = 1;
    if (debugInfo && readable(reinterpret_cast<const void*>(debugInfo), sizeof(void*)))
        probe.score += 2;
    if (semaphore == 0 || readable(reinterpret_cast<const void*>(semaphore), sizeof(void*)))
        ++probe.score;
    return probe;
}

std::optional<RuntimeActorReceiverProbe> findRuntimeActorReceiver(void* level) noexcept {
    std::optional<RuntimeActorReceiverProbe> best;
    bool ambiguous = false;
    for (std::ptrdiff_t adjustment = -0x200; adjustment <= 0x200; adjustment += 8) {
        auto candidate = probeRuntimeActorReceiver(level, adjustment);
        if (!candidate) continue;
        if (!best || candidate->score > best->score) {
            best = std::move(candidate);
            ambiguous = false;
        } else if (candidate->score == best->score) {
            ambiguous = true;
        }
    }
    return ambiguous ? std::nullopt : best;
}

bool tryAcquireLevelPointerFromClient(void** output) noexcept;

int captureRuntimeActorException(EXCEPTION_POINTERS* exception,
                                 unsigned long* exceptionCode,
                                 std::uintptr_t* exceptionAddress) noexcept {
    if (exceptionCode && exception && exception->ExceptionRecord)
        *exceptionCode = exception->ExceptionRecord->ExceptionCode;
    if (exceptionAddress && exception && exception->ExceptionRecord)
        *exceptionAddress = reinterpret_cast<std::uintptr_t>(
            exception->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Keep native dispatch inside a tiny SEH boundary with no owning C++ objects
// on its stack. The candidate signature is only usable on an exact build.
bool invokeRuntimeActorList(void* level, void* function,
                            std::vector<RuntimeActorRecord>* actors,
                            unsigned long* exceptionCode,
                            std::uintptr_t* exceptionAddress) noexcept {
    if (exceptionCode) *exceptionCode = 0;
    if (exceptionAddress) *exceptionAddress = 0;
    if (!level || !actors || !readable(level, sizeof(void*)) || !executable(function)) return false;
    __try {
        reinterpret_cast<RuntimeActorListFn>(function)(level, actors);
        return true;
    } __except (captureRuntimeActorException(GetExceptionInformation(),
                                              exceptionCode, exceptionAddress)) {
        return false;
    }
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

std::optional<bedrock::HeldItemInfo> TabListGameAccess::heldItem(void* actor) const {
    if (!isActorValid(actor)) return std::nullopt;
    void* supplies = nullptr;
    int selectedSlot = -1;
    void* inventory = nullptr;
    if (!safeRead(actor, playerSuppliesOffset, supplies) || !supplies ||
        !readable(supplies, suppliesInventoryOffset + sizeof(void*)) ||
        !safeRead(supplies, suppliesSelectedSlotOffset, selectedSlot) ||
        selectedSlot < 0 || selectedSlot > 8 ||
        !safeRead(supplies, suppliesInventoryOffset, inventory) || !inventory)
        return std::nullopt;

    void* stack = callInventoryGetItem(inventory, selectedSlot);
    if (!stack || !readable(stack, itemStackCountOffset + sizeof(std::uint8_t)))
        return std::nullopt;

    bedrock::HeldItemInfo item;
    std::uint8_t count = 0;
    (void)safeRead(stack, itemStackCountOffset, count);
    item.count = count;
    void** itemReference = nullptr;
    void* nativeItem = nullptr;
    if (safeRead(stack, itemStackItemPointerOffset, itemReference) && itemReference)
        (void)safeRead(itemReference, 0, nativeItem);
    if (nativeItem && readable(static_cast<const std::byte*>(nativeItem) +
                               itemNamespacedIdStringOffset, 0x20)) {
        (void)copyNativeString(static_cast<const std::byte*>(nativeItem) +
                               itemNamespacedIdStringOffset, item.identifier);
        if (!item.identifier.empty() && item.identifier.find(':') == std::string::npos)
            item.identifier.insert(0, "minecraft:");
    }
    // Only use a hover-name function resolved from the exact executable build;
    // the adapter catches native faults and the scan is deliberately throttled.
    item.displayName = item.identifier;
    std::string localizedName;
    if (callItemStackHoverName(stack, &localizedName) && !localizedName.empty())
        item.displayName = std::move(localizedName);
    if (!item.valid()) return std::nullopt;
    return item;
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

void* actorPointerFromRecord(const RuntimeActorRecord& record, std::size_t offset) noexcept {
    if (offset + sizeof(void*) > record.bytes.size()) return nullptr;
    void* actor = nullptr;
    std::memcpy(&actor, record.bytes.data() + offset, sizeof(actor));
    return actor;
}

int discoverActorPointerOffset(const std::vector<RuntimeActorRecord>& records) {
    const auto sampleCount = std::min<std::size_t>(records.size(), 64);
    std::size_t bestCount = 0;
    int bestOffset = -1;
    for (std::size_t offset = 0; offset + sizeof(void*) <= sizeof(RuntimeActorRecord);
         offset += sizeof(void*)) {
        std::size_t matchingNames = 0;
        for (std::size_t i = 0; i < sampleCount; ++i) {
            auto* actor = actorPointerFromRecord(records[i], offset);
            if (!looksLikeActor(actor)) continue;
            std::string nameTag;
            if (copyActorNameTag(actor, &nameTag) && !nameTag.empty() && nameTag.size() <= 512 &&
                !tablist::stripMinecraftFormatting(nameTag).empty()) {
                ++matchingNames;
            }
        }
        if (matchingNames > bestCount) {
            bestCount = matchingNames;
            bestOffset = static_cast<int>(offset);
        }
    }
    return bestCount >= 2 ? bestOffset : -1;
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
                ActorNameTagEntry{nameTag, color, sequence});
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

void updateRuntimeActorNameTags(void* level) {
    auto* function = runtimeActorListFunction.load(std::memory_order_acquire);
    if (!function || !originalActorGetNameTag.load(std::memory_order_acquire)) return;
    const auto cachedAdjustment = runtimeActorReceiverAdjustment.load(std::memory_order_acquire);
    const bool cachedForThisLevel = runtimeActorReceiverLevel.load(std::memory_order_acquire) == level;
    auto receiver = !cachedForThisLevel || cachedAdjustment == std::numeric_limits<int>::min()
        ? findRuntimeActorReceiver(level)
        : probeRuntimeActorReceiver(level, cachedAdjustment);
    void* receiverLevel = level;
    // _subTick's `this` may be a derived-level subobject, while the SDK method
    // is declared on Level. Cross-check the Level pointer reached through the
    // live ClientInstance/session chain before declaring the exact-build
    // candidate unusable. The session path is already used for player-list
    // recovery, but normally is skipped while the tick hook is fresh.
    void* sessionLevel = nullptr;
    if (!receiver && tryAcquireLevelPointerFromClient(&sessionLevel) &&
        sessionLevel && sessionLevel != level) {
        receiver = findRuntimeActorReceiver(sessionLevel);
        receiverLevel = sessionLevel;
        if (receiver && !loggedActorReceiverAdjustment.exchange(true, std::memory_order_acq_rel))
            diagnostic("runtime Actor receiver validated via ClientInstance session Level; tick_level=0x",
                       std::hex, reinterpret_cast<std::uintptr_t>(level), " session_level=0x",
                       reinterpret_cast<std::uintptr_t>(sessionLevel), std::dec);
    }
    if (!receiver) {
        runtimeActorReceiverAdjustment.store(std::numeric_limits<int>::min(),
                                             std::memory_order_release);
        if (!loggedActorListFailure.exchange(true, std::memory_order_acq_rel))
            diagnostic("runtime Actor receiver validation failed; no unique Level adjustment has a valid lock and 0x40-byte actor vector; color sort inactive");
        return;
    }
    runtimeActorReceiverAdjustment.store(static_cast<int>(receiver->adjustment),
                                         std::memory_order_release);
    runtimeActorReceiverLevel.store(receiverLevel, std::memory_order_release);
    if (receiver->adjustment != 0 &&
        !loggedActorReceiverAdjustment.exchange(true, std::memory_order_acq_rel))
        diagnostic("runtime Actor receiver adjustment=0x", std::hex,
                   receiver->adjustment, std::dec, "; preflight records=", receiver->count);
    std::vector<RuntimeActorRecord> actors;
    unsigned long exceptionCode = 0;
    std::uintptr_t exceptionAddress = 0;
    if (!invokeRuntimeActorList(receiver->receiver, function, &actors,
                                &exceptionCode, &exceptionAddress)) {
        runtimeActorListFunction.store(nullptr, std::memory_order_release);
        if (!loggedActorListFailure.exchange(true, std::memory_order_acq_rel)) {
            const auto levelAddress = reinterpret_cast<std::uintptr_t>(receiver->receiver);
            const auto managerAddress = levelAddress + 0x2E8;
            const auto lockAddress = managerAddress + 0x88;
            std::array<std::uintptr_t, 3> actorVector{};
            std::array<std::uintptr_t, 5> criticalSection{};
            const bool vectorReadable = safeRead(reinterpret_cast<void*>(managerAddress), 0x1A0,
                                                 actorVector[0]) &&
                safeRead(reinterpret_cast<void*>(managerAddress), 0x1A8, actorVector[1]) &&
                safeRead(reinterpret_cast<void*>(managerAddress), 0x1B0, actorVector[2]);
            const bool lockReadable = safeRead(reinterpret_cast<void*>(lockAddress), 0,
                                               criticalSection);
            diagnostic("Level::getRuntimeActorList invocation raised SEH exception=0x",
                       std::hex, exceptionCode, " at=0x", exceptionAddress,
                       " level=0x", levelAddress, " manager=0x", managerAddress,
                       " lock=0x", lockAddress,
                       " lock_state=", lockReadable ? "readable" : "unreadable",
                       " lock_words=0x", criticalSection[0], ",0x", criticalSection[1],
                       ",0x", criticalSection[2], ",0x", criticalSection[3],
                       ",0x", criticalSection[4],
                       " actor_vector=", vectorReadable ? "readable" : "unreadable",
                       " begin=0x", actorVector[0], " end=0x", actorVector[1],
                       " capacity=0x", actorVector[2], std::dec,
                       "; actor color sort inactive");
        }
        return;
    }
    if (actors.size() > 4096 ||
        (actors.size() && !readable(actors.data(), actors.size() * sizeof(RuntimeActorRecord)))) {
        runtimeActorListFunction.store(nullptr, std::memory_order_release);
        if (!loggedActorListFailure.exchange(true, std::memory_order_acq_rel))
            diagnostic("Level::getRuntimeActorList returned invalid record buffer; records=",
                       actors.size(), " record_size=", sizeof(RuntimeActorRecord),
                       "; actor color sort inactive");
        return;
    }

    int actorOffset = runtimeActorPointerOffset.load(std::memory_order_acquire);
    if (actorOffset < 0) {
        actorOffset = discoverActorPointerOffset(actors);
        if (actorOffset >= 0) {
            runtimeActorPointerOffset.store(actorOffset, std::memory_order_release);
            diagnostic("runtime Actor record layout identified; Actor* at +0x",
                       std::hex, actorOffset, std::dec);
        } else if (!loggedActorListFailure.exchange(true, std::memory_order_acq_rel)) {
            diagnostic("Level::getRuntimeActorList returned ", actors.size(),
                       " 0x40-byte records, but no Actor* field matched Actor::getNameTag");
        }
        if (actorOffset < 0) return;
    }

    std::unordered_map<void*, ActorNameTagEntry> updated;
    updated.reserve(actors.size());
    for (const auto& record : actors) {
        void* actor = actorPointerFromRecord(record, static_cast<std::size_t>(actorOffset));
        if (!looksLikeActor(actor)) continue;
        std::string nameTag;
        if (!copyActorNameTag(actor, &nameTag) || nameTag.empty() || nameTag.size() > 512)
            continue;
        if (tablist::stripMinecraftFormatting(nameTag).empty()) continue;
        const auto color = tablist::minecraftNameColorOrder(nameTag);
        const auto sequence = actorNameTagSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
        updated.insert_or_assign(actor, ActorNameTagEntry{std::move(nameTag), color, sequence});
    }
    const auto nameTagCount = updated.size();
    {
        std::lock_guard lock(actorNameTagMutex);
        actorNameTags.swap(updated);
    }
    if (!loggedActorListSuccess.exchange(true, std::memory_order_acq_rel))
        diagnostic("World Actor list active; ", actors.size(), " actor(s), ",
                   nameTagCount, " NameTag(s) read");
}

bool levelIsFresh() noexcept {
    const auto heartbeat = levelHeartbeatMs.load(std::memory_order_acquire);
    return heartbeat > 0 && steadyMilliseconds() - heartbeat <= 1500;
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
                ActorNameTagEntry{item.nameTag, color, item.sequence});
        }
    };
    append(bedrock::NameTagSource::View);
    append(bedrock::NameTagSource::ActorGetter); // getter is newest/authoritative on duplicates
    result.reserve(byActor.size());
    for (auto& [_, entry] : byActor) result.push_back(std::move(entry));
    return result;
}

bool collectRowsUnsafe(std::vector<PlayerRow>& rows, bool includeFaces,
                       bool sortByNameColor,
                       bool murderDetect,
                       const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
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
            if (colorChanged) {
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
                 bool sortByNameColor,
                 bool murderDetect,
                 const std::unordered_map<std::string, tablist::MurderRole>& murderRoles,
                 const std::vector<ActorNameTagEntry>& actorTags) noexcept {
    // Keep SEH outside the function that owns C++ objects. A player joining or
    // leaving during the read cannot crash Minecraft; that frame is skipped.
    __try {
        return collectRowsUnsafe(rows, includeFaces, sortByNameColor, murderDetect,
                                 murderRoles, actorTags);
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

struct ActorMurderSnapshot {
    bool isPlayer = false;
    std::string name;
    std::optional<bedrock::HeldItemInfo> item;
};

void readActorMurderSnapshot(const bedrock::Actor& actor,
                             ActorMurderSnapshot& snapshot) {
    snapshot.isPlayer = actor.isPlayer();
    if (!snapshot.isPlayer) return;
    snapshot.name = actor.getName();
    if (snapshot.name.empty() || snapshot.name.size() > 128) return;
    snapshot.item = actor.getHeldItem();
}

// Keep native Actor/inventory access behind a per-actor SEH boundary. One stale
// cached Actor or invalid inventory layout must not take down Minecraft.
bool readActorMurderSnapshotSeh(const bedrock::Actor* actor,
                                ActorMurderSnapshot* snapshot) noexcept {
    if (!actor || !snapshot) return false;
    __try {
        readActorMurderSnapshot(*actor, *snapshot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void updateMurderDetection(bool debug) noexcept {
    // Native Minecraft objects are thread-affine. Only inspect one cached
    // Actor per game tick, and only while the player list is actually open.
    if (!gameApi || !tabHeld.load(std::memory_order_acquire)) return;
    try {
        void* level = levelInstance.load(std::memory_order_acquire);
        if (!level) return;
        if (murderScanLevel != level) {
            murderScanActors.clear();
            murderScanCursor = 0;
            murderScanLevel = level;
        }
        if (murderScanCursor >= murderScanActors.size()) {
            murderScanActors = gameApi->actors(level);
            murderScanCursor = 0;
        }
        if (murderScanActors.empty()) return;

        const auto& actor = murderScanActors[murderScanCursor++];
        ActorMurderSnapshot snapshot;
        if (!readActorMurderSnapshotSeh(&actor, &snapshot)) {
            if (!loggedMurderActorReadFault.exchange(true, std::memory_order_acq_rel))
                diagnostic("MurderDetect skipped an Actor whose native read raised an exception");
            return;
        }
        if (!snapshot.isPlayer || snapshot.name.empty() || snapshot.name.size() > 128)
            return;

        const auto& item = snapshot.item;
        const auto identifier = item ? item->identifier : std::string{};
        const auto displayName = item ? item->displayName : std::string{};
        std::string itemKey = identifier.empty() ? displayName : identifier;
        if (itemKey.size() > 256) return;
        std::transform(itemKey.begin(), itemKey.end(), itemKey.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto role = item ? tablist::murderRoleForHeldItem(identifier, displayName)
                               : tablist::MurderRole::None;

        bool firstSeen = false;
        bool changed = false;
        std::string previous;
        {
            std::lock_guard lock(murderDetectMutex);
            const auto old = lastHeldItemByPlayer.find(snapshot.name);
            changed = old == lastHeldItemByPlayer.end() || old->second != itemKey;
            if (old != lastHeldItemByPlayer.end()) previous = old->second;
            if (itemKey.empty()) lastHeldItemByPlayer.erase(snapshot.name);
            else {
                lastHeldItemByPlayer.insert_or_assign(snapshot.name, itemKey);
                firstSeen = loggedHeldItemIdentifiers.insert(itemKey).second;
            }
            if (role == tablist::MurderRole::None) murderRolesByPlayer.erase(snapshot.name);
            else murderRolesByPlayer.insert_or_assign(snapshot.name, role);
        }
        if (firstSeen) diagnostic("MurderDetect held-item candidate: ", itemKey);
        if (debug && changed && item) {
            diagnostic("MurderDetect actor item: player=", snapshot.name,
                       " item=", itemKey,
                       " display=", displayName,
                       " count=", item->count,
                       previous.empty() ? "" : " previous=", previous);
        }
    } catch (...) {
        // Item inspection is observational; never interfere with the game tick.
    }
}

void __fastcall onLevelSubTick(void* level) {
    recordLevel(level);
    if (auto original = originalLevelSubTick.load(std::memory_order_acquire)) original(level);
    // Poll at most one cached Actor per second as a fallback for hidden labels;
    // visible labels are captured immediately by the getter hook.
    static unsigned refreshTicks = 0;
    if (++refreshTicks >= 20) {
        refreshTicks = 0;
        if (gameApi) (void)gameApi->refreshCachedActorNameTags(1);
    }
    const auto config = liveConfig.load(std::memory_order_acquire);
    if (config->murderDetect) updateMurderDetection(config->debug);
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
    std::unordered_map<std::string, tablist::MurderRole> murderRoles;
    if (config->sortByNameColor) actorTags = snapshotSdkActorNameTags();
    if (config->murderDetect) {
        std::lock_guard lock(murderDetectMutex);
        murderRoles = murderRolesByPlayer;
    }
    if (!collectRows(players, config->showFaces, config->sortByNameColor,
                     config->murderDetect, murderRoles, actorTags) || players.empty()) {
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

    std::vector<float> widths(layout.columns, config->minimumColumnWidth);
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto column = i / config->maximumRows;
        const auto roleText = players[i].murderRole == tablist::MurderRole::Sheriff
            ? " [Sheriff]" : players[i].murderRole == tablist::MurderRole::Murderer
                ? " [Murderer]" : "";
        widths[column] = std::max(widths[column],
            ImGui::CalcTextSize(players[i].name.c_str()).x +
            ImGui::CalcTextSize(roleText).x +
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
        const ImU32 nameColor = players[i].hasNameColor && players[i].nameColorOrder <
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
    gameHooks.reset();
    gameApi.reset();
    originalKeyboardFeed.store(nullptr);
    originalGrabCursor.store(nullptr);
    originalReleaseCursor.store(nullptr);
    originalGetFov.store(nullptr);
    originalLevelSubTick.store(nullptr);
    originalActorGetNameTag.store(nullptr, std::memory_order_release);
    resolvedActorGetNameTag.store(nullptr, std::memory_order_release);
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
    std::optional<bedrock::Resolution> hoverName;
    const auto resolveActorNameTag = [&] {
        actorNameTag = resolver.resolve("Actor::getNameTag", true);
        if (actorNameTag &&
            (!actorNameTag->exact_build || actorNameTag->method != "exact-rva"))
            actorNameTag.reset();
    };
    const auto resolveHoverName = [&] {
        hoverName = resolver.resolve("ItemStackBase::getHoverName", true);
        if (hoverName && (!hoverName->exact_build || hoverName->method != "exact-rva"))
            hoverName.reset();
    };
    resolveActorNameTag();
    resolveHoverName();
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
                resolveHoverName();
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
    if (!actorNameTag || !hoverName) {
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
            if (!hoverName) {
                hoverName = embeddedResolver.resolve("ItemStackBase::getHoverName", true);
                if (hoverName && (!hoverName->exact_build ||
                    hoverName->method != "exact-rva"))
                    hoverName.reset();
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

    if (hoverName) {
        itemStackHoverName.store(reinterpret_cast<ItemStackHoverNameFn>(hoverName->address),
                                 std::memory_order_release);
        diagnostic("held-item display-name resolver ready [exact-build exact-rva]");
    } else {
        itemStackHoverName.store(nullptr, std::memory_order_release);
        diagnostic("held-item display-name resolver unavailable; IDs will be shown");
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
    runtimeActorListFunction.store(nullptr, std::memory_order_release);
    resolvedRuntimeActorListFunction.store(nullptr, std::memory_order_release);
    diagnostic("world Actor source: SDK ActorNameTag event=",
               actorNameTagHookReady ? "active" : "unavailable",
               " runtimeActorList=disabled");

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

