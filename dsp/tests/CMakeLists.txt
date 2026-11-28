# Host-only unit tests for the dsp library

add_executable(dsp_tests test_tdoa.c)
target_link_libraries(dsp_tests PRIVATE dsp)
add_test(NAME dsp_tests COMMAND dsp_tests)

add_executable(detector_tests test_detector.c)
target_link_libraries(detector_tests PRIVATE dsp)
add_test(NAME detector_tests COMMAND detector_tests)
