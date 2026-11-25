# CMakeList.txt for dsp
#   Build a library (`dsp`) with the TDOA / direction estimator
#   Use header as: #include "dsp/tdoa.h"

file(GLOB MY_SOURCES "src/*.c")

add_library(dsp STATIC ${MY_SOURCES})

target_include_directories(dsp PUBLIC include)

# Math library (sqrt, asin, hypot, ...)
target_link_libraries(dsp PUBLIC m)

# Command-line tool: run the estimator on a window exported to a file
add_executable(tdoa_cli tools/tdoa_cli.c)
target_link_libraries(tdoa_cli PRIVATE dsp)
