# CMakeList.txt for audio
#   Build a library (`audio`) for sampling the microphones through the HAL
#   Use headers as: #include "audio/audio_capture.h"

file(GLOB MY_SOURCES "src/*.c")

add_library(audio STATIC ${MY_SOURCES})

target_include_directories(audio PUBLIC include)

# Make use of the HAL library
target_link_libraries(audio PUBLIC hal)
