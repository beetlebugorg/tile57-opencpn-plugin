// tile57_chart.cpp — see tile57_chart.h.
#include "tile57_chart.h"
#include "gl.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/jsonreader.h>
#include <wx/jsonval.h>
#include <wx/log.h>

// OpenCPN 5.12 DECLARES GetEnableLightDescriptionsDisplay in the 1.19 API header but does
// NOT export it from the executable — the setter (EnableLightDescriptionsDisplay) is
// exported, the getter is not. Calling it normally would leave the plugin with a symbol
// the host cannot resolve. Declare it weak: on a host that lacks it the address is null
// and we fall back to the config file, and on one that has it we get the live value.
#if defined(__GNUC__) || defined(__clang__)
extern DECL_EXP bool GetEnableLightDescriptionsDisplay(int CanvasIndex) __attribute__((weak));
#define TILE57_WEAK_LIGHTDESC 1
#endif

// wxWidgets can create ChartTile57 by class name; OpenCPN uses this to
// instantiate the chart for each matching file it finds in a chart directory.
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57, PlugInChartBaseExtended);
// The concrete class OpenCPN instantiates by name; it only sets GetFileSearchMask,
// everything else is the shared base.
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57Pmtiles, ChartTile57);

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
double display_size_scale(double csf) {
    double mm = PlugInGetDisplaySizeMM();
    int sw = wxGetDisplaySize().GetWidth();
    if (mm < 1.0 || sw <= 0)
        return size_cal_factor();
    // S-52 sizes symbology in physical mm, so we need the display's PHYSICAL px/mm.
    // wxGetDisplaySize() returns LOGICAL points on macOS (DIPs generally), so scale by
    // the content-scale factor (csf) to recover physical pixels first. Without this, a
    // HiDPI display (csf=2) renders every symbol/label HALF size — measured: the 5mm
    // CHKSYM01 square came out 2.5mm on a csf=2 (4K @ 2x) external monitor. csf=1
    // (non-HiDPI / Linux) makes this a no-op. IMPORTANT: csf must be the RENDER-time
    // value — at construction OCPN_GetDisplayContentScaleFactor() returns 1 (the chart's
    // canvas isn't on its display yet), so render_pass recomputes this when csf is known.
    // TILE57_SIZE trims. (This scales SIZES only — the SCAMIN cull is not coupled to it.)
    if (!(csf > 0.0))
        csf = 1.0;
    double s = (sw * csf / mm) / kTile57RefPxPerMm * size_cal_factor();
    return (s > 0.1 && s < 12.0) ? s : 1.0;
}
// display_size_scale TIMES OpenCPN's "Chart object scale factor" slider — composed in ONE
// place because it used to be two, and the render pass's copy left the slider out, so the
// object scale vanished on the first frame whose csf differed from the cached one.
double composed_size_scale(double csf) {
    const float obj = GetOCPNChartScaleFactor_Plugin();
    return display_size_scale(csf) * (obj > 0.1f ? obj : 1.0f);
}
// Pixels per metre of a nominal 96-DPI display; turns a ground resolution into
// an OpenCPN "1:N" scale denominator.
constexpr double kPxPerMetre = 96.0 / 0.0254;

// Live registry of open charts, so the plugin's mouse handler can find the ones
// covering the cursor to query them.
//
// MUST be locked. OpenCPN builds the chart database on a THREAD POOL for plugins at
// API 118+ (it only defers chart tickets to the main thread for older plugins, which it
// assumes aren't thread-safe), so adding a chart directory constructs many ChartTile57
// concurrently — an unsynchronised push_back here corrupted the vector mid-scan. The
// mouse handler then reads the registry from the main thread while that scan is still
// running, so instances() hands back a SNAPSHOT rather than a reference into a vector a
// scan thread may be reallocating.
std::mutex& registry_mtx() {
    static std::mutex m;
    return m;
}
std::vector<ChartTile57*>& chart_registry() {
    static std::vector<ChartTile57*> v;
    return v;
}

// S-52 §14.5 viewing group for light (LIGHTS) in the S-101 portrayal catalogue. OpenCPN's
// "Show Lights" has no tile57_mariner field of its own — lights are a viewing group, so
// switching them off means denying this group.
constexpr int32_t kVGLights = 27070;

// The S52 settings OpenCPN pushes as the "OpenCPN Config" message. Written on the main
// thread from SetPluginMessage, read on the main thread during render; the mutex is
// belt-and-braces (OpenCPN constructs plugin charts on a scan thread pool, so a chart's
// first refresh can race a message). `gen` bumps per message so a chart knows to re-fold
// it in. `have` stays false until the first message: until then the config file (which
// OpenCPN DOES refresh from the Options dialog) is the fallback.
struct S52Config {
    std::mutex mtx;
    uint64_t gen = 0;
    bool have = false;
    bool meta = false, important_text_only = false, use_scamin = true;
    bool anchor = false, quality_of_data = false;
    bool two_shades = false;
    double safety_depth = 0, shallow_contour = 0, deep_contour = 0;
    // The two ENC size sliders, as their raw -5..+5 slider positions (0 = catalogue size).
    // OpenCPN scales ENC TEXT and SOUNDINGS independently of the "Chart Object" factor, and
    // pushes both here (chcanv.cpp: "OpenCPN S52PLIB TextFactor"/"SoundingsFactor").
    int text_factor = 0, sounding_factor = 0;
};
S52Config& s52cfg() {
    static S52Config c;
    return c;
}

// The two ENC size sliders, converted to multipliers EXACTLY as OpenCPN does
// (s52plib.cpp: "Precalulate the ENC scale factors"):
//     m_TextScaleFactor      = exp(n * (log(2.0) / 5.0));   // 2^(n/5): 0.5x .. 2.0x
//     m_SoundingsScaleFactor = (n * .1) + 1;                // linear:  0.5x .. 1.5x
// Note they are NOT the same curve — text is exponential, soundings linear — so don't be
// tempted to share one. n is the slider position, clamped to its -5..+5 range.
double enc_text_scale(int n) {
    n = std::clamp(n, -5, 5);
    return std::exp(n * (std::log(2.0) / 5.0));
}
double enc_sounding_scale(int n) {
    n = std::clamp(n, -5, 5);
    return (n * 0.1) + 1.0;
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

} // namespace

ChartTile57::ChartTile57() {
    m_ChartType = PI_CHART_TYPE_PLUGIN;
    m_ChartFamily = PI_CHART_FAMILY_VECTOR;
    m_projection = PI_PROJECTION_MERCATOR;
    tile57_mariner_defaults(&mariner_);
    // Placeholder — csf reads 1 here (canvas not yet realized); render_pass recomputes
    // it once the real content-scale is known.
    mariner_.size_scale = display_size_scale(OCPN_GetDisplayContentScaleFactor());
    {
        std::lock_guard<std::mutex> lk(registry_mtx());
        chart_registry().push_back(this);
    }
}

ChartTile57::~ChartTile57() {
    {
        std::lock_guard<std::mutex> lk(registry_mtx());
        auto& reg = chart_registry();
        reg.erase(std::remove(reg.begin(), reg.end(), this), reg.end());
    }
    renderer_.shutdown();
}

// Pull bbox / native scale / M_COVR coverage from an open chart handle into this
// chart's members (drives GetChartExtent, m_Chart_Scale, GetCOVR*).
void ChartTile57::apply_info(tile57_chart* h, const tile57_info& info) {
    center_lat_ = 0.5 * (info.north + info.south);
    // A cell reports its real compilation scale (CSCL); fall back to the finest baked
    // zoom band otherwise. Using the CSCL is essential — the raw zoom range is 0..18,
    // which would yield an absurd 1:~1700 and OpenCPN would deselect the chart.
    m_Chart_Scale =
        info.native_scale > 0 ? info.native_scale : scale_denom(info.max_zoom, center_lat_);
    m_Chart_Skew = 0.0;
    m_depth_unit_id = PI_DEPTH_UNIT_METERS;
    m_DepthUnits = _T("Meters");

    // Coverage rectangle fallback: OpenCPN COVR points are float_2Dpt (lat, lon).
    covr_[0] = (float)info.north;
    covr_[1] = (float)info.west; // NW
    covr_[2] = (float)info.north;
    covr_[3] = (float)info.east; // NE
    covr_[4] = (float)info.south;
    covr_[5] = (float)info.east; // SE
    covr_[6] = (float)info.south;
    covr_[7] = (float)info.west; // SW
    covr_valid_ = true;

    // Real M_COVR data-coverage polygons: report these so OpenCPN quilts gaps to
    // coarser cells instead of over-claiming the bbox above.
    covr_tables_.clear();
    tile57_coverage_cb ccb{&covr_tables_, [](void* ctx, const double* ll, size_t n) {
                               auto* tables = static_cast<std::vector<std::vector<float>>*>(ctx);
                               std::vector<float> ring;
                               ring.reserve(n * 2);
                               for (size_t i = 0; i < n;
                                    ++i) { // tile57 gives lon,lat; OpenCPN wants lat,lon
                                   ring.push_back((float)ll[2 * i + 1]);
                                   ring.push_back((float)ll[2 * i]);
                               }
                               tables->push_back(std::move(ring));
                           }};
    tile57_chart_coverage(h, &ccb, nullptr);

    bounds_west_ = info.west;
    bounds_south_ = info.south;
    bounds_east_ = info.east;
    bounds_north_ = info.north;
    min_zoom_ = info.min_zoom;
    max_zoom_ = info.max_zoom;
}

int ChartTile57::Init(const wxString& full_path, int init_flags) {
    m_FullPath = full_path;
    m_Name = wxFileName(full_path).GetName();
    m_Description = _T("tile57 S-57/S-101 ENC (EXPERIMENTAL — NOT FOR NAVIGATION)");
    m_ID = full_path;

    // The plugin registers only the *.pmtiles class, so every chart is a pre-baked
    // tile57 bundle: open the archive directly (a cheap peek) and render straight away —
    // no cell parse, no bake. Metadata (bounds, native scale, M_COVR coverage) comes
    // from the archive itself. Cells are baked to *.pmtiles up front via the Build
    // Charts dialog (see build_charts.cpp).
    std::string path = std::string(full_path.mb_str());
    if (!renderer_.open_chart(path))
        return PI_INIT_FAIL_REMOVE;
    tile57_info info{};
    if (!renderer_.get_info(info) || !info.has_bounds)
        return PI_INIT_FAIL_REMOVE;
    apply_info(renderer_.chart_handle(), info);
    wxLogMessage("tile57 Init: %s bounds[W%.5f S%.5f E%.5f N%.5f] zoom[%d..%d] scale=1:%d flags=%d",
                 m_FullPath.c_str(), info.west, info.south, info.east, info.north,
                 (int)info.min_zoom, (int)info.max_zoom, m_Chart_Scale, init_flags);
    m_bReadyToRender = true;
    return PI_INIT_OK;
}

void ChartTile57::SetColorScheme(int cs, bool /*bApplyImmediate*/) {
    switch (cs) {
    case PI_GLOBAL_COLOR_SCHEME_DUSK:
        mariner_.scheme = TILE57_SCHEME_DUSK;
        break;
    case PI_GLOBAL_COLOR_SCHEME_NIGHT:
        mariner_.scheme = TILE57_SCHEME_NIGHT;
        break;
    default:
        mariner_.scheme = TILE57_SCHEME_DAY;
        break;
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
    return target_scale_ppm; // continuous scale support
}

bool ChartTile57::GetChartExtent(ExtentPI* pext) {
    if (!pext || !covr_valid_)
        return false;
    pext->NLAT = bounds_north_;
    pext->SLAT = bounds_south_;
    pext->WLON = bounds_west_;
    pext->ELON = bounds_east_;
    return true;
}

void ChartTile57::GetValidCanvasRegion(const PlugIn_ViewPort& vp, wxRegion* pValidRegion) {
    // The chart paints the whole view; OpenCPN clips to the COVR coverage during
    // quilting. Report the full canvas as valid.
    if (pValidRegion)
        *pValidRegion = wxRegion(0, 0, vp.pix_width, vp.pix_height);
}

// Parse the "OpenCPN Config" message body. OpenCPN emits a flat JSON object of its live
// S52PLIB state (PlugInManager::SendS52ConfigToAllPlugIns).
void ChartTile57::ApplyS52ConfigMessage(const wxString& body) {
    wxJSONValue root;
    wxJSONReader reader;
    if (reader.Parse(body, &root) > 0) // >0 == parse errors
        return;

    auto get_bool = [&](const char* key, bool dflt) {
        const wxJSONValue v = root.Get(key, wxJSONValue(dflt));
        return v.IsBool() ? v.AsBool() : (v.IsInt() ? v.AsInt() != 0 : dflt);
    };
    auto get_double = [&](const char* key, double dflt) {
        const wxJSONValue v = root.Get(key, wxJSONValue(dflt));
        return v.IsDouble() ? v.AsDouble() : (v.IsInt() ? (double)v.AsInt() : dflt);
    };

    S52Config& c = s52cfg();
    std::lock_guard<std::mutex> lk(c.mtx);
    c.meta = get_bool("OpenCPN S52PLIB MetaDisplay", c.meta);
    c.important_text_only =
        get_bool("OpenCPN S52PLIB ShowImportantTextOnly", c.important_text_only);
    c.use_scamin = get_bool("OpenCPN S52PLIB UseSCAMIN", c.use_scamin);
    c.anchor = get_bool("OpenCPN S52PLIB ShowAnchorConditions", c.anchor);
    c.quality_of_data = get_bool("OpenCPN S52PLIB ShowQualityOfData", c.quality_of_data);
    // ColorShades is S52_MAR_TWO_SHADES: 1 == two shades, 0 == four.
    c.two_shades = get_double("OpenCPN S52PLIB ColorShades", c.two_shades ? 1.0 : 0.0) != 0.0;
    c.safety_depth = get_double("OpenCPN S52PLIB Safety Depth", c.safety_depth);
    c.shallow_contour = get_double("OpenCPN S52PLIB Shallow Contour", c.shallow_contour);
    c.deep_contour = get_double("OpenCPN S52PLIB Deep Contour", c.deep_contour);
    // The ENC Text / ENC Sounding sliders (raw -5..+5). These live ONLY on the sliders and
    // in the config file — there is no plugin getter for either — so this push and the
    // config-file seed in refresh_mariner are the only ways to see them.
    c.text_factor = (int)std::lround(get_double("OpenCPN S52PLIB TextFactor", c.text_factor));
    c.sounding_factor =
        (int)std::lround(get_double("OpenCPN S52PLIB SoundingsFactor", c.sounding_factor));
    c.have = true;
    ++c.gen;
}

// The per-canvas ENC state — OpenCPN's "Chart Panel Options" flyout (and the ENC menu /
// keyboard toggles). These live ONLY on the ChartCanvas: it pushes them straight into
// OpenCPN's s52plib at render time and never writes them to the config object, so before
// API 1.19 a plugin simply could not see them. Read every refresh rather than behind the
// PLIB-hash gate below: they are trivial accessors, and the canvas can change one without
// the plib hash having been bumped yet at the moment we look.
//
// Canvas 0 (the primary). OpenCPN can run several canvases with DIFFERENT ENC settings,
// but a chart object is shared across them and tile57's mariner state is per-chart, so
// there is no per-canvas answer to give; the primary canvas wins.
void ChartTile57::apply_canvas_enc_options() {
    const int ci = 0;
    const bool text = GetEnableENCTextDisplay(ci);
    const bool soundings = GetEnableENCDepthSoundingsDisplay(ci);
    const bool lights = GetEnableLightsDisplay(ci);
    // Light descriptions: live where the host exports the getter, else the config seed
    // (see the weak declaration above — OpenCPN 5.12 does not export it).
    bool light_desc = light_desc_cfg_;
#ifdef TILE57_WEAK_LIGHTDESC
    if (GetEnableLightDescriptionsDisplay)
        light_desc = GetEnableLightDescriptionsDisplay(ci);
#endif

    mariner_.text_names = text;
    mariner_.text_other = text && !important_text_only_;
    mariner_.show_light_descriptions = light_desc;

    // PI_DisCat: 'D' base, 'S' standard, 'O' other/all, 'M' mariner's standard.
    const PI_DisCat cat = GetENCDisplayCategory(ci);
    mariner_.display_base = true;
    mariner_.display_standard = (cat != PI_DISPLAYBASE);
    // tile57 has no dedicated soundings switch — soundings are OTHER category — so
    // honour OpenCPN's explicit soundings toggle alongside the category.
    mariner_.display_other = (cat == PI_OTHER) || soundings;

    // Lights aren't a mariner flag in S-52, they're viewing group 27070: switching them
    // off means denying that group. Keep the vector stable so the pointer we hand the
    // engine stays valid (it must outlive the call).
    vg_off_.clear();
    if (!lights)
        vg_off_.push_back(kVGLights);
    mariner_.viewing_groups_off = vg_off_.empty() ? nullptr : vg_off_.data();
    mariner_.viewing_groups_off_len = (uint32_t)vg_off_.size();
}

void ChartTile57::refresh_mariner() {
    // ORDER MATTERS. apply_canvas_enc_options() consumes state this function LOADS
    // (light_desc_cfg_, important_text_only_), so it has to run AFTER the loads below, not
    // before. It used to run first, which meant a chart's FIRST frame portrayed every tile
    // with the not-yet-loaded defaults, the loads then changed them, and the SECOND frame saw
    // a different mariner hash and threw the whole cache away — a full double portray on
    // every chart open. OpenCPN purges and reopens charts constantly (its chart cache holds
    // ~20 while a quilt walks hundreds of cells), so that was being paid over and over.
    //
    // The loads are gated on OpenCPN's S52 state hash + our config-message generation, so
    // between actual settings changes this whole block is two int compares; the canvas ENC
    // toggles at the bottom are live and cheap, and run every frame.
    const int hash = PI_GetPLIBStateHash();
    uint64_t gen;
    {
        S52Config& c = s52cfg();
        std::lock_guard<std::mutex> lk(c.mtx);
        gen = c.gen;
    }
    if (hash != plib_hash_ || gen != s52_msg_gen_) {
        plib_hash_ = hash;
        s52_msg_gen_ = gen;
        load_s52_state();
    }

    // Live per-canvas ENC toggles (Chart Panel flyout / ENC menu). These read the canvas
    // directly every frame, and now see the settings loaded above on the very first one.
    apply_canvas_enc_options();
    // important_text_only_ may have just moved; recompose the text gate it feeds.
    mariner_.text_other = mariner_.text_names && !important_text_only_;

    // SUPER-SCAMIN: impute a SCAMIN for features this cell left ungated, from the cell's own
    // compilation scale (ChartRenderer::feature_scamin explains why; s52plib does the same).
    // It rides the host's "Use SCAMIN" switch — turning SCAMIN off must show EVERYTHING, and
    // an imputed threshold is still a SCAMIN. TILE57_SUPERSCAMIN=0 disables it on its own,
    // which is the A/B for "is the imputation what's hiding this feature".
    //
    // NOT wired to OpenCPN's own UseSUPER_SCAMIN flag, whose default is OFF (navutil.cpp
    // reads it with default 0): the core can afford that because its quilt rarely puts a fine
    // cell under a coarse view, while every tile57 chart IS one cell and lands under any view
    // the quilt hands it. Defaulting to the core's OFF would just restore the carpet.
    static const bool super_off = [] {
        const char* e = std::getenv("TILE57_SUPERSCAMIN");
        return e && std::atoi(e) == 0;
    }();
    renderer_.set_super_scamin(!super_off && !mariner_.ignore_scamin, (double)m_Chart_Scale);
}

// The S52 state OpenCPN only changes when the user changes a setting: the PI_GetPLIB*
// getters, the config file, and the pushed "OpenCPN Config" message. Split out of
// refresh_mariner so the ordering above is stated once and cannot drift back.
void ChartTile57::load_s52_state() {
    mariner_.safety_contour = PI_GetPLIBMarinerSafetyContour();
    // PI_GetPLIBSymbolStyle/BoundaryStyle return S52 LUP table names:
    // 'L' simplified / 'R' paper-chart points; 'N' plain / 'O' symbolized.
    mariner_.simplified_points = (PI_GetPLIBSymbolStyle() == 'L');
    // Area boundaries: 'O' symbolized (richer edge symbology) / 'N' plain. Was
    // hardcoded to symbolized, so OpenCPN's Plain/Symbolized toggle did nothing.
    mariner_.boundary_style =
        (PI_GetPLIBBoundaryStyle() == 'O') ? TILE57_BOUNDARY_SYMBOLIZED : TILE57_BOUNDARY_PLAIN;
    mariner_.depth_unit = (PI_GetPLIBDepthUnitInt() == 0) ? TILE57_DEPTH_FEET : TILE57_DEPTH_METERS;
    // Information callouts (INFORM/TXTDSC balloons) clutter the chart — keep them off
    // unconditionally (OpenCPN has no equivalent toggle).
    mariner_.show_inform_callouts = false;

    // OpenCPN's "Chart object scale factor" slider, folded into the physical symbol scale
    // the display calibration already computed (see composed_size_scale).
    const double csf = size_scale_csf_ > 0 ? size_scale_csf_ : OCPN_GetDisplayContentScaleFactor();
    mariner_.size_scale = composed_size_scale(csf);

    // The config file: OpenCPN refreshes it whenever the Options dialog is applied
    // (MyFrame::ProcessOptionsDialog ends in pConfig->UpdateSettings()), so these are
    // current for every setting that lives ONLY in that dialog. Read first, so the
    // pushed message below can override where the two overlap.
    if (wxFileConfig* cfg = GetOCPNConfigObject()) {
        const wxString path = cfg->GetPath();
        cfg->SetPath("/Settings/GlobalState");

        bool imp_only = false;
        cfg->Read("bShowS57ImportantTextOnly", &imp_only, false);
        important_text_only_ = imp_only;

        bool meta = false;
        cfg->Read("bShowMeta", &meta, false);
        mariner_.show_meta_bounds = meta;

        double v;
        // Prefer the stored contour values (the mariner's set depths) over the PI getter,
        // which can hand back the snapped/displayed safety contour.
        if (cfg->Read("S52_MAR_SAFETY_CONTOUR", &v))
            mariner_.safety_contour = v;
        if (cfg->Read("S52_MAR_SHALLOW_CONTOUR", &v))
            mariner_.shallow_contour = v;
        if (cfg->Read("S52_MAR_DEEP_CONTOUR", &v))
            mariner_.deep_contour = v;
        if (cfg->Read("S52_MAR_SAFETY_DEPTH", &v))
            mariner_.safety_depth = v;
        long two_shades = 0;
        if (cfg->Read("S52_MAR_TWO_SHADES", &two_shades))
            mariner_.four_shade_water = !two_shades;

        // Full-length light sector lines (OpenCPN "Extended light sectors").
        bool full_sectors = true;
        cfg->Read("bExtendLightSectors", &full_sectors, true);
        mariner_.show_full_sector_lines = full_sectors;
        // SCAMIN on/off (bUseSCAMIN); off => show every feature regardless of scale.
        bool use_scamin = true;
        cfg->Read("bUseSCAMIN", &use_scamin, true);
        mariner_.ignore_scamin = !use_scamin;

        // The ENC Text / ENC Sounding size sliders. OpenCPN keeps these directly under
        // /Settings (NOT /Settings/GlobalState like the block above), and rewrites them
        // whenever the Options dialog is applied — so this is a live value, and it is the
        // only one available before the first pushed message arrives.
        cfg->SetPath("/Settings");
        long enc_text = 0, enc_snd = 0;
        cfg->Read("ENCTextScaleFactor", &enc_text, 0);
        cfg->Read("ENCSoundingScaleFactor", &enc_snd, 0);
        text_factor_ = (int)enc_text;
        sounding_factor_ = (int)enc_snd;

        // These two are per-canvas: OpenCPN only rewrites their config copy at shutdown,
        // so what we read here is a STARTUP SEED, not a live value. Data quality is kept
        // live by the pushed message below; light descriptions are kept live by the weak
        // getter above where the host exports it.
        cfg->SetPath("/Canvas/CanvasConfig1");
        bool dq = false;
        cfg->Read("canvasENCShowDataQuality", &dq, false);
        mariner_.data_quality = dq;
        bool ldis = false;
        cfg->Read("canvasENCShowLightDescriptions", &ldis, false);
        light_desc_cfg_ = ldis;

        cfg->SetPath(path);
    }

    // OpenCPN's live push (see ApplyS52ConfigMessage). Authoritative once it has arrived:
    // it reflects the S52PLIB as actually configured, including the per-canvas
    // data-quality toggle that never reaches the config file.
    S52Config& c = s52cfg();
    std::lock_guard<std::mutex> lk(c.mtx);
    if (c.have) {
        mariner_.show_meta_bounds = c.meta;
        important_text_only_ = c.important_text_only;
        mariner_.ignore_scamin = !c.use_scamin;
        mariner_.data_quality = c.quality_of_data;
        mariner_.four_shade_water = !c.two_shades;
        mariner_.safety_depth = c.safety_depth;
        mariner_.shallow_contour = c.shallow_contour;
        mariner_.deep_contour = c.deep_contour;
        text_factor_ = c.text_factor;
        sounding_factor_ = c.sounding_factor;
    }
    // The ENC Text / ENC Sounding sliders, scaled the way OpenCPN scales them. These sit ON
    // TOP of size_scale (the physical/DPI calibration + the Chart Object slider, both folded
    // in above), because that is how OpenCPN stacks them: the object factor moves symbols,
    // lines and text together, and these two then move text / soundings alone. tile57 scales
    // the glyph AND its declutter box together, so enlarged labels still declutter correctly
    // — which is exactly why this cannot be done on our side of the API.
    mariner_.text_size_scale = enc_text_scale(text_factor_);
    mariner_.sounding_size_scale = enc_sounding_scale(sounding_factor_);
}

void ChartTile57::draw_calibration() const {
    static const bool on = std::getenv("TILE57_CALIB") != nullptr;
    if (!on)
        return;
    GLint gvp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, gvp);
    const float W = (float)gvp[2], H = (float)gvp[3];
    if (W <= 0.f || H <= 0.f)
        return;

    // A 5mm square at the SAME effective px/mm the chart symbols use (kTile57RefPxPerMm
    // is tile57's size_scale=1 density; mariner_.size_scale carries the display + HiDPI
    // scaling). Measured with a ruler it should be exactly 5mm — S-52 CHKSYM01.
    const float side = (float)(5.0 * kTile57RefPxPerMm * mariner_.size_scale);
    const float cx = W * 0.5f, cy = H * 0.5f, hs = side * 0.5f;
    static bool logged = false;
    if (!logged) {
        logged = true;
        wxLogMessage("tile57 CALIB: 5mm square = %.1f px  (size_scale=%.3f, %.2f px/mm, "
                     "displayMM=%.1f, csf=%.2f)",
                     side, mariner_.size_scale, kTile57RefPxPerMm * mariner_.size_scale,
                     PlugInGetDisplaySizeMM(), OCPN_GetDisplayContentScaleFactor());
    }

    glUseProgram(0);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glColor3f(0.f, 0.f, 0.f); // filled black 5mm square (like CHKSYM01)
    glBegin(GL_QUADS);
    glVertex2f(cx - hs, cy - hs);
    glVertex2f(cx + hs, cy - hs);
    glVertex2f(cx + hs, cy + hs);
    glVertex2f(cx - hs, cy + hs);
    glEnd();
    glColor3f(1.f, 0.25f, 0.25f); // crisp red edge for measuring
    glLineWidth(1.f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(cx - hs, cy - hs);
    glVertex2f(cx + hs, cy - hs);
    glVertex2f(cx + hs, cy + hs);
    glVertex2f(cx - hs, cy + hs);
    glEnd();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
}

// Clip this chart's drawing to the quilt patch it actually OWNS.
//
// OpenCPN hands an EXTENDED plugin chart its patch region and then takes its OWN clip away:
//
//     glChartCanvas::SetClipRect(VPoint, VPoint.rv_rect, false);  // clean slate: whole view
//     glChartCanvas::DisableClipRegion();                         // <- core clip REMOVED
//     wxRegion *r = RectRegion.GetNew_wxRegion();
//     ppicb_x->RenderRegionViewOnGLNoText(glc, pivp, *r, s_b_useStencil);
//
// (the LEGACY PlugInChartBaseGL path right below it loops the rects and scissors each one
// itself — the newer Extended path we implement does not, so clipping is OUR job). The
// region is pqp->ActiveRegion: the part of the screen this cell owns after finer cells have
// carved themselves out of coarser ones.
//
// Ignoring it — as this chart did — let EVERY cell in the quilt paint its whole extent over
// the whole canvas: the coarse cell underneath drew its own soundings, symbols and labels
// across the entire view on top of the fine cells drawing theirs. Everything rendered two or
// three times over, at every zoom. That is not a SCAMIN problem; no cull can fix drawing the
// same water twice.
namespace {
// Does the CURRENT draw buffer actually have a stencil buffer? The host's b_use_stencil
// only says whether OPENCPN clips with stencil — on macOS it renders into an FBO and
// passes false, yet that FBO still HAS a stencil attachment we may use ourselves. Asking
// GL directly is what matters, and it is what let the (wrong) bounding-box fallback below
// run on a machine that could have masked exactly.
bool draw_buffer_has_stencil() {
    GLint bits = 0;
    glGetIntegerv(GL_STENCIL_BITS, &bits);
    return bits > 0;
}

class QuiltClip {
  public:
    // `scale` is logical canvas px -> framebuffer px: the same device_scale the projection
    // uses (1 when the host hands us a physical-px ViewPort, contentScale on a HiDPI canvas).
    // Deriving it from fbw/vp.pix_width instead would be wrong exactly where it matters —
    // OpenCPN hands charts an EXPANDED ViewPort under rotation (rv_rect) and in the text
    // pass, so pix_width is not the canvas width there, while the region rects always are.
    QuiltClip(const wxRegion& region, double scale, uint32_t fbw, uint32_t fbh, bool have_stencil) {
        const double sx = scale > 0 ? scale : 1.0, sy = sx;
        std::vector<std::array<GLint, 4>> rects;
        for (wxRegionIterator it(region); it; ++it) {
            const wxRect r = it.GetRect();
            if (r.width <= 0 || r.height <= 0)
                continue;
            // GL scissor/window origin is BOTTOM-left; wx rects are top-left.
            GLint x = (GLint)std::lround(r.x * sx);
            GLint w = (GLint)std::lround(r.width * sx);
            GLint h = (GLint)std::lround(r.height * sy);
            GLint y = (GLint)std::lround((double)fbh - (r.y + r.height) * sy);
            rects.push_back({x, y, w, h});
        }
        if (rects.empty()) {
            empty_ = true; // this cell owns none of the screen this frame — draw nothing
            return;
        }
        // Whole-viewport single rect: nothing to clip (the common unquilted case).
        if (rects.size() == 1 && rects[0][0] <= 0 && rects[0][1] <= 0 &&
            rects[0][2] >= (GLint)fbw && rects[0][3] >= (GLint)fbh)
            return;

        glGetBooleanv(GL_SCISSOR_TEST, &prev_scissor_);
        glGetIntegerv(GL_SCISSOR_BOX, prev_box_);
        if (rects.size() == 1) { // one rect -> a scissor is exact and free
            scissor_ = true;
            glEnable(GL_SCISSOR_TEST);
            glScissor(rects[0][0], rects[0][1], rects[0][2], rects[0][3]);
            return;
        }
        // Several rects and NO stencil to mask with: draw the scene once per rect, each under
        // an exact scissor (multipass). The old fallback here took the rects' BOUNDING BOX —
        // and a coarse cell's patch is typically a RING around the finer cells, whose bbox is
        // the WHOLE SCREEN. Every such cell then painted its full symbology over the fine
        // cells that had carved it out: soundings stacked several cells deep, drawn as one
        // unreadable blob. A bbox is not a clip; it is the absence of one.
        //
        // Multipass costs one extra draw per rect (geometry is cached in VBOs; nothing is
        // re-portrayed), and patches are a handful of rects, so this is cheap and — unlike
        // the stencil path — correct on every backend.
        if (!have_stencil && !draw_buffer_has_stencil()) {
            multipass_ = true;
            rects_ = std::move(rects);
            scissor_ = true;
            glEnable(GL_SCISSOR_TEST);
            return;
        }
        // Several rects: build a real mask. Use stencil BIT 1 — the core writes bit 0
        // (glStencilMask(0x1) in SetClipRegion), so leave its bit alone.
        stencil_ = true;
        glDisable(GL_SCISSOR_TEST); // the clear below must reach the whole buffer
        glUseProgram(0);
        glEnable(GL_STENCIL_TEST);
        glStencilMask(0x2);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT); // masked: clears only OUR bit
        glStencilFunc(GL_ALWAYS, 0x2, 0x2);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, (GLdouble)fbw, 0, (GLdouble)fbh, -1, 1); // bottom-left, like the rects
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glBegin(GL_QUADS);
        for (const auto& r : rects) {
            glVertex2i(r[0], r[1]);
            glVertex2i(r[0] + r[2], r[1]);
            glVertex2i(r[0] + r[2], r[1] + r[3]);
            glVertex2i(r[0], r[1] + r[3]);
        }
        glEnd();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glPopMatrix();
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilMask(0x0); // draw, don't write
        glStencilFunc(GL_EQUAL, 0x2, 0x2);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    }
    ~QuiltClip() {
        if (stencil_) {
            glStencilMask(0xFF);
            glDisable(GL_STENCIL_TEST);
        }
        if (scissor_) {
            if (prev_scissor_)
                glScissor(prev_box_[0], prev_box_[1], prev_box_[2], prev_box_[3]);
            else
                glDisable(GL_SCISSOR_TEST);
        }
    }
    QuiltClip(const QuiltClip&) = delete;
    QuiltClip& operator=(const QuiltClip&) = delete;
    bool empty() const { return empty_; }
    // True when the caller must draw the scene once per rect() under a scissor (no stencil
    // available to mask a multi-rect patch in one pass). Every other case is already clipped
    // by the constructor and draws exactly once.
    bool multipass() const { return multipass_; }
    const std::vector<std::array<GLint, 4>>& rects() const { return rects_; }

  private:
    bool empty_ = false, scissor_ = false, stencil_ = false, multipass_ = false;
    std::vector<std::array<GLint, 4>> rects_; // multipass only: the patch, rect by rect
    GLboolean prev_scissor_ = GL_FALSE;
    GLint prev_box_[4] = {0, 0, 0, 0};
};
} // namespace

int ChartTile57::render_pass(const PlugIn_ViewPort& vp, t57::ChartRenderer::Pass pass,
                             bool stencil_clip, const wxRegion* clip_region) {
    // TILE57_DEBUG: a black canvas with NO other tile57 log line means the host never got
    // past one of the three gates below. Report which, once per chart, so "nothing draws"
    // is never a silent failure (a failed ensure_gl — shader compile / GL loader — looked
    // identical to "never called").
    static const bool dbg_entry = std::getenv("TILE57_DEBUG") != nullptr;
    if (dbg_entry && !logged_entry_) {
        logged_entry_ = true;
        wxLogMessage("tile57 ENTRY: %s pass=%d bValid=%d has_chart=%d ensure_gl=%d",
                     m_Name.c_str(), (int)pass, (int)vp.bValid, (int)renderer_.has_chart(),
                     (int)renderer_.ensure_gl());
    }
    if (!vp.bValid)
        return false;
    if (!renderer_.has_chart())
        return true;
    if (!renderer_.ensure_gl())
        return false;
    refresh_mariner();
    // Remember the canvas (on the GL thread, where it's valid) for the deferred-tile
    // CallAfter(Refresh) below — a settled view with cold tiles still pending asks for
    // one more redraw so they fill in.
    if (!canvas_)
        canvas_ = GetOCPNCanvasWindow();

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
    // Physical symbol/text size (size_scale) needs the display content-scale, which is
    // only reliable HERE — at construction the chart's canvas isn't on its display yet, so
    // csf read 1 and symbols came out half size on a 2x display. Recompute when csf
    // changes (once, on the first real frame); this also refreshes cull_bias below.
    if (csf != size_scale_csf_) {
        size_scale_csf_ = csf;
        mariner_.size_scale = composed_size_scale(csf); // NOT display_size_scale — see there
    }
    // When OpenCPN hands a LOGICAL-unit ViewPort against a HiDPI framebuffer, the raw
    // ppm is logical. Instead of bumping the zoom by contentScale (which also shifts
    // tile detail + SCAMIN, over-crowding when zoomed out on a 2x display), keep the
    // zoom GEOGRAPHIC and pass contentScale as device_scale so ONLY the projection
    // scales up to fill the physical framebuffer.
    double device_scale =
        (csf > 1.0 && fbw == (uint32_t)std::lround(vp.pix_width * csf)) ? csf : 1.0;
    double zoom = zoom_for_ppm(ppm); // geographic (un-bumped)
    last_zoom_ = zoom;               // remembered for the object-query pick tolerance / SCAMIN
    // SCAMIN cull bias: 0 by default — native parity. This used to default to
    // log2(size_scale) ("symbols are drawn enlarged, so thin them early"), but native
    // OpenCPN enlarges its symbols for display DPI too and STILL culls at the raw
    // denominator (chart_scale > Scamin, nothing else), so the bias made every SCAMIN'd
    // symbol and sounding appear ~log2(size_scale) zoom levels later than the native
    // charts — on any real display (size_scale is px/mm-derived and >1 everywhere):
    // soundings "way too late". Worse, it applied to the base pass only, while labels
    // culled at the true denominator (render()), so in the window between denom and
    // denom*2^bias a feature showed its TEXT with no SYMBOL.
    // TILE57_DECLUTTER=<levels> opts back in (applied to ALL layers, labels included, so
    // symbol + label still hide together): 0 = native, higher = thin earlier.
    static const double declutter_override = [] {
        const char* e = std::getenv("TILE57_DECLUTTER");
        return e ? std::atof(e) : -1.0; // <0 sentinel: no bias (native parity)
    }();
    double cull_bias = declutter_override >= 0.0 ? declutter_override : 0.0;

    // The SCAMIN cull compares the host's REAL display scale denominator against each
    // feature's SCAMIN — the same test native OpenCPN applies to its own ENCs:
    //     if (vp_plib.chart_scale > rzRules->obj->Scamin) b_visible = false;  // s52plib.cpp
    // vp.chart_scale IS that number (ChartCanvas: m_canvas_scale_factor / view_scale_ppm), so
    // it already carries the display's true DPI and the view latitude. Deriving a denominator
    // from the web-mercator zoom instead would assume the equator and drift by log2(cos lat)
    // — about a third of a zoom level at latitude 39. Fall back to computing one only if the
    // host hands us no scale at all.
    //
    // "Use SCAMIN" OFF (OpenCPN's toggle, mariner ignore_scamin) => 0, which culls nothing:
    // the flag has to act HERE, because tile57 reports every feature's SCAMIN regardless of
    // it (its surface path emits ungated and leaves the scale cull to the host), so left to
    // itself the shader would go on culling and the toggle would do nothing at all.
    double scamin_display_denom =
        mariner_.ignore_scamin
            ? 0.0
            : (vp.chart_scale > 0 ? (double)vp.chart_scale : scale_denom(zoom, vp.clat));
    // TILE57_DEBUG: one line per zoom step exposing the HiDPI coupling — scamin_denom is what
    // the SCAMIN shader test uses (a feature hides when scamin_denom > its SCAMIN; 0 = cull
    // off). gate = fbw vs pix_width*csf shows why device_scale resolves to 1.
    //
    // dbg_zoom_ is PER CHART, deliberately: a quilt draws a dozen cells into one view, and a
    // function-level static let only the first of them ever log — precisely hiding the thing
    // worth seeing, which is WHICH CELLS paint a given view. `cell` is this chart's own
    // compilation scale; when chart_scale > super_scamin, everything but the display-base
    // skeleton of THIS cell should be gone from the screen.
    static const bool dbg = std::getenv("TILE57_DEBUG") != nullptr;
    if (dbg && pass != t57::ChartRenderer::Pass::kText &&
        std::fabs(zoom - dbg_zoom_) > 0.02) {
        dbg_zoom_ = zoom;
        wxLogMessage("tile57 DBG: %s zoom=%.3f scamin_denom=1:%.0f (chart_scale=1:%.0f "
                     "bias=%.2f) super_scamin=1:%.0f cell=1:%d dev_scale=%.2f csf=%.2f "
                     "pixW=%d fbW=%u gate(pixW*csf)=%ld size_scale=%.3f ppm=%.5f",
                     m_Name.c_str(), zoom, scamin_display_denom * std::pow(2.0, cull_bias),
                     (double)vp.chart_scale, cull_bias,
                     mariner_.ignore_scamin ? 0.0 : m_Chart_Scale * 2.0, m_Chart_Scale,
                     device_scale, csf, vp.pix_width, fbw, std::lround(vp.pix_width * csf),
                     mariner_.size_scale, ppm);
    }
    // Chart rotation (course-up / head-up, and the manual rotate control). OpenCPN does NOT
    // rotate the framebuffer for us — a GL chart is handed the rotation and is expected to
    // draw its own geometry turned, exactly as the core's native vector charts do (see
    // ViewPort::GetPixFromLL). Ignoring it left tile57 stubbornly north-up under a rotated
    // ownship/route overlay. Skew is the canvas's chart-skew compensation and is 0 for a
    // mercator plugin chart (we report m_Chart_Skew = 0), so rotation alone is the angle.
    //
    // TILE57_ROT overrides the angle (degrees) for bring-up: TILE57_ROT=30 pins a 30° turn
    // regardless of what the host says, so "does our transform rotate at all" can be
    // answered without a heading source or a working host handshake.
    static const double rot_override = [] {
        const char* e = std::getenv("TILE57_ROT");
        return e ? std::atof(e) * kPi / 180.0 : 1e9; // 1e9 sentinel: use the host's angle
    }();
    const double rot = rot_override < 1e8 ? rot_override : vp.rotation;
    // What the host actually handed us. Logged on CHANGE (not per frame) so turning the
    // chart prints one line: if rotation stays 0.0 while the ownship turns, the angle never
    // reached the plugin and the transform below is not the problem.
    if (dbg && pass != t57::ChartRenderer::Pass::kText) {
        static double dbg_rot = 1e9;
        if (std::fabs(vp.rotation - dbg_rot) > 0.002) {
            dbg_rot = vp.rotation;
            wxLogMessage("tile57 ROT: vp.rotation=%.4f rad (%.1f°) vp.skew=%.4f rad (%.1f°) "
                         "applied=%.1f° rv_rect=[%d,%d %dx%d] pix=%dx%d fb=%ux%u",
                         vp.rotation, vp.rotation * 180.0 / kPi, vp.skew, vp.skew * 180.0 / kPi,
                         rot * 180.0 / kPi, vp.rv_rect.x, vp.rv_rect.y, vp.rv_rect.width,
                         vp.rv_rect.height, vp.pix_width, vp.pix_height, fbw, fbh);
        }
    }
    // Clip to the patch this cell owns BEFORE drawing anything (see QuiltClip). Scoped, so
    // the GL clip state is restored however we leave.
    {
        QuiltClip clip(clip_region ? *clip_region
                                   : wxRegion(0, 0, (int)vp.pix_width, (int)vp.pix_height),
                       device_scale, fbw, fbh, stencil_clip);
        if (clip.empty())
            return true; // this cell owns none of the screen this frame
        // The patch's bounding box in framebuffer px (top-left origin — the frame the shader
        // projects into; QuiltClip's own rects are GL bottom-left, hence the separate math).
        // Handed to render() so a quilted cell portrays only the tiles it can actually paint.
        int patch_fb[4] = {0, 0, (int)fbw, (int)fbh};
        if (clip_region) {
            const wxRect b = clip_region->GetBox();
            patch_fb[0] = (int)std::lround(b.x * device_scale);
            patch_fb[1] = (int)std::lround(b.y * device_scale);
            patch_fb[2] = (int)std::lround(b.width * device_scale);
            patch_fb[3] = (int)std::lround(b.height * device_scale);
        }
        if (clip.multipass()) {
            // No stencil: the patch is drawn rect by rect, each under an exact scissor. The
            // scene is identical every pass — only the scissor moves — so the tile cache and
            // the label cache are hit, not rebuilt.
            for (const auto& r : clip.rects()) {
                glScissor(r[0], r[1], r[2], r[3]);
                renderer_.render(vp.clon, vp.clat, zoom, fbw, fbh, mariner_, pass, stencil_clip,
                                 device_scale, cull_bias, rot, scamin_display_denom, patch_fb);
            }
        } else {
            renderer_.render(vp.clon, vp.clat, zoom, fbw, fbh, mariner_, pass, stencil_clip,
                             device_scale, cull_bias, rot, scamin_display_denom, patch_fb);
        }
    }
    // The tiled renderer portrays only a budget of new tiles per frame (so a big
    // first-visit burst doesn't freeze one frame). If it deferred some, schedule
    // another redraw so they fill in progressively.
    // Ask for one more redraw only if this render deferred tiles/labels. Coalesce across
    // the whole canvas: one paint fans out to every pass (base/text) and every quilt cell,
    // and each used to queue its own CallAfter(Refresh) — N redundant full-canvas repaints
    // for one frame's worth of pending. A single in-flight guard collapses them to one:
    // one Refresh already repaints all cells. The flag is cleared inside the marshalled
    // callback (main thread) BEFORE Refresh, so the paint it triggers can queue the next.
    static std::atomic<bool> refresh_inflight{false};
    if (renderer_.tiles_pending() && canvas_ && !refresh_inflight.exchange(true))
        canvas_->CallAfter([w = canvas_]() {
            refresh_inflight.store(false);
            w->Refresh(false);
        });
    if (pass != t57::ChartRenderer::Pass::kText)
        draw_calibration();
    return true;
}

int ChartTile57::RenderRegionViewOnGL(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                      const wxRegion& Region, bool b_use_stencil) {
    // Single-chart (unquilted) path. Region is normally the whole canvas here, but honour it
    // anyway — it costs nothing and it is the same contract as the quilted passes.
    return render_pass(vp, t57::ChartRenderer::Pass::kAll, b_use_stencil, &Region);
}

int ChartTile57::RenderRegionViewOnGLNoText(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                            const wxRegion& Region, bool b_use_stencil) {
    // Quilt geometry pass. The core does NOT pre-clip for an Extended plugin chart — it
    // disables its own clip and hands us the patch region to apply ourselves (see QuiltClip).
    return render_pass(vp, t57::ChartRenderer::Pass::kBase, b_use_stencil, &Region);
}

int ChartTile57::RenderRegionViewOnGLTextOnly(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                              const wxRegion& Region, bool b_use_stencil) {
    // Quilt text pass. Clipped to this cell's patch like the geometry: the core calls this
    // once PER CHART, so an unclipped text pass drew every cell's labels over the whole
    // canvas — the coarse cell's names and light descriptions stacked on the fine cell's.
    return render_pass(vp, t57::ChartRenderer::Pass::kText, b_use_stencil, &Region);
}

// ---- object query (S-52 §10.8 pick) ----------------------------------------
namespace {
struct QueryHit {
    std::string cls, s57, cell;
};
void collect_hit(void* ctx, const char* cls, size_t cl, const char* s57, size_t sl,
                 const char* cell, size_t cel) {
    static_cast<std::vector<QueryHit>*>(ctx)->push_back(
        {std::string(cls, cl), std::string(s57, sl), std::string(cell, cel)});
}

// Append a flat JSON object {"KEY":"VAL",...} (the pick blob) as HTML table rows.
void json_rows(const std::string& j, wxString& out) {
    size_t i = 0, n = j.size();
    auto ws = [&] {
        while (i < n && (j[i] == ' ' || j[i] == '\n' || j[i] == '\t' || j[i] == '\r'))
            ++i;
    };
    auto str = [&](std::string& s) -> bool {
        if (i >= n || j[i] != '"')
            return false;
        ++i;
        s.clear();
        while (i < n && j[i] != '"') {
            if (j[i] == '\\' && i + 1 < n)
                ++i;
            s += j[i++];
        }
        if (i < n)
            ++i;
        return true;
    };
    ws();
    if (i < n && j[i] == '{')
        ++i;
    while (i < n) {
        ws();
        if (i < n && j[i] == '}')
            break;
        std::string k, v;
        if (!str(k))
            break;
        ws();
        if (i < n && j[i] == ':')
            ++i;
        ws();
        if (!str(v))
            break;
        out += "<tr><td valign=top><b>" + wxString::FromUTF8(k.c_str()) + "</b></td><td>" +
               wxString::FromUTF8(v.c_str()) + "</td></tr>";
        ws();
        if (i < n && j[i] == ',')
            ++i;
    }
}

// Feature blocks — the plugin wraps the concatenation in <html><body>. Identical
// features (same class + attributes + cell) are collapsed: one S-57 feature can be
// split across several MVT features, and a zoomed-out pick spans many like ones.
wxString build_query_html(const std::vector<QueryHit>& hits) {
    wxString h;
    std::unordered_set<std::string> seen;
    for (const auto& f : hits) {
        if (!seen.insert(f.cls + '\0' + f.s57 + '\0' + f.cell).second)
            continue;
        h += "<font size=+1><b>" + wxString::FromUTF8(f.cls.c_str()) + "</b></font>";
        if (!f.cell.empty())
            h += "  <font size=-1 color=#808080>" + wxString::FromUTF8(f.cell.c_str()) + "</font>";
        h += "<table>";
        json_rows(f.s57, h);
        h += "</table><hr>";
    }
    return h;
}
} // namespace

std::vector<ChartTile57*> ChartTile57::instances() {
    std::lock_guard<std::mutex> lk(registry_mtx());
    return chart_registry(); // a snapshot — the DB scan may be adding charts right now
}

wxString ChartTile57::QueryDescription(double lon, double lat) const {
    tile57_chart* ch = renderer_.chart_handle();
    if (!ch)
        return wxEmptyString;
    std::vector<QueryHit> hits;
    tile57_query_cb cb{&hits, collect_hit};
    tile57_chart_query(ch, lon, lat, last_zoom_, &cb, nullptr);
    if (hits.empty())
        return wxEmptyString;
    return build_query_html(hits);
}

bool ChartTile57::covers(double lon, double lat) const {
    return covr_valid_ && lon >= bounds_west_ && lon <= bounds_east_ && lat >= bounds_south_ &&
           lat <= bounds_north_;
}

wxBitmap& ChartTile57::transparent_bitmap(const PlugIn_ViewPort& vp) {
    int w = vp.pix_width > 0 ? vp.pix_width : 1;
    int h = vp.pix_height > 0 ? vp.pix_height : 1;
    wxImage img(w, h);
    img.InitAlpha();
    std::memset(img.GetAlpha(), 0, (size_t)w * h); // fully transparent
    dc_bmp_ = wxBitmap(img);
    return dc_bmp_;
}

wxBitmap& ChartTile57::RenderRegionView(const PlugIn_ViewPort& vp, const wxRegion& /*Region*/) {
    return transparent_bitmap(vp);
}

wxBitmap& ChartTile57::RenderRegionViewOnDCNoText(const PlugIn_ViewPort& vp,
                                                  const wxRegion& /*Region*/) {
    return transparent_bitmap(vp);
}

bool ChartTile57::RenderRegionViewOnDCTextOnly(wxMemoryDC& /*dc*/,
                                               const PlugIn_ViewPort& /*VPoint*/,
                                               const wxRegion& /*Region*/) {
    return false; // GL-only plugin; nothing to draw on the DC canvas
}
