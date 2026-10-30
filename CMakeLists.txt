cmake_minimum_required(VERSION 3.20)
project(dmc1_practice LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DMC1 HD is 64-bit: configure with -A x64")
endif()

include(FetchContent)

# Dear ImGui (draws the overlay). No CMakeLists of its own, so we list its files below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9)

# MinHook (hooks the game's Present call so we can draw each frame).
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4)

# nlohmann/json (reads the preset files).
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)

FetchContent_MakeAvailable(imgui json)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
endif()

add_library(minhook_static STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(dinput8 SHARED
    src/main.cpp
    src/proxy.cpp
    src/config.cpp
    src/watch.cpp
    src/hooks.cpp
    src/overlay.cpp
    src/log.cpp
    src/pb.cpp
    src/builder.cpp
    src/presets.cpp
    src/savefile.cpp
    src/menu.cpp
    src/input_block.cpp
    src/stat_fix.cpp
    src/reset.cpp
    src/dump.cpp
    src/criteria.cpp
    src/capture.cpp
    src/game_addrs.cpp
    src/backup.cpp
    src/steam_redirect.cpp
    src/mode.cpp
    src/dinput8.def
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp)

target_include_directories(dinput8 PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)

target_compile_definitions(dinput8 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(dinput8 PRIVATE nlohmann_json::nlohmann_json minhook_static d3d11 dxgi d3dcompiler dwmapi)

if(MSVC)
    # Static CRT: no extra redistributable needed on the player's PC.
    set_property(TARGET dinput8 minhook_static PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    target_link_options(dinput8 PRIVATE -static)
endif()

# Output is named dinput8.dll, matching the proxy-DLL approach of the camera mod.
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")
