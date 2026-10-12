# CMakeList.txt for common
#   Build a library (`common`) shared by the node and master
#   Use header as: #include "common/protocol.h"

file(GLOB MY_SOURCES "src/*.c")

add_library(common STATIC ${MY_SOURCES})

target_include_directories(common PUBLIC include)
