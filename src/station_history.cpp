#include "jma_stations.h"
#include <algorithm>

bool JmaStation::AtTime(int date, int hhmm, JmaStation& out) const {
    out = *this;
    if (!history || date == 0) return true;
    if (date < 19610101) {
        // Archive lookup can still use a station ID. Its old position and heights are unknown.
        out.alt = out.barometer_m = kUnknownAltitude;
        return true;
    }
    const long long time = static_cast<long long>(date) * 10000 + hhmm;
    const auto* end = history + history_count;
    const auto* p = std::lower_bound(history, end, time,
        [](const StationPeriod& period, long long t) { return period.last_jst < t; });
    if (p == end || time < p->first_jst) return false;
    out.lat = p->lat;
    out.lon = p->lon;
    out.alt = p->ground_m;
    out.barometer_m = p->barometer_m;
    return true;
}
