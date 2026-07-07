cmake_minimum_required(VERSION 3.16)
project(tile57_opencpn_plugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
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

# OpenCPN's plugin API header (ocpn_plugin.h).
set(OCPN_INCLUDE "${CMAKE_SOURCE_DIR}/../OpenCPN/include" CACHE PATH "directory containing ocpn_plugin.h")

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
  src/chart_renderer.cpp
  src/safety_overlay.cpp)

target_include_directories(tile57_pi PRIVATE
  ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/third_party
  ${TILE57_INCLUDE} ${OCPN_INCLUDE})
target_compile_definitions(tile57_pi PRIVATE ${wxWidgets_DEFINITIONS})
target_compile_options(tile57_pi PRIVATE ${wxWidgets_CXX_FLAGS})

target_link_libraries(tile57_pi PRIVATE
  ${wxWidgets_LIBRARIES} ${OPENGL_LIBRARIES} ${GLEW_LIB} ${TILE57_LIB} m pthread)

# OpenCPN loads plugins as .dylib on macOS; silence its OpenGL deprecation noise.
if(APPLE)
  set_target_properties(tile57_pi PROPERTIES SUFFIX ".dylib")
  target_compile_definitions(tile57_pi PRIVATE GL_SILENCE_DEPRECATION)
endif()
