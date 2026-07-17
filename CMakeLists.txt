cmake_minimum_required(VERSION 3.25)

if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(GravitarisNG CXX C)

# Corrade v2020.06 (and a few other deps) use cmake_minimum_required(VERSION 2.x)
# which CMake 3.27+ rejects. This variable, also supported in 3.27+, instructs
# CMake to accept any minimum version >= 3.5 in subdirectories.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Windows min/max macros from <windows.h> break Magnum and other deps.
# Define NOMINMAX globally so they are never injected.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Dependency versions — update these to pin to specific releases
# ---------------------------------------------------------------------------
set(GT_CORRADE_TAG    "master")
set(GT_MAGNUM_TAG     "master")
set(GT_SDL2_TAG       "release-2.28.5")
set(GT_CHIPMUNK_TAG   "Chipmunk-7.0.3")
set(GT_PHYSFS_TAG     "release-3.2.0")
set(GT_FLECS_TAG      "v4.1.6")
set(GT_YAMLCPP_TAG    "0.8.0")
set(GT_FREETYPE_TAG   "VER-2-13-2")
set(GT_RMLUI_TAG      "6.0")
set(GT_POLY2TRI_TAG   "master")
set(GT_IMGUI_TAG      "v1.91.5")
set(GT_MAGNUMINT_TAG  "master")
set(GT_OPENALSOFT_TAG "1.23.1")

# ---------------------------------------------------------------------------
# Corrade — must be fetched before Magnum; OVERRIDE_FIND_PACKAGE so that
# Magnum's internal find_package(Corrade) resolves to the fetched copy.
# ---------------------------------------------------------------------------
FetchContent_Declare(Corrade
    GIT_REPOSITORY  https://github.com/mosra/corrade.git
    GIT_TAG         ${GT_CORRADE_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Corrade)

# CORRADE_LIB_SUFFIX_MODULE is normally set by FindCorrade.cmake, but
# OVERRIDE_FIND_PACKAGE skips running that script. Magnum v2020.06 line 330
# does include(${CORRADE_LIB_SUFFIX_MODULE}), so we must set it manually.
set(CORRADE_LIB_SUFFIX_MODULE "${corrade_SOURCE_DIR}/modules/CorradeLibSuffix.cmake")
list(APPEND CMAKE_MODULE_PATH "${corrade_SOURCE_DIR}/modules")

# OVERRIDE_FIND_PACKAGE also bypasses FindCorrade.cmake which normally sets
# CORRADE_TARGET_* CMake variables. Magnum's Platform CMakeLists.txt depends on
# these to decide which context objects to build (e.g. MagnumWglContextObjects
# that provides flextGLInit on Windows). Set them explicitly here so Magnum
# inherits the correct values when it is added as a subdirectory.
if(WIN32)
    set(CORRADE_TARGET_WINDOWS TRUE)
elseif(APPLE)
    set(CORRADE_TARGET_APPLE TRUE)
elseif(UNIX)
    set(CORRADE_TARGET_UNIX TRUE)
endif()
if(EMSCRIPTEN)
    set(CORRADE_TARGET_EMSCRIPTEN TRUE)
endif()

# ---------------------------------------------------------------------------
# SDL2 — fetched before Magnum; OVERRIDE_FIND_PACKAGE so Magnum's
# find_package(SDL2) resolves to the fetched copy.
# ---------------------------------------------------------------------------
FetchContent_Declare(SDL2
    GIT_REPOSITORY  https://github.com/libsdl-org/SDL.git
    GIT_TAG         ${GT_SDL2_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
# Avoid an "uninstall" custom target name clash with PhysFS, which also
# defines one when fetched as a sibling subdirectory.
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# Same OVERRIDE_FIND_PACKAGE no-op issue as Freetype: Magnum's internal
# find_package(SDL2) won't set SDL2_FOUND, so Sdl2Application gets skipped.
set(SDL2_FOUND TRUE)

# ---------------------------------------------------------------------------
# Freetype — fetched before RmlUi; OVERRIDE_FIND_PACKAGE so RmlUi's
# find_package(Freetype) resolves to the fetched copy.
# ---------------------------------------------------------------------------
FetchContent_Declare(Freetype
    GIT_REPOSITORY  https://github.com/freetype/freetype.git
    GIT_TAG         ${GT_FREETYPE_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Freetype)

# Freetype's EXPORT_NAME "Freetype::Freetype" only applies to its install
# targets, not the in-build target. Create the alias manually so that
# consumers (RmlUi) that look for TARGET Freetype::Freetype find it.
# Also set FREETYPE_FOUND because OVERRIDE_FIND_PACKAGE no-ops subsequent
# find_package(Freetype) calls without setting it.
if(TARGET freetype AND NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()
set(FREETYPE_FOUND TRUE)

# ---------------------------------------------------------------------------
# OpenAL Soft — fetched before Magnum; Magnum's FindOpenAL detects the
# subproject `OpenAL` target directly. Bundled (rather than Apple's system
# OpenAL.framework) so EFX effects behave identically on every platform.
# STATIC is load-bearing on Windows: a shared build produces OpenAL32.dll,
# whose name collides with the legacy Creative router many machines have in
# System32 — whichever the loader picks wins, and the router crashes against
# a modern context. Static linking removes the name from the equation.
# ---------------------------------------------------------------------------
set(LIBTYPE STATIC)
# Magnum's internal MagnumAudioObjects object-library pulls only OpenAL's
# include dirs (not its INTERFACE_COMPILE_DEFINITIONS), so the static-linkage
# define has to be applied globally or its objects keep dllimport AL symbols.
add_compile_definitions(AL_LIBTYPE_STATIC)
set(ALSOFT_UTILS    OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(OpenALSoft
    GIT_REPOSITORY  https://github.com/kcat/openal-soft.git
    GIT_TAG         ${GT_OPENALSOFT_TAG}
    GIT_SHALLOW     TRUE
)
FetchContent_MakeAvailable(OpenALSoft)

# ---------------------------------------------------------------------------
# Magnum — requires Corrade and SDL2 to already be available
# ---------------------------------------------------------------------------
# v2020.06 used WITH_*, master uses MAGNUM_WITH_* — set both for safety
set(WITH_SDL2APPLICATION        ON  CACHE BOOL "" FORCE)
set(MAGNUM_WITH_SDL2APPLICATION ON  CACHE BOOL "" FORCE)
set(WITH_GL                     ON  CACHE BOOL "" FORCE)
set(MAGNUM_WITH_GL              ON  CACHE BOOL "" FORCE)
set(WITH_SHADERS                ON  CACHE BOOL "" FORCE)
set(MAGNUM_WITH_SHADERS         ON  CACHE BOOL "" FORCE)
set(WITH_AUDIO                  ON  CACHE BOOL "" FORCE)
set(MAGNUM_WITH_AUDIO           ON  CACHE BOOL "" FORCE)
# Disable unused modules to keep configure/build time reasonable
set(WITH_DEBUGTOOLS       OFF CACHE BOOL "" FORCE)
set(MAGNUM_WITH_DEBUGTOOLS OFF CACHE BOOL "" FORCE)
set(WITH_MESHTOOLS        OFF CACHE BOOL "" FORCE)
set(MAGNUM_WITH_MESHTOOLS  OFF CACHE BOOL "" FORCE)
set(WITH_PRIMITIVES       OFF CACHE BOOL "" FORCE)
set(MAGNUM_WITH_PRIMITIVES OFF CACHE BOOL "" FORCE)
set(WITH_SCENEGRAPH       OFF CACHE BOOL "" FORCE)
set(MAGNUM_WITH_SCENEGRAPH OFF CACHE BOOL "" FORCE)
set(WITH_TEXT             OFF CACHE BOOL "" FORCE)
set(MAGNUM_WITH_TEXT       OFF CACHE BOOL "" FORCE)
set(WITH_TEXTURETOOLS     OFF CACHE BOOL "" FORCE)
set(MAGNUM_WITH_TEXTURETOOLS OFF CACHE BOOL "" FORCE)
set(WITH_TRADE            OFF CACHE BOOL "" FORCE)
set(MAGNUM_WITH_TRADE      OFF CACHE BOOL "" FORCE)

FetchContent_Declare(Magnum
    GIT_REPOSITORY  https://github.com/mosra/magnum.git
    GIT_TAG         ${GT_MAGNUM_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(Magnum)

# ---------------------------------------------------------------------------
# Chipmunk2D
# ---------------------------------------------------------------------------
set(BUILD_DEMOS     OFF CACHE BOOL "" FORCE)
set(INSTALL_DEMOS   OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(Chipmunk2D
    GIT_REPOSITORY  https://github.com/slembcke/Chipmunk2D.git
    GIT_TAG         ${GT_CHIPMUNK_TAG}
    GIT_SHALLOW     TRUE
)
FetchContent_MakeAvailable(Chipmunk2D)

# ---------------------------------------------------------------------------
# PhysFS
# ---------------------------------------------------------------------------
set(PHYSFS_BUILD_STATIC     ON  CACHE BOOL "" FORCE)
set(PHYSFS_BUILD_SHARED     OFF CACHE BOOL "" FORCE)
set(PHYSFS_BUILD_DOCS       OFF CACHE BOOL "" FORCE)
set(PHYSFS_BUILD_TEST       OFF CACHE BOOL "" FORCE)

FetchContent_Declare(physfs
    GIT_REPOSITORY  https://github.com/icculus/physfs.git
    GIT_TAG         ${GT_PHYSFS_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(physfs)

# PhysFS forces CMAKE_SKIP_RPATH ON as a cache variable, which would disable
# RPATH handling for the whole project (breaking our own executable's ability
# to find the fetched shared libs at runtime). Restore it.
set(CMAKE_SKIP_RPATH OFF CACHE BOOL "Skip RPATH" FORCE)

# ---------------------------------------------------------------------------
# flecs — the ECS (replaced EnTT)
# ---------------------------------------------------------------------------
set(FLECS_STATIC ON  CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(flecs
    GIT_REPOSITORY  https://github.com/SanderMertens/flecs.git
    GIT_TAG         ${GT_FLECS_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(flecs)

# ---------------------------------------------------------------------------
# yaml-cpp
# ---------------------------------------------------------------------------
set(YAML_CPP_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(yaml-cpp
    GIT_REPOSITORY  https://github.com/jbeder/yaml-cpp.git
    GIT_TAG         ${GT_YAMLCPP_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(yaml-cpp)

# ---------------------------------------------------------------------------
# RmlUi
# ---------------------------------------------------------------------------
set(RMLUI_BACKEND   "native"    CACHE STRING  "" FORCE)
set(RMLUI_SAMPLES   OFF         CACHE BOOL    "" FORCE)
set(RMLUI_TESTS     OFF         CACHE BOOL    "" FORCE)
set(RMLUI_LUA_BINDINGS OFF      CACHE BOOL    "" FORCE)

FetchContent_Declare(RmlUi
    GIT_REPOSITORY  https://github.com/mikke89/RmlUi.git
    GIT_TAG         ${GT_RMLUI_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(RmlUi)

# ---------------------------------------------------------------------------
# poly2tri
# ---------------------------------------------------------------------------
FetchContent_Declare(poly2tri
    GIT_REPOSITORY  https://github.com/jhasse/poly2tri.git
    GIT_TAG         ${GT_POLY2TRI_TAG}
    GIT_SHALLOW     TRUE
)
FetchContent_MakeAvailable(poly2tri)

# ---------------------------------------------------------------------------
# Dear ImGui + Magnum's ImGuiIntegration (dev/debug overlay only)
# ---------------------------------------------------------------------------
# ImGui ships no CMakeLists.txt, so it's only populated (not add_subdirectory'd);
# magnum-integration compiles the needed imgui .cpp itself, given IMGUI_DIR.
FetchContent_Declare(ImGui
    GIT_REPOSITORY  https://github.com/ocornut/imgui.git
    GIT_TAG         ${GT_IMGUI_TAG}
    GIT_SHALLOW     TRUE
)
FetchContent_MakeAvailable(ImGui)
set(IMGUI_DIR "${imgui_SOURCE_DIR}" CACHE PATH "" FORCE)

# Only the ImGui integration is needed; keep everything else off.
set(WITH_IMGUI        ON  CACHE BOOL "" FORCE)
set(MAGNUM_WITH_IMGUI ON  CACHE BOOL "" FORCE)

# magnum-integration's CMakeLists has unconditional install(FILES ...) calls
# using these dir variables. When Magnum is a fetched subproject they're set
# only in Magnum's own scope, so they arrive here empty and install() errors
# out at configure time. We never run `cmake --install`, so just give them
# Magnum's own default values to keep the install() calls well-formed.
set(MAGNUM_BINARY_INSTALL_DIR  bin)
set(MAGNUM_LIBRARY_INSTALL_DIR lib)
set(MAGNUM_INCLUDE_INSTALL_DIR include/Magnum)

FetchContent_Declare(MagnumIntegration
    GIT_REPOSITORY  https://github.com/mosra/magnum-integration.git
    GIT_TAG         ${GT_MAGNUMINT_TAG}
    GIT_SHALLOW     TRUE
    OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(MagnumIntegration)

# ---------------------------------------------------------------------------
# Main target
# ---------------------------------------------------------------------------
add_executable(GravitarisNG
    src/client/gravitaris.cpp
    src/game/resource/body.cpp
    extlibs/nanosvg/nanosvg.c
    include/gravitaris/game/resource/common/iresource.hpp
    include/gravitaris/game/resource/common/detail/resource-loader.inl
    include/gravitaris/game/id.hpp
    include/gravitaris/game/resource/common/detail/typed-resource-id.inl
    include/gravitaris/game/fs/ifilesystem.hpp
    include/gravitaris/game/fs/filesystem-physfs.hpp
    src/game/fs/filesystem-phys-fs.cpp
    extlibs/clipper/clipper.cpp
    extlibs/clipper/clipper.hpp
    extlibs/physfs/physfs.cpp
    extlibs/physfs/physfs.hpp
    include/gravitaris/game/logging.hpp
    include/gravitaris/game/resource/common/resource-loader.hpp
    include/gravitaris/game/resource/common/resource-ptr.hpp
    src/game/id.cpp
    include/gravitaris/game/resource/body.hpp
    src/game/resource/detail/shape-common.hpp
    src/game/resource/detail/shape-common.cpp
    src/game/resource/detail/casteljau.cpp
    src/game/resource/detail/casteljau.hpp
    src/game/logging.cpp
    include/gravitaris/gravitaris.hpp
    include/gravitaris/cgame/resource/shape.hpp
    src/cgame/resource/shape.cpp
    src/client/triangle-example.cpp
    src/client/triangle-example.hpp
    include/gravitaris/game/game.hpp
    include/gravitaris/game/component/transform.hpp
    include/gravitaris/game/util/chipmunk-safe.hpp
    include/gravitaris/game/component/physics.hpp
    include/gravitaris/game/component/controls.hpp
    include/gravitaris/game/component/input-queue.hpp
    include/gravitaris/game/component/team.hpp
    include/gravitaris/game/component/bullet.hpp
    include/gravitaris/game/component/damageable.hpp
    include/gravitaris/game/component/planet.hpp
    include/gravitaris/game/component/net-id.hpp
    include/gravitaris/game/util/splitmix.hpp
    include/gravitaris/game/input/input-command.hpp
    include/gravitaris/game/input/input-log.hpp
    src/game/input/input-log.cpp
    include/gravitaris/game/system/input-system.hpp
    src/game/system/input-system.cpp
    include/gravitaris/game/component/ai-pilot.hpp
    include/gravitaris/game/system/ai-pilot-system.hpp
    src/game/system/ai-pilot-system.cpp
    include/gravitaris/game/gnc/nav/trajectory-predictor.hpp
    src/game/gnc/nav/trajectory-predictor.cpp
    include/gravitaris/game/gnc/control/flight-controller.hpp
    src/game/gnc/control/flight-controller.cpp
    include/gravitaris/game/gnc/guidance/behaviors.hpp
    src/game/gnc/guidance/behaviors.cpp
    include/gravitaris/game/gnc/ai-personality-presets.hpp
    src/game/gnc/ai-personality-presets.cpp
    include/gravitaris/game/system/bullet-lifetime-system.hpp
    src/game/system/bullet-lifetime-system.cpp
    include/gravitaris/game/system/damage-system.hpp
    src/game/system/damage-system.cpp
    include/gravitaris/game/system/death-system.hpp
    src/game/system/death-system.cpp
    include/gravitaris/game/system/physics-system.hpp
    src/game/system/physics-system.cpp
    include/gravitaris/game/perf-monitor.hpp
    src/game/perf-monitor.cpp
    src/game/game.cpp
    include/gravitaris/cgame/cgame.hpp
    include/gravitaris/cgame/team-color.hpp
    src/cgame/cgame.cpp
    include/gravitaris/cgame/audio/audio-system.hpp
    src/cgame/audio/audio-system.cpp
    include/gravitaris/cgame/audio/audio-backend.hpp
    include/gravitaris/cgame/audio/audio-backend-factory.hpp
    src/cgame/audio/audio-backend-factory.cpp
    include/gravitaris/cgame/audio/backend/magnum-openal-backend.hpp
    src/cgame/audio/backend/magnum-openal-backend.cpp
    include/gravitaris/cgame/audio/backend/miniaudio-backend.hpp
    src/cgame/audio/backend/miniaudio-backend.cpp
    extlibs/miniaudio/miniaudio_impl.c
    include/gravitaris/cgame/resource/audio-clip.hpp
    src/cgame/resource/audio-clip.cpp
    include/gravitaris/cgame/resource/model.hpp
    src/cgame/resource/model.cpp
    include/gravitaris/cgame/renderer/simple-model-renderer.hpp
    include/gravitaris/cgame/component/renderable.hpp
    src/cgame/renderer/simple-model-renderer.cpp
    include/gravitaris/game/resource/common/detail/resource-wrapper.hpp
    include/gravitaris/game/system/ship-controls-system.hpp
    include/gravitaris/game/spawner/entity-spawner.hpp
    src/game/spawner/entity-spawner.cpp
    include/gravitaris/cgame/spawner/centity-spawner.hpp
    src/cgame/spawner/centity-spawner.cpp
    include/gravitaris/cgame/fwd.hpp
    src/game/system/ship-controls-system.cpp
    include/gravitaris/cgame/renderer/shader/simple-line-shader.hpp
    src/cgame/renderer/shader/basic-lines-shader.cpp
    include/gravitaris/cgame/renderer/shader/line2-shader.hpp
    src/cgame/renderer/shader/line2-shader.cpp
    include/gravitaris/cgame/renderer/model-renderer2.hpp
    src/cgame/renderer/model-renderer2.cpp
    include/gravitaris/cgame/renderer/shader/starfield-shader.hpp
    src/cgame/renderer/shader/starfield-shader.cpp
    include/gravitaris/cgame/renderer/starfield-renderer.hpp
    src/cgame/renderer/starfield-renderer.cpp
    include/gravitaris/cgame/renderer/gl-safe-upload.hpp
    include/gravitaris/cgame/renderer/minimap-renderer.hpp
    src/cgame/renderer/minimap-renderer.cpp
    include/gravitaris/cgame/camera.hpp
    include/gravitaris/cgame/renderer/shader/glow-threshold-shader.hpp
    src/cgame/renderer/shader/glow-threshold-shader.cpp
    include/gravitaris/cgame/renderer/shader/glow-blur-shader.hpp
    src/cgame/renderer/shader/glow-blur-shader.cpp
    include/gravitaris/cgame/renderer/shader/glow-composite-shader.hpp
    src/cgame/renderer/shader/glow-composite-shader.cpp
    include/gravitaris/cgame/renderer/shader/crt-shader.hpp
    src/cgame/renderer/shader/crt-shader.cpp
    include/gravitaris/cgame/renderer/glow-post-process.hpp
    src/cgame/renderer/glow-post-process.cpp
    src/cgame/ui/detail/system-interface.hpp
    src/cgame/ui/detail/system-interface.cpp
    src/cgame/ui/detail/render-interface-gl3.hpp
    src/cgame/ui/detail/render-interface-gl3.cpp
    include/gravitaris/ui/ui.hpp
    src/cgame/ui/ui.cpp
    src/cgame/ui/detail/file-interface.hpp
    src/cgame/ui/detail/file-interface.cpp
    src/cgame/ui/debug/debug-ui.hpp
    src/cgame/ui/debug/debug-ui.cpp
    src/cgame/ui/debug/post-process-panel.hpp
    src/cgame/ui/debug/post-process-panel.cpp
    src/cgame/ui/debug/audio-panel.hpp
    src/cgame/ui/debug/audio-panel.cpp
    src/cgame/ui/debug/renderer-panel.hpp
    src/cgame/ui/debug/renderer-panel.cpp
    src/cgame/ui/debug/spawn-panel.hpp
    src/cgame/ui/debug/spawn-panel.cpp
    src/cgame/ui/debug/trajectory-panel.hpp
    src/cgame/ui/debug/trajectory-panel.cpp
    src/cgame/ui/debug/flight-panel.hpp
    src/cgame/ui/debug/flight-panel.cpp
    src/cgame/ui/debug/physics-panel.hpp
    src/cgame/ui/debug/physics-panel.cpp
    src/cgame/ui/debug/perf-panel.hpp
    src/cgame/ui/debug/perf-panel.cpp
    src/cgame/ui/debug/starfield-panel.hpp
    src/cgame/ui/debug/starfield-panel.cpp
    src/cgame/ui/debug/camera-panel.hpp
    src/cgame/ui/debug/camera-panel.cpp
    src/cgame/ui/debug/hud-panel.hpp
    src/cgame/ui/debug/hud-panel.cpp
    src/cgame/ui/debug/world-to-ui.hpp
)

# DPI-aware manifest, per https://doc.magnum.graphics/magnum/platforms-windows.html --
# without it, Windows doesn't consider the app DPI-aware and dpiScaling()/
# framebufferSize() report meaningless values on scaled displays. On MSVC the
# raw .manifest is listed directly as a source so CMake merges it into the
# linker's own auto-generated manifest (/MANIFESTINPUT); wrapping it in an
# RT_MANIFEST .rc resource instead collides with that auto-generated manifest
# at the same resource ID and silently loses. MinGW has no such auto-manifest,
# so it uses the .rc route instead.
if(WIN32)
    if(MSVC)
        target_sources(GravitarisNG PRIVATE src/client/windows-hidpi.manifest)
    elseif(MINGW)
        target_sources(GravitarisNG PRIVATE src/client/windows-hidpi.rc)
    endif()
endif()

target_compile_features(GravitarisNG PRIVATE cxx_std_20)

target_include_directories(GravitarisNG PRIVATE
    src/
    include/
    extlibs/
)

target_compile_definitions(GravitarisNG PRIVATE
    GRAVITARIS_COMPILETIME_SOURCE_PATH="${CMAKE_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# Chipmunk2D doesn't always export its include dir via its target in older
# versions of its CMakeLists, so we add it explicitly as a fallback.
# ---------------------------------------------------------------------------
target_include_directories(GravitarisNG PRIVATE
    ${chipmunk2d_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------
target_link_libraries(GravitarisNG PRIVATE
    flecs::flecs_static

    # Chipmunk2D — BUILD_SHARED OFF so only chipmunk_static is built
    chipmunk_static

    yaml-cpp::yaml-cpp

    Magnum::Magnum
    Magnum::GL
    Magnum::Shaders
    Magnum::Sdl2Application
    Magnum::Audio

    MagnumIntegration::ImGui

    Freetype::Freetype

    RmlUi::Core
    RmlUi::Debugger

    poly2tri

    "${CMAKE_DL_LIBS}"
)

# PhysFS target name varies across versions — resolve whichever exists
if(TARGET PhysFS::PhysFS)
    target_link_libraries(GravitarisNG PRIVATE PhysFS::PhysFS)
elseif(TARGET physfs-static)
    target_link_libraries(GravitarisNG PRIVATE physfs-static)
elseif(TARGET physfs)
    target_link_libraries(GravitarisNG PRIVATE physfs)
endif()

# On macOS the fetched shared libs (Magnum, Corrade, etc.) carry no install
# name fixup, and the executable gets no LC_RPATH by default, so it can't
# find them at runtime. Point it at its own lib output directory.
if(APPLE)
    set_target_properties(GravitarisNG PROPERTIES
        BUILD_RPATH "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
    )
endif()

# ---------------------------------------------------------------------------
# gravitaris-sim-test — headless determinism harness (docs/networking-plan.md
# Phase 0.4 / ADR 0001's "cheap test"). Same game/ sources as the main
# target's sim side, deliberately NONE of cgame/, Magnum::GL, Sdl2Application,
# Audio, RmlUi or ImGui: if this stops linking, something pulled a rendering/
# window/audio dependency into the sim.
# ---------------------------------------------------------------------------
add_executable(gravitaris-sim-test
    tools/sim-test/main.cpp
    src/game/resource/body.cpp
    extlibs/nanosvg/nanosvg.c
    extlibs/clipper/clipper.cpp
    extlibs/physfs/physfs.cpp
    src/game/fs/filesystem-phys-fs.cpp
    src/game/id.cpp
    src/game/logging.cpp
    src/game/perf-monitor.cpp
    src/game/resource/detail/shape-common.cpp
    src/game/resource/detail/casteljau.cpp
    src/game/input/input-log.cpp
    src/game/system/input-system.cpp
    src/game/system/ai-pilot-system.cpp
    src/game/system/ship-controls-system.cpp
    src/game/system/bullet-lifetime-system.cpp
    src/game/system/damage-system.cpp
    src/game/system/death-system.cpp
    src/game/system/physics-system.cpp
    src/game/gnc/nav/trajectory-predictor.cpp
    src/game/gnc/control/flight-controller.cpp
    src/game/gnc/guidance/behaviors.cpp
    src/game/gnc/ai-personality-presets.cpp
    src/game/spawner/entity-spawner.cpp
    src/game/game.cpp
)

target_include_directories(gravitaris-sim-test PRIVATE
    src/
    include/
    extlibs/
)

target_include_directories(gravitaris-sim-test PRIVATE
    ${chipmunk2d_SOURCE_DIR}/include
)

target_compile_definitions(gravitaris-sim-test PRIVATE
    GRAVITARIS_COMPILETIME_SOURCE_PATH="${CMAKE_SOURCE_DIR}"
)

target_compile_features(gravitaris-sim-test PRIVATE cxx_std_20)

target_link_libraries(gravitaris-sim-test PRIVATE
    flecs::flecs_static
    chipmunk_static
    yaml-cpp::yaml-cpp
    Magnum::Magnum
    "${CMAKE_DL_LIBS}"
)

if(TARGET PhysFS::PhysFS)
    target_link_libraries(gravitaris-sim-test PRIVATE PhysFS::PhysFS)
elseif(TARGET physfs-static)
    target_link_libraries(gravitaris-sim-test PRIVATE physfs-static)
elseif(TARGET physfs)
    target_link_libraries(gravitaris-sim-test PRIVATE physfs)
endif()
