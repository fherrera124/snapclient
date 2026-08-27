cmake_minimum_required(VERSION 3.16)

project(snapclient_cli)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BELL_BACKEND_PORTAUDIO ON CACHE BOOL "Enable PortAudio output backend" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)

add_executable(snapclient_cli main.cpp)
target_link_libraries(snapclient_cli PRIVATE snapclient_core)
