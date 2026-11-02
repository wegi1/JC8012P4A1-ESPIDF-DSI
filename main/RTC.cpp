//=====================================================================================================
#include "rtc.hpp"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <esp_timer.h>

// Naglowki FreeRTOS oraz ESP-IDF
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_cache.h"
#include "esp_log.h"

// Naglowki Twojego podsystemu graficznego i sprzetu
#include "renderer.hpp"
#include "fonty.h"
#include "jd9365_lcd.h"
#include "gsl3680_touch.h"
#include "pins_config.h"
//=====================================================================================================
// Deklaracje obiektow i buforow zdefiniowanych w main.cpp
extern jd9365_lcd lcd;

// ZAMYKAMY DEKLARACJĘ DOTYKU W BLOKU ZGODNOŚCI "C":
extern gsl3680_touch touch;

extern uint16_t *fbs[2];
extern volatile int back_buffer_idx;
extern SemaphoreHandle_t vsync_sem;
// Wskaźnik (uchwyt) do zadania gaszenia dwukropka
TaskHandle_t hTaskDisableColon = NULL;

static const size_t BUFFER_SIZE = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);

// Zmienne statyczne do obsługi czasu POSIX
static struct tm dane_czasu;
static time_t sekundy_epoch;

// Bufory tekstowe na sformatowane cyfry
static char txt_godz[4];
static char txt_min[4];
static char txt_sek[4];

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// ==========================================
// CZĘŚĆ 2 Z 3: ZADANIE FREE_RTOS I INICJALIZACJA
// ==========================================
void vTaskDisableColon(void *pvParameters)
{
    uint64_t rtc_start = 0;
    uint64_t rtc_end = 0;
    // 3. Definiujemy bufory w sposób absolutny i bezpieczny:
    uint16_t *back_buffer = (uint16_t *)fbs[0]; // wolny bufor tła
    while (1)
    {
        // 1. Czekamy na sygnał wybudzenia z funkcji print_RTC()
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // 2. Odliczamy dokładnie 500 milisekund (dwukropek świeci)
        vTaskDelay(pdMS_TO_TICKS(500));

        // rtc_start = esp_timer_get_time();
        // 3. Od razu na nim GASIMY kwadraciki dwukropka
        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 605, 138, 50, 50, 0x0000);
        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 605, 324, 50, 50, 0x0000);
        esp_cache_msync((void *)back_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        xSemaphoreTake(vsync_sem, portMAX_DELAY);

        // 7. Wysłanie klatki bez dwukropka na ekran przez DMA
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)back_buffer);

        // rtc_end = esp_timer_get_time();
        // printf("[PROFILER] RENDER COLON (vTaskDisableColon) zajął: %llu us\n", (rtc_end - rtc_start));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskTouch(void *pvParameters)
{
    uint16_t touchX = 0;
    uint16_t touchY = 0;

    printf("[LOG] Wielowątkowe zadanie obsługi dotyku vTaskTouch URUCHOMIONE w module RTC!\n");

    while (1)
    {
        // Pobieramy współrzędne z kontrolera GSL3680 co 20 ms
        bool touched = touch.getTouch(&touchX, &touchY);

        if (touched)
        {
            printf("[TOUCH] Kliknięcie wykryte w RTC.cpp! X = %d, Y = %d\n", touchX, touchY);

            // Tutaj w przyszłości będziemy przechwytywać kliknięcia w menu ustawień!
        }

        // Bezpieczne 20 ms odpoczynku dla jądra FreeRTOS
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Funkcja wywoływana raz w app_main() w celu rejestracji zadań w jądrze FreeRTOS
void init_RTC_subsystem(void)
{
    // 1. Rejestracja zadania wygaszania dwukropka (Priorytet 5)
    xTaskCreate(vTaskDisableColon, "TaskDisableColon", 4096, NULL, 5, &hTaskDisableColon);

    // 2. Rejestracja zadania szybkiego skanowania dotyku (Priorytet 4)
    xTaskCreate(vTaskTouch, "vTaskTouch", 4096, NULL, 4, NULL);

    printf("[LOG] Podsystem RTC: Zadania mrugania i dotyku zostały pomyślnie zarejestrowane.\n");
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Zmienna do filtrowania zbocza sekundy (Edge Detection)
static int old_seconds = 111;
void print_RTC(void)
{
    uint64_t rtc_start = 0;
    uint64_t rtc_end = 0;

    // rtc_start = esp_timer_get_time();
    // 1. Pobieramy aktualny czas UNIX (liczba sekund od 1970 r.)
    time(&sekundy_epoch);

    // 2. Parsujemy sekundy na czytelną strukturę kalendarzową
    localtime_r(&sekundy_epoch, &dane_czasu);

    // 3. W tym momencie w 'dane_czasu.tm_sec' masz aktualną sekundę (od 0 do 59)
    int second_now = dane_czasu.tm_sec;
    if (old_seconds == second_now)
    {
        return;
    }
    old_seconds = second_now;
    snprintf(txt_godz, sizeof(txt_godz), "%02d", dane_czasu.tm_hour);
    snprintf(txt_min, sizeof(txt_min), "%02d", dane_czasu.tm_min);
    snprintf(txt_sek, sizeof(txt_sek), "%02d", dane_czasu.tm_sec);

    // 1. Pobieramy wskaźniki na oba sprzętowe bufory z main.cpp

    uint16_t *main_buffer = (uint16_t *)fbs[0];

    // 1. Czyszczenie jedynego bufora na czarno
    safe_clear_screen(main_buffer, 0x0000);

    // 2. Rysowanie bazy zegara (czcionki wektorowe bezpośrednio na nim)
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_godz, 0, 0, 370.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_min, 680, 0, 370.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_sek, 140, 610, 100.0f, 0xFFFF);

    // 3. Od razu na nim zapalamy kwadraciki dwukropka
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 138, 50, 50, 0xFFFF);
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 324, 50, 50, 0xFFFF);

    // 4. Jedno, czyste i pewne czyszczenie Cache dla tego bufora
    esp_cache_msync((void *)main_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // 5. Czekamy na VSYNC i wypychamy DMA na ekran MIPI-DSI
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)main_buffer);

    // 6. Wybudzamy task gaszenia dwukropka (który po 500 ms tylko go zamaluje na czarno)
    if (hTaskDisableColon != NULL)
    {
        xTaskNotifyGive(hTaskDisableColon);
    }
    // rtc_end = esp_timer_get_time();
    // printf("[PROFILER] RENDER RTC (print_RTC) zajął: %llu us\n", (rtc_end - rtc_start));
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
