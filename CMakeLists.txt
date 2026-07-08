cmake_minimum_required(VERSION 3.16)
project(tile57_opencpn_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# clangd reads build/compile_commands.json for editor diagnostics.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra)

# --- external dependencies --------------------------------------------------
# tile57: its public header (tile57.h) and static library (libtile57.a), built
# with `zig build` in the tile57 checkout.
set(TILE57_DIR "${CMAKE_SOURCE_DIR}/../tile57" CACHE PATH "tile57 checkout")
set(TILE57_INCLUDE "${TILE57_DIR}/include" CACHE PATH "directory containing tile57.h")
set(TILE57_LIB "${TILE57_DIR}/zig-out/lib/libtile57.a" CACHE FILEPATH "path to libtile57.a")

# OpenCPN plugin API via the vendored opencpn-libs submodule. api-18 ==
# opencpn_plugin_118, the base class Tile57Plugin derives from. This exposes the
# header-only `ocpn::api` target (adds ocpn_plugin.h; links the import lib on
# win32). The base-class + API-helper symbols are provided by the OpenCPN
# executable and resolved when the host dlopen's the plugin (see the APPLE
# dynamic_lookup below; Linux allows undefined module symbols by default).
add_subdirectory(opencpn-libs/api-18)

find_package(wxWidgets REQUIRED COMPONENTS core base gl)
find_package(OpenGL REQUIRED)
find_library(GLEW_LIB GLEW REQUIRED)   # Homebrew on macOS; distro package on Linux
include(${wxWidgets_USE_FILE})

if(NOT EXISTS ${TILE57_LIB})
  message(FATAL_ERROR "libtile57.a not found at ${TILE57_LIB}. Build tile57 first "
                      "(cd ${TILE57_DIR} && zig build), or set -DTILE57_LIB=<path>.")
endif()

# --- the plugin -------------------------------------------------------------
add_library(tile57_pi MODULE
  src/tile57_pi.cpp
  src/tile57_chart.cpp
  src/chart_renderer.cpp
  src/bake_manager.cpp
  src/build_charts.cpp)

target_include_directories(tile57_pi PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
  ${TILE57_INCLUDE})
target_compile_definitions(tile57_pi PRIVATE ${wxWidgets_DEFINITIONS})
target_compile_options(tile57_pi PRIVATE ${wxWidgets_CXX_FLAGS})

target_link_libraries(tile57_pi PRIVATE
  ocpn::api
  ${wxWidgets_LIBRARIES} ${OPENGL_LIBRARIES} ${GLEW_LIB} ${TILE57_LIB} m pthread)

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
