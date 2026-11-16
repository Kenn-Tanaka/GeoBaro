#pragma once
#include <string>
#include <vector>
#include <functional>

struct LocationInfo {
    bool success = false;
    std::string query;
    std::string resolved_address;
    double lat = 0.0;
    double lon = 0.0;
    double elevation = 0.0;
    std::string hsrc;
    std::string error_message;
};

struct TilePiece {
    int draw_x = 0; // 256x256キャンバス上の描画開始X座標
    int draw_y = 0; // 256x256キャンバス上の描画開始Y座標
    std::vector<unsigned char> png_data;
};

struct TileInfo {
    int z = 15;             // 表示ズームレベル (10〜18)
    int fetch_z = 15;       // 実際の取得ズームレベル (relief時は最大15)
    int digital_scale = 1;  // 拡大倍率 (1 << (z - fetch_z))
    int x = 0;
    int y = 0;
    std::string layer = "std";
    int view_width = 256;
    int view_height = 256;
    int view_left_z = 0;    // 表示ズームz上のビューポート左上X(ワールドpx)
    int view_top_z = 0;     // 表示ズームz上のビューポート左上Y(ワールドpx)
    int src_px_left = 0;    // fetch_z上の切り出し元左上X(ワールドpx)
    int src_px_top = 0;     // fetch_z上の切り出し元左上Y(ワールドpx)
    int src_w = 256;        // fetch_z上の切り出し幅(px)
    int src_h = 256;        // fetch_z上の切り出し高さ(px)
    double world_px_z = 0.0;
    double world_py_z = 0.0;
    double center_lat = 0.0;
    double center_lon = 0.0;
    int pixel_x = 128;      // ビューポート内でのマーカーX座標
    int pixel_y = 128;      // ビューポート内でのマーカーY座標
    std::vector<TilePiece> pieces;
};

class GsiService {
public:
    // 住所または「緯度, 経度」から座標・住所・標高を取得
    static LocationInfo QueryLocation(
        const std::string& input,
        const std::function<bool()>& should_cancel = nullptr
    );

    // 緯度・経度を指定して標高のみを取得
    static bool GetElevation(
        double lat, double lon,
        double& out_elev, std::string& out_hsrc,
        const std::function<bool()>& should_cancel = nullptr
    );

    // 緯度・経度 <-> ワールドピクセル座標変換 (Webメルカトル)
    static void LatLonToWorldPx(double lat, double lon, int zoom, double& out_px, double& out_py);
    static void WorldPxToLatLon(double world_px, double world_py, int zoom, double& out_lat, double& out_lon);

    // 緯度・経度から中心揃えの地図タイル(LRUキャッシュ＋必要枚数合成用)を取得
    // layer: "std" (標準地図), "pale" (淡色地図), "relief" (色別標高図)
    static TileInfo GetMapTile(
        double lat, double lon,
        int zoom = 15,
        const std::string& layer = "std",
        int view_width = 256,
        int view_height = 256,
        const std::function<bool()>& should_cancel = nullptr
    );

    // メモリ上のタイルキャッシュ枚数を取得（検証用）
    static size_t GetTileCacheCount();

private:
    static bool GeocodeAddress(
        const std::string& address,
        double& out_lat, double& out_lon, std::string& out_title,
        const std::function<bool()>& should_cancel = nullptr
    );
};
