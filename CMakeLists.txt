cmake_minimum_required(VERSION 3.24)
project(BedrockTabList LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(BEDROCK_SDK_REF "main" CACHE STRING "BedrockDynamicSDK branch/tag/commit")
set(BEDROCK_SDK_ENABLE_GUI ON CACHE BOOL "Enable the SDK in-game overlay" FORCE)
set(BEDROCK_SDK_BUILD_TOOLS OFF CACHE BOOL "Do not build SDK tools" FORCE)
FetchContent_Declare(
    bedrock_sdk
    GIT_REPOSITORY https://github.com/gamelist1990/BedrockDynamicSDK.git
    GIT_TAG ${BEDROCK_SDK_REF}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(bedrock_sdk)

add_library(tablist_core STATIC src/config.cpp src/model.cpp)
target_include_directories(tablist_core PUBLIC include)
target_link_libraries(tablist_core PUBLIC bedrock::symbols)
target_compile_features(tablist_core PUBLIC cxx_std_20)
if(MSVC)
    set_property(TARGET tablist_core PROPERTY MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(WIN32)
    # Match the EnTT revision used by the bundled Latite reference. The game
    # stores Actor equipment in its entity registry; only the hand container is
    # queried (never the player's full inventory).
    FetchContent_Declare(
        entt
        GIT_REPOSITORY https://github.com/skypjack/entt.git
        GIT_TAG fe8d7d78c4823e8a66a050bf86f5c6318cf76ce7
        GIT_SHALLOW FALSE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(entt)
    add_library(BedrockTabList SHARED src/minecraft_client.cpp)
    target_link_libraries(BedrockTabList PRIVATE tablist_core bedrock::gui_overlay
        bedrock::symbols bedrock::logging bedrock::hook_runtime EnTT::EnTT shell32 ole32)
    target_compile_features(BedrockTabList PRIVATE cxx_std_20)
    bedrock_enable_hook_runtime(BedrockTabList)
    if(MSVC)
        set_property(TARGET BedrockTabList PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(tablist_test tests/model_test.cpp)
    target_link_libraries(tablist_test PRIVATE tablist_core)
    if(MSVC)
        target_compile_options(tablist_test PRIVATE /UNDEBUG)
        set_property(TARGET tablist_test PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        target_compile_options(tablist_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME tablist_test COMMAND tablist_test)
endif()

