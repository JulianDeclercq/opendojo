cmake_minimum_required(VERSION 3.20)

project(OpenDojo
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "Tekken 8 practice-mode drill export/import — standalone DLL")

if(NOT WIN32)
    message(FATAL_ERROR "OpenDojo is Windows-only (Tekken 8 runs on Windows).")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Configure for x64. For example:\n"
        "    cmake -B build -A x64\n"
        "(Got ${CMAKE_SIZEOF_VOID_P}-byte pointers.)")
endif()

set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Static CRT — end users don't need the VC++ redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# FreeType — RmlUi's font engine needs it. Pinned to a recent stable.
# Disable everything that depends on system libs (PNG/zlib/harfbuzz/brotli)
# so a clean checkout builds without external prerequisites.
# ---------------------------------------------------------------------------
set(FT_DISABLE_ZLIB     ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON  CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON  CACHE BOOL "" FORCE)
FetchContent_Declare(
    freetype
    GIT_REPOSITORY https://github.com/freetype/freetype.git
    GIT_TAG        VER-2-13-3
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(freetype)
if(MSVC AND TARGET freetype)
    target_compile_options(freetype PRIVATE /W0)
endif()
# RmlUi looks for an imported `Freetype::Freetype` target (the find_package
# convention). FreeType's own CMakeLists exports it; if not, alias it.
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
# Pre-populate the find_package variables so RmlUi's find_package(Freetype)
# call below succeeds without searching the host system.
set(FREETYPE_FOUND        TRUE                                  CACHE BOOL   "" FORCE)
set(FREETYPE_INCLUDE_DIRS "${freetype_SOURCE_DIR}/include"      CACHE PATH   "" FORCE)
set(FREETYPE_LIBRARIES    freetype                              CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# RmlUi — HTML/CSS-styled UI library. Replaces ImGui as our menu surface.
# Build only Core; we provide our own DX12 renderer and Win32 backend.
# ---------------------------------------------------------------------------
set(RMLUI_BACKEND      "auto"   CACHE STRING "" FORCE)
set(RMLUI_SAMPLES      OFF      CACHE BOOL   "" FORCE)
set(RMLUI_LUA_BINDINGS OFF      CACHE BOOL   "" FORCE)
set(RMLUI_HARFBUZZ_SAMPLE OFF   CACHE BOOL   "" FORCE)
set(RMLUI_FONT_ENGINE  "freetype" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS  OFF      CACHE BOOL   "" FORCE)
FetchContent_Declare(
    rmlui
    GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
    GIT_TAG        6.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(rmlui)
if(MSVC)
    foreach(t rmlui_core rmlui_debugger)
        if(TARGET ${t})
            target_compile_options(${t} PRIVATE /W0)
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# MinHook — inline function detours.
# ---------------------------------------------------------------------------
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(minhook)
if(MSVC AND TARGET minhook)
    target_compile_options(minhook PRIVATE /W0)
endif()

# ---------------------------------------------------------------------------
# Proxy target
# ---------------------------------------------------------------------------
set(OPENDOJO_PROXY "dinput8" CACHE STRING
    "Which Windows DLL OpenDojo masquerades as. dinput8 is the default.")
set_property(CACHE OPENDOJO_PROXY PROPERTY STRINGS dinput8)

set(OPENDOJO_PROXY_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/proxy_${OPENDOJO_PROXY}.cpp")
set(OPENDOJO_PROXY_DEF "${CMAKE_CURRENT_SOURCE_DIR}/proxy_${OPENDOJO_PROXY}.def")

if(NOT EXISTS "${OPENDOJO_PROXY_SRC}" OR NOT EXISTS "${OPENDOJO_PROXY_DEF}")
    message(FATAL_ERROR
        "Unsupported OPENDOJO_PROXY=${OPENDOJO_PROXY}. Add proxy_${OPENDOJO_PROXY}.cpp "
        "and proxy_${OPENDOJO_PROXY}.def to the dll/ directory.")
endif()

# ---------------------------------------------------------------------------
# The DLL
# ---------------------------------------------------------------------------
add_library(opendojo SHARED
    src/main.cpp
    src/autosave.cpp
    src/commands.cpp
    src/config.cpp
    src/drill.cpp
    src/hotkeys.cpp
    src/log.cpp
    src/memory.cpp
    src/menu.cpp
    src/players.cpp
    src/render_hook.cpp
    src/rmlui_backend.cpp
    src/slot.cpp
    src/subsystems.cpp
    "${OPENDOJO_PROXY_SRC}")

target_include_directories(opendojo PRIVATE src)
target_link_libraries(opendojo PRIVATE
    rmlui_core
    minhook
    user32
    d3d12
    dxgi
    d3dcompiler)
target_link_options(opendojo PRIVATE "/DEF:${OPENDOJO_PROXY_DEF}")

set_target_properties(opendojo PROPERTIES
    OUTPUT_NAME "${OPENDOJO_PROXY}"
    PREFIX      "")

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS opendojo RUNTIME DESTINATION ".")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/"
        DESTINATION "opendojo/ui")

# ---------------------------------------------------------------------------
# Post-build deploy
# ---------------------------------------------------------------------------
set(OPENDOJO_DEPLOY_DIR
    "E:/Steam/steamapps/common/TEKKEN 8/Polaris/Binaries/Win64"
    CACHE PATH "Tekken 8 install dir to copy the built DLL into after each build")

if(OPENDOJO_DEPLOY_DIR AND EXISTS "${OPENDOJO_DEPLOY_DIR}")
    add_custom_command(TARGET opendojo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:opendojo>"
            "${OPENDOJO_DEPLOY_DIR}/$<TARGET_FILE_NAME:opendojo>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/ui"
            "${OPENDOJO_DEPLOY_DIR}/opendojo/ui"
        COMMENT "Deploying $<TARGET_FILE_NAME:opendojo> + assets to ${OPENDOJO_DEPLOY_DIR}"
        VERBATIM)
endif()
