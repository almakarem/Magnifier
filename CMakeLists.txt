cmake_minimum_required(VERSION 3.20)

project(Magnifier
    VERSION 0.1.4
    DESCRIPTION "Overlay magnifier for streamers (lens + full-screen, controller + hotkeys + CLI)"
    HOMEPAGE_URL "https://github.com/almakarem/Magnifier"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Project-wide settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# Allow vcpkg/system overrides; we do not require vcpkg.
include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)

option(MAGNIFIER_BUILD_TESTS    "Build GoogleTest unit tests"   ON)
option(MAGNIFIER_ENABLE_IMGUI   "Build with Dear ImGui settings UI" ON)
option(MAGNIFIER_WARNINGS_AS_ERRORS "Treat warnings as errors"  ON)

if(NOT WIN32)
    message(FATAL_ERROR "Magnifier is Windows-only.")
endif()

# Target Windows 10 1809 (0x0A00) — features beyond are runtime-detected.
add_compile_definitions(
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    NTDDI_VERSION=0x0A000006   # NTDDI_WIN10_RS5 (1809)
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
)

# ---------------------------------------------------------------------------
# Third-party deps (FetchContent — no vcpkg required)
# ---------------------------------------------------------------------------
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
    GIT_SHALLOW    TRUE
)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG        v3.4.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(tomlplusplus)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

if(MAGNIFIER_ENABLE_IMGUI)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.90.9
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )
    target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    set_target_properties(imgui PROPERTIES FOLDER "third_party")
endif()

if(MAGNIFIER_BUILD_TESTS)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# Helper: common warning / compile settings for our targets only
# ---------------------------------------------------------------------------
function(magnifier_target_setup tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
            /EHsc /MP
            $<$<CONFIG:Release>:/O2 /GL /Gy>
        )
        target_link_options(${tgt} PRIVATE
            $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
        )
        if(MAGNIFIER_WARNINGS_AS_ERRORS)
            target_compile_options(${tgt} PRIVATE /WX)
        endif()
        target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_property(TARGET ${tgt} PROPERTY VS_GLOBAL_RootNamespace Magnifier)
endfunction()

# ---------------------------------------------------------------------------
# Core library (everything testable lives here)
# ---------------------------------------------------------------------------
add_library(magnifier_core STATIC
    src/app/App.cpp
    src/app/App.h
    src/mag/MagController.cpp
    src/mag/MagController.h
    src/mag/StateModel.cpp
    src/mag/StateModel.h
    src/input/HotkeyManager.cpp
    src/input/HotkeyManager.h
    src/input/ControllerPoll.cpp
    src/input/ControllerPoll.h
    src/input/WgiGamepad.cpp
    src/input/WgiGamepad.h
    src/input/InputRouter.cpp
    src/input/InputRouter.h
    src/input/Actions.h
    src/ipc/IpcServer.cpp
    src/ipc/IpcServer.h
    src/ipc/CliParser.cpp
    src/ipc/CliParser.h
    src/ipc/Commands.h
    src/ipc/JsonFraming.cpp
    src/ipc/JsonFraming.h
    src/update/Updater.cpp
    src/update/Updater.h
    src/config/ConfigStore.cpp
    src/config/ConfigStore.h
    src/ui/TrayIcon.cpp
    src/ui/TrayIcon.h
    src/ui/SettingsWindow.cpp
    src/ui/SettingsWindow.h
    src/util/Log.cpp
    src/util/Log.h
    src/util/Crash.cpp
    src/util/Crash.h
    src/util/Paths.cpp
    src/util/Paths.h
    src/util/StringConv.cpp
    src/util/StringConv.h
    src/util/WinError.h
    src/util/Version.h.in
)

target_include_directories(magnifier_core PUBLIC src)
target_include_directories(magnifier_core PRIVATE app)
target_link_libraries(magnifier_core PUBLIC
    spdlog::spdlog
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    Magnification     # magnification.dll
    Xinput            # XInput controllers
    Dbghelp           # MiniDumpWriteDump
    Shell32           # tray icon
    Comctl32          # common controls v6
    Shlwapi
    Ole32
    User32
    Gdi32
    Advapi32
    Winhttp
    Ws2_32
    Httpapi           # http.sys for HTTP IPC
    Runtimeobject     # WinRT
    Windowsapp        # WinRT umbrella (Windows.Gaming.Input)
)
if(MAGNIFIER_ENABLE_IMGUI)
    target_link_libraries(magnifier_core PUBLIC imgui d3d11 dxgi d3dcompiler)
    target_compile_definitions(magnifier_core PUBLIC MAGNIFIER_HAVE_IMGUI=1)
else()
    target_compile_definitions(magnifier_core PUBLIC MAGNIFIER_HAVE_IMGUI=0)
endif()
magnifier_target_setup(magnifier_core)

# Generate version header
configure_file(src/util/Version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/Version.h @ONLY)
target_include_directories(magnifier_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(Magnifier WIN32
    src/main.cpp
    app/Magnifier.rc
)
target_link_libraries(Magnifier PRIVATE magnifier_core)
target_sources(Magnifier PRIVATE app/Magnifier.exe.manifest)
target_include_directories(Magnifier PRIVATE app)
# Bake the app icon into the .exe resource table when present. The .ico is
# optional - run scripts/make-icon.ps1 to generate it from app/icon.png.
# Without the file the build still succeeds and the tray uses the system
# default IDI_APPLICATION icon.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app/Magnifier.ico")
    target_compile_definitions(Magnifier PRIVATE HAVE_ICON)
    target_compile_definitions(magnifier_core PRIVATE HAVE_ICON)
endif()
set_target_properties(Magnifier PROPERTIES
    OUTPUT_NAME "Magnifier"
)
magnifier_target_setup(Magnifier)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(MAGNIFIER_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install + packaging (portable zip via CPack; MSI is built separately by WiX)
# ---------------------------------------------------------------------------
install(TARGETS Magnifier RUNTIME DESTINATION .)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/packaging/portable/config.default.toml
    ${CMAKE_CURRENT_SOURCE_DIR}/README.md
    ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
    DESTINATION .
)

set(CPACK_PACKAGE_NAME "Magnifier")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "Magnifier Project")
set(CPACK_PACKAGE_FILE_NAME "Magnifier-${PROJECT_VERSION}-win64")
set(CPACK_GENERATOR "ZIP")
include(CPack)
