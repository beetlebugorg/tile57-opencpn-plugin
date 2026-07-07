// tile57_chart.cpp — see tile57_chart.h.
#include "tile57_chart.h"
#include <wx/filename.h>
#include <wx/image.h>
#include <cmath>
#include <cstring>

// wxWidgets can create ChartTile57 by class name; OpenCPN uses this to
// instantiate the chart for each matching file it finds in a chart directory.
wxIMPLEMENT_DYNAMIC_CLASS(ChartTile57, PlugInChartBaseGL);

namespace {
constexpr double kEarthR = 6378137.0;
constexpr double kPi = 3.14159265358979323846;
// Pixels per metre of a nominal 96-DPI display; turns a ground resolution into
// an OpenCPN "1:N" scale denominator.
constexpr double kPxPerMetre = 96.0 / 0.0254;

// Web-mercator ground resolution (metres/pixel) at a zoom and latitude.
double resolution_m_per_px(double zoom, double lat_deg) {
    double world_m = std::cos(lat_deg * kPi / 180.0) * 2.0 * kPi * kEarthR;
    return world_m / (256.0 * std::pow(2.0, zoom));
}

// Continuous web-mercator zoom for a ground resolution — inverse of the above.
double zoom_for_resolution(double mpp, double lat_deg) {
    double world_m = std::cos(lat_deg * kPi / 180.0) * 2.0 * kPi * kEarthR;
    return std::log2(world_m / (256.0 * mpp));
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
    // Opening a PMTiles bundle is cheap, so header/thumb/full inits are handled
    // identically — each needs the extent and scale.
    m_FullPath = full_path;
    m_Name = wxFileName(full_path).GetName();
    m_Description = _T("tile57 S-57/S-101 ENC (EXPERIMENTAL — NOT FOR NAVIGATION)");
    m_ID = full_path;

    // Opening a PMTiles bundle is cheap (it only peeks the archive metadata), so
    // we open it for every init flavour — header/thumb reads need the extent and
    // scale just as a full init does. GL setup is deferred to first render.
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
    // Allow zooming in past native detail (tile57 re-portrays live at any zoom).
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

int ChartTile57::RenderRegionViewOnGL(const wxGLContext& /*glc*/, const PlugIn_ViewPort& vp,
                                      const wxRegion& /*Region*/, bool /*b_use_stencil*/) {
    if (!vp.bValid) return false;
    if (!renderer_.ensure_gl()) return false;

    // OpenCPN ViewPort -> tile57 camera (centre + web-mercator zoom).
    double mpp = (vp.view_scale_ppm > 0) ? 1.0 / vp.view_scale_ppm : 100.0;
    double zoom = zoom_for_resolution(mpp, vp.clat);
    renderer_.render(vp.clon, vp.clat, zoom,
                     (uint32_t)vp.pix_width, (uint32_t)vp.pix_height,
                     mariner_, /*draw_text=*/true);
    return true;
}

wxBitmap& ChartTile57::RenderRegionView(const PlugIn_ViewPort& vp, const wxRegion& /*Region*/) {
    int w = vp.pix_width > 0 ? vp.pix_width : 1;
    int h = vp.pix_height > 0 ? vp.pix_height : 1;
    wxImage img(w, h);
    img.InitAlpha();
    std::memset(img.GetAlpha(), 0, (size_t)w * h);  // fully transparent
    dc_bmp_ = wxBitmap(img);
    return dc_bmp_;
}
