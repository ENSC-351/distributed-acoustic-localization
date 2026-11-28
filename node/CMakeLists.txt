# Build the node executable

include_directories(include)
file(GLOB MY_SOURCES "src/*.c")

# NODE_FAKE_DSP=ON: made-up events instead of the microphones and DSP, for
# testing the networking without hardware (src/fakeDsp.c)
option(NODE_FAKE_DSP "Use fakeDsp.c instead of the real DSP" OFF)
if(NODE_FAKE_DSP)
  list(FILTER MY_SOURCES EXCLUDE REGEX "/dspLink\\.c$")
else()
  list(FILTER MY_SOURCES EXCLUDE REGEX "/fakeDsp\\.c$")
endif()

add_executable(node ${MY_SOURCES})

# Make use of the common, audio (microphones) and dsp (detector, estimator) libraries
target_link_libraries(node LINK_PRIVATE common)
if(NOT NODE_FAKE_DSP)
  target_link_libraries(node LINK_PRIVATE audio dsp)
endif()

# Copy executable to the public NFS directory
add_custom_command(TARGET node POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:node>" "${DEPLOY_DIR}/node"
  COMMENT "Copying node to public NFS directory")
