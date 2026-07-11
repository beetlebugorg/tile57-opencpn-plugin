#!/bin/sh
# ~~~
# Summary:     Retarget a plugin's wxWidgets references to OpenCPN.app's copy.
# License:     GPLv3+
# Copyright (c) 2022 Alec Leamas
#
# Adapted from the OpenCPN plugin build framework's cmake/fix-macos-libs.sh
# (github.com/Rasbats/ShipDriver_pi). Two changes from the original:
#   1. it takes the built plugin + the app's Frameworks dir as arguments,
#      instead of globbing a staged app/files/ tree we do not produce;
#   2. it RESOLVES each wx reference against OpenCPN.app's actual bundled wx
#      file, rather than only swapping the directory and keeping the basename.
# (2) matters when the plugin is built against a different wx 3.2.x micro than
# the app ships (e.g. Homebrew libwx_..-3.2.0.4.3 vs the release's 3.2.0.4.2):
# a plain directory swap would point at a file that isn't in the bundle. wx
# keeps ABI compatibility across the 3.2.x series, so binding to the app's copy
# at runtime is safe — and it guarantees exactly ONE wxWidgets loads. Two
# different wx builds in one process duplicate Obj-C classes and crash.
# ~~~
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; either version 3 of the License, or (at your option) any later
# version.
#
# Usage: fix-macos-libs.sh <plugin.dylib> <OpenCPN.app/Contents/Frameworks>
set -eu

plugin="$1"
frameworks="$2"
runtime_path="@executable_path/../Frameworks"

if [ ! -d "$frameworks" ]; then
    echo "fix-macos-libs.sh: WARNING: '$frameworks' not found — wxWidgets NOT retargeted."
    echo "  Pass -DOCPN_APP=/path/to/OpenCPN.app and rebuild, else the plugin loads a"
    echo "  second wxWidgets and will not appear in OpenCPN."
    exit 0
fi

changed=0
for lib in $(otool -L "$plugin" | awk '/libwx/ {print $1}'); do
    base=${lib##*/}
    # Stable prefix through the wx minor version, e.g. libwx_osx_cocoau_core-3.2
    prefix=$(printf '%s' "$base" | sed -E 's/(-[0-9]+\.[0-9]+)[.-].*/\1/')
    # The app's matching wx file (the full versioned dylib sorts before a symlink).
    appfile=$(ls "$frameworks" | grep -E "^${prefix}[.-]" | sort | head -1 || true)
    if [ -z "$appfile" ]; then
        echo "fix-macos-libs.sh: WARNING: no match for '$base' in $frameworks"
        continue
    fi
    install_name_tool -change "$lib" "$runtime_path/$appfile" "$plugin"
    changed=$((changed + 1))
done

echo "fix-macos-libs.sh: retargeted $changed wxWidgets reference(s):"
otool -L "$plugin" | grep -i wx || true

# install_name_tool INVALIDATES the linker's ad-hoc code signature. macOS then
# SIGKILLs the plugin at dlopen with "Code Signature Invalid / Invalid Page" (dyld
# faults reading the header in isUniversal/compatibleSlice) and OpenCPN disables it.
# Re-sign ad-hoc so it loads; --force mints a fresh CDHash, which also sidesteps a
# stale kernel code-signing cache on in-place reinstalls.
if [ "$changed" -gt 0 ]; then
    codesign --remove-signature "$plugin" 2>/dev/null || true
    codesign --force --sign - "$plugin" && echo "fix-macos-libs.sh: re-signed (ad-hoc)."
fi
