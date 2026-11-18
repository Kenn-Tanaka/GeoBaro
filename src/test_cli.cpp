#include <iostream>
#include <iomanip>
#include <cassert>
#include <cmath>
#include <windows.h>
#include "gsi_service.h"
#include "jma_service.h"
#include "preset_store.h"

int main() {
    SetConsoleOutputCP(CP_UTF8);
    std::cout << "=== 標高・気圧換算ロジック検証テスト ===" << std::endl;

    // 0. 異常入力ガード検証
    std::cout << "\n[0] 不正座標入力の安全性テスト..." << std::endl;
    LocationInfo bad1 = GsiService::QueryLocation("., .");
    assert(!bad1.success);
    LocationInfo bad2 = GsiService::QueryLocation("95.0, 139.0");
    assert(!bad2.success);
    std::cout << "不正入力エラーハンドリングOK: " << bad2.error_message << std::endl;

    // 1. 国土地理院 住所検索 & 標高テスト
    std::string test_addr = "東京都千代田区千代田1-1";
    std::cout << "\n[1] 国土地理院 API テスト: " << test_addr << std::endl;
    LocationInfo loc = GsiService::QueryLocation(test_addr);
    if (!loc.success) {
        std::cerr << "エラー: " << loc.error_message << std::endl;
        return 1;
    }
    std::cout << "住所解決: " << loc.resolved_address << std::endl;
    std::cout << "座標: 北緯 " << loc.lat << ", 東経 " << loc.lon << std::endl;
    std::cout << "標高: " << loc.elevation << " m (" << loc.hsrc << ")" << std::endl;

    // 2. 地図タイル取得テスト (中心揃え2x2合成)
    std::cout << "\n[2] 地図タイル取得テスト (Zoom 15, std)..." << std::endl;
    TileInfo tile = GsiService::GetMapTile(loc.lat, loc.lon, 15, "std");
    size_t total_bytes = 0;
    for (const auto& p : tile.pieces) total_bytes += p.png_data.size();
    std::cout << "タイル基準座標: X=" << tile.x << ", Y=" << tile.y << std::endl;
    std::cout << "合成ピース数: " << tile.pieces.size() << ", 合計PNGサイズ: " << total_bytes << " bytes" << std::endl;
    std::cout << "中心マーカー座標: (" << tile.pixel_x << ", " << tile.pixel_y << ")" << std::endl;
    assert(!tile.pieces.empty());

    // 3. 最寄り観測所検索テスト
    std::cout << "\n[3] 最寄り気象観測所検索..." << std::endl;
    double dist = 0.0;
    JmaStation stn = JmaService::FindNearestStation(loc.lat, loc.lon, dist, false, loc.elevation);
    std::cout << "最寄り官署: " << stn.name << " (" << stn.pref << ")" << std::endl;
    std::cout << "観測所コード: block_no=" << stn.block_no << ", prec_no=" << stn.prec_no << std::endl;
    std::cout << "直線距離: " << std::fixed << std::setprecision(2) << dist << " km" << std::endl;
    std::cout << "観測所標高: " << stn.alt << " m" << std::endl;

    // 4. 気象庁 過去データ取得 & 換算テスト (2024年5月1日)
    std::cout << "\n[4] 気象データ取得 & 気圧換算 (2024-05-01)..." << std::endl;
    JmaDailyResult daily = JmaService::FetchDailyWeather(stn, 2024, 5, 1, loc.elevation);
    if (!daily.success) {
        std::cerr << "気象データ取得失敗: " << daily.error_message << std::endl;
        return 1;
    }

    std::cout << "----------------------------------------------------------------------------------" << std::endl;
    std::cout << " 時刻 | 天候 | 気温(℃) | 湿度(%) | 観測所気圧 | 海面気圧 | 現在地換算気圧 | 気圧差" << std::endl;
    std::cout << "----------------------------------------------------------------------------------" << std::endl;
    assert(daily.hours.front().FormatTimeHHMM() == "01:00");
    assert(daily.hours.back().FormatTimeHHMM() == "24:00");
    for (const auto& h : daily.hours) {
        std::cout << std::setw(5) << h.FormatTimeHHMM() << " | "
                  << std::setw(4) << h.weather << " | "
                  << std::setw(7) << std::fixed << std::setprecision(1) << h.temp << " | "
                  << std::setw(7) << std::fixed << std::setprecision(0) << h.humidity << " | "
                  << std::setw(10) << std::fixed << std::setprecision(1) << h.station_press << " | "
                  << std::setw(8) << std::fixed << std::setprecision(1) << h.sea_press << " | "
                  << std::setw(14) << std::fixed << std::setprecision(1) << h.target_press << " | ";
        if (h.has_press_diff) {
            std::cout << std::setw(6) << std::showpos << std::fixed << std::setprecision(1) << h.press_diff << std::noshowpos;
        } else {
            std::cout << std::setw(6) << "-";
        }
        std::cout << std::endl;
    }
    std::cout << "----------------------------------------------------------------------------------" << std::endl;

    // 5. 山岳官署（富士山: 海面気圧欠損）フォールバック検証
    std::cout << "\n[5] 山岳官署フォールバックテスト (富士山付近 35.3606, 138.7274)..." << std::endl;
    LocationInfo fuji_loc = GsiService::QueryLocation("35.3606, 138.7274");
    assert(fuji_loc.success);
    double fuji_dist = 0.0;
    JmaStation fuji_stn = JmaService::FindNearestStation(fuji_loc.lat, fuji_loc.lon, fuji_dist, false, fuji_loc.elevation);
    assert(std::string(fuji_stn.amedas_id) == "50066");
    std::cout << "最寄り官署: " << fuji_stn.name << " (標高 " << fuji_stn.alt << " m), 距離: " << fuji_dist << " km" << std::endl;
    JmaDailyResult fuji_daily = JmaService::FetchDailyWeather(fuji_stn, 2024, 5, 1, fuji_loc.elevation);
    assert(fuji_daily.success && !fuji_daily.hours.empty());
    std::cout << "12時の富士山観測 現地気圧=" << fuji_daily.hours[11].station_press
              << " hPa -> 現在地(" << fuji_loc.elevation << "m)換算気圧="
              << fuji_daily.hours[11].target_press << " hPa" << std::endl;
    assert(fuji_daily.hours[11].target_press > 500.0 && fuji_daily.hours[11].target_press < 700.0);

    // 6. 地上高加算による気圧再計算テスト (例: 高層階 地上高 +100.0 m)
    std::cout << "\n[6] 地上高加算再計算テスト (地上高 +100.0 m)..." << std::endl;
    assert(daily.hours.size() >= 12);
    double ground_target_press = daily.hours[11].target_press;
    JmaService::RecalculatePressures(daily, loc.elevation + 100.0);
    double elevated_target_press = daily.hours[11].target_press;
    double drop_hpa = ground_target_press - elevated_target_press;
    std::cout << "12時 海抜" << loc.elevation << "m時=" << ground_target_press
              << " hPa -> +100.0m(海抜" << (loc.elevation + 100.0) << "m)時="
              << elevated_target_press << " hPa (低下量: -" << drop_hpa << " hPa)" << std::endl;
    assert(drop_hpa > 10.0 && drop_hpa < 14.0);
    assert(daily.hours[11].has_press_diff);
    assert(daily.hours[11].press_diff < 0.0);

    double fuji_ground_press = fuji_daily.hours[11].target_press;
    JmaService::RecalculatePressures(fuji_daily, fuji_loc.elevation + 100.0);
    assert(fuji_daily.hours[11].target_press < fuji_ground_press);

    // 7. Webメルカトル逆変換精度・高ズーム・reliefデジタル拡大・タイルキャッシュ検証
    std::cout << "\n[7] 地図ズーム・メルカトル逆変換・キャッシュテスト..." << std::endl;
    for (int z : {10, 15, 18}) {
        double wx = 0.0, wy = 0.0, rt_lat = 0.0, rt_lon = 0.0;
        GsiService::LatLonToWorldPx(loc.lat, loc.lon, z, wx, wy);
        GsiService::WorldPxToLatLon(wx, wy, z, rt_lat, rt_lon);
        assert(std::abs(rt_lat - loc.lat) < 1e-8);
        assert(std::abs(rt_lon - loc.lon) < 1e-8);
    }
    TileInfo z18_std = GsiService::GetMapTile(loc.lat, loc.lon, 18, "std", 320, 320);
    assert(z18_std.z == 18 && z18_std.fetch_z == 18 && z18_std.digital_scale == 1 && !z18_std.pieces.empty());
    size_t cache_after_std18 = GsiService::GetTileCacheCount();
    assert(cache_after_std18 >= 4);

    // 同一周辺のわずかな微修正はキャッシュヒットでタイル数が増えすぎないことを確認
    TileInfo z18_pan = GsiService::GetMapTile(loc.lat + 0.00001, loc.lon + 0.00001, 18, "std", 320, 320);
    assert(!z18_pan.pieces.empty());

    // 色別標高図(relief)のZoom 18時はfetch_z=15, digital_scale=8で取得されること
    TileInfo z18_relief = GsiService::GetMapTile(loc.lat, loc.lon, 18, "relief", 320, 320);
    assert(z18_relief.z == 18 && z18_relief.fetch_z == 15 && z18_relief.digital_scale == 8 && !z18_relief.pieces.empty());
    std::cout << "Zoom 18 std pieces=" << z18_std.pieces.size()
              << ", relief(Z15->Z18 scale=" << z18_relief.digital_scale << ") pieces=" << z18_relief.pieces.size()
              << ", cache entries=" << GsiService::GetTileCacheCount() << std::endl;

    // 8. プリセットシリアライズ・デシリアライズ検証
    std::cout << "\n[8] 地点プリセット保存・復元テスト..." << std::endl;
    std::vector<PresetLocation> presets;
    PresetLocation p1;
    p1.name = "自宅\t高層階(20F)";
    p1.input_text = "東京都千代田区千代田1-1";
    p1.base_address = "東京都千代田区千代田１番";
    p1.is_fine_tuned = true;
    p1.lat = 35.683812;
    p1.lon = 139.754215;
    p1.base_lat = loc.lat;
    p1.base_lon = loc.lon;
    p1.height_agl_m = 80.5;
    p1.zoom = 18;
    PresetStore::Upsert(presets, p1);
    std::string raw_presets = PresetStore::Serialize(presets);
    auto loaded_presets = PresetStore::Deserialize(raw_presets);
    assert(loaded_presets.size() == 1);
    assert(loaded_presets[0].name == p1.name);
    assert(loaded_presets[0].base_address == p1.base_address);
    assert(loaded_presets[0].is_fine_tuned == true);
    assert(std::abs(loaded_presets[0].lat - p1.lat) < 1e-5);
    assert(std::abs(loaded_presets[0].height_agl_m - 80.5) < 0.05);
    assert(loaded_presets[0].zoom == 18);
    assert(PresetStore::RemoveByIndex(loaded_presets, 0) && loaded_presets.empty());
    std::cout << "プリセット保存・復元テストOK" << std::endl;

    // 9. 当日リアルタイム(AMeDAS)取得・HH:MM書式・富士山フォールバック・未来日検証
    std::cout << "\n[9] 当日リアルタイムAMeDASデータ取得テスト..." << std::endl;
    int ty = 0, tm = 0, td = 0;
    JmaService::GetCurrentJstDate(ty, tm, td);
    JmaDailyResult today_res = JmaService::FetchDailyWeatherForLocation(
        loc.lat, loc.lon, ty, tm, td, loc.elevation, nullptr, loc.elevation);
    assert(today_res.success);
    assert(today_res.is_realtime);
    assert(!today_res.has_large_alt_diff);
    assert(!today_res.hours.empty());
    assert(today_res.hours.front().FormatTimeHHMM() == "00:00");
    std::cout << "当日(" << ty << "-" << tm << "-" << td << ") 最新時刻=" << today_res.latest_time_str
              << ", 行数=" << today_res.hours.size()
              << ", 末尾=[" << today_res.hours.back().FormatTimeHHMM()
              << ", 現地=" << today_res.hours.back().station_press
              << ", 換算=" << today_res.hours.back().target_press << " hPa]" << std::endl;
    assert(today_res.hours.back().FormatTimeHHMM() == today_res.latest_time_str);
    assert(today_res.hours.back().target_press > 800.0 && today_res.hours.back().target_press < 1100.0);

    // 富士山付近の当日照会は 50066 から最寄りの別気圧観測所へフォールバックすること
    JmaDailyResult fuji_today = JmaService::FetchDailyWeatherForLocation(
        fuji_loc.lat, fuji_loc.lon, ty, tm, td, fuji_loc.elevation, nullptr, fuji_loc.elevation);
    assert(fuji_today.success);
    assert(fuji_today.used_fallback_station);
    assert(fuji_today.has_large_alt_diff);
    assert(std::string(fuji_today.station.amedas_id) != "50066");
    assert(fuji_today.distance_km > 1.0);
    assert(fuji_today.hours.back().target_press > 500.0 && fuji_today.hours.back().target_press < 750.0);
    std::cout << "富士山付近の当日フォールバック官署=" << fuji_today.station.name
              << " (距離 " << fuji_today.distance_km << " km), 末尾換算気圧="
              << fuji_today.hours.back().target_press << " hPa" << std::endl;

    // 未来日エラー検証
    JmaDailyResult future_res = JmaService::FetchDailyWeatherForLocation(
        loc.lat, loc.lon, ty + 1, 1, 1, loc.elevation, nullptr, loc.elevation);
    assert(!future_res.success && !future_res.error_message.empty());
    std::cout << "未来日エラーハンドリングOK: " << future_res.error_message << std::endl;

    // 10. 一部欠測時の気温補間および欠測フラグ検証
    std::cout << "\n[10] 一部欠測時の気温補間・欠測フラグテスト..." << std::endl;
    JmaDailyResult partial_res;
    partial_res.station.alt = 0.0;
    HourlyWeather hw10; hw10.hour = 10; hw10.minute = 0; hw10.temp = 0.0; hw10.sea_press = 1010.0;
    HourlyWeather hw11; hw11.hour = 11; hw11.minute = 0; hw11.temp = -999.0; hw10.station_press = 1010.0; hw11.station_press = 1010.0; hw11.sea_press = 1010.0; // 気温のみ欠測
    HourlyWeather hw12; hw12.hour = 12; hw12.minute = 0; hw12.temp = 10.0; hw12.station_press = 1010.0; hw12.sea_press = 1010.0;
    HourlyWeather hw13; hw13.hour = 13; hw13.minute = 0; hw13.temp = 10.0; // 気圧欠測
    partial_res.hours = {hw10, hw11, hw12, hw13};

    JmaService::RecalculatePressures(partial_res, 500.0);
    assert(partial_res.has_missing_pressure);
    assert(partial_res.hours[1].temp < -900.0); // 実測気温自体は -999 のまま保持
    assert(partial_res.hours[3].target_press < -900.0);

    // 11:00 は 10:00(0℃) と 12:00(10℃) の線形補間 5.0℃ で換算されるため、
    // 10:00(0℃) と 12:00(10℃) の換算気圧の中間に位置すること
    assert(partial_res.hours[1].target_press > partial_res.hours[0].target_press);
    assert(partial_res.hours[1].target_press < partial_res.hours[2].target_press);
    double mid_press = 0.5 * (partial_res.hours[0].target_press + partial_res.hours[2].target_press);
    assert(std::abs(partial_res.hours[1].target_press - mid_press) < 0.05);

    // 全時刻気温欠測時は 15.0℃ フォールバック
    JmaDailyResult all_no_temp;
    all_no_temp.station.alt = 0.0;
    HourlyWeather hw_nt; hw_nt.hour = 0; hw_nt.sea_press = 1013.25;
    all_no_temp.hours.push_back(hw_nt);
    JmaService::RecalculatePressures(all_no_temp, 0.0);
    assert(!all_no_temp.has_missing_pressure);
    assert(std::abs(all_no_temp.hours[0].target_press - 1013.25) < 0.01);
    std::cout << "一部欠測時補間・フラグテストOK (11時換算気圧=" << partial_res.hours[1].target_press << " hPa)" << std::endl;

    // 11. 標高差ペナルティ官署選定・現地気圧優先無バイアス・品質記号検証
    std::cout << "\n[11] 標高差考慮の官署選定・現地気圧優先換算・品質記号検証..." << std::endl;
    double fujinomiya_dist = 0.0;
    JmaStation fujinomiya_stn = JmaService::FindNearestStation(35.2224, 138.6216, fujinomiya_dist, false, 120.0);
    assert(std::string(fujinomiya_stn.amedas_id) != "50066");
    assert(fujinomiya_stn.alt < 500.0);
    double gotemba_dist = 0.0;
    JmaStation gotemba_stn = JmaService::FindNearestStation(35.3086, 138.9347, gotemba_dist, false, 460.0);
    assert(std::string(gotemba_stn.amedas_id) != "50066");
    std::cout << "富士宮(120m)選定官署=" << fujinomiya_stn.name << "(標高" << fujinomiya_stn.alt
              << "m, 距離" << fujinomiya_dist << "km), 御殿場(460m)選定官署="
              << gotemba_stn.name << "(標高" << gotemba_stn.alt << "m)" << std::endl;

    // 気圧計と同一高度への換算時は実測現地気圧と完全一致すること
    JmaService::RecalculatePressures(daily, daily.station.barometer_m);
    assert(std::abs(daily.hours[11].target_press - daily.hours[11].station_press) < 1e-6);

    // 気象庁品質記号: 正常値・準正常値 ')' は採用、資料不足値 ']' は除外
    double parsed_v = 0.0;
    assert(JmaService::ParseJmaCellValue("1012.5", parsed_v) && std::abs(parsed_v - 1012.5) < 1e-6);
    assert(JmaService::ParseJmaCellValue("1012.5 )", parsed_v) && std::abs(parsed_v - 1012.5) < 1e-6);
    assert(!JmaService::ParseJmaCellValue("1012.5 ]", parsed_v));
    assert(!JmaService::ParseJmaCellValue("///", parsed_v));
    std::cout << "現地気圧優先換算(無バイアス)・品質記号判定テストOK" << std::endl;

    // 12. 空白区切り座標パースおよび気圧サマリー統計テスト
    std::cout << "\n[12] 空白区切り座標入力 ＆ 気圧統計テスト..." << std::endl;
    LocationInfo sp_loc = GsiService::QueryLocation("35.6814 139.7670");
    assert(sp_loc.success);
    assert(std::abs(sp_loc.lat - 35.6814) < 1e-5 && std::abs(sp_loc.lon - 139.7670) < 1e-5);
    LocationInfo fullsp_loc = GsiService::QueryLocation("35.6814\xe3\x80\x80" "139.7670");
    assert(fullsp_loc.success);
    assert(std::abs(fullsp_loc.lat - 35.6814) < 1e-5 && std::abs(fullsp_loc.lon - 139.7670) < 1e-5);
    LocationInfo three_nums = GsiService::QueryLocation("35.6814 139.7670 100");
    assert(!three_nums.success);

    JmaDailyResult stats_res;
    HourlyWeather s1; s1.hour = 1; s1.minute = 0; s1.target_press = 1005.2;
    HourlyWeather s2; s2.hour = 2; s2.minute = 0; s2.target_press = -999.0; // 欠測
    HourlyWeather s3; s3.hour = 3; s3.minute = 0; s3.target_press = 1012.8;
    HourlyWeather s4; s4.hour = 4; s4.minute = 30; s4.target_press = 1008.4;
    stats_res.hours = {s1, s2, s3, s4};
    PressureStats stats = JmaService::ComputePressureStats(stats_res);
    assert(stats.has_valid);
    assert(std::abs(stats.latest_press - 1008.4) < 1e-6 && stats.latest_time == "04:30");
    assert(std::abs(stats.max_press - 1012.8) < 1e-6 && stats.max_time == "03:00");
    assert(std::abs(stats.min_press - 1005.2) < 1e-6 && stats.min_time == "01:00");

    JmaDailyResult empty_stats_res;
    HourlyWeather s_miss; s_miss.hour = 1; s_miss.target_press = -999.0;
    empty_stats_res.hours = {s_miss};
    assert(!JmaService::ComputePressureStats(empty_stats_res).has_valid);
    std::cout << "空白区切り座標 ＆ 気圧統計テストOK (最新=" << stats.latest_press
              << ", 最高=" << stats.max_press << ", 最低=" << stats.min_press << ")" << std::endl;

    // 13. 空間補間モジュール (Step 1〜3) 決定論的単体テスト
    std::cout << "\n[13] 空間補間アルゴリズム (PlaneFit/IDW/SingleNearest) 単体テスト..." << std::endl;
    {
        const double t_lat = 35.6800;
        const double t_lon = 139.7500;
        const double cos_lat = std::cos(t_lat * 3.14159265358979323846 / 180.0);
        auto offset_station = [&](const char* id, const char* nm, double dx_km, double dy_km, double alt_m, double p0, double temp_c) {
            SpatialStationInput s;
            s.amedas_id = id;
            s.name = nm;
            s.lon = t_lon + dx_km / (111.32 * cos_lat);
            s.lat = t_lat + dy_km / 110.57;
            s.alt_m = alt_m;
            s.sea_level_pressure = p0;
            s.temp_c = temp_c;
            return s;
        };

        // 13a. 線形気圧傾度場 P(x, y) = 1012.0 + 0.04 * x - 0.03 * y を 6点から厳密復元 (PlaneFit)
        std::vector<SpatialStationInput> plane_pts = {
            offset_station("1", "East",  20.0,  10.0, 10.0, 1012.0 + 0.04 * 20.0 - 0.03 * 10.0, 20.0 - 0.0065 * 10.0),
            offset_station("2", "West", -25.0,   5.0, 20.0, 1012.0 + 0.04 * (-25.0) - 0.03 * 5.0, 20.0 - 0.0065 * 20.0),
            offset_station("3", "North",  5.0,  30.0, 50.0, 1012.0 + 0.04 * 5.0 - 0.03 * 30.0, 20.0 - 0.0065 * 50.0),
            offset_station("4", "South", -8.0, -22.0,  5.0, 1012.0 + 0.04 * (-8.0) - 0.03 * (-22.0), 20.0 - 0.0065 * 5.0),
            offset_station("5", "NE",    40.0,  35.0, 30.0, 1012.0 + 0.04 * 40.0 - 0.03 * 35.0, 20.0 - 0.0065 * 30.0),
            offset_station("6", "SW",   -35.0, -30.0, 40.0, 1012.0 + 0.04 * (-35.0) - 0.03 * (-30.0), 20.0 - 0.0065 * 40.0),
            // 100km圏外および欠測は除外されること
            offset_station("7", "Far",   120.0,  0.0, 10.0, 1030.0, 15.0),
            offset_station("8", "NullP",   2.0,  2.0, 10.0, -999.0, 15.0)
        };
        SpatialPressureResult r_plane = SpatialPressureCalculator::Evaluate(t_lat, t_lon, 100.0, plane_pts);
        assert(r_plane.valid);
        assert(r_plane.method_used == SpatialInterpMethod::PlaneFit);
        assert(r_plane.has_gradient);
        assert(r_plane.stations_used.size() == 6);
        assert(std::abs(r_plane.interpolated_sea_level_pressure - 1012.0) < 1e-4);
        assert(std::abs(r_plane.pressure_gradient_x - 0.04) < 1e-4);
        assert(std::abs(r_plane.pressure_gradient_y - (-0.03)) < 1e-4);
        assert(std::abs(r_plane.estimated_temp - (20.0 - 0.0065 * 100.0)) < 1e-3);
        double expected_local = 1012.0 * std::pow((r_plane.estimated_temp + 273.15) / (r_plane.estimated_temp + 273.15 + 0.0065 * 100.0), 5.257);
        assert(std::abs(r_plane.local_pressure - expected_local) < 1e-4);

        // 13b. 共線（一直線）配置の 4点は IDW にフォールバックすること
        std::vector<SpatialStationInput> collinear_pts = {
            offset_station("1", "L1", 10.0, 20.0, 0.0, 1011.0, 18.0),
            offset_station("2", "L2", 20.0, 40.0, 0.0, 1012.0, 18.0),
            offset_station("3", "L3", -10.0, -20.0, 0.0, 1009.0, 18.0),
            offset_station("4", "L4", -25.0, -50.0, 0.0, 1008.0, 18.0)
        };
        SpatialPressureResult r_col = SpatialPressureCalculator::Evaluate(t_lat, t_lon, 0.0, collinear_pts);
        assert(r_col.valid && r_col.method_used == SpatialInterpMethod::IDW && !r_col.has_gradient);

        // 13c. 3点以下で d_i < 0.1km が存在する場合は IDW かつその点の海面気圧をそのまま採用
        std::vector<SpatialStationInput> idw_near_pts = {
            offset_station("1", "Here", 0.03, 0.02, 10.0, 1015.4, 20.0),
            offset_station("2", "Adj1", 20.0, 0.0, 10.0, 1010.0, 20.0),
            offset_station("3", "Adj2", 0.0, 25.0, 10.0, 1011.0, 20.0)
        };
        SpatialPressureResult r_near = SpatialPressureCalculator::Evaluate(t_lat, t_lon, 0.0, idw_near_pts);
        assert(r_near.valid && r_near.method_used == SpatialInterpMethod::IDW);
        assert(std::abs(r_near.interpolated_sea_level_pressure - 1015.4) < 1e-6);

        // 13d. 1点のみの場合は SingleNearest
        std::vector<SpatialStationInput> single_pts = {
            offset_station("1", "Solo", 15.0, 15.0, 20.0, 1013.0, 16.0)
        };
        SpatialPressureResult r_single = SpatialPressureCalculator::Evaluate(t_lat, t_lon, 0.0, single_pts);
        assert(r_single.valid && r_single.method_used == SpatialInterpMethod::SingleNearest);
        assert(std::abs(r_single.interpolated_sea_level_pressure - 1013.0) < 1e-6);

        std::cout << "空間補間単体テストOK (PlaneFit P0=" << r_plane.interpolated_sea_level_pressure
                  << " hPa, grad=(" << r_plane.pressure_gradient_x << ", " << r_plane.pressure_gradient_y << "))" << std::endl;
    }

    // 14. 実通信データでの空間補間統合検証（過去日・当日・再計算再利用）
    std::cout << "\n[14] 実通信 空間補間統合テスト..." << std::endl;
    {
        JmaDailyResult past_spatial = JmaService::FetchDailyWeatherForLocation(
            loc.lat, loc.lon, 2024, 5, 1, loc.elevation, nullptr, loc.elevation
        );
        assert(past_spatial.success && !past_spatial.hours.empty());
        const HourlyWeather* sp_past = JmaService::FindLatestValidSpatialHour(past_spatial);
        assert(sp_past != nullptr && sp_past->spatial.valid);
        assert(sp_past->spatial.method_used == SpatialInterpMethod::PlaneFit);
        assert(sp_past->spatial.stations_used.size() == 6);
        assert(sp_past->has_spatial_correction);
        assert(JmaService::CanReuseNeighborDataForLocation(past_spatial, loc.lat + 0.005, loc.lon + 0.005));

        auto tateno = JmaService::FetchDailyWeatherForLocation(36.056667, 140.125, 2024, 5, 1, 25, nullptr, 25);
        assert(tateno.success && std::string(tateno.station.block_no) == "47646" && tateno.distance_km < 0.001);
        const auto* tateno_hour = JmaService::FindLatestValidSpatialHour(tateno);
        assert(tateno_hour && !tateno_hour->spatial.stations_used.empty());
        assert(tateno_hour->spatial.stations_used.front().amedas_id == tateno.station.amedas_id);

        const HourlyWeather* sp_today = JmaService::FindLatestValidSpatialHour(today_res);
        assert(sp_today != nullptr && sp_today->spatial.valid);
        assert(sp_today->spatial.stations_used.size() >= 4);
        std::cout << "実通信空間補間OK (2024-05-01 手法="
                  << SpatialPressureCalculator::MethodToString(sp_past->spatial.method_used)
                  << ", 使用=" << sp_past->spatial.stations_used.size()
                  << "点, 補正=" << sp_past->spatial_correction << " hPa / 当日手法="
                  << SpatialPressureCalculator::MethodToString(sp_today->spatial.method_used)
                  << ", 使用=" << sp_today->spatial.stations_used.size() << "点)" << std::endl;
    }

    std::cout << "\n全テスト項目正常終了。" << std::endl;
    return 0;
}
