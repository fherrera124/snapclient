cmake_minimum_required(VERSION 3.16)

project(snapclient_core)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SOURCES "src/*.cpp")

set(BELL_DISABLE_MQTT ON CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_DISABLE_MBEDTLS ON CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_CODEC_AAC OFF CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_CODEC_HQLC OFF CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_CODEC_LC3PLUS OFF CACHE BOOL "Unused by snapclient" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../external/bell ${CMAKE_CURRENT_BINARY_DIR}/bell)

add_library(snapclient_core STATIC ${SOURCES})
target_link_libraries(snapclient_core PUBLIC bell)
target_include_directories(snapclient_core PUBLIC "include")

option(SNAPCLIENT_CORE_BUILD_TESTS "Build snapclient_core unit tests" OFF)
if(SNAPCLIENT_CORE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
