# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(geiger_counter)

set(OTA_IMAGE_DIR "${CMAKE_SOURCE_DIR}/ota")
set(OTA_IMAGE "${OTA_IMAGE_DIR}/geiger_counter_ota.bin")
add_custom_command(TARGET gen_project_binary POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E make_directory "${OTA_IMAGE_DIR}"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${CMAKE_BINARY_DIR}/${PROJECT_NAME}.bin"
			"${OTA_IMAGE}"
	COMMENT "Creating OTA image: ${OTA_IMAGE}"
)

# The u8g2 component only requires the legacy `driver` component, which no longer
# pulls in driver/gpio.h since IDF 6 split it out.
idf_component_get_property(u8g2_lib nixy4__u8g2 COMPONENT_LIB)
target_link_libraries(${u8g2_lib} PRIVATE idf::esp_driver_gpio)
