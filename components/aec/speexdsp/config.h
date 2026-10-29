/* sipwire: build configuration for the vendored SpeexDSP files, in place of
 * the configure-generated config.h (compiled with -DHAVE_CONFIG_H). */
#ifndef SIPWIRE_SPEEXDSP_CONFIG_H
#define SIPWIRE_SPEEXDSP_CONFIG_H

/* Single-precision float build (the ESP32-S3 has a float FPU, no double). */
#define FLOATING_POINT
/* Upstream's default FFT for float builds (real FFT, any size of 2^a 3^b 5^c). */
#define USE_SMALLFT
/* No print_vec() debug helper. */
#define RELEASE
/* Static library: no symbol visibility attribute. */
#define EXPORT
/* Allocation, warnings and fatal errors go through the aec component
 * (os_support_custom.h): no stdio, internal RAM on the device. */
#define OS_SUPPORT_CUSTOM

/* Keep the float build in single precision without editing upstream code:
 * the files are compiled with -fsingle-precision-constant (unsuffixed
 * constants are float), and <tgmath.h> turns sqrt/exp/floor/... on float
 * arguments into sqrtf/expf/floorf/... This header is included before any of
 * the files include <math.h>, whose later inclusion is then a no-op. */
#include <tgmath.h>

#endif
