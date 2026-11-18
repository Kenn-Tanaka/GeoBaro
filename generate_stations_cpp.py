"""Generate the embedded catalog. --import-history accepts the JMA smaster.index ZIP."""
import argparse
import json
from datetime import date, timedelta
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parent
MASTER = ROOT / "jma_stations.json"


def coordinate(field):
    return round(int(field[:-4]) + int(field[-4:]) / 6000, 6)


def following_day(value):
    d = date(int(value[:4]), int(value[4:6]), int(value[6:]))
    return (d + timedelta(days=1)).strftime("%Y%m%d")


def import_history(stations, archive):
    by_code = {s["block_no"][-3:]: s for s in stations}
    for s in stations:
        s["history"] = []
    with ZipFile(archive) as z:
        rows = z.read("smaster.index").splitlines()
    for row in rows:
        # Offsets in the official format are bytes (Shift-JIS), not characters.
        s = by_code.get(row[:3].decode("ascii"))
        if s is None:
            continue
        start, end = row[64:72].decode(), row[72:80].decode()
        ground = row[118:123].strip()
        barometer = row[54:59].strip()
        fields = dict(lat=coordinate(row[36:42]), lon=coordinate(row[42:49]),
                      ground_m=int(ground) / 10 if ground else None,
                      barometer_m=int(barometer) / 10 if barometer else None)
        history = s["history"]
        if history and history[-1]["through"] >= start:
            raise ValueError(f"Overlapping periods for {s['block_no']} at {start}")
        if (history and following_day(history[-1]["through"]) == start
                and all(history[-1][k] == v for k, v in fields.items())):
            history[-1]["through"] = end
        else:
            history.append(dict(since=start, through=end, **fields))
    for s in stations:
        if not s["history"] or s["history"][-1]["through"] != "99999999":
            raise ValueError(f"Check station coverage for {s['block_no']}")
        latest = s["history"][-1]
        s.update(lat=latest["lat"], lon=latest["lon"], alt=latest["ground_m"])
    MASTER.write_text(json.dumps(stations, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def number(value):
    return "kUnknownAltitude" if value is None else str(value)


def generate(stations):
    lines = ['#include "jma_stations.h"', '', 'namespace {']
    for s in stations:
        lines.append(f'constexpr StationPeriod history_{s["block_no"]}[] = {{')
        for p in s["history"]:
            start, end = p["since"] + "0000", p["through"] + "2359"
            # Tokyo switched sites at 09:40 JST, within the daily metadata boundary.
            if s["block_no"] == "47662":
                if p["since"] == "20141202":
                    start = "201412020940"
                if p["through"] == "20141201":
                    end = "201412020939"
            lines.append(f'    {{{start}LL, {end}LL, {p["lat"]}, {p["lon"]}, '
                         f'{number(p["ground_m"])}, {number(p["barometer_m"])}}},')
        lines.append('};')
    lines.extend(['} // namespace', '', 'const std::vector<JmaStation> G_JMA_STATIONS = {'])
    for s in stations:
        block = s["block_no"]
        latest = s["history"][-1]
        flags = str(s.get("amedas_pressure", True)).lower()
        lines.append(f'    {{"{s["amedas_id"]}", "{block}", "{s["prec_no"]}", '
                     f'u8"{s["name"]}", u8"{s["pref"]}", {latest["lat"]}, {latest["lon"]}, '
                     f'{number(latest["ground_m"])}, {number(latest["barometer_m"])}, {flags}, '
                     f'history_{block}, std::size(history_{block})}},')
    lines.append('};')
    (ROOT / "src/jma_stations.cpp").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--import-history", type=Path)
    args = parser.parse_args()
    stations = json.loads(MASTER.read_text(encoding="utf-8"))
    if args.import_history:
        import_history(stations, args.import_history)
    generate(stations)
    print(f"Generated {len(stations)} stations.")
