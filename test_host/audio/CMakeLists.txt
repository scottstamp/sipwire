add_executable(test_audio test_audio_dsp.c ${SIPWIRE_COMPONENTS}/audio/audio_dsp.c)
target_include_directories(test_audio PRIVATE ${SIPWIRE_COMPONENTS}/audio)
target_link_libraries(test_audio PRIVATE unity m)
add_test(NAME audio COMMAND test_audio)
