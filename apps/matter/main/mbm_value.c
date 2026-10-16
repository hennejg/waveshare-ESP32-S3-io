#include "mbm_value.h"

#include <math.h>
#include "app_config.h"

bool mbm_matter_value(uint8_t kind, double v, int64_t *out)
{
    double f, lo, hi;
    switch (kind) {
    case MBM_MATTER_TEMPERATURE: f = 100;  lo = -27315; hi = 32767;  break;
    case MBM_MATTER_HUMIDITY:    f = 100;  lo = 0;      hi = 10000;  break;
    case MBM_MATTER_PRESSURE:    f = 1;    lo = -32767; hi = 32767;  break;
    case MBM_MATTER_FLOW:        f = 10;   lo = 0;      hi = 65534;  break;
    case MBM_MATTER_POWER:       f = 1e3;  lo = -4611686018427387904.0; hi = 4611686018427387904.0; break;
    case MBM_MATTER_ENERGY:      f = 1e6;  lo = 0;      hi = 4611686018427387904.0; break;
    default: return false;
    }
    /* NaN fails both comparisons. Checked after rounding: 327.674 is 32767,
       327.675 is not. */
    double r = round(v * f);
    if (!(r >= lo && r <= hi)) return false;
    *out = (int64_t)r;
    return true;
}
