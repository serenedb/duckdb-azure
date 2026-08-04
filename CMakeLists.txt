cmake_minimum_required(VERSION 3.5...3.29)

# Set extension name here
set(TARGET_NAME azure)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
project(${TARGET_NAME})
include_directories(src/include)

set(EXTENSION_SOURCES
    src/azure_extension.cpp
    src/azure_secret.cpp
    src/azure_filesystem.cpp
    src/azure_http_state.cpp
    src/azure_storage_account_client.cpp
    src/azure_blob_filesystem.cpp
    src/azure_dfs_filesystem.cpp
    src/http_logging_policy.cpp
    src/http_state_policy.cpp
    src/azure_parsed_url.cpp)
add_library(${EXTENSION_NAME} STATIC ${EXTENSION_SOURCES})

set(PARAMETERS "-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${EXTENSION_SOURCES})

# Vendored Azure SDK monolith (SereneDB third_party/azure-cmake).
target_link_libraries(${EXTENSION_NAME} _azure_sdk)
target_link_libraries(${TARGET_NAME}_loadable_extension _azure_sdk)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
