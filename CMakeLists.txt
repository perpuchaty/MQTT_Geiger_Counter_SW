# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(geiger_counter)

# The u8g2 component only requires the legacy `driver` component, which no longer
# pulls in driver/gpio.h since IDF 6 split it out.
idf_component_get_property(u8g2_lib nixy4__u8g2 COMPONENT_LIB)
target_link_libraries(${u8g2_lib} PRIVATE idf::esp_driver_gpio)
