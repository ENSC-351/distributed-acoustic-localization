# Build the node executable

file(GLOB MY_SOURCES "src/*.c")
add_executable(node ${MY_SOURCES})

# Make use of the common library
target_link_libraries(node LINK_PRIVATE common)

# Copy executable to the public NFS directory
add_custom_command(TARGET node POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:node>" "${DEPLOY_DIR}/node"
  COMMENT "Copying node to public NFS directory")
