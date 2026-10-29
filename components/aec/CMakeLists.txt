# Echo canceller: our wrapper (aec.c) + vendored SpeexDSP (speexdsp/, BSD-3).
# No IDF dependencies; test_host/aec builds the same sources with the same flags.
set(spx_srcs
    "speexdsp/libspeexdsp/mdf.c"
    "speexdsp/libspeexdsp/preprocess.c"
    "speexdsp/libspeexdsp/fftwrap.c"
    "speexdsp/libspeexdsp/smallft.c"
    "speexdsp/libspeexdsp/filterbank.c")

idf_component_register(
    SRCS "aec.c" ${spx_srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "speexdsp" "speexdsp/include" "speexdsp/libspeexdsp"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H)
# Single precision throughout (the S3 FPU has no double): see speexdsp/config.h.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wdouble-promotion -fsingle-precision-constant)
# Upstream code has unused parameters (fixed-point/float variants share signatures).
set_source_files_properties(${spx_srcs} PROPERTIES COMPILE_OPTIONS -Wno-unused-parameter)
