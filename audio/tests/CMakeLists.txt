# Host-only unit tests for the audio library's R5 backend (fake ring and timer)

add_executable(audio_r5_tests test_audio_r5.c)
target_link_libraries(audio_r5_tests PRIVATE audio m)
target_include_directories(audio_r5_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/audio/src" "${CMAKE_SOURCE_DIR}/r5capture/include")
add_test(NAME audio_r5_tests COMMAND audio_r5_tests)
