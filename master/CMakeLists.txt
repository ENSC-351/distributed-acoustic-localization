# Build the master executable

include_directories(include)
file(GLOB MY_SOURCES "src/*.c")
add_executable(master ${MY_SOURCES})
target_include_directories(master PRIVATE include)

# Make use of the common library, and libm for the demo source
target_link_libraries(master LINK_PRIVATE common m)

# Copy executable to the public NFS directory
add_custom_command(TARGET master POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:master>" "${DEPLOY_DIR}/master"
  COMMENT "Copying master to public NFS directory")
