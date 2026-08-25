# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(BELL_DISABLE_MQTT ON CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_DISABLE_MBEDTLS ON CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_CODEC_AAC OFF CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_CODEC_HQLC OFF CACHE BOOL "Unused by snapclient" FORCE)
set(BELL_CODEC_LC3PLUS OFF CACHE BOOL "Unused by snapclient" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(snapclient-esp32)
