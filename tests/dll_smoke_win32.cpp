#include <windows.h>
#include <chrono>
#include <thread>
#include <iostream>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 1;
    const auto module = LoadLibraryW(argv[1]);
    if (!module) { std::cerr << "LoadLibrary failed: " << GetLastError(); return 2; }
    const auto status = reinterpret_cast<unsigned(*)()>(GetProcAddress(module,"TabList_GetStatus"));
    const auto stop = reinterpret_cast<void(*)()>(GetProcAddress(module,"TabList_Stop"));
    if (!status || !stop) return 3;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(60);
    while(std::chrono::steady_clock::now()<deadline) {
        const auto value=status();
        if(value==4) { stop(); return 0; } // Failed: reject this non-Minecraft host.
        if(value==2) { std::cerr << "Accepted unrelated executable"; return 4; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 5;
    // Worker callbacks pin the DLL. Do not unload it during shutdown.
}
