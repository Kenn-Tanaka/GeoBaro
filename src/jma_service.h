#pragma once
#include <string>
#include <vector>
#include <utility>
#include <functional>
#include "jma_stations.h"
#include "spatial_pressure.h"

struct HourlyWeather {
    int hour = 0;                  // 過去日: 1〜24, 当日: 0〜23
    int minute = 0;                // 0 (正時) または 10〜50 (最新10分値)
    double temp = -999.0;          // 気温 (℃)
    double humidity = -999.0;      // 湿度 (％)
    double station_press = -999.0; // 観測所現地気圧 (hPa)
    double sea_press = -999.0;     // 海面気圧 (hPa)
    double single_target_press = -999.0; // 最寄り1官署のみでの換算気圧 (hPa)
    double target_press = -999.0;  // 現在地標高での換算気圧 (空間補間適用後, hPa)
    double press_diff = 0.0;       // 海面気圧との差 (target - interpolated P0 または sea_press)
    bool has_press_diff = false;   // 海面気圧が存在し気圧差を算出できたか
    double spatial_correction = 0.0; // 空間補間補正値 (target_press - single_target_press)
    bool has_spatial_correction = false;
    SpatialPressureResult spatial; // 該当時刻の空間補間結果・診断メタデータ
    std::string weather;           // 天候 (晴れ, 曇, 雨 等; 当日AMeDASは "-")

    std::string FormatTimeHHMM() const;
};

struct PressureStats {
    bool has_valid = false;
    double latest_press = -999.0;
    std::string latest_time;
    double min_press = 0.0;
    std::string min_time;
    double max_press = 0.0;
    std::string max_time;
};

struct NeighborTimeObs {
    int hour = 0;
    int minute = 0;
    double sea_press = -999.0;
    double temp = -999.0;
};

struct NeighborStationSeries {
    JmaStation station;
    std::vector<NeighborTimeObs> obs;
};

struct JmaDailyResult {
    bool success = false;
    bool is_realtime = false;      // true: AMeDAS JSON (当日速報等), false: hourly_s1.php
    bool has_chunk_error = false;       // 一部3時間チャンクの取得に失敗したか
    bool has_missing_pressure = false;  // 表示行の中に換算気圧が欠測の行があるか
    bool used_fallback_station = false; // 気圧非対応/連続欠測により代替官署を使用したか
    bool has_large_alt_diff = false;    // 現在地と官署の標高差が大きく(1000m以上)外挿誤差に注意が必要か
    std::string latest_time_str;   // 例: "18:40" (is_realtime時)
    std::string error_message;
    JmaStation station;
    double distance_km = 0.0;
    double target_lat = 0.0;
    double target_lon = 0.0;
    bool has_target_coords = false;
    int year = 0;
    int month = 0;
    int day = 0;
    std::vector<HourlyWeather> hours; // 過去日: 01:00〜24:00, 当日: 00:00〜最新
    std::vector<NeighborStationSeries> neighbor_series; // 空間補間用 周辺観測所時系列
};

class JmaService {
public:
    // 現在の日本標準時 (JST) の日付を取得
    static void GetCurrentJstDate(int& out_year, int& out_month, int& out_day);

    // 指定日が JST の当日以降（リアルタイムAMeDAS対象候補）か判定
    static bool IsRealtimeDateCandidate(int year, int month, int day);

    // 現在地から最寄りの気圧観測所を探索
    // require_amedas_pressure=true の場合、AMeDAS JSONで気圧を観測しない地点(例: 富士山 50066)を除外
    // ground_elev_m > -9000.0 の場合、大圏距離に標高差ペナルティ(50mあたり1km相当)を加味して選定
    static JmaStation FindNearestStation(
        double lat, double lon,
        double& out_distance_km,
        bool require_amedas_pressure = false,
        double ground_elev_m = -9999.0, int yyyymmdd = 0
    );

    // 距離（および標高差ペナルティ付きスコア）順に観測所一覧を探索
    static std::vector<std::pair<JmaStation, double>> FindStationsSortedByDistance(
        double lat, double lon,
        bool require_amedas_pressure = false,
        double ground_elev_m = -9999.0, int yyyymmdd = 0
    );

    // 指定観測所・指定日の気象データを取得し、現在地標高に換算
    static JmaDailyResult FetchDailyWeather(
        const JmaStation& station,
        int year, int month, int day,
        double target_elevation_m,
        const std::function<bool()>& should_cancel = nullptr
    );

    // 現在地座標から指定日の気象データを取得（当日・直近日のAMeDAS切替、観測所フォールバック、空間補間対応）
    static JmaDailyResult FetchDailyWeatherForLocation(
        double lat, double lon,
        int year, int month, int day,
        double target_elevation_m,
        const std::function<bool()>& should_cancel = nullptr,
        double ground_elevation_m = -9999.0
    );

    // 位置微修正時に、既存の周辺観測所データを再利用して空間補間のみ再計算可能か判定
    static bool CanReuseNeighborDataForLocation(
        const JmaDailyResult& prev,
        double new_lat,
        double new_lon
    );

    // 既に取得済みの観測結果に対し、指定標高（地盤標高 + 地上高）で換算気圧・空間補間を再計算
    static void RecalculatePressures(JmaDailyResult& result, double target_elevation_m);

    // 有効な最新時刻（または最終時刻）の空間補間結果を取得
    static const HourlyWeather* FindLatestValidSpatialHour(const JmaDailyResult& result);

    // 換算気圧の統計情報（最新・最高・最低）を計算
    static PressureStats ComputePressureStats(const JmaDailyResult& result);

    // 気象庁過去データ表の数値セル文字列をパース（正常値および準正常値 ')' のみ採用し、資料不足値 ']' 等は除外）
    static bool ParseJmaCellValue(const std::string& raw, double& out_val);

private:
    static double CalculateDistanceKm(double lat1, double lon1, double lat2, double lon2);
    static double CalculatePressureFromSeaLevel(double sea_press, double sea_temp_c, double target_elevation_m);
    static double CalculatePressureFromStation(double station_press, double station_alt_m, double sea_temp_c, double target_elevation_m);
};
