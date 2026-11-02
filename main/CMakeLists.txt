idf_component_register(SRCS "RTC.cpp" "renderer.cpp" "rx8025t.c" "main.cpp" 
                       INCLUDE_DIRS "." "fonts"
                       REQUIRES lcd esp_timer touch driver esp_mm esp_hw_support)

