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
#   <top>/OpenCPN.app/Contents/PlugIns/libtile57_pi.dylib   (macOS)
# The <top> dir is stripped on install; its name is cosmetic. On macOS the
# installer (apple_entry_set_install_path) keys on the "Contents/PlugIns" path
# component and drops everything before it, so the OpenCPN.app/ prefix is
# convention, not contract.
#
# The <target>/<target-version> tuple mirrors OpenCPN's cmake/TargetSetup.cmake so
# PluginHandler::IsCompatible() accepts the tarball on a matching host. On macOS
# the version is not compared at all; the check is that <target-arch> contains the
# host's arch string (arm64, or x86_64 — what a Rosetta-translated OpenCPN also
# reports), so the two per-arch tarballs land on the right hosts.

# PKG_VERSION comes from CMakeLists.txt: the git tag on release builds, a
# `git describe` fallback otherwise.

# Distinguishes the macOS tarballs: their PKG_TARGET is the fixed "darwin-wx32",
# unlike Linux, where the arch is already inside PKG_TARGET.
set(_pkg_name_suffix "")

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
elseif(APPLE)
  set(PKG_TARGET "darwin-wx32")
  execute_process(COMMAND sw_vers -productVersion
                  OUTPUT_VARIABLE PKG_TARGET_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(PKG_ARCH "${CMAKE_SYSTEM_PROCESSOR}") # arm64 / x86_64
  set(_pkg_name_suffix "-${PKG_ARCH}")
  set(_pkg_bin_subdir "OpenCPN.app/Contents/PlugIns")
  set(_pkg_bin_name "libtile57_pi.dylib")
elseif(UNIX)
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
  return() # other: no package target
endif()

string(TOLOWER "${PKG_TARGET}" PKG_TARGET)
string(TOLOWER "${PKG_TARGET_VERSION}" PKG_TARGET_VERSION)

set(_pkg_base "tile57_pi_${PKG_VERSION}_${PKG_TARGET}${_pkg_name_suffix}")
set(_pkg_stage "${CMAKE_BINARY_DIR}/pkg/${_pkg_base}")

# Render the manifest with the resolved tuple (PKG_TARGET / VERSION / ARCH above).
configure_file(${CMAKE_SOURCE_DIR}/cmake/metadata.xml.in
               ${CMAKE_BINARY_DIR}/metadata.xml @ONLY)

# Assemble the staging tree and tar it. All commands go through `cmake -E` so this
# works identically on every platform's runner. The tar runs via `cmake -E chdir`
# (not the target's WORKING_DIRECTORY, which must exist before the FIRST command runs
# and pkg/ does not yet) so the tarball's top dir is just ${_pkg_base}, no pkg/ prefix.
add_custom_target(package
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/pkg
  COMMAND ${CMAKE_COMMAND} -E make_directory ${_pkg_stage}/${_pkg_bin_subdir}
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tile57_pi>
          ${_pkg_stage}/${_pkg_bin_subdir}/${_pkg_bin_name}
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/metadata.xml ${_pkg_stage}/metadata.xml
  COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_BINARY_DIR}/pkg
          ${CMAKE_COMMAND} -E tar czf ${CMAKE_BINARY_DIR}/${_pkg_base}.tar.gz ${_pkg_base}
  DEPENDS tile57_pi
  COMMENT "Packaging ${_pkg_base}.tar.gz (OpenCPN import tarball)"
  VERBATIM)
