# CMakeList.txt for audio
#   Build a library (`audio`) for sampling the microphones, either on Linux
#   through the HAL or by reading the R5 sampler's shared-memory ring
#   Use headers as: #include "audio/audio_capture.h"

file(GLOB MY_SOURCES "src/*.c")

# The Linux reader of the R5 ring (r5capture/linux/r5cap_reader.c)
set(R5CAP_DIR "${CMAKE_SOURCE_DIR}/r5capture")

add_library(audio STATIC ${MY_SOURCES} "${R5CAP_DIR}/linux/r5cap_reader.c")

target_include_directories(audio PUBLIC include)
target_include_directories(audio PRIVATE src "${R5CAP_DIR}/include" "${R5CAP_DIR}/linux")

# Make use of the HAL library
target_link_libraries(audio PUBLIC hal)
