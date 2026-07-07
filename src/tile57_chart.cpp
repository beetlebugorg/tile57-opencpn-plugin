// tile57_chart.cpp — see tile57_chart.h.
#include "tile57_chart.h"
#include "gl.h"
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/log.h>
#include <cmath>
#include <cstring>

// wxWidgets can create ChartTile57 by class name; OpenCPN uses this to
// instantiate the chart for each matching file it finds in a chart directory.
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57, PlugInChartBaseExtended);

namespace {
constexpr double kEarthR = 6378137.0;
constexpr double kPi = 3.14159265358979323846;
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
    // Keep the chart selected out to (well past) its coarsest baked zoom.
    return scale_denom(min_zoom_, center_lat_) * 4.0;
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

int ChartTile57::render_pass(const PlugIn_ViewPort& vp, t57::ChartRenderer::Pass pass,
                             bool stencil_clip, const char* tag) {
    if (!vp.bValid) return false;
    if (!renderer_.ensure_gl()) return false;

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

    // TEMP diagnostic (throttled to view changes) — remove once macOS is confirmed.
    if (vp.clat != dbg_lat_ || vp.clon != dbg_lon_ || zoom != dbg_zoom_) {
        dbg_lat_ = vp.clat; dbg_lon_ = vp.clon; dbg_zoom_ = zoom;
        wxLogMessage("tile57 GL[%s]: clat=%.4f clon=%.4f ppm=%.6g rot=%.3f pix=%dx%d "
                     "glVp=[%d,%d %dx%d] csf=%.2f stencil=%d -> zoom=%.4f",
                     tag, vp.clat, vp.clon, vp.view_scale_ppm, vp.rotation,
                     vp.pix_width, vp.pix_height, gvp[0], gvp[1], gvp[2], gvp[3],
                     csf, (int)stencil_clip, zoom);
    }

    renderer_.render(vp.clon, vp.clat, zoom, fbw, fbh, mariner_, pass, stencil_clip);
    return true;
}

int ChartTile57::RenderRegionViewOnGL(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                      const wxRegion& /*Region*/, bool /*b_use_stencil*/) {
    // Single-chart (unquilted) path: the core wrapper scissors per rect; draw all.
    return render_pass(vp, t57::ChartRenderer::Pass::kAll, false, "all");
}

int ChartTile57::RenderRegionViewOnGLNoText(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                            const wxRegion& /*Region*/, bool b_use_stencil) {
    // Quilt geometry pass. The core pre-writes this chart's patch region into
    // the stencil buffer (SetClipRegion) and expects the chart to clip itself.
    return render_pass(vp, t57::ChartRenderer::Pass::kBase, b_use_stencil, "base");
}

int ChartTile57::RenderRegionViewOnGLTextOnly(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                              const wxRegion& /*Region*/, bool /*b_use_stencil*/) {
    // Quilt text pass: once across the whole quilt, unclipped (text declutters
    // across patch boundaries).
    return render_pass(vp, t57::ChartRenderer::Pass::kText, false, "text");
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
