import json

with open("jma_stations.json", "r", encoding="utf-8") as f:
    stations = json.load(f)

lines = []
lines.append('#include "jma_stations.h"')
lines.append('')
lines.append('const JmaStation G_JMA_STATIONS[154] = {')

for s in stations:
    amedas_id = s["amedas_id"]
    block_no = s["block_no"]
    prec_no = s["prec_no"]
    name = s["name"]
    pref = s["pref"]
    lat = s["lat"]
    lon = s["lon"]
    alt = s["alt"]
    lines.append(f'    {{"{amedas_id}", "{block_no}", "{prec_no}", u8"{name}", u8"{pref}", {lat}, {lon}, {alt}}},')

lines.append('};')
lines.append('')

with open("src/jma_stations.cpp", "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

print(f"Generated src/jma_stations.cpp with {len(stations)} stations.")
