#pragma GCC push_options
#pragma GCC optimize("O3")

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include "pins_config.h"
#include "jd9365_lcd.h"
#include "gsl3680_touch.h"

jd9365_lcd lcd(LCD_RST);

// JEDEN STAŁY BUFOR W PSRAM DLA CAŁEGO INTERFEJSU
uint16_t *backbuffer = NULL;

void safe_clear_screen(uint16_t color)
{
    // CAŁKOWITY RESET DO CZYSTEGO RGB565!
    // Ponieważ driver ma sprzętowo ustawione LCD_RGB_ELEMENT_ORDER_RGB,
    // podajemy kolor dokładnie taki, jaki wpisujemy w main.cpp!
    uint16_t raw_rgb_color = color;

    // Wypełniamy całą tablicę bezpośrednio w PSRAM z pełną prędkością -O3
    size_t total_pixels = LCD_H_RES * LCD_V_RES;
    for (size_t i = 0; i < total_pixels; i++)
    {
        backbuffer[i] = raw_rgb_color;
    }
}

extern "C" void app_main(void)
{
    printf("[LOG] INICJALIZACJA SYSTEMU BARE-METAL...\n");
    lcd.begin();

    // ZAALOKUJ BUFOR W PSRAM TYLKO RAZ NA STARTE SYSTEMU (OK. 2 MB)
    size_t buffer_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    backbuffer = (uint16_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);

    if (backbuffer == NULL)
    {
        printf("[CRITICAL ERROR] Brak pamieci PSRAM na Backbuffer!\n");
        return;
    }

    printf("[LOG] STABILNA PĘTLA GRAFICZNA URUCHOMIONA!\n");

    while (1)
    {
        // 1. CZYSZCZENIE EKRANU W PSRAM -> CZERWONY
        printf("[LOG] RENDER -> CZERWONY\n");
        safe_clear_screen(0xF800);

        // JEDEN POTĘŻNY, STABILNY TRANSFER SPRZĘTOWY PRZEZ DMA (800x1280)
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)backbuffer);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Czekamy całą sekundę - DMA bez problemu skończy transfer!

        // 2. CZYSZCZENIE EKRANU W PSRAM -> NIEBIESKI
        printf("[LOG] RENDER -> NIEBIESKI\n");
        safe_clear_screen(0x001F);

        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)backbuffer);
        vTaskDelay(pdMS_TO_TICKS(1000));

        printf("[LOG] RENDER -> ZIELONY\n");
        safe_clear_screen(0x07E0);

        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)backbuffer);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
