#include "../src/jma_service.h"
#include "../src/http_transport.h"
#include "../src/preset_store.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
int latest_requests;
bool cancel_after_html;
bool cancelled;
bool empty_amedas;
std::string archive_page;
const std::string row = "<tr><td>1</td><td>1000</td><td>1010</td><td>-</td><td>20</td>"
    "<td>-</td><td>-</td><td>60</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td><td>-</td></tr>";

void CheckArchivesAndFallback() {
    double distance;
    const auto tateno = JmaService::FindNearestStation(36.056667, 140.125, distance, false, 25, 20240501);
    assert(std::string(tateno.block_no) == "47646" && distance < 0.001);
    for (bool by_location : {false, true}) {
        auto fetch = [&](int day) {
            latest_requests = 0;
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
    auto read = [&] {
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), {});
    };
    assert(PresetStore::Save(file, presets));
    const auto original = read();
    const auto loaded = PresetStore::Deserialize(original);
    assert(loaded.size() == presets.size());
    for (size_t i = 0; i < loaded.size(); ++i) assert(loaded[i].name == presets[i].name);
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
        return "2024-05-19T10:00:00+09:00";
    }
    if (cancel_after_html) cancelled = true;
    return archive_page;
}

std::vector<std::string> HttpTransport::GetStrings(
    const std::vector<std::wstring>& urls, const std::function<bool()>&) {
    std::vector<std::string> pages;
    for (const auto& url : urls) {
        if (url.find(L"hourly_s1.php") != std::wstring::npos) {
            pages.push_back(archive_page);
        } else if (empty_amedas) {
            pages.emplace_back("{}");
        } else {
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
    CheckPresetFiles();
    std::cout << "Offline regression tests passed.\n";
}
