#include "preset_store.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

static std::string EscapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

static std::string UnescapeField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char next = s[i + 1];
            switch (next) {
                case '\\': out.push_back('\\'); i++; break;
                case 't':  out.push_back('\t'); i++; break;
                case 'n':  out.push_back('\n'); i++; break;
                case 'r':  out.push_back('\r'); i++; break;
                default:   out.push_back(s[i]); break;
            }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::vector<std::string> SplitTab(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t pos = line.find('\t', start);
        if (pos == std::string::npos) {
            fields.push_back(line.substr(start));
            break;
        }
        fields.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return fields;
}

std::string PresetStore::Serialize(const std::vector<PresetLocation>& presets) {
    std::ostringstream oss;
    oss << "# GeoBaro Presets v1\n";
    for (const auto& p : presets) {
        if (p.name.empty()) continue;
        oss << EscapeField(p.name) << "\t"
            << EscapeField(p.input_text) << "\t"
            << EscapeField(p.base_address) << "\t"
            << (p.is_fine_tuned ? "1" : "0") << "\t"
            << std::fixed << std::setprecision(6) << p.lat << "\t"
            << std::fixed << std::setprecision(6) << p.lon << "\t"
            << std::fixed << std::setprecision(6) << p.base_lat << "\t"
            << std::fixed << std::setprecision(6) << p.base_lon << "\t"
            << std::fixed << std::setprecision(1) << p.height_agl_m << "\t"
            << std::clamp(p.zoom, 10, 18) << "\n";
    }
    return oss.str();
}

std::vector<PresetLocation> PresetStore::Deserialize(const std::string& content) {
    std::vector<PresetLocation> result;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') continue;

        auto fields = SplitTab(line);
        if (fields.size() < 10) continue;

        try {
            PresetLocation p;
            p.name = UnescapeField(fields[0]);
            p.input_text = UnescapeField(fields[1]);
            p.base_address = UnescapeField(fields[2]);
            p.is_fine_tuned = (fields[3] == "1");
            p.lat = std::stod(fields[4]);
            p.lon = std::stod(fields[5]);
            p.base_lat = std::stod(fields[6]);
            p.base_lon = std::stod(fields[7]);
            p.height_agl_m = std::clamp(std::stod(fields[8]), 0.0, 2000.0);
            p.zoom = std::clamp(std::stoi(fields[9]), 10, 18);

            if (!p.name.empty() &&
                p.lat >= -85.051128 && p.lat <= 85.051128 &&
                p.lon >= -180.0 && p.lon <= 180.0) {
                result.push_back(std::move(p));
            }
        } catch (...) {
            continue;
        }
    }
    return result;
}

void PresetStore::Upsert(std::vector<PresetLocation>& presets, const PresetLocation& item) {
    for (auto& existing : presets) {
        if (existing.name == item.name) {
            existing = item;
            return;
        }
    }
    presets.push_back(item);
}

bool PresetStore::RemoveByIndex(std::vector<PresetLocation>& presets, size_t index) {
    if (index >= presets.size()) return false;
    presets.erase(presets.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}
