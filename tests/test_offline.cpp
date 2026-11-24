#include "../src/jma_service.h"
#include "../src/http_transport.h"
#include "../src/preset_store.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
int latest_requests;
int amedas_requests;
int archive_neighbor_requests;
bool cancel_after_html;
bool cancelled;
bool empty_amedas;
bool drop_first_neighbor;
std::string latest_timestamp = "2024-05-19T10:00:00+09:00";
std::vector<std::wstring> archive_requests;
std::wstring missing_archive_block;
std::string archive_page;
const std::string row = "<tr><td>1</td><td>1000</td><td>1010</td><td>-</td><td>20</td>"
    "<td>-</td><td>-</td><td>60</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td></tr>";

void CheckArchivesAndFallback() {
    double distance;
    const auto tateno = JmaService::FindNearestStation(36.056667, 140.125, distance, false, 25, 20240501);
    assert(std::string(tateno.block_no) == "47646" && distance < 0.001);
    for (bool by_location : {false, true}) {
        auto fetch = [&](int day) {
            latest_requests = amedas_requests = 0;
            cancelled = false;
            auto stop = [] { return cancelled; };
            return by_location
                ? JmaService::FetchDailyWeatherForLocation(tateno.lat, tateno.lon, 2024, 5, day, 25, stop, 25)
                : JmaService::FetchDailyWeather(tateno, 2024, 5, day, 25, stop);
        };
        archive_page = row;
        auto archived = fetch(1);
        assert(archived.success && !archived.is_realtime && latest_requests == 0);
        if (by_location) {
            assert(archived.hours.front().spatial.valid);
            const auto& used = archived.hours.front().spatial.stations_used;
            assert(!used.empty() && used.front().amedas_id == tateno.amedas_id);
        }
        for (auto page : {"", "<html></html>"}) {
            archive_page = page;
            for (int day : {18, 10}) {
                auto fallback = fetch(day);
                assert(fallback.success && fallback.is_realtime && latest_requests == 1);
                assert(fallback.hours.front().target_press > 0);
            }
            assert(!fetch(9).success && latest_requests == 0);
            empty_amedas = true;
            assert(!fetch(18).success && latest_requests == 1);
            empty_amedas = false;
            cancel_after_html = true;
            assert(!fetch(18).success && latest_requests == 0);
            cancel_after_html = false;
        }

        latest_timestamp = "2024-05-20T00:00:00+09:00";
        archive_page.clear();
        const auto midnight = fetch(19);
        assert(midnight.success && midnight.is_realtime && latest_requests == 1 && amedas_requests > 0);
        assert(midnight.hours.back().hour == 24);
        assert(!fetch(10).success && latest_requests == 1 && amedas_requests == 0);
        cancel_after_html = true;
        assert(!fetch(19).success && latest_requests == 1 && amedas_requests == 0);
        cancel_after_html = false;
        archive_page = row;
        assert(fetch(19).success && latest_requests == 1 && amedas_requests == 0);
        latest_timestamp = "2024-05-19T10:00:00+09:00";
    }
}

void CheckHistoryAndPressure() {
    double distance;
    auto osaka = JmaService::FindNearestStation(34.681667, 135.518333, distance, false, 23, 20240501);
    assert(osaka.alt == 23 && osaka.barometer_m == 83);
    JmaDailyResult result;
    result.station = osaka;
    result.year = 2024; result.month = 5; result.day = 1;
    HourlyWeather h;
    h.hour = 1; h.temp = 20; h.station_press = 1000;
    result.hours.push_back(h);
    JmaService::RecalculatePressures(result, 83);
    assert(std::abs(result.hours[0].single_target_press - 1000) < 1e-8);
    JmaService::RecalculatePressures(result, 23);
    assert(std::abs(result.hours[0].target_press - 1007.0) < 0.1);
    const double ground_press = result.hours[0].target_press;
    JmaService::RecalculatePressures(result, 23 - 5);
    assert(result.hours[0].target_press > ground_press);
    auto historical_osaka = JmaService::FindNearestStation(34.681667, 135.518333, distance, false, 23, 19880501);
    assert(std::string(historical_osaka.block_no) == "47772" && std::isnan(historical_osaka.alt));
    result.station = historical_osaka;
    result.year = 1988;
    result.hours[0].temp = 0;
    JmaService::RecalculatePressures(result, 300);
    const double cold_pressure = result.hours[0].single_target_press;
    result.hours[0].temp = 30;
    JmaService::RecalculatePressures(result, 300);
    assert(cold_pressure > 0 && result.hours[0].single_target_press > cold_pressure);
    result.station = JmaStation{};
    result.station.alt = 23;
    JmaService::RecalculatePressures(result, 23);
    assert(result.has_missing_pressure && !result.hours[0].has_spatial_correction);
    result.hours[0].sea_press = 1013;
    JmaService::RecalculatePressures(result, 0);
    assert(result.hours[0].target_press == 1013);
    auto fuji = JmaService::FindNearestStation(35.36, 138.726667, distance, false, 3775, 20240519);
    assert(std::string(fuji.block_no) == "47639" && !fuji.amedas_pressure);
    assert(!JmaService::FetchDailyWeather(fuji, 2024, 5, 19, 3775).success);
    auto historical_fuji = JmaService::FindNearestStation(35.351667, 138.73, distance, false, 3775, 19880501);
    assert(std::string(historical_fuji.block_no) == "47639" && std::isnan(historical_fuji.alt));
    result.station = historical_fuji;
    JmaService::RecalculatePressures(result, 120);
    assert(result.has_large_alt_diff);
    assert(std::string(JmaService::FindNearestStation(35.2224, 138.6216, distance, false, 120, 19880501).block_no) != "47639");
    assert(std::string(JmaService::FindNearestStation(35.3086, 138.9347, distance, false, 460, 19880501).block_no) != "47639");

    const auto tokyo = JmaService::FindNearestStation(35.69, 139.76, distance, false, 20, 20141202);
    JmaStation old, moved;
    assert(tokyo.AtTime(20141202, 939, old) && tokyo.AtTime(20141202, 940, moved));
    assert(old.lon == 139.76 && moved.lon == 139.75);
    assert(old.barometer_m == 36.5 && moved.barometer_m == 24.2);
    archive_page = row;
    for (int day : {1, 3}) {
        const auto& expected = day == 1 ? old : moved;
        auto archive = JmaService::FetchDailyWeatherForLocation(expected.lat, expected.lon, 2014, 12, day, 20);
        assert(archive.success && archive.distance_km < 0.001);
        const auto& used = archive.hours.front().spatial.stations_used;
        assert(!used.empty() && used.front().amedas_id == tokyo.amedas_id && used.front().distance_km < 0.001);
        assert(JmaService::CanReuseNeighborDataForLocation(archive, expected.lat, expected.lon));
        const auto& hour = archive.hours.front();
        assert(hour.has_spatial_correction && std::abs(hour.target_press - hour.single_target_press - hour.spatial_correction) < 1e-8);
    }
    auto early = JmaService::FetchDailyWeatherForLocation(tokyo.lat, tokyo.lon, 1960, 1, 1, 0);
    assert(early.success && !std::isfinite(early.station.barometer_m));
    assert(!early.hours.front().spatial.valid && early.hours.front().target_press == 1010);
}

void CheckNeighborRequests() {
    std::string last_row = row;
    last_row.replace(last_row.find("<td>1</td>"), std::string("<td>1</td>").size(), "<td>24</td>");
    archive_page = row + last_row;
    for (int year : {1960, 1988}) {
        archive_neighbor_requests = 0;
        const auto archive = JmaService::FetchDailyWeatherForLocation(35.69, 139.76, year, 12, 31, 20);
        assert(archive.success && archive.hours.back().hour == 24);
        assert(archive_neighbor_requests == 0 && !JmaService::FindLatestValidSpatialHour(archive));
        assert(JmaService::CanReuseNeighborDataForLocation(archive, 35.69002, 139.76002));
    }
    drop_first_neighbor = true;
    archive_neighbor_requests = 0;
    const auto partial = JmaService::FetchDailyWeatherForLocation(35.69, 139.76, 2024, 5, 1, 20);
    assert(partial.success && archive_neighbor_requests > 0);
    assert(partial.attempted_neighbor_blocks.size() == partial.neighbor_series.size());
    assert(JmaService::CanReuseNeighborDataForLocation(partial, 35.69002, 139.76002));
    assert(!JmaService::CanReuseNeighborDataForLocation(partial, 34.68, 135.52));
    drop_first_neighbor = false;

    double nearest;
    const auto stations = JmaService::FindStationsSortedByDistance(34.986667, 139.865, false, 20, 19600101);
    assert(stations.size() >= 3 && std::string(stations.front().first.block_no) == "47672");
    missing_archive_block = L"block_no=47672";
    archive_requests.clear();
    const auto fallback = JmaService::FetchDailyWeatherForLocation(34.986667, 139.865, 1960, 1, 1, 20);
    assert(fallback.success && fallback.used_fallback_station && fallback.station.block_no == std::string(stations[1].first.block_no));
    assert(fallback.distance_km == stations[1].second && archive_requests.size() == 2);
    assert(JmaService::CanReuseNeighborDataForLocation(fallback, 34.986667, 139.865, &nearest) && nearest == fallback.distance_km);
    missing_archive_block.clear();
    archive_page = "<html></html>";
    archive_requests.clear();
    const auto unavailable = JmaService::FetchDailyWeatherForLocation(34.986667, 139.865, 1960, 1, 1, 20);
    assert(!unavailable.success && archive_requests.size() == 3 && !unavailable.error_message.empty());
}

void CheckPresetFiles() {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / ("GeoBaro-test-" + std::to_string(GetCurrentProcessId()) +
                                              "-" + std::to_string(GetTickCount64()));
    assert(fs::create_directory(dir));
    auto file = dir / "presets.dat";
    std::vector<PresetLocation> presets;
    for (auto name : {"#Tokyo", "# GeoBaro Presets v1", "Osaka"}) {
        PresetLocation p;
        p.name = name;
        presets.push_back(p);
    }
    presets[0].height_agl_m = -5.3;
    auto read = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    assert(PresetStore::Save(file, presets));
    const auto original = read();
    const auto loaded = PresetStore::Deserialize(original);
    assert(loaded.size() == presets.size());
    assert(loaded[0].height_agl_m == -5.3);
    for (size_t i = 0; i < loaded.size(); ++i) assert(loaded[i].name == presets[i].name);
    const auto invalid = PresetStore::Deserialize(
        "NaN\tA\tA\t0\t35\t139\t35\t139\tnan\t15\n"
        "Inf\tA\tA\t0\t35\t139\t35\t139\tinf\t15\n"
        "Base\tA\tA\t1\t35\t139\tinf\t139\t1\t15\n"
        "Bounds\tA\tA\t1\t35\t139\t35\t181\t1\t15\n");
    assert(invalid.empty());
    presets.erase(presets.begin());
    assert(!PresetStore::Save(dir / "missing" / "presets.dat", presets));
    HANDLE lock = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    assert(lock != INVALID_HANDLE_VALUE);
    assert(!PresetStore::Save(file, presets));
    CloseHandle(lock);
    assert(read() == original);
    assert(PresetStore::Save(file, presets));
    assert(read() == PresetStore::Serialize(presets));
    assert(fs::remove(file) && fs::remove(dir));
}
} // namespace

// The normal entry points use a fixed JST clock and fixture responses in this executable.
void JmaService::GetCurrentJstDate(int& year, int& month, int& day) {
    year = 2024; month = 5; day = 19;
}

std::string HttpTransport::GetString(const std::wstring& url, const std::function<bool()>&) {
    if (url.find(L"latest_time.txt") != std::wstring::npos) {
        ++latest_requests;
        return latest_timestamp;
    }
    if (cancel_after_html) cancelled = true;
    archive_requests.push_back(url);
    if (!missing_archive_block.empty() && url.find(missing_archive_block) != std::wstring::npos) return "<html></html>";
    return archive_page;
}

std::vector<std::string> HttpTransport::GetStrings(
    const std::vector<std::wstring>& urls, const std::function<bool()>&) {
    std::vector<std::string> pages;
    for (const auto& url : urls) {
        if (url.find(L"hourly_s1.php") != std::wstring::npos) {
            ++archive_neighbor_requests;
            pages.push_back(drop_first_neighbor && pages.empty() ? std::string() : archive_page);
        } else if (empty_amedas) {
            pages.emplace_back("{}");
        } else {
            ++amedas_requests;
            auto day = url.substr(url.rfind(L'/') + 1, 8);
            pages.push_back("{\"" + std::string(day.begin(), day.end()) +
                "010000\":{\"pressure\":[1000,0],\"normalPressure\":[1010,0],\"temp\":[20,0]}}");
        }
    }
    return pages;
}

int main() {
    CheckHistoryAndPressure();
    CheckArchivesAndFallback();
    CheckNeighborRequests();
    CheckPresetFiles();
    std::cout << "Offline regression tests passed.\n";
}
