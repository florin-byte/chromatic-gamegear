// cwl_civil.h: unix seconds <-> calendar date and time (UTC), no library state. Header only so the unit's code and the Mac tests share it.
#pragma once
#include <stdint.h>

// days since 1970-01-01 of a civil date (Howard Hinnant's algorithm); m 1..12, d 1..31
static inline int64_t CwlCivil_Days(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static inline int CwlCivil_DaysIn(int y, int m)
{
    static const uint8_t k[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : k[(m - 1) % 12];
}
static inline int64_t CwlCivil_Unix(int y, int mo, int d, int hh, int mi, int ss)
{
    return CwlCivil_Days(y, (unsigned)mo, (unsigned)d) * 86400 + hh * 3600 + mi * 60 + ss;
}
static inline void CwlCivil_Split(int64_t t, int *y, int *mo, int *d, int *hh, int *mi, int *ss)
{
    int64_t days = t / 86400;
    int64_t rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    *hh = (int)(rem / 3600); *mi = (int)(rem % 3600 / 60); *ss = (int)(rem % 60);
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*mo <= 2));
}
