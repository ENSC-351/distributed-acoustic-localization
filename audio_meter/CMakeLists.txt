# Build the audio meter: a standalone microphone test and sampling benchmark

file(GLOB MY_SOURCES "src/*.c")
add_executable(audio_meter ${MY_SOURCES})
target_include_directories(audio_meter PRIVATE include)

# Make use of the audio library (which brings in the HAL)
target_link_libraries(audio_meter LINK_PRIVATE audio m)

# Copy executable to the public NFS directory
add_custom_command(TARGET audio_meter POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:audio_meter>" "${DEPLOY_DIR}/audio_meter"
  COMMENT "Copying audio_meter to public NFS directory")
