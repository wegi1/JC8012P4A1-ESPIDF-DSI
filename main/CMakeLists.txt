idf_component_register(
    SRCS 
        "main.cpp"
        "RTC.cpp"
        "renderer.cpp"
        "rx8025t.c"
    INCLUDE_DIRS 
        "."
        "fonts"
    REQUIRES 
        lcd 
        touch 
        driver 
        esp_timer 
        esp_mm 
        esp_hw_support 
        esp_wifi 
        esp_netif 
        nvs_flash 
        lwip
        spiffs
)

#spiffs_create_partition_image(storage "${CMAKE_CURRENT_LIST_DIR}/../FOTOS" FLASH_IN_PROJECT)
