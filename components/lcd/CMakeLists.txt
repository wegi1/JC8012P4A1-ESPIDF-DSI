file(GLOB SOURCES "*.cpp" "*.c")

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer esp_hw_support esp_common soc

)
