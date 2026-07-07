// tile57_chart.cpp — see tile57_chart.h.
#include "tile57_chart.h"
#include "gl.h"
#include <wx/fileconf.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/log.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// wxWidgets can create ChartTile57 by class name; OpenCPN uses this to
// instantiate the chart for each matching file it finds in a chart directory.
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57, PlugInChartBaseExtended);

namespace {
constexpr double kEarthR = 6378137.0;
constexpr double kPi = 3.14159265358979323846;
// tile57 sizes symbols/text/line-widths at size_scale=1.0 against a 72-DPI
// reference (2.8346 px/mm). Scale that to the real display's pixels-per-mm so
// S-52 symbology is physically the right size — matching native OpenCPN, which
// derives its own symbol scale from the same display width. Without this, dense
// modern displays render everything at a fraction of size ("2x density").
constexpr double kTile57RefPxPerMm = 2.8346;
double display_size_scale() {
    double mm = PlugInGetDisplaySizeMM();
    int sw = wxGetDisplaySize().GetWidth();
    if (mm < 1.0 || sw <= 0) return 1.0;
    double s = (sw / mm) / kTile57RefPxPerMm;
    return (s > 0.1 && s < 12.0) ? s : 1.0;
}
// Pixels per metre of a nominal 96-DPI display; turns a ground resolution into
// an OpenCPN "1:N" scale denominator.
constexpr double kPxPerMetre = 96.0 / 0.0254;

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
}

ChartTile57::~ChartTile57() { renderer_.shutdown(); }

int ChartTile57::Init(const wxString& full_path, int /*init_flags*/) {
    // Opening a PMTiles bundle is cheap (it only peeks the archive metadata), so
    // header/thumb/full inits are handled identically — each needs the extent
    // and scale. GL setup is deferred to first render.
    m_FullPath = full_path;
    m_Name = wxFileName(full_path).GetName();
    m_Description = _T("tile57 S-57/S-101 ENC (EXPERIMENTAL — NOT FOR NAVIGATION)");
    m_ID = full_path;

    if (!renderer_.open_chart(std::string(full_path.mb_str()))) return PI_INIT_FAIL_REMOVE;

    tile57_chart_info info{};
    if (!renderer_.get_info(info) || !info.has_bounds) return PI_INIT_FAIL_REMOVE;

    center_lat_ = 0.5 * (info.north + info.south);

    // Native scale = finest detail the bundle carries; display range spans the
    // baked zoom band, generously padded so this single chart stays selected
    // across OpenCPN's zoom transitions.
    m_Chart_Scale = scale_denom(info.max_zoom, center_lat_);
    m_Chart_Skew = 0.0;
    m_depth_unit_id = PI_DEPTH_UNIT_METERS;
    m_DepthUnits = _T("Meters");

    // Coverage rectangle: OpenCPN COVR points are float_2Dpt (lat, lon) pairs.
    covr_[0] = (float)info.north; covr_[1] = (float)info.west;   // NW
    covr_[2] = (float)info.north; covr_[3] = (float)info.east;   // NE
    covr_[4] = (float)info.south; covr_[5] = (float)info.east;   // SE
    covr_[6] = (float)info.south; covr_[7] = (float)info.west;   // SW
    covr_valid_ = true;

    // Stash the bounds for GetChartExtent.
    bounds_west_ = info.west; bounds_south_ = info.south;
    bounds_east_ = info.east; bounds_north_ = info.north;
    min_zoom_ = info.min_zoom; max_zoom_ = info.max_zoom;

    wxLogMessage("tile57 Init: %s  bounds[W%.5f S%.5f E%.5f N%.5f] zoom[%d..%d] "
                 "centerLat=%.5f nativeScale=1:%d",
                 m_FullPath.c_str(), info.west, info.south, info.east, info.north,
                 (int)info.min_zoom, (int)info.max_zoom, center_lat_, m_Chart_Scale);

    m_bReadyToRender = true;
    return PI_INIT_OK;
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
    // Symbolized area boundaries (S-52 'O' LUP) — richer edge symbology.
    mariner_.boundary_style = TILE57_BOUNDARY_SYMBOLIZED;
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

        cfg->SetPath(path);
    }
}

int ChartTile57::render_pass(const PlugIn_ViewPort& vp, t57::ChartRenderer::Pass pass,
                             bool stencil_clip) {
    if (!vp.bValid) return false;
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
    if (csf > 1.0 && fbw == (uint32_t)std::lround(vp.pix_width * csf)) ppm *= csf;
    double zoom = zoom_for_ppm(ppm);
    renderer_.render(vp.clon, vp.clat, zoom, fbw, fbh, mariner_, pass, stencil_clip);
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

wxString build_query_html(const std::vector<QueryHit>& hits) {
    wxString h = "<html><body>";
    for (const auto& f : hits) {
        h += "<font size=+1><b>" + wxString::FromUTF8(f.cls.c_str()) + "</b></font>";
        if (!f.cell.empty())
            h += "  <font size=-1 color=#808080>" + wxString::FromUTF8(f.cell.c_str()) + "</font>";
        h += "<table>";
        json_rows(f.s57, h);
        h += "</table><hr>";
    }
    h += "</body></html>";
    return h;
}
}  // namespace

ListOfPI_S57Obj* ChartTile57::GetObjRuleListAtLatLon(float lat, float lon, float /*radius*/,
                                                     PlugIn_ViewPort* /*vp*/) {
    auto* list = new ListOfPI_S57Obj;
    query_html_.Clear();
    tile57_chart* ch = renderer_.chart_handle();
    if (!ch) return list;
    std::vector<QueryHit> hits;
    tile57_query_cb cb{ &hits, collect_hit };
    tile57_chart_query(ch, lon, lat, &cb);
    if (hits.empty()) return list;
    query_html_ = build_query_html(hits);
    // Allocate each PI_S57Obj WITHOUT its constructor (the ctor symbol isn't
    // exported on macOS, and calling it would fail plugin load). Zeroed is fine:
    // OpenCPN reads FeatureName + the reference point and frees them.
    for (const auto& f : hits) {
        auto* o = static_cast<PI_S57Obj*>(std::calloc(1, sizeof(PI_S57Obj)));
        if (!o) break;
        std::strncpy(o->FeatureName, f.cls.c_str(), sizeof(o->FeatureName) - 1);
        o->m_lat = lat;
        o->m_lon = lon;
        list->Append(o);
    }
    return list;
}

wxString ChartTile57::CreateObjDescriptions(ListOfPI_S57Obj* /*obj_list*/) {
    return query_html_;
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
