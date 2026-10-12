cmake_minimum_required(VERSION 3.16)

file(GLOB SOURCES "*.cpp")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_gptimer esp_driver_i2s
                  esp_driver_uart esp_driver_usb_serial_jtag esp_eth esp_event
                  esp_netif esp_pm esp_timer esp_wifi nvs_flash
)

# taojson's operator"" _foo() syntax (space before the suffix) is
# deprecated in C++23; this toolchain's GCC treats it as a hard error
# under -Werror. Third-party vendored header, not fixable here.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=deprecated-literal-operator" APPEND)

# Same category: this toolchain's GCC flags switch-fallthrough and
# designated-initializer patterns bell's own vendored dependencies use.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=implicit-fallthrough" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=missing-field-initializers" APPEND)

option(BUILD_SHARED_LIBS OFF)
option(BUILD_TESTING OFF)

# Import the portable core library (lives in core/ - project(snapclient_core)
# is just its CMake project name, not its directory)
add_subdirectory("../core" ${CMAKE_CURRENT_BINARY_DIR}/snapclient_core)

target_link_libraries(${COMPONENT_LIB} PUBLIC snapclient_core)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
