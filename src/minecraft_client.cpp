#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <MinHook.h>
#include <bedrock/generated_symbols.hpp>
#include <bedrock/gui_overlay.hpp>
#include <bedrock/logger.hpp>
#include <bedrock/resolver.hpp>
#include <imgui.h>
#include "tablist/model.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
enum class Status : unsigned { NotStarted, Loading, Running, Stopped, Failed };

using KeyboardFeedFn = bool(__fastcall*)(int, bool);
using CursorFn = void(__fastcall*)(void*);
using GetFovFn = float(__fastcall*)(void*, float, void*, void*);

// These layouts come from the matching Latite SDK revision and are used only
// after BedrockDynamicSDK has verified the exact 1.26.52.3 executable hash.
constexpr std::size_t clientMinecraftOffset = 0x1B0;
constexpr std::size_t minecraftGameSessionOffset = 0xC8;
constexpr std::size_t sessionReadyOffset = 0x28;
constexpr std::size_t sessionLevelControlOffset = 0x30;
constexpr std::size_t sessionLevelOffset = 0x40;
constexpr std::size_t levelPlayerListOffset = 0x4E0;
constexpr int tabInputKey = 0x09;

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
    std::byte afterName[0xA8 - 0x18 - sizeof(std::string)];
};
static_assert(sizeof(std::string) == 0x20);
static_assert(sizeof(NativePlayerListEntry) == 0xA8);
using NativePlayerList = std::unordered_map<Uuid, NativePlayerListEntry, UuidHash>;

std::atomic<Status> status{Status::NotStarted};
std::atomic<bool> stopRequested{false};
std::atomic<bool> tabHeld{false};
std::atomic<bool> gameplayActive{false};
std::atomic<bool> cursorStateObserved{false};
std::atomic<void*> clientInstance{nullptr};
std::atomic<KeyboardFeedFn> originalKeyboardFeed{nullptr};
std::atomic<CursorFn> originalGrabCursor{nullptr};
std::atomic<CursorFn> originalReleaseCursor{nullptr};
std::atomic<GetFovFn> originalGetFov{nullptr};
std::array<void*, 4> hookTargets{};
bool ownsMinHook = false;

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

bool collectNamesUnsafe(std::vector<std::string>& names) {
    void* client = clientInstance.load(std::memory_order_acquire);
    if (!validateClient(client)) return false;

    void* minecraft = nullptr;
    void* session = nullptr;
    void* control = nullptr;
    void* level = nullptr;
    std::uint8_t ready = 0;
    std::uint8_t controlReady = 0;
    NativePlayerList* players = nullptr;
    if (!safeRead(client, clientMinecraftOffset, minecraft) || !minecraft ||
        !safeRead(minecraft, minecraftGameSessionOffset, session) || !session ||
        !safeRead(session, sessionReadyOffset, ready) || ready != 1 ||
        !safeRead(session, sessionLevelControlOffset, control) || !control ||
        !safeRead(control, 0, controlReady) || controlReady != 1 ||
        !safeRead(session, sessionLevelOffset, level) || !level ||
        !safeRead(level, levelPlayerListOffset, players) || !players ||
        !readable(players, sizeof(NativePlayerList))) return false;

    const auto count = players->size();
    if (count > 512) return false;
    names.clear();
    names.reserve(count);
    std::size_t visited = 0;
    for (const auto& item : *players) {
        if (++visited > 512) return false;
        const auto& nativeName = item.second.name;
        const auto length = nativeName.size();
        if (length == 0 || length > 128 || !readable(nativeName.data(), length)) continue;
        auto name = tablist::stripMinecraftFormatting(
            std::string_view(nativeName.data(), length));
        if (!name.empty()) names.push_back(std::move(name));
    }
    tablist::sortPlayerNames(names);
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return true;
}

bool collectNames(std::vector<std::string>& names) noexcept {
    // Keep SEH outside the function that owns C++ objects. A player joining or
    // leaving during the read cannot crash Minecraft; that frame is skipped.
    __try {
        return collectNamesUnsafe(names);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        names.clear();
        return false;
    }
}

bool __fastcall onKeyboardFeed(int inputKey, bool down) {
    if (inputKey == tabInputKey) tabHeld.store(down, std::memory_order_release);
    if (auto original = originalKeyboardFeed.load(std::memory_order_acquire))
        return original(inputKey, down);
    return false;
}

void __fastcall onGrabCursor(void* self) {
    if (auto original = originalGrabCursor.load(std::memory_order_acquire)) original(self);
    if (validateClient(self)) clientInstance.store(self, std::memory_order_release);
    cursorStateObserved.store(true, std::memory_order_release);
    gameplayActive.store(true, std::memory_order_release);
}

void __fastcall onReleaseCursor(void* self) {
    if (validateClient(self)) clientInstance.store(self, std::memory_order_release);
    cursorStateObserved.store(true, std::memory_order_release);
    gameplayActive.store(false, std::memory_order_release);
    tabHeld.store(false, std::memory_order_release);
    if (auto original = originalReleaseCursor.load(std::memory_order_acquire)) original(self);
}

float __fastcall onGetFov(void* self, float inputFov, void* a3, void* a4) {
    if (!cursorStateObserved.load(std::memory_order_acquire))
        gameplayActive.store(true, std::memory_order_release);
    if (auto original = originalGetFov.load(std::memory_order_acquire))
        return original(self, inputFov, a3, a4);
    return inputFov;
}

void drawTabList(void*) {
    if (!tabHeld.load(std::memory_order_acquire) ||
        !gameplayActive.load(std::memory_order_acquire)) return;

    std::vector<std::string> names;
    if (!collectNames(names) || names.empty()) return;
    const auto layout = tablist::layoutFor(names.size());
    auto* draw = ImGui::GetForegroundDrawList();
    const auto display = ImGui::GetIO().DisplaySize;
    constexpr float rowHeight = 24.0f;
    constexpr float headerHeight = 32.0f;
    constexpr float horizontalPadding = 14.0f;
    constexpr float columnGap = 10.0f;

    std::vector<float> widths(layout.columns, 150.0f);
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto column = i / 16;
        widths[column] = std::max(widths[column],
            ImGui::CalcTextSize(names[i].c_str()).x + horizontalPadding * 2.0f);
    }
    float totalWidth = 0.0f;
    for (float width : widths) totalWidth += width;
    totalWidth += columnGap * static_cast<float>(layout.columns - 1);
    const float totalHeight = headerHeight + rowHeight * static_cast<float>(layout.rows);
    const ImVec2 topLeft{(display.x - totalWidth) * 0.5f, 22.0f};
    const ImVec2 bottomRight{topLeft.x + totalWidth, topLeft.y + totalHeight};

    draw->AddRectFilled(topLeft, bottomRight, IM_COL32(10, 13, 20, 205), 7.0f);
    draw->AddRect(topLeft, bottomRight, IM_COL32(110, 145, 220, 170), 7.0f,
                  ImDrawFlags_None, 1.2f);
    const std::string title = "Players  " + std::to_string(names.size());
    const auto titleSize = ImGui::CalcTextSize(title.c_str());
    draw->AddText({topLeft.x + (totalWidth - titleSize.x) * 0.5f,
                   topLeft.y + (headerHeight - titleSize.y) * 0.5f},
                  IM_COL32(225, 232, 247, 255), title.c_str());
    draw->AddLine({topLeft.x + 8.0f, topLeft.y + headerHeight},
                  {bottomRight.x - 8.0f, topLeft.y + headerHeight},
                  IM_COL32(110, 145, 220, 95));

    std::vector<float> columnX(layout.columns, topLeft.x);
    for (std::size_t column = 1; column < layout.columns; ++column)
        columnX[column] = columnX[column - 1] + widths[column - 1] + columnGap;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto column = i / 16;
        const auto row = i % 16;
        const float y = topLeft.y + headerHeight + rowHeight * static_cast<float>(row);
        if ((row & 1U) != 0)
            draw->AddRectFilled({columnX[column], y},
                                {columnX[column] + widths[column], y + rowHeight},
                                IM_COL32(255, 255, 255, 10));
        draw->AddCircleFilled({columnX[column] + 10.0f, y + rowHeight * 0.5f},
                              2.5f, IM_COL32(100, 220, 155, 255));
        const auto textSize = ImGui::CalcTextSize(names[i].c_str());
        draw->AddText({columnX[column] + horizontalPadding + 4.0f,
                       y + (rowHeight - textSize.y) * 0.5f},
                      IM_COL32(242, 245, 251, 255), names[i].c_str());
    }
}

template <class Original>
bool createHook(void* target, void* detour, std::atomic<Original>& original) {
    Original captured = nullptr;
    if (MH_CreateHook(target, detour, reinterpret_cast<void**>(&captured)) != MH_OK)
        return false;
    original.store(captured, std::memory_order_release);
    return true;
}

void removeNativeHooks() noexcept {
    for (void* target : hookTargets) if (target) MH_DisableHook(target);
    for (void* target : hookTargets) if (target) MH_RemoveHook(target);
    hookTargets.fill(nullptr);
    originalKeyboardFeed.store(nullptr);
    originalGrabCursor.store(nullptr);
    originalReleaseCursor.store(nullptr);
    originalGetFov.store(nullptr);
    clientInstance.store(nullptr);
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
    bedrock::Resolver resolver;
    std::string error;
    if (!resolver.loadJson(bedrock::generated::kSymbolDatabase, error) ||
        !resolver.attachWindowsMainModule(error) || !resolver.exactBuild()) {
        logger.error("Unsupported Minecraft executable: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    const auto keyboard = resolver.resolve("Keyboard::feed");
    const auto grab = resolver.resolve("ClientInstance::grabCursor", true);
    const auto release = resolver.resolve("ClientInstance::releaseCursor", true);
    const auto fov = resolver.resolve("LevelRendererPlayer::getFov");
    const auto exact = [](const auto& symbol) {
        return symbol && symbol->exact_build && symbol->method == "exact-rva";
    };
    if (!exact(keyboard) || !keyboard->verified || !exact(grab) || !exact(release) ||
        !exact(fov) || !fov->verified) {
        logger.error("Required native input/render symbols are unavailable");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }
    ownsMinHook = initialized == MH_OK;
    hookTargets = {reinterpret_cast<void*>(keyboard->address),
                   reinterpret_cast<void*>(grab->address),
                   reinterpret_cast<void*>(release->address),
                   reinterpret_cast<void*>(fov->address)};
    const bool created =
        createHook(hookTargets[0], reinterpret_cast<void*>(&onKeyboardFeed), originalKeyboardFeed) &&
        createHook(hookTargets[1], reinterpret_cast<void*>(&onGrabCursor), originalGrabCursor) &&
        createHook(hookTargets[2], reinterpret_cast<void*>(&onReleaseCursor), originalReleaseCursor) &&
        createHook(hookTargets[3], reinterpret_cast<void*>(&onGetFov), originalGetFov);
    bool queued = created;
    if (queued) for (void* target : hookTargets)
        queued = queued && MH_QueueEnableHook(target) == MH_OK;
    if (!queued || MH_ApplyQueued() != MH_OK) {
        removeNativeHooks();
        if (ownsMinHook) MH_Uninitialize();
        ownsMinHook = false;
        logger.error("Could not enable native TabList hooks");
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    bedrock::gui::OverlayOptions options;
    options.toggleVirtualKey = 0;
    options.initiallyVisible = true;
    options.captureInput = false;
    options.nativeCursor = false;
    if (!bedrock::gui::Overlay::install(&drawTabList, nullptr, error, options)) {
        removeNativeHooks();
        if (ownsMinHook) MH_Uninitialize();
        ownsMinHook = false;
        logger.error("Could not install in-game renderer: ", error);
        status.store(Status::Failed, std::memory_order_release);
        return 1;
    }

    logger.info("Minecraft native TabList ready; hold Tab in a world");
    status.store(Status::Running, std::memory_order_release);
    while (!stopRequested.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bedrock::gui::Overlay::shutdown();
    removeNativeHooks();
    if (ownsMinHook) MH_Uninitialize();
    ownsMinHook = false;
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

