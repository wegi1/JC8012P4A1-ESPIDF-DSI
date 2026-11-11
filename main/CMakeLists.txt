idf_component_register(
    SRCS 
        "main.cpp"
        "RTC.cpp"
        "renderer.cpp"
        "rx8025t.c"
        "wifi_ntp.cpp"
        "vectordotsball.cpp"
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
        esp_event
        nvs_flash 
        lwip
        spiffs
        esp_new_jpeg
)

#spiffs_create_partition_image(storage "${CMAKE_CURRENT_LIST_DIR}/../FOTOS" FLASH_IN_PROJECT)
