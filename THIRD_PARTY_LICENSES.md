# Third-Party Licenses

Third-party source bundled in this repository:

| Component | License | Origin | Path |
|-----------|---------|--------|------|
| **OpenCPN plugin API** | GPL-2.0-or-later | https://github.com/OpenCPN/opencpn-libs | `opencpn-libs/` (submodule) |
| **fix-macos-libs.sh** | GPL-3.0-or-later | https://github.com/Rasbats/ShipDriver_pi (OpenCPN plugin build framework) | `cmake/fix-macos-libs.sh` |

`opencpn-libs` is a git submodule; only its `api-18` header (`ocpn_plugin.h`,
© David S. Register / OpenCPN) is compiled into the plugin. `cmake/fix-macos-libs.sh`
is adapted from the OpenCPN plugin build framework (© 2022 Alec Leamas) and runs
only at build time on macOS. This plugin is GPL-2.0-or-later, compatible with both.

tile57 itself is consumed as a prebuilt static library (`libtile57.a`, MIT) and
is not vendored here; see the tile57 repository for its own dependencies,
including the IHO S-101 Portrayal Catalogue (© IHO).
