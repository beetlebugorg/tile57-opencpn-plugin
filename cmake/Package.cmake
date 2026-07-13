# OpenCPN managed-plugin packaging.
#
#   cmake --build build --target package        # (+ --config Release on MSVC)
#
# Produces build/tile57_pi_<version>_<target>.tar.gz — a tarball OpenCPN installs
# via Options -> Plugins -> "Import plugin...". The layout mirrors what OpenCPN's
# PluginHandler expects when it explodes the tarball (model/src/plugin_handler.cpp):
#   <top>/metadata.xml              the manifest (cmake/metadata.xml.in)
#   <top>/lib/opencpn/libtile57_pi.so   (Linux)
#   <top>/plugins/tile57_pi.dll         (Windows)
# The <top> dir is stripped on install; its name is cosmetic.
#
# Linux + Windows only for now (no macOS CI). The <target>/<target-version> tuple
# mirrors OpenCPN's cmake/TargetSetup.cmake so PluginHandler::IsCompatible() accepts
# the tarball on a matching host.

set(PKG_VERSION "0.1.0" CACHE STRING "Plugin package version, as it appears in the manifest")

if(MSVC)
  set(PKG_TARGET "msvc-wx32")
  if(CMAKE_SYSTEM_VERSION)
    set(PKG_TARGET_VERSION "${CMAKE_SYSTEM_VERSION}")
  else()
    set(PKG_TARGET_VERSION "10")
  endif()
  set(PKG_ARCH "x86") # OpenCPN's Windows process is 32-bit
  set(_pkg_bin_subdir "plugins")
  set(_pkg_bin_name "tile57_pi.dll")
elseif(UNIX AND NOT APPLE)
  find_program(LSB_RELEASE lsb_release)
  if(LSB_RELEASE)
    execute_process(COMMAND ${LSB_RELEASE} -is
                    OUTPUT_VARIABLE _distro OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND ${LSB_RELEASE} -rs
                    OUTPUT_VARIABLE PKG_TARGET_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
  else()
    set(_distro "unknown")
    set(PKG_TARGET_VERSION "0")
  endif()
  set(PKG_ARCH "${CMAKE_SYSTEM_PROCESSOR}") # x86_64 / aarch64
  set(PKG_TARGET "${_distro}-${PKG_ARCH}")
  set(_pkg_bin_subdir "lib/opencpn")
  set(_pkg_bin_name "libtile57_pi.so")
else()
  return() # macOS / other: no package target
endif()

string(TOLOWER "${PKG_TARGET}" PKG_TARGET)
string(TOLOWER "${PKG_TARGET_VERSION}" PKG_TARGET_VERSION)

set(_pkg_base "tile57_pi_${PKG_VERSION}_${PKG_TARGET}")
set(_pkg_stage "${CMAKE_BINARY_DIR}/pkg/${_pkg_base}")

# Render the manifest with the resolved tuple (PKG_TARGET / VERSION / ARCH above).
configure_file(${CMAKE_SOURCE_DIR}/cmake/metadata.xml.in
               ${CMAKE_BINARY_DIR}/metadata.xml @ONLY)

# Assemble the staging tree and tar it. All commands go through `cmake -E` so this
# works identically on Linux and the MSVC runner.
add_custom_target(package
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/pkg
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_pkg_stage}/${_pkg_bin_subdir}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tile57_pi>
          ${_pkg_stage}/${_pkg_bin_subdir}/${_pkg_bin_name}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/metadata.xml ${_pkg_stage}/metadata.xml
  COMMAND ${CMAKE_COMMAND} -E tar czf ${CMAKE_BINARY_DIR}/${_pkg_base}.tar.gz ${_pkg_base}
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/pkg
  DEPENDS tile57_pi
  COMMENT "Packaging ${_pkg_base}.tar.gz (OpenCPN import tarball)"
  VERBATIM)
