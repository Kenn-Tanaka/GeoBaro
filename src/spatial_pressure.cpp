#include "spatial_pressure.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSmoothingEpsKm = 1.0;
constexpr double kDirectNearestDistKm = 0.1;
constexpr double kExtrapMarginHpa = 2.0;
constexpr double kMinCovTraceKm2 = 1e-3;
constexpr double kMinCollinearityRatio = 1e-3; // 4 * det(C) / trace(C)^2
constexpr double kMaxExtrapolationLeverage = 12.0;
constexpr double kHypsometricExp = 5.257;

struct PreparedStation {
    SpatialStationInput input;
    double x_km = 0.0;
    double y_km = 0.0;
    double dist_km = 0.0;
    double raw_weight = 0.0;
    double norm_weight = 0.0;
    double sea_temp_c = 0.0;
};

bool TryFitWeightedPlane(
    const std::vector<PreparedStation>& pts,
    double& out_p0,
    double& out_grad_x,
    double& out_grad_y
) {
    if (pts.size() < 4) return false;

    double mean_x = 0.0;
    double mean_y = 0.0;
    double mean_p = 0.0;
    double min_p = pts[0].input.sea_level_pressure;
    double max_p = pts[0].input.sea_level_pressure;

    for (const auto& pt : pts) {
        mean_x += pt.norm_weight * pt.x_km;
        mean_y += pt.norm_weight * pt.y_km;
        mean_p += pt.norm_weight * pt.input.sea_level_pressure;
        if (pt.input.sea_level_pressure < min_p) min_p = pt.input.sea_level_pressure;
        if (pt.input.sea_level_pressure > max_p) max_p = pt.input.sea_level_pressure;
    }

    // 重み付き重心で中心化した 2x2 共分散行列 C と右辺ベクトルを構築（単位非依存・高精度）
    double cxx = 0.0, cxy = 0.0, cyy = 0.0;
    double rxp = 0.0, ryp = 0.0;
    for (const auto& pt : pts) {
        const double dx = pt.x_km - mean_x;
        const double dy = pt.y_km - mean_y;
        const double dp = pt.input.sea_level_pressure - mean_p;
        const double w = pt.norm_weight;
        cxx += w * dx * dx;
        cxy += w * dx * dy;
        cyy += w * dy * dy;
        rxp += w * dx * dp;
        ryp += w * dy * dp;
    }

    const double trace = cxx + cyy;
    const double det = cxx * cyy - cxy * cxy;
    if (!std::isfinite(trace) || !std::isfinite(det) ||
        trace <= kMinCovTraceKm2 || det <= 0.0 ||
        (4.0 * det) < kMinCollinearityRatio * trace * trace) {
        return false;
    }

    const double inv_det = 1.0 / det;
    const double inv00 = cyy * inv_det;
    const double inv01 = -cxy * inv_det;
    const double inv11 = cxx * inv_det;

    // 目標地点 (0, 0) の正規化外挿てこ比 1 + [mean_x, mean_y] C^-1 [mean_x, mean_y]^T を評価
    const double leverage = 1.0 + (inv00 * mean_x * mean_x + 2.0 * inv01 * mean_x * mean_y + inv11 * mean_y * mean_y);
    if (!std::isfinite(leverage) || leverage <= 0.0 || leverage > kMaxExtrapolationLeverage) {
        return false;
    }

    const double grad_x = inv00 * rxp + inv01 * ryp;
    const double grad_y = inv01 * rxp + inv11 * ryp;
    const double c = mean_p - grad_x * mean_x - grad_y * mean_y;

    if (!std::isfinite(grad_x) || !std::isfinite(grad_y) || !std::isfinite(c)) {
        return false;
    }
    if (c < min_p - kExtrapMarginHpa || c > max_p + kExtrapMarginHpa) {
        return false;
    }

    out_p0 = c;
    out_grad_x = grad_x;
    out_grad_y = grad_y;
    return true;
}

} // namespace

void SpatialPressureCalculator::CalculateLocalCoordsKm(
    double target_lat,
    double target_lon,
    double stn_lat,
    double stn_lon,
    double& out_x_km,
    double& out_y_km,
    double& out_dist_km
) {
    double cos_lat0 = std::cos(target_lat * kPi / 180.0);
    out_x_km = (stn_lon - target_lon) * 111.32 * cos_lat0;
    out_y_km = (stn_lat - target_lat) * 110.57;
    out_dist_km = std::sqrt(out_x_km * out_x_km + out_y_km * out_y_km);
}

bool SpatialPressureCalculator::ApplyElevation(
    double interpolated_sea_press,
    double interpolated_sea_temp_c,
    double total_elevation_m,
    double& out_local_press,
    double& out_estimated_temp_c
) {
    if (!std::isfinite(interpolated_sea_press) || interpolated_sea_press <= 0.0 ||
        !std::isfinite(interpolated_sea_temp_c) || !std::isfinite(total_elevation_m)) {
        return false;
    }

    double est_temp = interpolated_sea_temp_c - kLapseRateKPerM * total_elevation_m;
    double denom = est_temp + 273.15 + kLapseRateKPerM * total_elevation_m;
    if (denom <= 1.0) {
        return false;
    }
    double ratio = (est_temp + 273.15) / denom;
    if (!std::isfinite(ratio) || ratio <= 0.0) {
        return false;
    }

    double local_p = interpolated_sea_press * std::pow(ratio, kHypsometricExp);
    if (!std::isfinite(local_p) || local_p <= 0.0) {
        return false;
    }

    out_estimated_temp_c = est_temp;
    out_local_press = local_p;
    return true;
}

SpatialPressureResult SpatialPressureCalculator::Evaluate(
    double target_lat,
    double target_lon,
    double total_elevation_m,
    const std::vector<SpatialStationInput>& candidates,
    int max_stations,
    double max_radius_km
) {
    SpatialPressureResult result;
    if (!std::isfinite(target_lat) || !std::isfinite(target_lon) || !std::isfinite(total_elevation_m) ||
        max_stations <= 0 || max_radius_km <= 0.0) {
        return result;
    }

    // Step 1: 有効観測所の抽出・直交座標変換・距離フィルタ
    std::vector<PreparedStation> valid_pts;
    valid_pts.reserve(candidates.size());

    for (const auto& c : candidates) {
        if (!std::isfinite(c.lat) || !std::isfinite(c.lon) || !std::isfinite(c.alt_m)) continue;
        if (!std::isfinite(c.sea_level_pressure) || c.sea_level_pressure <= 0.0) continue;
        if (!std::isfinite(c.temp_c) || c.temp_c <= -900.0) continue;

        PreparedStation ps;
        ps.input = c;
        CalculateLocalCoordsKm(target_lat, target_lon, c.lat, c.lon, ps.x_km, ps.y_km, ps.dist_km);
        if (!std::isfinite(ps.dist_km) || ps.dist_km > max_radius_km) {
            continue;
        }
        ps.sea_temp_c = c.temp_c + kLapseRateKPerM * c.alt_m;
        double d_eps = ps.dist_km + kSmoothingEpsKm;
        ps.raw_weight = 1.0 / (d_eps * d_eps);
        valid_pts.push_back(ps);
    }

    if (valid_pts.empty()) {
        return result;
    }

    std::sort(valid_pts.begin(), valid_pts.end(), [](const PreparedStation& a, const PreparedStation& b) {
        return a.dist_km < b.dist_km;
    });
    if (static_cast<int>(valid_pts.size()) > max_stations) {
        valid_pts.resize(static_cast<size_t>(max_stations));
    }

    double sum_w = 0.0;
    for (const auto& pt : valid_pts) {
        sum_w += pt.raw_weight;
    }
    if (sum_w <= 0.0) {
        return result;
    }

    double hat_t0 = 0.0;
    for (auto& pt : valid_pts) {
        pt.norm_weight = pt.raw_weight / sum_w;
        hat_t0 += pt.norm_weight * pt.sea_temp_c;
    }

    // Step 2: 海面気圧の空間補間 (PlaneFit -> IDW -> SingleNearest)
    double hat_p0 = 0.0;
    if (valid_pts.size() == 1) {
        result.method_used = SpatialInterpMethod::SingleNearest;
        hat_p0 = valid_pts[0].input.sea_level_pressure;
    } else if (TryFitWeightedPlane(valid_pts, hat_p0, result.pressure_gradient_x, result.pressure_gradient_y)) {
        result.method_used = SpatialInterpMethod::PlaneFit;
        result.has_gradient = true;
    } else {
        result.method_used = SpatialInterpMethod::IDW;
        if (valid_pts[0].dist_km < kDirectNearestDistKm) {
            hat_p0 = valid_pts[0].input.sea_level_pressure;
        } else {
            for (const auto& pt : valid_pts) {
                hat_p0 += pt.norm_weight * pt.input.sea_level_pressure;
            }
        }
    }

    // Step 3: 目標高度への現地気圧換算
    double local_p = -999.0;
    double est_temp = -999.0;
    if (!ApplyElevation(hat_p0, hat_t0, total_elevation_m, local_p, est_temp)) {
        return result;
    }

    result.valid = true;
    result.interpolated_sea_level_pressure = hat_p0;
    result.interpolated_sea_level_temp = hat_t0;
    result.local_pressure = local_p;
    result.estimated_temp = est_temp;

    result.stations_used.reserve(valid_pts.size());
    for (const auto& pt : valid_pts) {
        SpatialStationContribution contrib;
        contrib.amedas_id = pt.input.amedas_id;
        contrib.name = pt.input.name;
        contrib.distance_km = pt.dist_km;
        contrib.weight = pt.norm_weight;
        contrib.raw_weight = pt.raw_weight;
        contrib.sea_level_pressure = pt.input.sea_level_pressure;
        result.stations_used.push_back(contrib);
    }
    return result;
}

const char* SpatialPressureCalculator::MethodToString(SpatialInterpMethod method) {
    switch (method) {
        case SpatialInterpMethod::PlaneFit:      return "PlaneFit";
        case SpatialInterpMethod::IDW:           return "IDW";
        case SpatialInterpMethod::SingleNearest: return "SingleNearest";
    }
    return "SingleNearest";
}
