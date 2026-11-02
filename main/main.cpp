// #define _GNU_SOURCE // << TO MAKRO MUSI BYĆ JAKO PIERWSZE NA SAMEJ GÓRZE PLIKU!

#pragma GCC push_options
#pragma GCC optimize("O3")

#include "renderer.hpp"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#include "pins_config.h"
#include "jd9365_lcd.h"
#include "driver/i2c.h"

// 1. Dołączamy Twoje tablice z czcionkami bezpośrednio z folderu fonts
#include "fonty.h"
#include "boldfonts.h"

// 2. Aktywujemy i dołączamy silnik stb_truetype

#include "esp_timer.h"
#include "esp_log.h"
#include <sys/time.h> // Zawiera settimeofday
#include <time.h>     // Zawiera struktury czasu
#include "freertos/semphr.h"
#include "esp_cache.h"
#include "esp_lcd_mipi_dsi.h"

//============================================================================================================
//============================================================================================================
//============================================================================================================

// Wymuszenie dla kompilatora C++, aby traktował plik producenta jako kod C++ / C kompatybilny
extern "C"
{
#include "gsl3680_touch.h"
#include "rx8025t.h"
}
//============================================================================================================
//============================================================================================================
//============================================================================================================

// DWA SPRZĘTOWE BUFORY DLA BEZKOPIOŁOWEGO PAGE FLIPPINGU
uint16_t *fbs[2] = {NULL, NULL};
volatile int back_buffer_idx = 1;
static SemaphoreHandle_t vsync_sem = NULL;

// Callback wywoływany automatycznie z przerwania sprzętowego VSYNC
static bool IRAM_ATTR on_frame_complete_cb(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *user_ctx)
{
    BaseType_t high_task_awoken = pdFALSE;
    if (vsync_sem != NULL)
    {
        xSemaphoreGiveFromISR(vsync_sem, &high_task_awoken);
    }
    return high_task_awoken;
}

//============================================================================================================
//============================================================================================================
//============================================================================================================

#include "driver/i2c.h" // Potrzebne do konfiguracji magistrali

jd9365_lcd lcd(LCD_RST);

// JEDEN STAŁY BUFOR W PSRAM DLA CAŁEGO INTERFEJSU
uint16_t *draw_ptr = NULL;
//============================================================================================================
//============================================================================================================
//============================================================================================================

//============================================================================================================
//============================================================================================================
//============================================================================================================
void sync_internal_rtc_with_rx8025t(void)
{
    rx8025t_time_t rtc_hardware_time;

    // 1. Pobieramy aktualny czas z fizycznej kostki RX8025T
    if (rx8025t_get_time(&rtc_hardware_time) == ESP_OK)
    {
        // 2. Przepisujemy na strukturę systemową 'struct tm'
        struct tm sys_time;
        sys_time.tm_sec = rtc_hardware_time.second;
        sys_time.tm_min = rtc_hardware_time.minute;
        sys_time.tm_hour = rtc_hardware_time.hour;
        sys_time.tm_mday = rtc_hardware_time.day;
        sys_time.tm_mon = rtc_hardware_time.month - 1;    // W systemie C miesiące są od 0 do 11
        sys_time.tm_year = rtc_hardware_time.year - 1900; // System C liczy lata od roku 1900
        sys_time.tm_wday = rtc_hardware_time.weekday;
        sys_time.tm_isdst = -1; // Brak informacji o czasie letnim

        // 3. Konwersja na znacznik czasu UNIX (liczba sekund od 1970 r.)
        time_t t = mktime(&sys_time);

        // 4. Ustawiamy wewnętrzny czas systemowy w ESP32-P4
        struct timeval now = {.tv_sec = t, .tv_usec = 0};
        settimeofday(&now, NULL);

        ESP_LOGI("SYSTEM_RTC", "Synchronizacja udana! Wewnetrzny zegar ESP32-P4 zostal ustawiony.");
    }
    else
    {
        ESP_LOGE("SYSTEM_RTC", "BLAD SYNCHRONIZACJI! Zewnetrzny RX8025T nie odpowiedzial.");
    }
}

//============================================================================================================
//============================================================================================================
//============================================================================================================
extern "C" esp_lcd_panel_handle_t get_my_dsi_handle(void);

extern "C" void app_main(void)
{

    printf("[LOG] INICJALIZACJA SYSTEMU BARE-METAL...\n");
    lcd.begin();
    //****************************************************************************************** */
    // Inicjalizacja semafora FreeRTOS dla synchronizacji VSYNC
    vsync_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(vsync_sem);

    // Pobieramy sprzętowy uchwyt panelu bezpośrednio z pliku .cpp przez funkcję globalną
    esp_lcd_panel_handle_t dpi_handle = get_my_dsi_handle();

    // Rejestracja callbacku przerwaniowego VSYNC (on_color_trans_done dla ESP-IDF 5.5.3)
    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_frame_complete_cb};
    esp_lcd_dpi_panel_register_event_callbacks(dpi_handle, &cbs, NULL);

    // Alokacja dwóch buforów w PSRAM z wyrównaniem do linii Cache (64 bajty)
    size_t buffer_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    fbs[0] = (uint16_t *)heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM);
    fbs[1] = (uint16_t *)heap_caps_aligned_alloc(64, buffer_size, MALLOC_CAP_SPIRAM);

    if (fbs[0] == NULL || fbs[1] == NULL)
    {
        printf("[CRITICAL ERROR] Brak pamieci PSRAM na Double Buffering!\n");
        return;
    }
    // Wyświetlamy zakresy adresów (początek i koniec) dla Double Bufferingu
    printf("[MEMORY] FB0 zakres: %p -> %p (Rozmiar: %d bajtow)\n",
           (void *)fbs[0],
           (void *)((uint8_t *)fbs[0] + buffer_size - 1),
           buffer_size);

    printf("[MEMORY] FB1 zakres: %p -> %p (Rozmiar: %d bajtow)\n",
           (void *)fbs[1],
           (void *)((uint8_t *)fbs[1] + buffer_size - 1),
           buffer_size);

    // Inicjalizacja bufora 1 MB oraz jednorazowe parsowanie OBU czcionek
    if (!init_renderer_scratchpad(digital_7_ttf, SourceCodePro_Bold_ttf))
    {
        printf("[CRITICAL ERROR] Stopujemy system, brak pamieci dla renderera!\n");
        return;
    }

    // JAWNE CZYSZCZENIE BUFORÓW NA CZARNO ZARAZ PO ALOKACJI (BEZPIECZNY START)
    safe_clear_screen(fbs[0], 0x0000);
    safe_clear_screen(fbs[1], 0x0000);

    // Koniecznie synchronizujemy cache dla obu, żeby PSRAM przyjął czarne piksele
    esp_cache_msync((void *)fbs[0], buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync((void *)fbs[1], buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    /***************************************************************************************************** */
    // ZAALOKUJ BUFOR W PSRAM TYLKO RAZ NA STARTE SYSTEMU (OK. 2 MB)
    // size_t buffer_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    // draw_ptr  = (uint16_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);

    printf("[LOG] STABILNA PĘTLA GRAFICZNA URUCHOMIONA!\n");

    // 2. Deklaracja obiektu (producent sam skonfiguruje I2C w środku)
    static gsl3680_touch touch(TP_I2C_SDA, TP_I2C_SCL, TP_RST, TP_INT);

    // 3. Odpalenie dotyku – teraz przejdzie bez błędu i wybudzi układ!
    touch.begin();
    printf("[LOG] Dotyk gsl3680 pomyślnie wystartował bez konfliktów magistrali!\n");
    //============================================================================================================
    rx8025t_init();
    sync_internal_rtc_with_rx8025t(); // <<< WYWOŁUJEMY RAZ PO RESECIE

    // =========================================================================
    // TEST DETEKCJI I ODCZYTU ZEGARA RTC RX8025T
    // =========================================================================

    // Szybki ping adresu I2C (0x32) w celu weryfikacji obecności układu na magistrali
    i2c_cmd_handle_t ping_cmd = i2c_cmd_link_create();
    i2c_master_start(ping_cmd);
    i2c_master_write_byte(ping_cmd, (0x32 << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(ping_cmd);

    esp_err_t ping_err = i2c_master_cmd_begin(I2C_NUM_0, ping_cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(ping_cmd);

    if (ping_err == ESP_OK)
    {
        ESP_LOGI("RTC_TEST", "SUKCES! Wykryto urzadzenie RX8025T pod adresem I2C 0x32.");

        // Jeśli układ żyje, spróbujmy odczytać z niego czas na starcie
        rx8025t_time_t test_time;
        if (rx8025t_get_time(&test_time) == ESP_OK)
        {
            ESP_LOGI("RTC_TEST", "Pierwszy odczyt czasu udany: %02d:%02d:%02d, Data: %02d-%02d-%04d",
                     test_time.hour, test_time.minute, test_time.second,
                     test_time.day, test_time.month, test_time.year);
        }
        else
        {
            ESP_LOGE("RTC_TEST", "Blad podczas parsowania rejestrow czasu!");
        }
    }
    else
    {
        ESP_LOGE("RTC_TEST", "BLAD! Urzadzenie pod adresem 0x32 nie odpowiada na szynie I2C. Sprawdz zasilanie/polaczenie.");
    }
    // ---------------------------------------------------------------------
    // JEDNORAZOWE USTAWIANIE AKTUALNEGO CZASU
    // ---------------------------------------------------------------------
    // rx8025t_time_t czas_na_start;
    // czas_na_start.hour = 10;   // Aktualna godzina (Format 24h)
    // czas_na_start.minute = 41; // Aktualna minuta
    // czas_na_start.second = 0;  // Sekundy na zero
    // czas_na_start.day = 31;    // 31 dzień miesiąca
    // czas_na_start.month = 10;  // Październik
    // czas_na_start.year = 2026; // Rok 2026
    // czas_na_start.weekday = 6; // 6 -> Sobota (licząc od 0=Niedziela lub 1=Poniedziałek wg Twojej konwencji)

    // if (rx8025t_set_time(&czas_na_start) == ESP_OK)
    // {
    //     ESP_LOGI("RTC_TEST", "Prawidlowy czas (31.10.2026 10:44) zapisany do pamieci RTC!");
    // }
    // else
    // {
    //     ESP_LOGE("RTC_TEST", "Blad zapisu czasu!");
    // }
    // ---------------------------------------------------------------------

    // =========================================================================

    //============================================================================================================
    uint16_t touchX = 0;
    uint16_t touchY = 0;

    // 1. CZYSZCZENIE EKRANU W PSRAM -> CZERWONY

    //============================================================================================================
    //============================================================================================================
    //============================================================================================================
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    time_t now_seconds;
    struct tm time_info;
    char str_godzina[4];
    char str_minuta[4];
    char str_sekunda[4];

    // Pętla główna Double Bufferingu
    while (1)
    {
        draw_ptr = fbs[back_buffer_idx];
        time(&now_seconds);
        localtime_r(&now_seconds, &time_info);

        // Bezpieczne formatowanie z kontrolą rozmiaru bufora (snprintf)
        snprintf(str_godzina, sizeof(str_godzina), "%02d", time_info.tm_hour);
        snprintf(str_minuta, sizeof(str_minuta), "%02d", time_info.tm_min);
        snprintf(str_sekunda, sizeof(str_sekunda), "%02d", time_info.tm_sec);

        // 2. Czyszczenie bufora roboczego na czarno
        safe_clear_screen(draw_ptr, 0x0000);

        // 3. Rysowanie cyfr zegara i kwadracików dwukropka bezpośrednio na draw_ptr
        draw_clock_perfect_monospace_90(draw_ptr, LCD_H_RES, LCD_V_RES, digital_7_ttf, str_godzina, 0, 0, 370.0f, 0xFFFF);
        draw_clock_perfect_monospace_90(draw_ptr, LCD_H_RES, LCD_V_RES, digital_7_ttf, str_minuta, 680, 0, 370.0f, 0xFFFF);
        draw_clock_perfect_monospace_90(draw_ptr, LCD_H_RES, LCD_V_RES, digital_7_ttf, str_sekunda, 140, 610, 100.0f, 0xFFFF);
        // draw_text_90(draw_ptr, LCD_H_RES, LCD_V_RES, str_sekunda, 140, 610, 100.0f, 0xFFFF);

        draw_rectangle_90(draw_ptr, LCD_H_RES, LCD_V_RES, 605, 138, 50, 50, 0xFFFF);
        draw_rectangle_90(draw_ptr, LCD_H_RES, LCD_V_RES, 605, 324, 50, 50, 0xFFFF);

        // Odświeżanie i rysowanie elementów na draw_ptr...
        // Synchronizacja cache, semafor VSYNC oraz wywołanie lcd.lcd_draw_bitmap
        esp_cache_msync((void *)draw_ptr, buffer_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        xSemaphoreTake(vsync_sem, portMAX_DELAY);
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)draw_ptr);

        back_buffer_idx = 1 - back_buffer_idx;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}