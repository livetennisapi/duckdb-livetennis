cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME livetennis)

# OpenSSL (via vcpkg) is required for HTTPS access to the Live Tennis API
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# HTTP via DuckDB's vendored cpp-httplib, JSON via DuckDB's vendored yyjson
include_directories(src/include duckdb/third_party/httplib
                    duckdb/third_party/yyjson/include ${OPENSSL_INCLUDE_DIR})

set(EXTENSION_SOURCES src/livetennis_extension.cpp)

if(MINGW)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

set(COMMON_LIBS duckdb_yyjson OpenSSL::SSL OpenSSL::Crypto)
if(MINGW)
  list(APPEND COMMON_LIBS crypt32 ws2_32 wsock32)
endif()

target_link_libraries(${EXTENSION_NAME} ${COMMON_LIBS})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${COMMON_LIBS})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
