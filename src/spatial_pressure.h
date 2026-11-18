#pragma once
#include <string>
#include <vector>

enum class SpatialInterpMethod {
    PlaneFit,
    IDW,
    SingleNearest
};

struct SpatialStationInput {
    std::string amedas_id;
    std::string name;
    double lat = 0.0;
    double lon = 0.0;
    double alt_m = 0.0;
    double sea_level_pressure = -999.0; // normalPressure (hPa)
    double temp_c = -999.0;             // 気温 (℃)
};

struct SpatialStationContribution {
    std::string amedas_id;
    std::string name;
    double distance_km = 0.0;
    double weight = 0.0;     // 正規化重み (総和 = 1.0)
    double raw_weight = 0.0; // 1 / (d_i + eps)^2
    double sea_level_pressure = -999.0;
};

struct SpatialPressureResult {
    bool valid = false;
    double local_pressure = -999.0;                  // Step 3: 目標高度の現地気圧 [hPa]
    double interpolated_sea_level_pressure = -999.0; // Step 2: 補間海面気圧 P_hat_0 [hPa]
    double estimated_temp = -999.0;                  // Step 3: 推定現地気温 T_hat_H [℃]
    double interpolated_sea_level_temp = -999.0;     // Step 2: 海面換算気温 T_hat_0 [℃]
    bool has_gradient = false;                       // PlaneFit 成功時のみ true
    double pressure_gradient_x = 0.0;                // 東西方向気圧傾度 [hPa/km]
    double pressure_gradient_y = 0.0;                // 南北方向気圧傾度 [hPa/km]
    SpatialInterpMethod method_used = SpatialInterpMethod::SingleNearest;
    std::vector<SpatialStationContribution> stations_used;
};

class SpatialPressureCalculator {
public:
    static constexpr int kDefaultMaxStations = 6;
    static constexpr double kDefaultMaxRadiusKm = 100.0;
    static constexpr double kLapseRateKPerM = 0.0065;

    // 目標地点 (target_lat, target_lon) 基準の局所直交座標 (x_km, y_km, dist_km) を計算
    static void CalculateLocalCoordsKm(
        double target_lat,
        double target_lon,
        double stn_lat,
        double stn_lon,
        double& out_x_km,
        double& out_y_km,
        double& out_dist_km
    );

    // Step 1〜3 を実行し、局所気圧および診断メタデータを返す
    static SpatialPressureResult Evaluate(
        double target_lat,
        double target_lon,
        double total_elevation_m,
        const std::vector<SpatialStationInput>& candidates,
        int max_stations = kDefaultMaxStations,
        double max_radius_km = kDefaultMaxRadiusKm
    );

    // Step 3 のみ再計算（地上高変更時の通信なし即時再計算用）
    static bool ApplyElevation(
        double interpolated_sea_press,
        double interpolated_sea_temp_c,
        double total_elevation_m,
        double& out_local_press,
        double& out_estimated_temp_c
    );

    static const char* MethodToString(SpatialInterpMethod method);
};
