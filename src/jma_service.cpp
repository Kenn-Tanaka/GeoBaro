#include "jma_service.h"
#include "http_transport.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <cctype>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <windows.h>

std::string HourlyWeather::FormatTimeHHMM() const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    return std::string(buf);
}

namespace {

struct SimpleDate {
    int year = 0;
    int month = 0;
    int day = 0;
};

int CompareDate(SimpleDate a, SimpleDate b) {
    if (a.year != b.year) return (a.year < b.year) ? -1 : 1;
    if (a.month != b.month) return (a.month < b.month) ? -1 : 1;
    if (a.day != b.day) return (a.day < b.day) ? -1 : 1;
    return 0;
}

bool DateToFileTimeNoon(SimpleDate d, ULONGLONG& out_ft) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(d.year);
    st.wMonth = static_cast<WORD>(d.month);
    st.wDay = static_cast<WORD>(d.day);
    st.wHour = 12;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    out_ft = uli.QuadPart;
    return true;
}

SimpleDate GetJstTodayInternal() {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    FILETIME ft{};
    SystemTimeToFileTime(&utc, &ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart += 9ULL * 3600ULL * 10000000ULL;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME jst{};
    FileTimeToSystemTime(&ft, &jst);
    return SimpleDate{jst.wYear, jst.wMonth, jst.wDay};
}

SimpleDate AddDays(SimpleDate d, int delta_days) {
    ULONGLONG ticks = 0;
    if (!DateToFileTimeNoon(d, ticks)) return d;
    LONGLONG shifted = static_cast<LONGLONG>(ticks) + static_cast<LONGLONG>(delta_days) * 86400LL * 10000000LL;
    if (shifted < 0) return d;
    ULARGE_INTEGER uli;
    uli.QuadPart = static_cast<ULONGLONG>(shifted);
    FILETIME ft{};
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) return d;
    return SimpleDate{st.wYear, st.wMonth, st.wDay};
}

int DaysBetween(SimpleDate from, SimpleDate to) {
    ULONGLONG t1 = 0, t2 = 0;
    if (!DateToFileTimeNoon(from, t1) || !DateToFileTimeNoon(to, t2)) return 9999;
    LONGLONG diff = static_cast<LONGLONG>(t2) - static_cast<LONGLONG>(t1);
    return static_cast<int>(diff / (86400LL * 10000000LL));
}

bool HasAmedasPressure(const JmaStation& stn) {
    // 富士山 (50066) は防災情報 AMeDAS JSON に現地気圧・海面気圧が含まれない
    return std::strcmp(stn.amedas_id, "50066") != 0;
}

std::string FormatDateYmd(SimpleDate d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d", d.year, d.month, d.day);
    return std::string(buf);
}

std::string FormatTimestamp14(SimpleDate d, int hour, int minute) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d00", d.year, d.month, d.day, hour, minute);
    return std::string(buf);
}

std::wstring ToWideAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

bool ParseTwoDigits(const std::string& s, size_t pos, int& out_val) {
    if (pos + 2 > s.size()) return false;
    if (!std::isdigit(static_cast<unsigned char>(s[pos])) ||
        !std::isdigit(static_cast<unsigned char>(s[pos + 1]))) return false;
    out_val = (s[pos] - '0') * 10 + (s[pos + 1] - '0');
    return true;
}

bool ParseFourDigits(const std::string& s, size_t pos, int& out_val) {
    if (pos + 4 > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[pos + i]))) return false;
        v = v * 10 + (s[pos + i] - '0');
    }
    out_val = v;
    return true;
}

bool FetchLatestAmedasTime(
    SimpleDate& out_date,
    int& out_hour,
    int& out_min,
    const std::function<bool()>& should_cancel = nullptr
) {
    std::string raw = HttpTransport::GetString(
        L"https://www.jma.go.jp/bosai/amedas/data/latest_time.txt",
        should_cancel
    );
    size_t first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return false;
    size_t last = raw.find_last_not_of(" \t\r\n");
    std::string s = raw.substr(first, last - first + 1);
    // 形式: YYYY-MM-DDTHH:MM:SS+09:00
    if (s.size() < 19 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':') {
        return false;
    }
    int y = 0, m = 0, d = 0, hh = 0, mm = 0;
    if (!ParseFourDigits(s, 0, y) || !ParseTwoDigits(s, 5, m) || !ParseTwoDigits(s, 8, d) ||
        !ParseTwoDigits(s, 11, hh) || !ParseTwoDigits(s, 14, mm)) {
        return false;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
        return false;
    }
    out_date = SimpleDate{y, m, d};
    out_hour = hh;
    out_min = mm;
    return true;
}

struct AmedasObs {
    double temp = -999.0;
    double humidity = -999.0;
    double station_press = -999.0;
    double sea_press = -999.0;

    bool HasValidPressure() const {
        return station_press > 0.0 || sea_press > 0.0;
    }
    bool HasAnyValid() const {
        return HasValidPressure() || temp > -900.0 || humidity > -900.0;
    }
};

class JsonScanner {
public:
    explicit JsonScanner(const std::string& src) : s_(src), pos_(0) {}

    void SkipWs() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    bool ConsumeChar(char expected) {
        SkipWs();
        if (pos_ < s_.size() && s_[pos_] == expected) {
            ++pos_;
            return true;
        }
        return false;
    }

    char Peek() {
        SkipWs();
        return (pos_ < s_.size()) ? s_[pos_] : '\0';
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                char esc = s_[pos_++];
                out.push_back(esc);
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseNumber(double& out_val) {
        SkipWs();
        if (pos_ >= s_.size()) return false;
        const char* start = s_.c_str() + pos_;
        char* end = nullptr;
        double v = std::strtod(start, &end);
        if (end == start) return false;
        pos_ = static_cast<size_t>(end - s_.c_str());
        if (!std::isfinite(v)) return false;
        out_val = v;
        return true;
    }

    bool SkipValue(int depth = 0) {
        if (depth > 64) return false;
        SkipWs();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '"') {
            std::string tmp;
            return ParseString(tmp);
        }
        if (c == '{') {
            ++pos_;
            if (ConsumeChar('}')) return true;
            while (pos_ < s_.size()) {
                std::string k;
                if (!ParseString(k)) return false;
                if (!ConsumeChar(':')) return false;
                if (!SkipValue(depth + 1)) return false;
                if (ConsumeChar('}')) return true;
                if (!ConsumeChar(',')) return false;
            }
            return false;
        }
        if (c == '[') {
            ++pos_;
            if (ConsumeChar(']')) return true;
            while (pos_ < s_.size()) {
                if (!SkipValue(depth + 1)) return false;
                if (ConsumeChar(']')) return true;
                if (!ConsumeChar(',')) return false;
            }
            return false;
        }
        if (c == 't' && s_.compare(pos_, 4, "true") == 0) { pos_ += 4; return true; }
        if (c == 'f' && s_.compare(pos_, 5, "false") == 0) { pos_ += 5; return true; }
        if (c == 'n' && s_.compare(pos_, 4, "null") == 0) { pos_ += 4; return true; }
        double num = 0.0;
        return ParseNumber(num);
    }

    bool ParseObsArray(double& out_val, bool& out_valid) {
        out_valid = false;
        if (!ConsumeChar('[')) return false;
        if (Peek() == ']') {
            ++pos_;
            return true;
        }

        bool has_num = false;
        double val = 0.0;
        if (Peek() == 'n' && s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
        } else {
            if (!ParseNumber(val)) return false;
            has_num = true;
        }

        int quality = -1;
        if (ConsumeChar(',')) {
            if (Peek() == 'n' && s_.compare(pos_, 4, "null") == 0) {
                pos_ += 4;
            } else {
                double qd = -1.0;
                if (!ParseNumber(qd)) return false;
                quality = static_cast<int>(std::llround(qd));
            }
        }

        while (ConsumeChar(',')) {
            if (!SkipValue(1)) return false;
        }
        if (!ConsumeChar(']')) return false;

        // 気象庁品質コード: 0〜4 を有効とし、5(欠測)・6・7以上は無効扱い
        if (has_num && quality >= 0 && quality <= 4) {
            out_val = val;
            out_valid = true;
        }
        return true;
    }

private:
    const std::string& s_;
    size_t pos_;
};

void ParseAmedasPointJson(const std::string& json, std::map<std::string, AmedasObs>& out_map) {
    if (json.empty()) return;
    JsonScanner sc(json);
    if (!sc.ConsumeChar('{')) return;
    if (sc.Peek() == '}') {
        sc.ConsumeChar('}');
        return;
    }

    while (true) {
        std::string tsKey;
        if (!sc.ParseString(tsKey)) break;
        if (!sc.ConsumeChar(':')) break;

        if (sc.Peek() == '{') {
            sc.ConsumeChar('{');
            AmedasObs obs;
            if (sc.Peek() != '}') {
                while (true) {
                    std::string field;
                    if (!sc.ParseString(field)) return;
                    if (!sc.ConsumeChar(':')) return;

                    if (field == "pressure" && sc.Peek() == '[') {
                        double v = 0.0; bool ok = false;
                        if (!sc.ParseObsArray(v, ok)) return;
                        if (ok && v > 300.0 && v < 1200.0) obs.station_press = v;
                    } else if (field == "normalPressure" && sc.Peek() == '[') {
                        double v = 0.0; bool ok = false;
                        if (!sc.ParseObsArray(v, ok)) return;
                        if (ok && v > 300.0 && v < 1200.0) obs.sea_press = v;
                    } else if (field == "temp" && sc.Peek() == '[') {
                        double v = 0.0; bool ok = false;
                        if (!sc.ParseObsArray(v, ok)) return;
                        if (ok && v > -90.0 && v < 60.0) obs.temp = v;
                    } else if (field == "humidity" && sc.Peek() == '[') {
                        double v = 0.0; bool ok = false;
                        if (!sc.ParseObsArray(v, ok)) return;
                        if (ok && v >= 0.0 && v <= 105.0) obs.humidity = v;
                    } else {
                        if (!sc.SkipValue(1)) return;
                    }

                    if (sc.ConsumeChar('}')) break;
                    if (!sc.ConsumeChar(',')) return;
                }
            } else {
                sc.ConsumeChar('}');
            }
            if (tsKey.size() == 14) {
                out_map[tsKey] = obs;
            }
        } else {
            if (!sc.SkipValue(1)) break;
        }

        if (sc.ConsumeChar('}')) break;
        if (!sc.ConsumeChar(',')) break;
    }
}

bool ParseTimestampHourMin(const std::string& ts14, int& out_h, int& out_m) {
    if (ts14.size() != 14) return false;
    return ParseTwoDigits(ts14, 8, out_h) && ParseTwoDigits(ts14, 10, out_m);
}

} // namespace

void JmaService::GetCurrentJstDate(int& out_year, int& out_month, int& out_day) {
    SimpleDate d = GetJstTodayInternal();
    out_year = d.year;
    out_month = d.month;
    out_day = d.day;
}

bool JmaService::IsRealtimeDateCandidate(int year, int month, int day) {
    SimpleDate target{year, month, day};
    return CompareDate(target, GetJstTodayInternal()) >= 0;
}

double JmaService::CalculateDistanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371.008; // 地球平均半径 (km)
    const double pi = 3.14159265358979323846;

    double dLat = (lat2 - lat1) * pi / 180.0;
    double dLon = (lon2 - lon1) * pi / 180.0;
    double rLat1 = lat1 * pi / 180.0;
    double rLat2 = lat2 * pi / 180.0;

    double a = std::sin(dLat / 2.0) * std::sin(dLat / 2.0) +
               std::cos(rLat1) * std::cos(rLat2) *
               std::sin(dLon / 2.0) * std::sin(dLon / 2.0);
    double c = 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
    return R * c;
}

static double CalculateStationSelectionScore(double dist_km, double ground_elev_m, double station_alt_m) {
    if (!std::isfinite(ground_elev_m) || ground_elev_m <= -9000.0) {
        return dist_km;
    }
    // 標高差 50m あたり水平距離 1km 相当のペナルティを加算して山岳官署と平地のミスマッチを防ぐ
    constexpr double kKmPerMeter = 1.0 / 50.0;
    return dist_km + std::fabs(ground_elev_m - station_alt_m) * kKmPerMeter;
}

std::vector<std::pair<JmaStation, double>> JmaService::FindStationsSortedByDistance(
    double lat, double lon,
    bool require_amedas_pressure,
    double ground_elev_m
) {
    struct StationCandidate {
        JmaStation stn;
        double dist_km;
        double score;
    };
    std::vector<StationCandidate> candidates;
    candidates.reserve(154);
    for (int i = 0; i < 154; ++i) {
        if (require_amedas_pressure && !HasAmedasPressure(G_JMA_STATIONS[i])) {
            continue;
        }
        double d = CalculateDistanceKm(lat, lon, G_JMA_STATIONS[i].lat, G_JMA_STATIONS[i].lon);
        double score = CalculateStationSelectionScore(d, ground_elev_m, G_JMA_STATIONS[i].alt);
        candidates.push_back(StationCandidate{G_JMA_STATIONS[i], d, score});
    }
    std::sort(candidates.begin(), candidates.end(), [](const StationCandidate& a, const StationCandidate& b) {
        return a.score < b.score;
    });
    std::vector<std::pair<JmaStation, double>> list;
    list.reserve(candidates.size());
    for (const auto& c : candidates) {
        list.emplace_back(c.stn, c.dist_km);
    }
    return list;
}

JmaStation JmaService::FindNearestStation(
    double lat, double lon,
    double& out_distance_km,
    bool require_amedas_pressure,
    double ground_elev_m
) {
    int best_idx = 0;
    double best_score = 1e18;
    double best_dist = 1e9;

    for (int i = 0; i < 154; ++i) {
        if (require_amedas_pressure && !HasAmedasPressure(G_JMA_STATIONS[i])) {
            continue;
        }
        double d = CalculateDistanceKm(lat, lon, G_JMA_STATIONS[i].lat, G_JMA_STATIONS[i].lon);
        double score = CalculateStationSelectionScore(d, ground_elev_m, G_JMA_STATIONS[i].alt);
        if (score < best_score) {
            best_score = score;
            best_dist = d;
            best_idx = i;
        }
    }
    out_distance_km = best_dist;
    return G_JMA_STATIONS[best_idx];
}

double JmaService::CalculatePressureFromSeaLevel(double sea_press, double sea_temp_c, double target_elevation_m) {
    if (sea_press <= 0.0) return -999.0;

    // 国際標準大気 (ICAO) / 測高公式に基づく気圧計算
    // P = P0 * (1 - L * h / (T0 + 273.15)) ^ 5.257
    const double L = 0.0065;
    const double exp_val = 5.257;

    double T0_k = sea_temp_c + 273.15;
    if (T0_k <= 1.0) T0_k = 288.15;

    double factor = 1.0 - (L * target_elevation_m) / T0_k;
    if (factor <= 0.0) return -999.0;

    return sea_press * std::pow(factor, exp_val);
}

double JmaService::CalculatePressureFromStation(
    double station_press,
    double station_alt_m,
    double sea_temp_c,
    double target_elevation_m
) {
    if (station_press <= 0.0) return -999.0;

    const double L = 0.0065;
    const double exp_val = 5.257;

    double T0_k = sea_temp_c + 273.15;
    if (T0_k <= 1.0) T0_k = 288.15;

    double factor_stn = 1.0 - (L * station_alt_m) / T0_k;
    double factor_loc = 1.0 - (L * target_elevation_m) / T0_k;
    if (factor_stn <= 0.0 || factor_loc <= 0.0) return -999.0;

    return station_press * std::pow(factor_loc / factor_stn, exp_val);
}

static std::string ToLowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

static std::string TrimWhitespace(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

static std::string StripHtmlTagsAndEntities(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char c : html) {
        if (c == '<') {
            in_tag = true;
        } else if (c == '>') {
            in_tag = false;
        } else if (!in_tag) {
            out.push_back(c);
        }
    }
    // &nbsp; を半角空白へ置換
    size_t pos = 0;
    while ((pos = out.find("&nbsp;", pos)) != std::string::npos) {
        out.replace(pos, 6, " ");
    }
    return TrimWhitespace(out);
}

static std::string ExtractImgAlt(const std::string& cell_html) {
    std::string lower = ToLowerAscii(cell_html);
    size_t img_pos = lower.find("<img");
    if (img_pos == std::string::npos) return "";

    size_t tag_end = lower.find('>', img_pos);
    if (tag_end == std::string::npos) return "";

    size_t alt_pos = lower.find("alt=", img_pos);
    if (alt_pos == std::string::npos || alt_pos >= tag_end) return "";

    size_t val_start = alt_pos + 4;
    if (val_start >= tag_end) return "";
    char quote = cell_html[val_start];
    if (quote == '"' || quote == '\'') {
        val_start++;
        size_t val_end = cell_html.find(quote, val_start);
        if (val_end != std::string::npos && val_end < tag_end) {
            return TrimWhitespace(cell_html.substr(val_start, val_end - val_start));
        }
    }
    return "";
}

static bool TryParseJmaDouble(const std::string& raw, double& out_val) {
    std::string s = TrimWhitespace(raw);
    if (s.empty() || s == "--" || s == "///" || s == "×" || s == "#") {
        return false;
    }
    // 資料不足値 ']' が含まれる場合は精度低下を防ぐため欠測扱いとする
    if (s.find(']') != std::string::npos) {
        return false;
    }
    // 気象庁の準正常値記号 ')' が末尾に付与されている場合は除去して採用
    while (!s.empty() && (s.back() == ')' || s.back() == ' ')) {
        s.pop_back();
    }
    if (s.empty()) return false;
    try {
        size_t idx = 0;
        double v = std::stod(s, &idx);
        if (idx == s.size() && std::isfinite(v)) {
            out_val = v;
            return true;
        }
    } catch (...) {}
    return false;
}

bool JmaService::ParseJmaCellValue(const std::string& raw, double& out_val) {
    return TryParseJmaDouble(raw, out_val);
}

static std::vector<std::string> SplitTableCells(const std::string& tr_html) {
    std::vector<std::string> cells;
    std::string lower = ToLowerAscii(tr_html);
    size_t pos = 0;

    while (pos < tr_html.size()) {
        size_t td_pos = lower.find("<td", pos);
        size_t th_pos = lower.find("<th", pos);
        size_t start = std::min(td_pos, th_pos);
        if (start == std::string::npos) break;

        // "<td" の次がスペースまたは '>' であることを確認
        if (start + 3 < lower.size()) {
            char next_ch = lower[start + 3];
            if (next_ch != '>' && !std::isspace(static_cast<unsigned char>(next_ch))) {
                pos = start + 3;
                continue;
            }
        }

        bool is_th = (start == th_pos);
        size_t open_end = lower.find('>', start);
        if (open_end == std::string::npos) break;

        const char* close_tag = is_th ? "</th>" : "</td>";
        size_t close_pos = lower.find(close_tag, open_end + 1);
        if (close_pos == std::string::npos) break;

        std::string raw_cell = tr_html.substr(open_end + 1, close_pos - (open_end + 1));
        std::string alt = ExtractImgAlt(raw_cell);
        if (!alt.empty()) {
            cells.push_back(alt);
        } else {
            cells.push_back(StripHtmlTagsAndEntities(raw_cell));
        }
        pos = close_pos + 5;
    }
    return cells;
}

namespace {

JmaDailyResult FetchHourlyS1Internal(
    const JmaStation& station,
    int year, int month, int day,
    bool& out_got_html,
    const std::function<bool()>& should_cancel = nullptr
) {
    JmaDailyResult result;
    result.station = station;
    result.year = year;
    result.month = month;
    result.day = day;
    out_got_html = false;

    if (should_cancel && should_cancel()) {
        result.error_message = "照会がキャンセルされました。";
        return result;
    }

    std::wostringstream oss;
    oss << L"https://www.data.jma.go.jp/obd/stats/etrn/view/hourly_s1.php?prec_no="
        << station.prec_no << L"&block_no=" << station.block_no
        << L"&year=" << year << L"&month=" << month << L"&day=" << day << L"&view=";

    std::string html = HttpTransport::GetString(oss.str(), should_cancel);
    if (should_cancel && should_cancel()) {
        result.error_message = "照会がキャンセルされました。";
        return result;
    }
    if (html.empty()) {
        result.success = false;
        result.error_message = "気象庁の過去データ取得に失敗しました (通信エラーまたは接続拒否)。";
        return result;
    }
    out_got_html = true;

    std::string lower_html = ToLowerAscii(html);
    size_t pos = 0;
    bool has_any_valid_pressure = false;
    while (pos < html.size()) {
        size_t tr_start = lower_html.find("<tr", pos);
        if (tr_start == std::string::npos) break;

        if (tr_start + 3 < lower_html.size()) {
            char next_ch = lower_html[tr_start + 3];
            if (next_ch != '>' && !std::isspace(static_cast<unsigned char>(next_ch))) {
                pos = tr_start + 3;
                continue;
            }
        }

        size_t tr_open_end = lower_html.find('>', tr_start);
        if (tr_open_end == std::string::npos) break;

        size_t tr_end = lower_html.find("</tr>", tr_open_end + 1);
        if (tr_end == std::string::npos) break;

        std::string tr_content = html.substr(tr_open_end + 1, tr_end - (tr_open_end + 1));
        pos = tr_end + 5;

        std::vector<std::string> cells = SplitTableCells(tr_content);
        if (cells.empty()) continue;

        const std::string& h_str = cells[0];
        if (h_str.empty() || h_str.size() > 2) continue;
        bool is_digit = true;
        for (char c : h_str) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                is_digit = false;
                break;
            }
        }
        if (!is_digit) continue;

        int h = std::stoi(h_str);
        if (h < 1 || h > 24 || cells.size() < 15) continue;

        HourlyWeather hw;
        hw.hour = h;
        hw.minute = 0;

        TryParseJmaDouble(cells[1], hw.station_press);
        TryParseJmaDouble(cells[2], hw.sea_press);
        TryParseJmaDouble(cells[4], hw.temp);
        TryParseJmaDouble(cells[7], hw.humidity);

        if (hw.station_press > 0.0 || hw.sea_press > 0.0) {
            has_any_valid_pressure = true;
        }

        if (!cells[14].empty() && cells[14] != "×" && cells[14] != "--") {
            hw.weather = cells[14];
        } else {
            hw.weather = "-";
        }

        result.hours.push_back(hw);
    }

    if (result.hours.empty() || !has_any_valid_pressure) {
        result.hours.clear();
        result.success = false;
        result.error_message = "指定日の観測データが存在しないか、まだ公開されていません。";
    } else {
        result.success = true;
    }

    return result;
}

void CopyObsToHourly(const AmedasObs& obs, HourlyWeather& hw) {
    hw.temp = obs.temp;
    hw.humidity = obs.humidity;
    hw.station_press = obs.station_press;
    hw.sea_press = obs.sea_press;
}

JmaDailyResult FetchAmedasPointForDateInternal(
    const JmaStation& station,
    SimpleDate target_date,
    SimpleDate latest_date,
    int latest_hour,
    int latest_min,
    bool past_day_mode,
    const std::function<bool()>& should_cancel,
    bool& out_has_valid_pressure,
    bool& out_has_recent_pressure
) {
    JmaDailyResult result;
    result.station = station;
    result.year = target_date.year;
    result.month = target_date.month;
    result.day = target_date.day;
    result.is_realtime = true;
    out_has_valid_pressure = false;
    out_has_recent_pressure = false;

    std::string ymd = FormatDateYmd(target_date);
    std::vector<std::wstring> urls;
    int max_hh3 = past_day_mode ? 21 : ((latest_hour / 3) * 3);
    for (int hh3 = 0; hh3 <= max_hh3; hh3 += 3) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "https://www.jma.go.jp/bosai/amedas/data/point/%s/%s_%02d.json",
            station.amedas_id, ymd.c_str(), hh3);
        urls.push_back(ToWideAscii(buf));
    }

    SimpleDate next_date = AddDays(target_date, 1);
    if (past_day_mode && CompareDate(next_date, latest_date) <= 0) {
        std::string next_ymd = FormatDateYmd(next_date);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
            "https://www.jma.go.jp/bosai/amedas/data/point/%s/%s_00.json",
            station.amedas_id, next_ymd.c_str());
        urls.push_back(ToWideAscii(buf));
    }

    std::vector<std::string> payloads = HttpTransport::GetStrings(urls, should_cancel);
    if (should_cancel && should_cancel()) {
        result.error_message = "照会がキャンセルされました。";
        return result;
    }

    std::map<std::string, AmedasObs> obs_map;
    bool got_any_payload = false;
    bool any_empty_chunk = false;
    for (const auto& body : payloads) {
        if (!body.empty()) {
            got_any_payload = true;
            ParseAmedasPointJson(body, obs_map);
        } else {
            any_empty_chunk = true;
        }
    }

    if (!got_any_payload) {
        result.error_message = "気象庁AMeDASデータの取得に失敗しました (通信エラーまたは未公開)。";
        return result;
    }
    result.has_chunk_error = any_empty_chunk;

    if (past_day_mode) {
        for (int h = 1; h <= 24; ++h) {
            std::string key = (h < 24)
                ? FormatTimestamp14(target_date, h, 0)
                : FormatTimestamp14(next_date, 0, 0);
            HourlyWeather hw;
            hw.hour = h;
            hw.minute = 0;
            hw.weather = "-";
            auto it = obs_map.find(key);
            if (it != obs_map.end()) {
                CopyObsToHourly(it->second, hw);
                if (it->second.HasValidPressure()) {
                    out_has_valid_pressure = true;
                }
            }
            result.hours.push_back(hw);
        }
        out_has_recent_pressure = out_has_valid_pressure;
        result.latest_time_str = "24:00";
        result.success = true;
        return result;
    }

    // 当日モード: 気圧に限らず有効要素が存在する最新時刻まで表示する
    std::string latest_limit = FormatTimestamp14(latest_date, latest_hour, latest_min);
    std::string latest_any_ts;
    for (const auto& kv : obs_map) {
        if (kv.first.compare(0, 8, ymd) != 0 || kv.first > latest_limit) {
            continue;
        }
        if (kv.second.HasValidPressure()) {
            out_has_valid_pressure = true;
        }
        if (kv.second.HasAnyValid()) {
            latest_any_ts = kv.first;
        }
    }

    // 直近30分（最新時刻から10分刻みで最大3スロット）の連続気圧欠測を判定
    int total_min = latest_hour * 60 + latest_min;
    for (int k = 0; k < 3; ++k) {
        int m = total_min - k * 10;
        if (m < 0) break;
        std::string ts = FormatTimestamp14(target_date, m / 60, m % 60);
        auto it = obs_map.find(ts);
        if (it != obs_map.end() && it->second.HasValidPressure()) {
            out_has_recent_pressure = true;
            break;
        }
    }

    if (latest_any_ts.empty()) {
        result.error_message = "指定日の最新観測データがまだ取得できません。";
        return result;
    }

    int act_h = 0, act_m = 0;
    if (!ParseTimestampHourMin(latest_any_ts, act_h, act_m)) {
        result.error_message = "最新観測時刻の解析に失敗しました。";
        return result;
    }

    // 案A + 0時開始: 00:00〜act_h:00 の正時行を並べる
    for (int h = 0; h <= act_h; ++h) {
        std::string key = FormatTimestamp14(target_date, h, 0);
        HourlyWeather hw;
        hw.hour = h;
        hw.minute = 0;
        hw.weather = "-";
        auto it = obs_map.find(key);
        if (it != obs_map.end()) {
            CopyObsToHourly(it->second, hw);
        }
        result.hours.push_back(hw);
    }

    // 最新時刻が正時でない場合のみ末尾に HH:MM 行を追加
    if (act_m != 0) {
        HourlyWeather hw;
        hw.hour = act_h;
        hw.minute = act_m;
        hw.weather = "-";
        auto it = obs_map.find(latest_any_ts);
        if (it != obs_map.end()) {
            CopyObsToHourly(it->second, hw);
        }
        result.hours.push_back(hw);
    }

    char time_buf[16];
    std::snprintf(time_buf, sizeof(time_buf), "%02d:%02d", act_h, act_m);
    result.latest_time_str = time_buf;
    result.success = !result.hours.empty();
    return result;
}

} // namespace

JmaDailyResult JmaService::FetchDailyWeather(
    const JmaStation& station,
    int year, int month, int day,
    double target_elevation_m,
    const std::function<bool()>& should_cancel
) {
    SimpleDate target{year, month, day};
    SimpleDate jst_today = GetJstTodayInternal();

    if (CompareDate(target, jst_today) >= 0) {
        SimpleDate latest_date{};
        int latest_h = 0, latest_m = 0;
        if (!FetchLatestAmedasTime(latest_date, latest_h, latest_m, should_cancel)) {
            JmaDailyResult err;
            err.station = station;
            err.year = year;
            err.month = month;
            err.day = day;
            err.error_message = (should_cancel && should_cancel())
                ? "照会がキャンセルされました。"
                : "気象庁の最新時刻データの取得に失敗しました (通信エラーまたは接続拒否)。";
            return err;
        }
        int cmp = CompareDate(target, latest_date);
        if (cmp > 0) {
            JmaDailyResult err;
            err.station = station;
            err.year = year;
            err.month = month;
            err.day = day;
            char msg[160];
            if (CompareDate(target, jst_today) == 0) {
                std::snprintf(msg, sizeof(msg),
                    "本日の最新データはまだ公開されていません (最新公開時刻: %04d/%02d/%02d %02d:%02d)。",
                    latest_date.year, latest_date.month, latest_date.day, latest_h, latest_m);
            } else {
                std::snprintf(msg, sizeof(msg),
                    "未来の日付は指定できません (最新公開時刻: %04d/%02d/%02d %02d:%02d)。",
                    latest_date.year, latest_date.month, latest_date.day, latest_h, latest_m);
            }
            err.error_message = msg;
            return err;
        }
        if (cmp == 0) {
            bool has_press = false;
            bool has_recent_press = false;
            JmaDailyResult res = FetchAmedasPointForDateInternal(
                station, target, latest_date, latest_h, latest_m, false, should_cancel, has_press, has_recent_press
            );
            if (res.success) {
                RecalculatePressures(res, target_elevation_m);
            }
            return res;
        }
    }

    bool got_html = false;
    JmaDailyResult res = FetchHourlyS1Internal(station, year, month, day, got_html, should_cancel);
    if (!res.success && got_html && DaysBetween(target, jst_today) <= 9) {
        SimpleDate latest_date{};
        int latest_h = 0, latest_m = 0;
        if (FetchLatestAmedasTime(latest_date, latest_h, latest_m, should_cancel) &&
            CompareDate(target, latest_date) <= 0 &&
            DaysBetween(target, latest_date) <= 9) {
            bool has_press = false;
            bool has_recent_press = false;
            res = FetchAmedasPointForDateInternal(
                station, target, latest_date, latest_h, latest_m,
                CompareDate(target, latest_date) < 0, should_cancel, has_press, has_recent_press
            );
        }
    }

    if (res.success) {
        RecalculatePressures(res, target_elevation_m);
    }
    return res;
}

JmaDailyResult JmaService::FetchDailyWeatherForLocation(
    double lat, double lon,
    int year, int month, int day,
    double target_elevation_m,
    const std::function<bool()>& should_cancel,
    double ground_elevation_m
) {
    SimpleDate target{year, month, day};
    SimpleDate jst_today = GetJstTodayInternal();
    double effective_ground_elev = (std::isfinite(ground_elevation_m) && ground_elevation_m > -9000.0)
        ? ground_elevation_m
        : target_elevation_m;

    auto run_amedas_with_fallback = [&](SimpleDate latest_date, int latest_h, int latest_m, bool past_day_mode) -> JmaDailyResult {
        double nearest_all_dist = 0.0;
        JmaStation nearest_all = FindNearestStation(lat, lon, nearest_all_dist, false, effective_ground_elev);
        auto candidates = FindStationsSortedByDistance(lat, lon, true, effective_ground_elev);
        JmaDailyResult last_err;
        JmaDailyResult soft_fallback;
        bool has_soft_fallback = false;
        size_t max_tries = std::min<size_t>(candidates.size(), 3);
        for (size_t i = 0; i < max_tries; ++i) {
            if (should_cancel && should_cancel()) {
                last_err.error_message = "照会がキャンセルされました。";
                return last_err;
            }
            bool has_press = false;
            bool has_recent_press = false;
            JmaDailyResult res = FetchAmedasPointForDateInternal(
                candidates[i].first, target, latest_date, latest_h, latest_m,
                past_day_mode, should_cancel, has_press, has_recent_press
            );
            res.distance_km = candidates[i].second;
            res.used_fallback_station = (std::strcmp(nearest_all.amedas_id, res.station.amedas_id) != 0);
            if (res.success && has_recent_press) {
                RecalculatePressures(res, target_elevation_m);
                return res;
            }
            if (res.success && has_press && !has_soft_fallback) {
                soft_fallback = res;
                has_soft_fallback = true;
            }
            last_err = res;
            if (last_err.error_message.empty()) {
                last_err.error_message = "最寄り観測所の最新気圧データが欠測しているため取得できませんでした。";
            }
        }
        if (has_soft_fallback) {
            RecalculatePressures(soft_fallback, target_elevation_m);
            return soft_fallback;
        }
        last_err.success = false;
        return last_err;
    };

    if (CompareDate(target, jst_today) >= 0) {
        SimpleDate latest_date{};
        int latest_h = 0, latest_m = 0;
        if (!FetchLatestAmedasTime(latest_date, latest_h, latest_m, should_cancel)) {
            JmaDailyResult err;
            err.year = year;
            err.month = month;
            err.day = day;
            err.error_message = (should_cancel && should_cancel())
                ? "照会がキャンセルされました。"
                : "気象庁の最新時刻データの取得に失敗しました (通信エラーまたは接続拒否)。";
            return err;
        }
        int cmp = CompareDate(target, latest_date);
        if (cmp > 0) {
            JmaDailyResult err;
            err.year = year;
            err.month = month;
            err.day = day;
            char msg[160];
            if (CompareDate(target, jst_today) == 0) {
                std::snprintf(msg, sizeof(msg),
                    "本日の最新データはまだ公開されていません (最新公開時刻: %04d/%02d/%02d %02d:%02d)。",
                    latest_date.year, latest_date.month, latest_date.day, latest_h, latest_m);
            } else {
                std::snprintf(msg, sizeof(msg),
                    "未来の日付は指定できません (最新公開時刻: %04d/%02d/%02d %02d:%02d)。",
                    latest_date.year, latest_date.month, latest_date.day, latest_h, latest_m);
            }
            err.error_message = msg;
            return err;
        }
        if (cmp == 0) {
            return run_amedas_with_fallback(latest_date, latest_h, latest_m, false);
        }
    }

    double dist = 0.0;
    JmaStation stn = FindNearestStation(lat, lon, dist, false, effective_ground_elev);
    bool got_html = false;
    JmaDailyResult res = FetchHourlyS1Internal(stn, year, month, day, got_html, should_cancel);
    res.distance_km = dist;

    if (!res.success && got_html && DaysBetween(target, jst_today) <= 9) {
        SimpleDate latest_date{};
        int latest_h = 0, latest_m = 0;
        if (FetchLatestAmedasTime(latest_date, latest_h, latest_m, should_cancel) &&
            CompareDate(target, latest_date) <= 0 &&
            DaysBetween(target, latest_date) <= 9) {
            return run_amedas_with_fallback(latest_date, latest_h, latest_m, CompareDate(target, latest_date) < 0);
        }
    }

    if (res.success) {
        RecalculatePressures(res, target_elevation_m);
    }
    return res;
}

void JmaService::RecalculatePressures(JmaDailyResult& result, double target_elevation_m) {
    const double L = 0.0065;
    const size_t n = result.hours.size();
    result.has_missing_pressure = false;
    result.has_large_alt_diff = (std::fabs(target_elevation_m - result.station.alt) >= 1000.0);

    // 欠測時の換算用気温を同日内の有効気温から線形補間（または直近値）で取得（hw.temp自体は変更しない）
    auto get_effective_temp = [&](size_t idx) -> double {
        if (result.hours[idx].temp > -900.0) {
            return result.hours[idx].temp;
        }
        int cur_min = result.hours[idx].hour * 60 + result.hours[idx].minute;
        int prev_idx = -1;
        for (size_t j = idx; j-- > 0;) {
            if (result.hours[j].temp > -900.0) {
                prev_idx = static_cast<int>(j);
                break;
            }
        }
        int next_idx = -1;
        for (size_t j = idx + 1; j < n; ++j) {
            if (result.hours[j].temp > -900.0) {
                next_idx = static_cast<int>(j);
                break;
            }
        }
        if (prev_idx >= 0 && next_idx >= 0) {
            int t_prev = result.hours[prev_idx].hour * 60 + result.hours[prev_idx].minute;
            int t_next = result.hours[next_idx].hour * 60 + result.hours[next_idx].minute;
            if (t_next > t_prev) {
                double ratio = static_cast<double>(cur_min - t_prev) / static_cast<double>(t_next - t_prev);
                return result.hours[prev_idx].temp +
                       (result.hours[next_idx].temp - result.hours[prev_idx].temp) * ratio;
            }
            return 0.5 * (result.hours[prev_idx].temp + result.hours[next_idx].temp);
        }
        if (prev_idx >= 0) return result.hours[prev_idx].temp;
        if (next_idx >= 0) return result.hours[next_idx].temp;
        return -999.0;
    };

    for (size_t i = 0; i < n; ++i) {
        auto& hw = result.hours[i];
        double effective_temp = get_effective_temp(i);
        // 海面基準気温 T0 = T_obs + 0.0065 * h_stn (全時刻欠測時のみ 15.0℃)
        double sea_temp_c = (effective_temp > -900.0) ? (effective_temp + L * result.station.alt) : 15.0;

        hw.target_press = -999.0;
        hw.press_diff = 0.0;
        hw.has_press_diff = false;

        // 現在地標高での気圧換算 (実測値である現地気圧＋官署標高からの直接換算を第一優先とし、現地気圧欠損時は海面気圧から換算)
        if (hw.station_press > 0.0) {
            hw.target_press = CalculatePressureFromStation(
                hw.station_press, result.station.alt, sea_temp_c, target_elevation_m
            );
        } else if (hw.sea_press > 0.0) {
            hw.target_press = CalculatePressureFromSeaLevel(hw.sea_press, sea_temp_c, target_elevation_m);
        }

        if (hw.target_press > 0.0 && hw.sea_press > 0.0) {
            hw.press_diff = hw.target_press - hw.sea_press;
            hw.has_press_diff = true;
        }

        if (hw.target_press <= -900.0) {
            result.has_missing_pressure = true;
        }
    }
}

PressureStats JmaService::ComputePressureStats(const JmaDailyResult& result) {
    PressureStats stats;
    for (const auto& hw : result.hours) {
        if (hw.target_press <= -900.0 || !std::isfinite(hw.target_press)) {
            continue;
        }
        std::string t = hw.FormatTimeHHMM();
        if (!stats.has_valid) {
            stats.has_valid = true;
            stats.min_press = hw.target_press;
            stats.min_time = t;
            stats.max_press = hw.target_press;
            stats.max_time = t;
        } else {
            if (hw.target_press < stats.min_press) {
                stats.min_press = hw.target_press;
                stats.min_time = t;
            }
            if (hw.target_press > stats.max_press) {
                stats.max_press = hw.target_press;
                stats.max_time = t;
            }
        }
        stats.latest_press = hw.target_press;
        stats.latest_time = t;
    }
    return stats;
}
