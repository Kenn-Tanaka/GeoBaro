#pragma once
#include <string>
#include <vector>

struct JmaStation {
    const char* amedas_id;
    const char* block_no;
    const char* prec_no;
    const char* name;
    const char* pref;
    double lat;
    double lon;
    double alt;
};

// 全国の気圧観測官署（154地点）マスターデータ
extern const JmaStation G_JMA_STATIONS[154];
