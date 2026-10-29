# Vendored SpeexDSP (echo canceller subset)

Upstream: https://github.com/xiph/speexdsp, tag `SpeexDSP-1.2.1`,
commit `1b28a0f61bc31162979e1f26f3981fc3637095c8` (2022-06-13).
Licence: BSD-3-Clause (Xiph.Org Foundation, Jean-Marc Valin and others), see `COPYING`.

Only what the acoustic echo canceller (MDF) and the preprocessor (used here
as the residual echo suppressor) need, built in floating point with the
smallft real FFT, upstream's default for float builds:

| File | What |
|---|---|
| `libspeexdsp/mdf.c` | echo canceller: multidelay block frequency-domain adaptive filter, two-path |
| `libspeexdsp/preprocess.c` | preprocessor: residual echo / noise suppression (AGC, VAD, dereverb unused) |
| `libspeexdsp/filterbank.c`, `filterbank.h` | Bark-scale filter bank for the preprocessor |
| `libspeexdsp/fftwrap.c`, `fftwrap.h` | FFT wrapper |
| `libspeexdsp/smallft.c`, `smallft.h` | real FFT (any size of 2^a 3^b 5^c; here 640) |
| `libspeexdsp/arch.h`, `math_approx.h`, `pseudofloat.h`, `os_support.h` | internal headers |
| `include/speex/speex_echo.h`, `speex_preprocess.h`, `speexdsp_types.h` | public headers |
| `COPYING` | licence |

## Local changes

**No upstream file is modified**: the files above are byte-identical to the tag.
Everything sipwire-specific is in files that upstream's own hooks pull in:

- `config.h` (new; upstream generates it with configure; compiled with
  `-DHAVE_CONFIG_H`): `FLOATING_POINT`, `USE_SMALLFT`, `RELEASE`, an empty
  `EXPORT`, and `OS_SUPPORT_CUSTOM`. It also includes `<tgmath.h>`, so the
  `sqrt`/`exp`/`floor`/`log`/`pow`/`cos`... calls on float arguments resolve to
  `sqrtf`/`expf`/... (the ESP32-S3 FPU is single precision; double math is
  software).
- `os_support_custom.h` (new; upstream's hook for `OS_SUPPORT_CUSTOM`):
  `speex_alloc`/`speex_free` go to `aec_set_allocator()` (internal RAM on the
  device), warnings are counted instead of printed (the only one the
  processing path raises is the filter-divergence reset, reported as
  `resets`), fatal errors `abort()`.
- `include/speex/speexdsp_config_types.h` (new; upstream generates it from
  `speexdsp_config_types.h.in`): the `spx_int*` types from `<stdint.h>`.

Build flags (`components/aec/CMakeLists.txt`, `test_host/aec/CMakeLists.txt`):
`-fsingle-precision-constant` (unsuffixed constants such as `1./n` or `.7*x`
are float, not double) and `-Wdouble-promotion` to catch any remaining
promotion; `-Wno-unused-parameter` for the upstream files only. Checked by
cross-compiling the five sources for the ESP32-S3: no double-precision
helper (`__adddf3`, `__muldf3`, `__divdf3`, `__extendsfdf2`, ...) and no
double libm function is referenced, only `sqrtf`, `expf`, `floorf`, `cosf`,
`sinf`, `logf`, `powf`, `atanf` and `__divsf3`.

## Updating

Copy the same files from a newer tag, keep the three sipwire files, rebuild
the host tests (`tools/ci.sh host-test`, which also checks `-Wdouble-promotion`
with `-Werror`) and update the tag and commit above. `aec_mem_estimate()` in
`../aec.c` holds the allocation sizes measured on the host; the host test
fails if they change.
