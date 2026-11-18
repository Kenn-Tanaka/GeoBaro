#include "jma_service.h"
#include <windows.h>

void JmaService::GetCurrentJstDate(int& year, int& month, int& day) {
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
    year = jst.wYear;
    month = jst.wMonth;
    day = jst.wDay;
}
