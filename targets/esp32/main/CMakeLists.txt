cmake_minimum_required(VERSION 3.16)

file(GLOB SOURCES "*.cpp")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
)

option(BUILD_SHARED_LIBS OFF)
option(BUILD_TESTING OFF)

# Import the portable core library (lives in core/ - project(snapclient_core)
# is just its CMake project name, not its directory)
add_subdirectory("../../../core" ${CMAKE_CURRENT_BINARY_DIR}/snapclient_core)

target_link_libraries(${COMPONENT_LIB} PUBLIC snapclient_core)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
