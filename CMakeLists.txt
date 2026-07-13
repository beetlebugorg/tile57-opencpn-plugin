cmake_minimum_required(VERSION 3.16)

# Default to Release, and do it BEFORE project(): CMake 4 defaults CMAKE_BUILD_TYPE
# to Debug for MSVC from inside project(), which would leave an `if(NOT
# CMAKE_BUILD_TYPE)` guard below it dead on Windows. That is not just a slow plugin —
# a Debug configure makes FindwxWidgets select the debug wx import libs
# (wxmsw32ud_*), whose DLLs OpenCPN does not ship, so the plugin links but never
# loads. Before project() the cache entry exists only if the user passed -D, so an
# explicit -DCMAKE_BUILD_TYPE=Debug is still honoured.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "build type")
endif()

project(tile57_opencpn_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# clangd reads build/compile_commands.json for editor diagnostics.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Static CRT on Windows. OpenCPN bundles its own msvcp140/vcruntime140 (14.12, from
# VS2017) in its program directory and loads them at startup, so they are already in
# the process by the time a plugin is loaded — a plugin linked against the shared CRT
# therefore runs on the HOST's runtime, whichever that happens to be, and shipping a
# newer DLL next to the plugin cannot override an already-loaded module. MSVC requires
# the redistributable to be at least as new as the toolset, so a plugin built with any
# current toolset faults inside msvcp140 the first time it touches std::mutex /
# std::thread (Build Charts crashed exactly there). We do not control OpenCPN, so link
# the CRT statically and depend on none of it.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
# MSVC spells its warning flags differently: it parses -Wextra as /Wextra and dies
# with D8021, so gate the GCC/Clang spelling.
#
# /utf-8: the sources are UTF-8 and put em dashes, ellipses and middle dots in
# user-visible strings ("Build Charts — tile57", "Baking %d cells…"). MSVC otherwise
# decodes source bytes as the system ANSI codepage, so those render as mojibake
# ("Build Charts â€” tile57"). GCC/Clang already assume UTF-8.
if(MSVC)
  add_compile_options(/W3 /utf-8)
else()
  add_compile_options(-Wall -Wextra)
endif()

# --- external dependencies --------------------------------------------------
# OpenCPN ships as a 32-bit binary on Windows, and a plugin must match the host it
# is loaded into: its wxWidgets/GLEW/opencpn.lib import libs are all x86. A 64-bit
# configure only fails later, as a wall of LNK4272 + unresolved wx symbols, so say
# so up front.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "OpenCPN for Windows is 32-bit, so the plugin must be built 32-bit too.\n"
    "Configure from an x86 developer prompt (vcvars32.bat) or pass -A Win32.")
endif()

# tile57: its public header (tile57.h) and static library, built with `zig build`
# in the tile57 checkout. Zig names it tile57.lib for the MSVC ABI, libtile57.a
# everywhere else.
set(TILE57_DIR "${CMAKE_SOURCE_DIR}/../tile57" CACHE PATH "tile57 checkout")
set(TILE57_INCLUDE "${TILE57_DIR}/include" CACHE PATH "directory containing tile57.h")
if(MSVC)
  set(_tile57_libname "tile57.lib")
else()
  set(_tile57_libname "libtile57.a")
endif()
set(TILE57_LIB "${TILE57_DIR}/zig-out/lib/${_tile57_libname}"
    CACHE FILEPATH "path to the tile57 static library")

# OpenCPN plugin API via the vendored opencpn-libs submodule. api-19 ==
# opencpn_plugin_119, the base class Tile57Plugin derives from. This exposes the
# header-only `ocpn::api` target (adds ocpn_plugin.h; links the import lib on
# win32). The base-class + API-helper symbols are provided by the OpenCPN
# executable and resolved when the host dlopen's the plugin (see the APPLE
# dynamic_lookup below; Linux allows undefined module symbols by default).
#
# 1.19 (not 1.18) because ONLY 1.19+ exposes the per-canvas ENC display state —
# GetEnableENCTextDisplay / ...DepthSoundings / ...Lights / ...LightDescriptions /
# ...BuoyLightLabels / GetENCDisplayCategory. Those are the "Chart Panel Options"
# toggles, and they are unreadable at 1.18: the canvas writes them straight into
# OpenCPN's s52plib without ever touching the config object a plugin can read.
add_subdirectory(opencpn-libs/api-19)

# Windows has no system package manager for wx/GLEW, but OpenCPN's own Windows
# build caches prebuilt 32-bit copies under <opencpn>/cache (populated by
# buildwin/win_deps.bat). Reuse them — the plugin has to link the very wxWidgets
# the host process already loaded, so matching OpenCPN's build is the point, not a
# shortcut. Everything below is an ordinary CACHE default: -D overrides still win.
if(WIN32)
  set(OCPN_SRC_DIR "${CMAKE_SOURCE_DIR}/../OpenCPN"
      CACHE PATH "OpenCPN source checkout whose cache/ supplies prebuilt wxWidgets + GLEW")
  file(GLOB _ocpn_wx "${OCPN_SRC_DIR}/cache/wxWidgets-3.2*")
  if(_ocpn_wx AND NOT wxWidgets_ROOT_DIR)
    list(GET _ocpn_wx 0 _wx_root)
    set(wxWidgets_ROOT_DIR "${_wx_root}" CACHE PATH "wxWidgets prefix")
    set(wxWidgets_LIB_DIR "${_wx_root}/lib/vc14x_dll" CACHE PATH "wxWidgets lib dir")
  endif()
endif()

# html: ocpn_plugin.h derives from wxHtmlWindow, so the plugin emits its vtable and
# must link wx html. Only Windows notices — Linux/macOS modules resolve the symbol
# from the OpenCPN process at load time.
find_package(wxWidgets REQUIRED COMPONENTS gl html core base)
find_package(OpenGL REQUIRED)
# GLEW: "GLEW" on macOS/Linux (Homebrew / distro), "glew32" on Windows.
find_library(GLEW_LIB NAMES GLEW glew32 glew32s REQUIRED
  HINTS "${OCPN_SRC_DIR}/cache/buildwin")
# GLEW's headers are NOT on the compiler's default search path on macOS (Homebrew
# under /opt/homebrew or /usr/local). Previously <GL/glew.h> was found only
# incidentally via wxWidgets' include dir, so a clean/relocated build could lose it.
# Resolve it explicitly (mirrors where find_library found libGLEW).
find_path(GLEW_INCLUDE_DIR GL/glew.h
  HINTS /opt/homebrew/include /usr/local/include)
if(NOT GLEW_INCLUDE_DIR AND WIN32)
  # OpenCPN's cache ships glew.h unprefixed (cache/buildwin/include/glew/glew.h) but
  # the sources — like GLEW's own docs — include <GL/glew.h>. Synthesise the GL/
  # directory the include expects rather than making every user copy headers around.
  find_path(_glew_h glew.h HINTS "${OCPN_SRC_DIR}/cache/buildwin/include/glew")
  if(_glew_h)
    file(WRITE "${CMAKE_BINARY_DIR}/glew-shim/GL/glew.h" "#include \"${_glew_h}/glew.h\"\n")
    # FORCE: find_path above already cached GLEW_INCLUDE_DIR-NOTFOUND, and a plain
    # set(CACHE) will not overwrite an existing entry. Only reached when the search
    # failed, so an explicit -DGLEW_INCLUDE_DIR still wins (find_path leaves it alone).
    set(GLEW_INCLUDE_DIR "${CMAKE_BINARY_DIR}/glew-shim" CACHE PATH
        "directory containing GL/glew.h" FORCE)
  endif()
endif()
if(NOT GLEW_INCLUDE_DIR)
  message(FATAL_ERROR "GL/glew.h not found. Install GLEW, or point -DGLEW_INCLUDE_DIR "
                      "at the directory containing GL/glew.h.")
endif()
include(${wxWidgets_USE_FILE})

# wxJSON decodes the "OpenCPN Config" plugin message (see Tile57Plugin::SetPluginMessage) —
# the only live source for the S52 settings that are neither PI getters nor written to the
# config object. It must come AFTER include(${wxWidgets_USE_FILE}): the vendored target
# picks the wx headers up from the directory-level include path that sets, not its own.
add_subdirectory(opencpn-libs/wxJSON)

if(NOT EXISTS ${TILE57_LIB})
  # Windows needs the target spelled out: zig defaults to the 64-bit host, and a
  # 64-bit (or gnu-ABI) tile57.lib fails the link with LNK1143 in compiler_rt.obj.
  set(_zig_build "zig build -Doptimize=ReleaseFast")
  if(WIN32)
    string(APPEND _zig_build " -Dtarget=x86-windows-msvc")
  endif()
  message(FATAL_ERROR "${_tile57_libname} not found at ${TILE57_LIB}. Build tile57 first "
                      "(cd ${TILE57_DIR} && ${_zig_build}), or set -DTILE57_LIB=<path>.")
endif()

# --- the plugin -------------------------------------------------------------
add_library(tile57_pi MODULE
  src/tile57_pi.cpp
  src/tile57_chart.cpp
  src/chart_renderer.cpp
  src/build_charts.cpp)

target_include_directories(tile57_pi PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
  ${TILE57_INCLUDE} ${GLEW_INCLUDE_DIR})
target_compile_definitions(tile57_pi PRIVATE ${wxWidgets_DEFINITIONS})

if(WIN32)
  # ocpn_plugin.h expands DECL_EXP to __declspec(dllexport) only when __WXMSW__ is
  # already defined — otherwise to nothing, and create_pi/destroy_pi never make it
  # into the DLL's export table, so OpenCPN silently refuses to load the plugin.
  # __WXMSW__ normally arrives via wx/platform.h, which leaves TUs that include
  # ocpn_plugin.h before any wx header (tile57_pi.cpp, tile57_chart.h) exporting
  # nothing. Define it up front so DECL_EXP is dllexport in every TU; wx guards its
  # own definition with #if !defined(__WXMSW__), so this does not conflict.
  target_compile_definitions(tile57_pi PRIVATE __WXMSW__)
  # getenv/sscanf etc. are flagged C4996 "unsafe" by MSVC; the code is portable C++.
  target_compile_definitions(tile57_pi PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_compile_options(tile57_pi PRIVATE ${wxWidgets_CXX_FLAGS})

# libm / libpthread are the Unix math + threads libs; MSVC has neither (they're in
# the CRT), so link them only off-Windows.
if(WIN32)
  # tile57 is a Zig static lib, and Zig's Windows std calls the native NT API
  # (NtAllocateVirtualMemory, LdrLoadDll, …) directly. `zig build` links ntdll
  # itself; here MSVC's link.exe does the final link, so name it explicitly.
  set(PLATFORM_LIBS ntdll)
else()
  set(PLATFORM_LIBS m pthread)
endif()

target_link_libraries(tile57_pi PRIVATE
  ocpn::api
  ocpn::wxjson
  ${wxWidgets_LIBRARIES} ${OPENGL_LIBRARIES} ${GLEW_LIB} ${TILE57_LIB} ${PLATFORM_LIBS})

# OpenCPN loads plugins as .dylib on macOS; silence its OpenGL deprecation noise.
if(APPLE)
  set_target_properties(tile57_pi PROPERTIES SUFFIX ".dylib")
  target_compile_definitions(tile57_pi PRIVATE GL_SILENCE_DEPRECATION)
  # opencpn_plugin_118's vtable/typeinfo/ctor and the API helper functions
  # (GetOCPNCanvasWindow, …) live in the OpenCPN executable, not the plugin —
  # defer them to load-time resolution instead of failing the link.
  target_link_options(tile57_pi PRIVATE -undefined dynamic_lookup)

  # Retarget wxWidgets to OpenCPN.app's bundled copy so only ONE wxWidgets loads
  # at runtime — otherwise the build-time wx (e.g. Homebrew) loads alongside the
  # app's, duplicating Obj-C classes and crashing. Mirrors the OpenCPN plugin
  # framework's cmake/fix-macos-libs.sh (see THIRD_PARTY_LICENSES.md).
  set(OCPN_APP "/Applications/OpenCPN.app"
      CACHE PATH "OpenCPN.app whose bundled wxWidgets the plugin binds to")
  add_custom_command(TARGET tile57_pi POST_BUILD
    COMMAND /bin/sh ${CMAKE_SOURCE_DIR}/cmake/fix-macos-libs.sh
            $<TARGET_FILE:tile57_pi> ${OCPN_APP}/Contents/Frameworks
    COMMENT "Retargeting wxWidgets to ${OCPN_APP}/Contents/Frameworks"
    VERBATIM)
endif()

# OpenCPN managed-plugin tarball: `cmake --build build --target package`.
include(${CMAKE_SOURCE_DIR}/cmake/Package.cmake)
