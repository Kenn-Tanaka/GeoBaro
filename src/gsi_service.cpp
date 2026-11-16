#include "gsi_service.h"
#include "http_transport.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <regex>
#include <cctype>
#include <algorithm>
#include <mutex>
#include <list>
#include <unordered_map>
#include <windows.h>

static std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (size <= 0) return L"";
    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
    return result;
}

static std::string UrlEncodeUtf8(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << std::uppercase << int(c);
        }
    }
    return escaped.str();
}

static void AppendUtf8CodePoint(std::string& out, unsigned int cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static bool ParseHex4(const std::string& s, size_t pos, unsigned int& out_val) {
    if (pos + 4 > s.size()) return false;
    unsigned int val = 0;
    for (size_t i = 0; i < 4; ++i) {
        char c = s[pos + i];
        val <<= 4;
        if (c >= '0' && c <= '9') val |= (c - '0');
        else if (c >= 'a' && c <= 'f') val |= (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val |= (c - 'A' + 10);
        else return false;
    }
    out_val = val;
    return true;
}

static std::string UnescapeJsonString(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[i + 1];
            switch (next) {
                case '"':  out.push_back('"');  i++; break;
                case '\\': out.push_back('\\'); i++; break;
                case '/':  out.push_back('/');  i++; break;
                case 'b':  out.push_back('\b'); i++; break;
                case 'f':  out.push_back('\f'); i++; break;
                case 'n':  out.push_back('\n'); i++; break;
                case 'r':  out.push_back('\r'); i++; break;
                case 't':  out.push_back('\t'); i++; break;
                case 'u': {
                    unsigned int cp1 = 0;
                    if (ParseHex4(raw, i + 2, cp1)) {
                        i += 5;
                        // サロゲートペア対応
                        if (cp1 >= 0xD800 && cp1 <= 0xDBFF && i + 6 < raw.size() &&
                            raw[i + 1] == '\\' && raw[i + 2] == 'u') {
                            unsigned int cp2 = 0;
                            if (ParseHex4(raw, i + 3, cp2) && cp2 >= 0xDC00 && cp2 <= 0xDFFF) {
                                unsigned int full_cp = 0x10000 + (((cp1 - 0xD800) << 10) | (cp2 - 0xDC00));
                                AppendUtf8CodePoint(out, full_cp);
                                i += 6;
                                break;
                            }
                        }
                        AppendUtf8CodePoint(out, cp1);
                    } else {
                        out.push_back(raw[i]);
                    }
                    break;
                }
                default:
                    out.push_back(next);
                    i++;
                    break;
            }
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

static bool ExtractJsonStringField(const std::string& json_str, const std::string& key, std::string& out_value) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json_str.find(pattern);
    if (pos == std::string::npos) return false;

    pos = json_str.find(':', pos + pattern.size());
    if (pos == std::string::npos) return false;

    pos++;
    while (pos < json_str.size() && std::isspace(static_cast<unsigned char>(json_str[pos]))) {
        pos++;
    }
    if (pos >= json_str.size() || json_str[pos] != '"') return false;

    pos++; // skip opening quote
    std::string raw;
    bool escaped = false;
    while (pos < json_str.size()) {
        char c = json_str[pos];
        if (escaped) {
            raw.push_back('\\');
            raw.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            out_value = UnescapeJsonString(raw);
            return true;
        } else {
            raw.push_back(c);
        }
        pos++;
    }
    return false;
}

bool GsiService::GeocodeAddress(
    const std::string& address,
    double& out_lat, double& out_lon, std::string& out_title,
    const std::function<bool()>& should_cancel
) {
    if (should_cancel && should_cancel()) return false;
    std::string encoded = UrlEncodeUtf8(address);
    std::wstring url = L"https://msearch.gsi.go.jp/address-search/AddressSearch?q=" + Utf8ToWide(encoded);
    std::string json_str = HttpTransport::GetString(url, should_cancel);
    if (json_str.empty()) return false;

    // 軽量正規表現パース: "coordinates": [lon, lat]
    std::regex coord_regex(R"("coordinates"\s*:\s*\[\s*([-+]?\d+(?:\.\d+)?)\s*,\s*([-+]?\d+(?:\.\d+)?)\s*\])");
    std::smatch match;
    if (std::regex_search(json_str, match, coord_regex)) {
        try {
            out_lon = std::stod(match[1].str());
            out_lat = std::stod(match[2].str());
        } catch (...) {
            return false;
        }

        if (!ExtractJsonStringField(json_str, "title", out_title) || out_title.empty()) {
            out_title = address;
        }
        return true;
    }
    return false;
}

bool GsiService::GetElevation(
    double lat, double lon,
    double& out_elev, std::string& out_hsrc,
    const std::function<bool()>& should_cancel
) {
    if (should_cancel && should_cancel()) return false;
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(6);
    oss << L"https://cyberjapandata2.gsi.go.jp/general/dem/scripts/getelevation.php?lat="
        << lat << L"&lon=" << lon << L"&outtype=JSON";

    std::string json_str = HttpTransport::GetString(oss.str(), should_cancel);
    if (json_str.empty()) return false;

    std::regex elev_regex(R"("elevation"\s*:\s*([-+]?\d+(?:\.\d+)?))");
    std::smatch match;
    if (std::regex_search(json_str, match, elev_regex)) {
        try {
            out_elev = std::stod(match[1].str());
        } catch (...) {
            return false;
        }

        if (!ExtractJsonStringField(json_str, "hsrc", out_hsrc)) {
            out_hsrc = "不明";
        }
        return true;
    }
    return false;
}

LocationInfo GsiService::QueryLocation(
    const std::string& input,
    const std::function<bool()>& should_cancel
) {
    LocationInfo info;
    info.query = input;

    std::string clean = input;
    for (char& c : clean) {
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    }
    // 全角カンマを半角カンマへ、全角スペースを半角スペースへ置換
    size_t pos = 0;
    while ((pos = clean.find("\xef\xbc\x8c", pos)) != std::string::npos) {
        clean.replace(pos, 3, ",");
    }
    pos = 0;
    while ((pos = clean.find("\xe3\x80\x80", pos)) != std::string::npos) {
        clean.replace(pos, 3, " ");
    }

    // 厳密な浮動小数点数形式（カンマ区切り、または空白区切りの2数値）を座標として認識
    std::regex latlon_regex(R"(^\s*([-+]?\d+(?:\.\d+)?)(?:\s*,\s*|\s+)([-+]?\d+(?:\.\d+)?)\s*$)");
    std::smatch match;
    if (std::regex_search(clean, match, latlon_regex)) {
        try {
            info.lat = std::stod(match[1].str());
            info.lon = std::stod(match[2].str());
        } catch (...) {
            info.success = false;
            info.error_message = "指定された座標の数値を解析できませんでした。";
            return info;
        }

        if (!std::isfinite(info.lat) || !std::isfinite(info.lon) ||
            info.lat < -85.051128 || info.lat > 85.051128 ||
            info.lon < -180.0 || info.lon > 180.0) {
            info.success = false;
            info.error_message = "緯度または経度が有効範囲外です（緯度: -85.05〜85.05、経度: -180〜180）。";
            return info;
        }
        info.resolved_address = "指定座標 (" + match[1].str() + ", " + match[2].str() + ")";
    } else {
        if (!GeocodeAddress(input, info.lat, info.lon, info.resolved_address, should_cancel)) {
            info.success = false;
            info.error_message = (should_cancel && should_cancel())
                ? "照会がキャンセルされました。"
                : "指定された住所または座標を特定できませんでした。";
            return info;
        }
    }

    if (should_cancel && should_cancel()) {
        info.success = false;
        info.error_message = "照会がキャンセルされました。";
        return info;
    }

    if (!GetElevation(info.lat, info.lon, info.elevation, info.hsrc, should_cancel)) {
        info.success = false;
        info.error_message = (should_cancel && should_cancel())
            ? "照会がキャンセルされました。"
            : "標高データを取得できませんでした (海域または日本国外の可能性があります)。";
        return info;
    }

    info.success = true;
    return info;
}

static int FloorDiv(int a, int b) {
    int q = a / b;
    int r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) {
        q--;
    }
    return q;
}

static constexpr size_t MAX_TILE_CACHE_ITEMS = 192;

struct TileCacheEntry {
    std::vector<unsigned char> data;
    std::list<std::string>::iterator lru_it;
};

static std::mutex g_tileCacheMutex;
static std::list<std::string> g_tileCacheLru;
static std::unordered_map<std::string, TileCacheEntry> g_tileCacheMap;

static bool TryGetCachedTile(const std::string& key, std::vector<unsigned char>& out_data) {
    std::lock_guard<std::mutex> lock(g_tileCacheMutex);
    auto it = g_tileCacheMap.find(key);
    if (it == g_tileCacheMap.end()) return false;
    g_tileCacheLru.splice(g_tileCacheLru.begin(), g_tileCacheLru, it->second.lru_it);
    out_data = it->second.data;
    return true;
}

static void StoreCachedTile(const std::string& key, const std::vector<unsigned char>& data) {
    if (data.empty()) return;
    std::lock_guard<std::mutex> lock(g_tileCacheMutex);
    auto it = g_tileCacheMap.find(key);
    if (it != g_tileCacheMap.end()) {
        it->second.data = data;
        g_tileCacheLru.splice(g_tileCacheLru.begin(), g_tileCacheLru, it->second.lru_it);
        return;
    }
    if (g_tileCacheMap.size() >= MAX_TILE_CACHE_ITEMS && !g_tileCacheLru.empty()) {
        const std::string& oldest = g_tileCacheLru.back();
        g_tileCacheMap.erase(oldest);
        g_tileCacheLru.pop_back();
    }
    g_tileCacheLru.push_front(key);
    g_tileCacheMap[key] = TileCacheEntry{data, g_tileCacheLru.begin()};
}

size_t GsiService::GetTileCacheCount() {
    std::lock_guard<std::mutex> lock(g_tileCacheMutex);
    return g_tileCacheMap.size();
}

void GsiService::LatLonToWorldPx(double lat, double lon, int zoom, double& out_px, double& out_py) {
    const double pi = 3.14159265358979323846;
    lat = std::clamp(lat, -85.051128, 85.051128);
    double lat_rad = lat * pi / 180.0;
    double n_double = std::pow(2.0, zoom);

    double x_exact = (lon + 180.0) / 360.0 * n_double;
    double y_exact = (1.0 - std::asinh(std::tan(lat_rad)) / pi) / 2.0 * n_double;

    out_px = x_exact * 256.0;
    out_py = y_exact * 256.0;
}

void GsiService::WorldPxToLatLon(double world_px, double world_py, int zoom, double& out_lat, double& out_lon) {
    const double pi = 3.14159265358979323846;
    double total_px = 256.0 * std::pow(2.0, zoom);

    double wrapped_x = std::fmod(world_px, total_px);
    if (wrapped_x < 0.0) wrapped_x += total_px;
    double clamped_y = std::clamp(world_py, 0.0, total_px);

    out_lon = (wrapped_x / total_px) * 360.0 - 180.0;
    double lat_rad = std::atan(std::sinh(pi * (1.0 - 2.0 * (clamped_y / total_px))));
    out_lat = std::clamp(lat_rad * 180.0 / pi, -85.051128, 85.051128);
}

TileInfo GsiService::GetMapTile(
    double lat, double lon,
    int zoom,
    const std::string& layer,
    int view_width,
    int view_height,
    const std::function<bool()>& should_cancel
) {
    TileInfo tile;
    tile.z = std::clamp(zoom, 10, 18);
    tile.layer = (layer == "pale" || layer == "relief") ? layer : "std";
    tile.fetch_z = (tile.layer == "relief" && tile.z > 15) ? 15 : tile.z;
    tile.digital_scale = 1 << (tile.z - tile.fetch_z);
    tile.view_width = (view_width > 0) ? view_width : 256;
    tile.view_height = (view_height > 0) ? view_height : 256;

    tile.center_lat = std::clamp(lat, -85.051128, 85.051128);
    tile.center_lon = lon;

    LatLonToWorldPx(tile.center_lat, tile.center_lon, tile.z, tile.world_px_z, tile.world_py_z);

    tile.x = static_cast<int>(std::floor(tile.world_px_z / 256.0));
    tile.y = static_cast<int>(std::floor(tile.world_py_z / 256.0));

    // マーカーがビューポート中央付近に来るよう表示ズーム上の左上座標を決定
    tile.view_left_z = static_cast<int>(std::floor(tile.world_px_z - tile.view_width / 2.0));
    tile.view_top_z = static_cast<int>(std::floor(tile.world_py_z - tile.view_height / 2.0));

    tile.pixel_x = static_cast<int>(std::round(tile.world_px_z - tile.view_left_z));
    tile.pixel_y = static_cast<int>(std::round(tile.world_py_z - tile.view_top_z));

    if (tile.digital_scale == 1) {
        tile.src_px_left = tile.view_left_z;
        tile.src_px_top = tile.view_top_z;
        tile.src_w = tile.view_width;
        tile.src_h = tile.view_height;
    } else {
        double scale = static_cast<double>(tile.digital_scale);
        tile.src_px_left = static_cast<int>(std::floor(tile.view_left_z / scale));
        tile.src_px_top = static_cast<int>(std::floor(tile.view_top_z / scale));
        int src_px_right = static_cast<int>(std::ceil((tile.view_left_z + tile.view_width) / scale));
        int src_px_bottom = static_cast<int>(std::ceil((tile.view_top_z + tile.view_height) / scale));
        tile.src_w = src_px_right - tile.src_px_left;
        tile.src_h = src_px_bottom - tile.src_px_top;
    }

    if (should_cancel && should_cancel()) {
        return tile;
    }

    int n_tiles = 1 << tile.fetch_z;
    int min_tx = FloorDiv(tile.src_px_left, 256);
    int max_tx = FloorDiv(tile.src_px_left + tile.src_w - 1, 256);
    int min_ty = FloorDiv(tile.src_px_top, 256);
    int max_ty = FloorDiv(tile.src_px_top + tile.src_h - 1, 256);

    struct PendingTile {
        TilePiece piece;
        std::string cache_key;
    };
    std::vector<PendingTile> pending;
    std::vector<std::wstring> fetch_urls;
    std::vector<size_t> fetch_indices;

    std::wstring wlayer = Utf8ToWide(tile.layer);
    for (int ty = min_ty; ty <= max_ty; ++ty) {
        if (ty < 0 || ty >= n_tiles) continue;
        for (int tx = min_tx; tx <= max_tx; ++tx) {
            int wrapped_tx = ((tx % n_tiles) + n_tiles) % n_tiles;

            PendingTile item;
            item.piece.draw_x = tx * 256 - tile.src_px_left;
            item.piece.draw_y = ty * 256 - tile.src_px_top;
            item.cache_key = tile.layer + "/" + std::to_string(tile.fetch_z) + "/" +
                             std::to_string(wrapped_tx) + "/" + std::to_string(ty);

            if (!TryGetCachedTile(item.cache_key, item.piece.png_data)) {
                std::wostringstream oss;
                oss << L"https://cyberjapandata.gsi.go.jp/xyz/" << wlayer << L"/"
                    << tile.fetch_z << L"/" << wrapped_tx << L"/" << ty << L".png";
                fetch_urls.push_back(oss.str());
                fetch_indices.push_back(pending.size());
            }
            pending.push_back(std::move(item));
        }
    }

    if (!fetch_urls.empty()) {
        auto fetched = HttpTransport::GetBinaries(fetch_urls, should_cancel);
        for (size_t i = 0; i < fetched.size() && i < fetch_indices.size(); ++i) {
            if (!fetched[i].empty()) {
                size_t idx = fetch_indices[i];
                StoreCachedTile(pending[idx].cache_key, fetched[i]);
                pending[idx].piece.png_data = std::move(fetched[i]);
            }
        }
    }

    if (should_cancel && should_cancel()) {
        return tile;
    }

    for (auto& item : pending) {
        if (!item.piece.png_data.empty()) {
            tile.pieces.push_back(std::move(item.piece));
        }
    }

    return tile;
}
