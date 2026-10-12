# Build the master executable

file(GLOB MY_SOURCES "src/*.c")
add_executable(master ${MY_SOURCES})

# Copy executable to the public NFS directory
add_custom_command(TARGET master POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:master>" "${DEPLOY_DIR}/master"
  COMMENT "Copying master to public NFS directory")
