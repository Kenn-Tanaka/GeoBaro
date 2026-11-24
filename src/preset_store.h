#pragma once
#include <string>
#include <vector>
#include <filesystem>

inline constexpr double kMinHeightAglM = -2000.0;
inline constexpr double kMaxHeightAglM = 2000.0;

struct PresetLocation {
    std::string name;
    std::string input_text;
    std::string base_address;
    bool is_fine_tuned = false;
    double lat = 0.0;
    double lon = 0.0;
    double base_lat = 0.0;
    double base_lon = 0.0;
    double height_agl_m = 0.0;
    int zoom = 15;
};

class PresetStore {
public:
    static std::string Serialize(const std::vector<PresetLocation>& presets);
    static std::vector<PresetLocation> Deserialize(const std::string& content);
    static bool Save(const std::filesystem::path& path, const std::vector<PresetLocation>& presets);
    static void Upsert(std::vector<PresetLocation>& presets, const PresetLocation& item);
    static bool RemoveByIndex(std::vector<PresetLocation>& presets, size_t index);
};
