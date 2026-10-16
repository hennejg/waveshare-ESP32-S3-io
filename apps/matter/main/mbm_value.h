#pragma once
/* A poll-table value as its Matter attribute carries it. Plain C, no SDK:
   test/host builds it as it is. */
#include <stdbool.h>
#include <stdint.h>

/* v is the scaled value, in the unit app_config.h names for kind
   (MBM_MATTER_*). The result is the attribute's integer:

     temperature   0.01 degC   -273.15 .. 327.67
     humidity      0.01 %         0 .. 100
     pressure      0.1 kPa (= hPa) -3276.7 .. 3276.7 kPa
     flow          0.1 m3/h       0 .. 6553.4
     power         mW          +-2^62 mW
     energy        mWh            0 .. 2^62 mWh

   Rounded to the nearest step. false, with *out untouched, for a value the
   attribute cannot carry and for an unknown kind: clamped, a meter at the
   end of its range would read as a plausible figure that is wrong. */
#ifdef __cplusplus
extern "C" {
#endif
bool mbm_matter_value(uint8_t kind, double v, int64_t *out);
#ifdef __cplusplus
}
#endif
