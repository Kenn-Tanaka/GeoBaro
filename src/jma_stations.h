#pragma once
#include <cstddef>
#include <iterator>
#include <limits>
#include <vector>

constexpr double kUnknownAltitude = std::numeric_limits<double>::quiet_NaN();

struct StationPeriod {
    long long first_jst, last_jst; // YYYYMMDDhhmm, inclusive
    double lat, lon, ground_m, barometer_m;
};

struct JmaStation {
    const char* amedas_id = "";
    const char* block_no = "";
    const char* prec_no = "";
    const char* name = "";
    const char* pref = "";
    double lat = 0, lon = 0;
    double alt = kUnknownAltitude;
    double barometer_m = kUnknownAltitude;
    bool amedas_pressure = false;
    const StationPeriod* history = nullptr;
    std::size_t history_count = 0;

    // date=0 selects the current catalog. A daily station summary represents noon JST.
    bool AtTime(int yyyymmdd, int hhmm, JmaStation& out) const;
};

extern const std::vector<JmaStation> G_JMA_STATIONS;
