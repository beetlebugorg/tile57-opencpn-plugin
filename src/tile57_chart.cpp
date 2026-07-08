// tile57_chart.cpp — see tile57_chart.h.
#include "tile57_chart.h"
#include "bake_manager.h"
#include "gl.h"
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/log.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

// wxWidgets can create ChartTile57 by class name; OpenCPN uses this to
// instantiate the chart for each matching file it finds in a chart directory.
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57, PlugInChartBaseExtended);
// The two concrete classes OpenCPN instantiates by name (baked bundles + live cells);
// they differ only in GetFileSearchMask, everything else is the shared base.
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57Pmtiles, ChartTile57);
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57Cell, ChartTile57);

namespace {
constexpr double kEarthR = 6378137.0;
constexpr double kPi = 3.14159265358979323846;
// tile57 sizes symbols/text/line-widths at size_scale=1.0 against a 72-DPI
// reference (2.8346 px/mm). Scale that to the real display's pixels-per-mm so
// S-52 symbology is physically the right size — matching native OpenCPN, which
// derives its own symbol scale from the same display width. Without this, dense
// modern displays render everything at a fraction of size ("2x density").
constexpr double kTile57RefPxPerMm = 2.8346;
// Optional manual calibration: TILE57_SIZE=<factor> multiplies the symbol/text scale,
// so if the CHKSYM01 square (TILE57_CALIB=1) doesn't measure 5mm you can dial it in
// without a rebuild (e.g. measures 6.5mm -> TILE57_SIZE=0.77).
static double size_cal_factor() {
    static double f = [] {
        const char* e = std::getenv("TILE57_SIZE");
        double v = e ? std::atof(e) : 1.0;
        return (v > 0.1 && v < 10.0) ? v : 1.0;
    }();
    return f;
}
double display_size_scale() {
    double mm = PlugInGetDisplaySizeMM();
    int sw = wxGetDisplaySize().GetWidth();
    double csf = OCPN_GetDisplayContentScaleFactor();
    if (!(csf > 0.5 && csf < 6.0)) csf = 1.0;
    if (mm < 1.0 || sw <= 0) return csf * size_cal_factor();
    // Enlarge symbols/text by the content scale (bigger on a HiDPI display). The
    // matching cull-zoom reduction (renderer: cull_zoom = zoom - log2(device_scale))
    // makes the extra size drop features out earlier so it doesn't over-crowd.
    double s = (sw / mm) / kTile57RefPxPerMm * csf * size_cal_factor();
    return (s > 0.1 && s < 12.0) ? s : 1.0;
}
// Pixels per metre of a nominal 96-DPI display; turns a ground resolution into
// an OpenCPN "1:N" scale denominator.
constexpr double kPxPerMetre = 96.0 / 0.0254;

// Live registry of open charts, so the plugin's mouse handler can find the ones
// covering the cursor to query them.
std::vector<ChartTile57*>& chart_registry() {
    static std::vector<ChartTile57*> v;
    return v;
}

// OpenCPN's Mercator scale constant: toSM easting = dLon_rad * a * k0 and
// screen px = easting * view_scale_ppm, so ppm counts PROJECTED metres.
constexpr double kMercatorK0 = 0.9996;

// Web-mercator ground resolution (metres/pixel) at a zoom and latitude.
double resolution_m_per_px(double zoom, double lat_deg) {
    double world_m = std::cos(lat_deg * kPi / 180.0) * 2.0 * kPi * kEarthR;
    return world_m / (256.0 * std::pow(2.0, zoom));
}

// tile57 web-mercator zoom for an OpenCPN view_scale_ppm. Both sides live in
// PROJECTED mercator metres — OpenCPN maps dLon_rad*a*k0 metres to pixels via
// ppm; tile57's world is 256*2^z px for 2*pi*R projected metres — so latitude
// plays no part. (Converting via ground resolution at the view latitude, as
// this plugin first did, rendered cos(lat) zoomed out: a NODTA inset ring
// around the cell, and content sliding against the quilt clip when panning.)
double zoom_for_ppm(double ppm) {
    return std::log2(2.0 * kPi * kEarthR * kMercatorK0 * ppm / 256.0);
}

// The chart's "1:N" scale denominator at a zoom (native S-52 scale sense).
int scale_denom(double zoom, double lat_deg) {
    return (int)std::llround(resolution_m_per_px(zoom, lat_deg) * kPxPerMetre);
}
}  // namespace

ChartTile57::ChartTile57() {
    m_ChartType = PI_CHART_TYPE_PLUGIN;
    m_ChartFamily = PI_CHART_FAMILY_VECTOR;
    m_projection = PI_PROJECTION_MERCATOR;
    tile57_mariner_defaults(&mariner_);
    mariner_.size_scale = display_size_scale();
    chart_registry().push_back(this);
}

ChartTile57::~ChartTile57() {
    // Stop the bake thread first (it publishes into pending_chart_ / calls back into
    // the canvas), then drain any handle it left, then tear down the renderer. join()
    // waits out at most the current bake phase (cancel_ is checked between phases).
    cancel_.store(true, std::memory_order_release);
    if (bake_thread_.joinable()) bake_thread_.join();
    if (tile57_chart* p = pending_chart_.exchange(nullptr, std::memory_order_acq_rel))
        tile57_chart_close(p);

    auto& reg = chart_registry();
    reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());
    renderer_.shutdown();
}

// Pull bbox / native scale / M_COVR coverage from an open chart handle into this
// chart's members (drives GetChartExtent, m_Chart_Scale, GetCOVR*).
void ChartTile57::apply_info(tile57_chart* h, const tile57_chart_info& info) {
    center_lat_ = 0.5 * (info.north + info.south);
    // A cell reports its real compilation scale (CSCL); fall back to the finest baked
    // zoom band otherwise. Using the CSCL is essential — the raw zoom range is 0..18,
    // which would yield an absurd 1:~1700 and OpenCPN would deselect the chart.
    m_Chart_Scale = info.native_scale > 0 ? info.native_scale
                                          : scale_denom(info.max_zoom, center_lat_);
    m_Chart_Skew = 0.0;
    m_depth_unit_id = PI_DEPTH_UNIT_METERS;
    m_DepthUnits = _T("Meters");

    // Coverage rectangle fallback: OpenCPN COVR points are float_2Dpt (lat, lon).
    covr_[0] = (float)info.north; covr_[1] = (float)info.west;   // NW
    covr_[2] = (float)info.north; covr_[3] = (float)info.east;   // NE
    covr_[4] = (float)info.south; covr_[5] = (float)info.east;   // SE
    covr_[6] = (float)info.south; covr_[7] = (float)info.west;   // SW
    covr_valid_ = true;

    // Real M_COVR data-coverage polygons: report these so OpenCPN quilts gaps to
    // coarser cells instead of over-claiming the bbox above.
    covr_tables_.clear();
    tile57_coverage_cb ccb{ &covr_tables_, [](void* ctx, const double* ll, size_t n) {
        auto* tables = static_cast<std::vector<std::vector<float>>*>(ctx);
        std::vector<float> ring;
        ring.reserve(n * 2);
        for (size_t i = 0; i < n; ++i) {   // tile57 gives lon,lat; OpenCPN wants lat,lon
            ring.push_back((float)ll[2 * i + 1]);
            ring.push_back((float)ll[2 * i]);
        }
        tables->push_back(std::move(ring));
    } };
    tile57_chart_coverage(h, &ccb);

    bounds_west_ = info.west; bounds_south_ = info.south;
    bounds_east_ = info.east; bounds_north_ = info.north;
    min_zoom_ = info.min_zoom; max_zoom_ = info.max_zoom;
}

// Bump when the bake format or portrayal changes so stale caches are ignored.
static constexpr int kBakeVersion = 1;

// <cache>/tile57/cells/<stem>-<pathhash>-<keyhash>.pmtiles. The keyhash covers the
// cell path, the freshest mtime across the .000 + its .001.. updates, the update
// count, the bake zoom, and kBakeVersion — so a re-issued cell, an added/edited
// update, a zoom change, or a format bump all miss and re-bake. The stable pathhash
// prefix lets the baker later sweep THIS cell's now-stale siblings. Pure/computation
// only (called during the DB scan, once per cell) — no I/O beyond ensuring the dir.
std::string ChartTile57::cache_path(const std::string& cell_path, uint8_t maxz) const {
    namespace fs = std::filesystem;
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    fs::path dir = (xdg && *xdg) ? fs::path(xdg) : fs::path(home ? home : "/tmp") / ".cache";
    dir = dir / "tile57" / "cells";
    std::error_code ec;
    fs::create_directories(dir, ec);

    long mt = 0;
    int ucount = 0;
    struct stat st {};
    if (::stat(cell_path.c_str(), &st) == 0) mt = (long)st.st_mtime;
    if (cell_path.size() > 4) {
        std::string stem = cell_path.substr(0, cell_path.size() - 4);   // strip ".000"
        for (int u = 1; u <= 999; ++u) {
            char sfx[16];
            std::snprintf(sfx, sizeof sfx, ".%03d", u);
            struct stat us {};
            if (::stat((stem + sfx).c_str(), &us) != 0) break;
            ++ucount;
            if ((long)us.st_mtime > mt) mt = (long)us.st_mtime;
        }
    }
    std::string key = cell_path + "|" + std::to_string(mt) + "|" + std::to_string(ucount) +
                      "|z" + std::to_string((int)maxz) + "|v" + std::to_string(kBakeVersion);
    char ph[12], kh[20];
    std::snprintf(ph, sizeof ph, "%08zx", std::hash<std::string>{}(cell_path) & 0xffffffffu);
    std::snprintf(kh, sizeof kh, "%016zx", std::hash<std::string>{}(key));
    std::string name = fs::path(cell_path).stem().string() + "-" + ph + "-" + kh + ".pmtiles";
    return (dir / name).string();
}

int ChartTile57::Init(const wxString& full_path, int init_flags) {
    m_FullPath = full_path;
    m_Name = wxFileName(full_path).GetName();
    m_Description = _T("tile57 S-57/S-101 ENC (EXPERIMENTAL — NOT FOR NAVIGATION)");
    m_ID = full_path;

    // Resolve the .t57 symlink to the real .000 so tile57 finds the .001.. update
    // chain in the cell's own directory (the symlink tree carries only the base link).
    std::string path = std::string(full_path.mb_str());
    if (char* rp = realpath(path.c_str(), nullptr)) { path = rp; std::free(rp); }

    auto ends_with = [&](const char* suf) {
        std::string s(suf);
        return path.size() >= s.size() && path.compare(path.size() - s.size(), s.size(), s) == 0;
    };

    // A pre-baked *.pmtiles bundle opens directly (cheap archive peek) and renders
    // straight away — no cell parse, no background bake. Metadata comes from the
    // archive (its coverage is the bounding box; a baked bundle has no M_COVR).
    if (ends_with(".pmtiles")) {
        if (!renderer_.open_chart(path)) return PI_INIT_FAIL_REMOVE;
        tile57_chart_info pinfo{};
        if (!renderer_.get_info(pinfo) || !pinfo.has_bounds) return PI_INIT_FAIL_REMOVE;
        apply_info(renderer_.chart_handle(), pinfo);
        m_bReadyToRender = true;
        return PI_INIT_OK;
    }

    // Metadata via a cheap header PARSE (bbox + native scale + coverage) — no tile
    // bake, so a chart-DB scan over hundreds of cells stays fast.
    tile57_chart* h = tile57_chart_open_header(path.c_str());
    if (!h) return PI_INIT_FAIL_REMOVE;
    tile57_chart_info info{};
    tile57_chart_get_info(h, &info);
    if (!info.has_bounds) { tile57_chart_close(h); return PI_INIT_FAIL_REMOVE; }
    apply_info(h, info);
    tile57_chart_close(h);

    // Native zoom + cache path. Cap the bake at the cell's NATIVE zoom (from the
    // CSCL): baking finer overzooms the data and the tile count (hence bake time) blows
    // up (a 1:80k coastal cell is ~11s to native z13, ~34s to z14); the GL renderer
    // overzooms native tiles past that, so nothing is lost.
    bake_path_ = path;
    int zn = info.native_scale > 0
                 ? (int)std::lround(std::log2(559082264.0 / (double)info.native_scale))
                 : 14;
    zn = std::max(4, std::min(zn, 16));
    bake_full_max_ = (uint8_t)zn;
    bake_quick_max_ = (uint8_t)std::max(5, zn - 3);
    cache_file_ = cache_path(path, bake_full_max_);

    // Pre-bake EVERY cell in the background (this runs during the DB scan too) so a
    // cell is cached before it is ever viewed — no per-cell stall while navigating.
    t57::BakeManager::instance().enqueue(path, bake_full_max_, cache_file_);

    wxLogMessage("tile57 Init: %s bounds[W%.5f S%.5f E%.5f N%.5f] zoom[%d..%d] "
                 "scale=1:%d flags=%d", m_FullPath.c_str(), info.west, info.south,
                 info.east, info.north, (int)info.min_zoom, (int)info.max_zoom,
                 m_Chart_Scale, init_flags);

    // Header/thumbnail scan: metadata only, nothing renders — done (and "ready").
    if (init_flags == PI_HEADER_ONLY || init_flags == PI_THUMB_ONLY) {
        ready_.store(true, std::memory_order_release);
        m_bReadyToRender = true;
        return PI_INIT_OK;
    }

    // Cache hit: the full bake already lives on disk — open it directly (fast) and
    // render straight away, no background bake.
    std::error_code ec;
    if (std::filesystem::exists(cache_file_, ec) && renderer_.open_chart(cache_file_)) {
        wxLogMessage("tile57: cache HIT %s", cache_file_.c_str());
        ready_.store(true, std::memory_order_release);
        m_bReadyToRender = true;
        return PI_INIT_OK;
    }
    std::filesystem::remove(cache_file_, ec);   // stale/corrupt (open failed) -> re-bake
    wxLogMessage("tile57: cache MISS, baking -> %s", cache_file_.c_str());

    // Cache miss: bake on a background thread (progressive — a coarse band for first
    // paint, then the full range persisted to the cache so the next run is instant).
    canvas_ = GetOCPNCanvasWindow();
    ready_.store(false, std::memory_order_release);
    cancel_.store(false, std::memory_order_release);
    loading_.store(true, std::memory_order_release);   // show the loader over this cell
    m_bReadyToRender = false;
    start_bake();
    return PI_INIT_OK;
}

// Bake thread: quick native band -> first paint, then full range -> smooth zoom.
// Only READS the warmed global registries; the allocator is thread-safe. Hands each
// baked handle to the render thread via publish(); never touches renderer_.chart_.
void ChartTile57::start_bake() {
    bake_thread_ = std::thread([this]() {
        // Quick coarse band, rendered in-memory (not cached) for a fast first paint.
        if (cancel_.load(std::memory_order_acquire)) return;
        tile57_chart* ov = tile57_chart_open_zoom(bake_path_.c_str(), 0, bake_quick_max_);
        if (cancel_.load(std::memory_order_acquire)) { if (ov) tile57_chart_close(ov); return; }
        if (ov) publish(ov);

        // Full band -> the shared bake manager persists it to the disk cache
        // (coordinated so the background sweep can't also bake this cell), then open
        // the written bundle so the next run/restart is a cache hit.
        if (cancel_.load(std::memory_order_acquire)) return;
        if (!t57::BakeManager::instance().bake_now(bake_path_, bake_full_max_, cache_file_)) return;
        if (cancel_.load(std::memory_order_acquire)) return;
        tile57_chart* full = tile57_chart_open_pmtiles(cache_file_.c_str());
        if (cancel_.load(std::memory_order_acquire)) { if (full) tile57_chart_close(full); return; }
        if (full) publish(full);
    });
}

// Bake thread -> render thread handoff. exchange() returns any prior pending handle
// the render thread never consumed; the bake thread closes that (it was never shared).
void ChartTile57::publish(tile57_chart* c) {
    tile57_chart* prev = pending_chart_.exchange(c, std::memory_order_acq_rel);
    if (prev) tile57_chart_close(prev);
    ready_.store(true, std::memory_order_release);
    if (canvas_) canvas_->CallAfter([w = canvas_]() { w->Refresh(false); });
}

// Render thread: adopt a freshly baked chart if one is waiting (sole owner of chart_).
void ChartTile57::adopt_pending() {
    if (tile57_chart* c = pending_chart_.exchange(nullptr, std::memory_order_acq_rel)) {
        renderer_.set_chart(c);
        loading_.store(false, std::memory_order_release);   // a band landed -> loader off
    }
}

void ChartTile57::SetColorScheme(int cs, bool /*bApplyImmediate*/) {
    switch (cs) {
        case PI_GLOBAL_COLOR_SCHEME_DUSK:  mariner_.scheme = TILE57_SCHEME_DUSK;  break;
        case PI_GLOBAL_COLOR_SCHEME_NIGHT: mariner_.scheme = TILE57_SCHEME_NIGHT; break;
        default:                           mariner_.scheme = TILE57_SCHEME_DAY;   break;
    }
}

double ChartTile57::GetNormalScaleMin(double /*canvas_scale_factor*/, bool b_allow_overzoom) {
    // Allow zooming in past native detail (the renderer overzooms the vectors).
    return m_Chart_Scale * (b_allow_overzoom ? 0.125 : 0.25);
}

double ChartTile57::GetNormalScaleMax(double /*canvas_scale_factor*/, int /*canvas_width*/) {
    // Zoomed-OUT limit: show the cell out to ~4x its native (finest-baked)
    // scale — a native ENC's usable range — so OpenCPN's quilt hands off to a
    // coarser-band cell past that instead of underzooming this one. tile57
    // bands max_zoom by compilation scale (overview cells max ~z10, harbour
    // ~z16), so m_Chart_Scale IS band-appropriate; min_zoom is only a uniform
    // bake floor (z8) and must NOT drive selection, or every cell claims to be
    // usable when zoomed right out.
    return m_Chart_Scale * 4.0;
}

double ChartTile57::GetNearestPreferredScalePPM(double target_scale_ppm) {
    return target_scale_ppm;  // continuous scale support
}

bool ChartTile57::GetChartExtent(ExtentPI* pext) {
    if (!pext || !covr_valid_) return false;
    pext->NLAT = bounds_north_;
    pext->SLAT = bounds_south_;
    pext->WLON = bounds_west_;
    pext->ELON = bounds_east_;
    return true;
}

void ChartTile57::GetValidCanvasRegion(const PlugIn_ViewPort& vp, wxRegion* pValidRegion) {
    // The chart paints the whole view; OpenCPN clips to the COVR coverage during
    // quilting. Report the full canvas as valid.
    if (pValidRegion) *pValidRegion = wxRegion(0, 0, vp.pix_width, vp.pix_height);
}

void ChartTile57::refresh_mariner() {
    // PI_GetPLIBStateHash changes whenever an S52/vector-chart option changes;
    // between changes this is a single int compare per render.
    int hash = PI_GetPLIBStateHash();
    if (hash == plib_hash_) return;
    plib_hash_ = hash;

    mariner_.safety_contour = PI_GetPLIBMarinerSafetyContour();
    // PI_GetPLIBSymbolStyle/BoundaryStyle return S52 LUP table names:
    // 'L' simplified / 'R' paper-chart points; 'N' plain / 'O' symbolized.
    mariner_.simplified_points = (PI_GetPLIBSymbolStyle() == 'L');
    // Area boundaries: 'O' symbolized (richer edge symbology) / 'N' plain. Was
    // hardcoded to symbolized, so OpenCPN's Plain/Symbolized toggle did nothing.
    mariner_.boundary_style = (PI_GetPLIBBoundaryStyle() == 'O') ? TILE57_BOUNDARY_SYMBOLIZED
                                                                 : TILE57_BOUNDARY_PLAIN;
    mariner_.depth_unit = (PI_GetPLIBDepthUnitInt() == 0) ? TILE57_DEPTH_FEET
                                                          : TILE57_DEPTH_METERS;
    // Information callouts (INFORM/TXTDSC balloons) clutter the chart — keep them
    // off unconditionally (set here, not in the config block below which may be
    // skipped when GetOCPNConfigObject() is null).
    mariner_.show_inform_callouts = false;

    // The remaining vector-chart options aren't exposed through PI getters;
    // OpenCPN's live config object carries them (the options dialog writes
    // them there, and the PLIB state hash bumps at the same time).
    if (wxFileConfig* cfg = GetOCPNConfigObject()) {
        wxString path = cfg->GetPath();
        cfg->SetPath("/Settings/GlobalState");

        bool soundings = true, text = true, ldis = false, imp_only = false;
        cfg->Read("bShowSoundg", &soundings, true);
        cfg->Read("bShowS57Text", &text, true);
        cfg->Read("bShowLdisText", &ldis, false);
        cfg->Read("bShowS57ImportantTextOnly", &imp_only, false);
        mariner_.text_names = text;
        mariner_.text_other = text && !imp_only;
        mariner_.show_light_descriptions = ldis;

        // nDisplayCategory holds an S52 _DisCat: 'D' base, 'S' standard,
        // 'O' other/all, 'M' mariner's standard.
        long cat = 'O';
        cfg->Read("nDisplayCategory", &cat, (long)'O');
        mariner_.display_base = true;
        mariner_.display_standard = (cat != 'D');
        // tile57 has no dedicated soundings switch; soundings are OTHER
        // category, so honour OpenCPN's explicit soundings toggle here.
        mariner_.display_other = (cat == 'O') || soundings;

        // bShowMeta drives OpenCPN's meta-object (M_* boundary) display.
        bool meta = false;
        cfg->Read("bShowMeta", &meta, false);
        mariner_.show_meta_bounds = meta;

        double v;
        // Prefer the stored contour values (the mariner's set depths) over the
        // PI getter, which can hand back the snapped/displayed safety contour.
        if (cfg->Read("S52_MAR_SAFETY_CONTOUR", &v)) mariner_.safety_contour = v;
        if (cfg->Read("S52_MAR_SHALLOW_CONTOUR", &v)) mariner_.shallow_contour = v;
        if (cfg->Read("S52_MAR_DEEP_CONTOUR", &v)) mariner_.deep_contour = v;
        if (cfg->Read("S52_MAR_SAFETY_DEPTH", &v)) mariner_.safety_depth = v;
        long two_shades = 0;
        if (cfg->Read("S52_MAR_TWO_SHADES", &two_shades)) mariner_.four_shade_water = !two_shades;

        // Full-length light sector lines (OpenCPN "Extended light sectors").
        bool full_sectors = true;
        cfg->Read("bExtendLightSectors", &full_sectors, true);
        mariner_.show_full_sector_lines = full_sectors;
        // SCAMIN on/off (bUseSCAMIN); off => show every feature regardless of scale.
        bool use_scamin = true;
        cfg->Read("bUseSCAMIN", &use_scamin, true);
        mariner_.ignore_scamin = !use_scamin;

        // Data-quality (M_QUAL/QUAPOS) display is a per-canvas setting.
        cfg->SetPath("/Canvas/CanvasConfig1");
        bool dq = false;
        cfg->Read("canvasENCShowDataQuality", &dq, false);
        mariner_.data_quality = dq;

        cfg->SetPath(path);
    }
}

void ChartTile57::draw_calibration() const {
    static const bool on = std::getenv("TILE57_CALIB") != nullptr;
    if (!on) return;
    GLint gvp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, gvp);
    const float W = (float)gvp[2], H = (float)gvp[3];
    if (W <= 0.f || H <= 0.f) return;

    // A 5mm square at the SAME effective px/mm the chart symbols use (kTile57RefPxPerMm
    // is tile57's size_scale=1 density; mariner_.size_scale carries the display + HiDPI
    // scaling). Measured with a ruler it should be exactly 5mm — S-52 CHKSYM01.
    const float side = (float)(5.0 * kTile57RefPxPerMm * mariner_.size_scale);
    const float cx = W * 0.5f, cy = H * 0.5f, hs = side * 0.5f;
    static bool logged = false;
    if (!logged) {
        logged = true;
        wxLogMessage("tile57 CALIB: 5mm square = %.1f px  (size_scale=%.3f, %.2f px/mm, "
                     "displayMM=%.1f, csf=%.2f)", side, mariner_.size_scale,
                     kTile57RefPxPerMm * mariner_.size_scale, PlugInGetDisplaySizeMM(),
                     OCPN_GetDisplayContentScaleFactor());
    }

    glUseProgram(0);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity();
    glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glDisable(GL_TEXTURE_2D); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    glColor3f(0.f, 0.f, 0.f);           // filled black 5mm square (like CHKSYM01)
    glBegin(GL_QUADS);
    glVertex2f(cx - hs, cy - hs); glVertex2f(cx + hs, cy - hs);
    glVertex2f(cx + hs, cy + hs); glVertex2f(cx - hs, cy + hs);
    glEnd();
    glColor3f(1.f, 0.25f, 0.25f);       // crisp red edge for measuring
    glLineWidth(1.f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(cx - hs, cy - hs); glVertex2f(cx + hs, cy - hs);
    glVertex2f(cx + hs, cy + hs); glVertex2f(cx - hs, cy + hs);
    glEnd();
    glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(GL_MODELVIEW); glPopMatrix();
}

int ChartTile57::render_pass(const PlugIn_ViewPort& vp, t57::ChartRenderer::Pass pass,
                             bool stencil_clip) {
    if (!vp.bValid) return false;
    // Adopt a freshly baked chart if the background thread has one waiting (main-thread
    // owner of chart_). Until the first band lands, draw nothing here — the plugin's GL
    // overlay paints the animated 8-bit loader over this cell's bbox (loading_extent).
    adopt_pending();
    if (!renderer_.has_chart()) return true;
    if (!renderer_.ensure_gl()) return false;
    refresh_mariner();

    // Render into the CURRENT GL viewport — that is what NDC maps to, and it is
    // OpenCPN's choice of target (canvas FBO, or a sub-rect of it). Do NOT size
    // the render from vp.pix_width: OpenCPN sometimes hands charts an expanded
    // ViewPort (rv_rect under rotation; the text pass grows pix_width laterally)
    // whose pixel dims exceed the render target, and sizing from them squeezes
    // the whole render (~"slightly zoomed out").
    GLint gvp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, gvp);
    uint32_t fbw = gvp[2] > 0 ? (uint32_t)gvp[2] : (uint32_t)vp.pix_width;
    uint32_t fbh = gvp[3] > 0 ? (uint32_t)gvp[3] : (uint32_t)vp.pix_height;

    // Scale comes from view_scale_ppm ALONE (metres per rendered pixel), which
    // is invariant under viewport expansion. In GL mode OpenCPN keeps ViewPort
    // pixels and view_scale_ppm in framebuffer (physical) units; if a build
    // hands us a logical-unit ViewPort against a HiDPI framebuffer (glViewport
    // == contentScale * pix_width), correct ppm by that factor.
    double ppm = (vp.view_scale_ppm > 0) ? vp.view_scale_ppm : 0.01;
    double csf = OCPN_GetDisplayContentScaleFactor();
    // When OpenCPN hands a LOGICAL-unit ViewPort against a HiDPI framebuffer, the raw
    // ppm is logical. Instead of bumping the zoom by contentScale (which also shifts
    // tile detail + SCAMIN, over-crowding when zoomed out on a 2x display), keep the
    // zoom GEOGRAPHIC and pass contentScale as device_scale so ONLY the projection
    // scales up to fill the physical framebuffer.
    double device_scale =
        (csf > 1.0 && fbw == (uint32_t)std::lround(vp.pix_width * csf)) ? csf : 1.0;
    double zoom = zoom_for_ppm(ppm);   // geographic (un-bumped)
    last_zoom_ = zoom;   // remembered for the object-query pick tolerance / SCAMIN
    renderer_.render(vp.clon, vp.clat, zoom, fbw, fbh, mariner_, pass, stencil_clip, device_scale);
    if (pass != t57::ChartRenderer::Pass::kText) draw_calibration();
    return true;
}

int ChartTile57::RenderRegionViewOnGL(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                      const wxRegion& /*Region*/, bool /*b_use_stencil*/) {
    // Single-chart (unquilted) path: the core wrapper scissors per rect; draw all.
    return render_pass(vp, t57::ChartRenderer::Pass::kAll, false);
}

int ChartTile57::RenderRegionViewOnGLNoText(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                            const wxRegion& /*Region*/, bool b_use_stencil) {
    // Quilt geometry pass. The core pre-writes this chart's patch region into
    // the stencil buffer (SetClipRegion) and expects the chart to clip itself.
    return render_pass(vp, t57::ChartRenderer::Pass::kBase, b_use_stencil);
}

int ChartTile57::RenderRegionViewOnGLTextOnly(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                              const wxRegion& /*Region*/, bool /*b_use_stencil*/) {
    // Quilt text pass: once across the whole quilt, unclipped (text declutters
    // across patch boundaries).
    return render_pass(vp, t57::ChartRenderer::Pass::kText, false);
}

// ---- object query (S-52 §10.8 pick) ----------------------------------------
namespace {
struct QueryHit { std::string cls, s57, cell; };
void collect_hit(void* ctx, const char* cls, size_t cl, const char* s57, size_t sl,
                 const char* cell, size_t cel) {
    static_cast<std::vector<QueryHit>*>(ctx)->push_back(
        {std::string(cls, cl), std::string(s57, sl), std::string(cell, cel)});
}

// Append a flat JSON object {"KEY":"VAL",...} (the pick blob) as HTML table rows.
void json_rows(const std::string& j, wxString& out) {
    size_t i = 0, n = j.size();
    auto ws = [&] { while (i < n && (j[i]==' '||j[i]=='\n'||j[i]=='\t'||j[i]=='\r')) ++i; };
    auto str = [&](std::string& s) -> bool {
        if (i >= n || j[i] != '"') return false;
        ++i; s.clear();
        while (i < n && j[i] != '"') { if (j[i] == '\\' && i + 1 < n) ++i; s += j[i++]; }
        if (i < n) ++i;
        return true;
    };
    ws(); if (i < n && j[i] == '{') ++i;
    while (i < n) {
        ws(); if (i < n && j[i] == '}') break;
        std::string k, v;
        if (!str(k)) break;
        ws(); if (i < n && j[i] == ':') ++i; ws();
        if (!str(v)) break;
        out += "<tr><td valign=top><b>" + wxString::FromUTF8(k.c_str()) +
               "</b></td><td>" + wxString::FromUTF8(v.c_str()) + "</td></tr>";
        ws(); if (i < n && j[i] == ',') ++i;
    }
}

// Feature blocks — the plugin wraps the concatenation in <html><body>. Identical
// features (same class + attributes + cell) are collapsed: one S-57 feature can be
// split across several MVT features, and a zoomed-out pick spans many like ones.
wxString build_query_html(const std::vector<QueryHit>& hits) {
    wxString h;
    std::unordered_set<std::string> seen;
    for (const auto& f : hits) {
        if (!seen.insert(f.cls + '\0' + f.s57 + '\0' + f.cell).second) continue;
        h += "<font size=+1><b>" + wxString::FromUTF8(f.cls.c_str()) + "</b></font>";
        if (!f.cell.empty())
            h += "  <font size=-1 color=#808080>" + wxString::FromUTF8(f.cell.c_str()) + "</font>";
        h += "<table>";
        json_rows(f.s57, h);
        h += "</table><hr>";
    }
    return h;
}
}  // namespace

const std::vector<ChartTile57*>& ChartTile57::instances() { return chart_registry(); }

wxString ChartTile57::QueryDescription(double lon, double lat) const {
    tile57_chart* ch = renderer_.chart_handle();
    if (!ch) return wxEmptyString;
    std::vector<QueryHit> hits;
    tile57_query_cb cb{ &hits, collect_hit };
    tile57_chart_query(ch, lon, lat, last_zoom_, &cb);
    if (hits.empty()) return wxEmptyString;
    return build_query_html(hits);
}

bool ChartTile57::covers(double lon, double lat) const {
    return covr_valid_ && lon >= bounds_west_ && lon <= bounds_east_
        && lat >= bounds_south_ && lat <= bounds_north_;
}

wxBitmap& ChartTile57::transparent_bitmap(const PlugIn_ViewPort& vp) {
    int w = vp.pix_width > 0 ? vp.pix_width : 1;
    int h = vp.pix_height > 0 ? vp.pix_height : 1;
    wxImage img(w, h);
    img.InitAlpha();
    std::memset(img.GetAlpha(), 0, (size_t)w * h);  // fully transparent
    dc_bmp_ = wxBitmap(img);
    return dc_bmp_;
}

wxBitmap& ChartTile57::RenderRegionView(const PlugIn_ViewPort& vp, const wxRegion& /*Region*/) {
    return transparent_bitmap(vp);
}

wxBitmap& ChartTile57::RenderRegionViewOnDCNoText(const PlugIn_ViewPort& vp, const wxRegion& /*Region*/) {
    return transparent_bitmap(vp);
}

bool ChartTile57::RenderRegionViewOnDCTextOnly(wxMemoryDC& /*dc*/, const PlugIn_ViewPort& /*VPoint*/,
                                               const wxRegion& /*Region*/) {
    return false;  // GL-only plugin; nothing to draw on the DC canvas
}
