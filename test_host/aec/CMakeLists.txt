set(AEC_DIR ${SIPWIRE_COMPONENTS}/aec)
set(SPX_DIR ${AEC_DIR}/speexdsp)
set(SPX_SRCS
    ${SPX_DIR}/libspeexdsp/mdf.c
    ${SPX_DIR}/libspeexdsp/preprocess.c
    ${SPX_DIR}/libspeexdsp/fftwrap.c
    ${SPX_DIR}/libspeexdsp/smallft.c
    ${SPX_DIR}/libspeexdsp/filterbank.c)

# Same sources and flags as components/aec/CMakeLists.txt: float math stays
# float (see speexdsp/config.h), checked with -Wdouble-promotion (-Werror is
# global here); upstream has unused parameters.
set_source_files_properties(${SPX_SRCS} PROPERTIES COMPILE_OPTIONS "-Wno-unused-parameter;-fsingle-precision-constant")
add_library(aec STATIC ${AEC_DIR}/aec.c ${SPX_SRCS})
target_include_directories(aec PUBLIC ${AEC_DIR}/include PRIVATE ${SPX_DIR} ${SPX_DIR}/include ${SPX_DIR}/libspeexdsp)
target_compile_definitions(aec PRIVATE HAVE_CONFIG_H)
target_compile_options(aec PRIVATE -Wdouble-promotion -fsingle-precision-constant)
target_link_libraries(aec PRIVATE m)

add_executable(test_aec test_aec.c)
target_compile_options(test_aec PRIVATE -Wdouble-promotion)
target_link_libraries(test_aec PRIVATE aec unity m)
add_test(NAME aec COMMAND test_aec)

# Optimised build of the same code for the micro-benchmark (no sanitizers):
# `build_host/aec/bench_aec`; not a ctest.
add_executable(bench_aec bench_aec.c ${AEC_DIR}/aec.c ${SPX_SRCS})
target_include_directories(bench_aec PRIVATE ${AEC_DIR}/include ${SPX_DIR} ${SPX_DIR}/include ${SPX_DIR}/libspeexdsp)
target_compile_definitions(bench_aec PRIVATE HAVE_CONFIG_H)
target_compile_options(bench_aec PRIVATE -O2 -fno-sanitize=all)
target_link_options(bench_aec PRIVATE -fno-sanitize=all)
target_link_libraries(bench_aec PRIVATE m)
