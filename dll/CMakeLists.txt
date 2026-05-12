cmake_minimum_required(VERSION 3.20)

project(OpenDojo
    VERSION     0.1.0
    LANGUAGES   CXX
    DESCRIPTION "Tekken 8 practice-mode drill export/import — standalone DLL")

# ---------------------------------------------------------------------------
# Sanity: Tekken 8 is Windows x64 only. Refuse to configure for anything else.
# ---------------------------------------------------------------------------
if(NOT WIN32)
    message(FATAL_ERROR "OpenDojo is Windows-only (Tekken 8 runs on Windows).")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Configure for x64. For example:\n"
        "    cmake -B build -A x64\n"
        "(Got ${CMAKE_SIZEOF_VOID_P}-byte pointers.)")
endif()

# ---------------------------------------------------------------------------
# Build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD          20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# Static CRT — end users don't need the VC++ redistributable installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8)
endif()

# ---------------------------------------------------------------------------
# Proxy target. Which Windows DLL we masquerade as. dinput8 is the standard
# fighting-game modding entry point. version is the universal fallback if we
# ever need it (every Win32 process imports from version.dll).
#
# The .def and .cpp files are picked up by name: proxy_<target>.def / .cpp.
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
    src/commands.cpp
    src/drill.cpp
    src/hotkeys.cpp
    src/log.cpp
    src/memory.cpp
    src/slot.cpp
    src/subsystems.cpp
    "${OPENDOJO_PROXY_SRC}")

target_include_directories(opendojo PRIVATE src)
target_link_libraries(opendojo PRIVATE user32)
target_link_options(opendojo PRIVATE "/DEF:${OPENDOJO_PROXY_DEF}")

# Output filename = the proxy DLL name we're impersonating, no "lib" prefix.
set_target_properties(opendojo PROPERTIES
    OUTPUT_NAME "${OPENDOJO_PROXY}"
    PREFIX      "")

# ---------------------------------------------------------------------------
# Install: convenience target so iteration is one command. Defaults to
# <build>/install — override with -DCMAKE_INSTALL_PREFIX="<game>\Polaris\..."
# to copy directly into the game folder.
# ---------------------------------------------------------------------------
install(TARGETS opendojo RUNTIME DESTINATION ".")

# ---------------------------------------------------------------------------
# Post-build deploy: copy the freshly built DLL into the Tekken game folder
# so `cmake --build` lands a working drop in one step. Skipped if the path
# doesn't exist (e.g. on a clean checkout where Tekken isn't installed at
# this exact path). Override with -DOPENDOJO_DEPLOY_DIR=... at configure time.
#
# If Tekken is running, the copy will fail and so will the build — that's
# the intended signal to "close the game before redeploying."
# ---------------------------------------------------------------------------
set(OPENDOJO_DEPLOY_DIR
    "E:/Steam/steamapps/common/TEKKEN 8/Polaris/Binaries/Win64"
    CACHE PATH "Tekken 8 install dir to copy the built DLL into after each build")

if(OPENDOJO_DEPLOY_DIR AND EXISTS "${OPENDOJO_DEPLOY_DIR}")
    add_custom_command(TARGET opendojo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:opendojo>"
            "${OPENDOJO_DEPLOY_DIR}/$<TARGET_FILE_NAME:opendojo>"
        COMMENT "Deploying $<TARGET_FILE_NAME:opendojo> to ${OPENDOJO_DEPLOY_DIR}"
        VERBATIM)
endif()
