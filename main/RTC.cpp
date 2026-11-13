//=====================================================================================================
#include "rtc.hpp"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <esp_timer.h>

// Naglowki FreeRTOS oraz ESP-IDF
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "driver/ledc.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"
#include "esp_jpeg_dec.h"

// Naglowki Twojego podsystemu graficznego i sprzetu
#include "renderer.hpp"
#include "fonty.h"
#include "jd9365_lcd.h"
#include "gsl3680_touch.h"
#include "pins_config.h"
#include "rx8025t.h"
#include "wifi_ntp.hpp" // Potrzebne do synchronizacji NTP

#include "vectordotsball.hpp"
//=====================================================================================================

//=====================================================================================================
// Definicje dla kanału PWM
#define BACKLIGHT_GPIO 23
#define BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_0
#define BACKLIGHT_LEDC_DUTY_RES LEDC_TIMER_10_BIT // Rozdzielczość 10-bitowa (0 - 1023)
#define BACKLIGHT_LEDC_FREQUENCY 5000             // Częstotliwość 5 kHz
//=====================================================================================================
// Deklaracje obiektow i buforow zdefiniowanych w main.cpp
extern jd9365_lcd lcd;

// ZAMYKAMY DEKLARACJĘ DOTYKU W BLOKU ZGODNOŚCI "C":
extern gsl3680_touch touch;

extern uint16_t *fbs[1]; // Bufory ramki (framebuffer) dla wyświetlacza

extern SemaphoreHandle_t vsync_sem;
// Wskaźnik (uchwyt) do zadania gaszenia dwukropka
TaskHandle_t hTaskDisableColon = NULL;
TaskHandle_t hTaskTouch = NULL; // Uchwyt do zadania dotyku
TaskHandle_t hTaskMenuEngine = NULL;
TaskHandle_t hTaskHardwareTouchDriver = NULL;

static const size_t BUFFER_SIZE = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);

// Zmienne statyczne do obsługi czasu POSIX
static struct tm dane_czasu;
static time_t sekundy_epoch;

// Bufory tekstowe na sformatowane cyfry
static char txt_godz[4];
static char txt_min[4];
static char txt_sek[4];

volatile bool is_editing_time = false;

volatile uint8_t BRIGHTNESS = 50; // Zakres 0 - 100
volatile uint32_t g_my_4h_counter = 0;
volatile bool g_trigger_auto_photo = false;
volatile int PHOTO_NR = 0;    // Zmienna globalna do przechowywania numeru aktualnego zdjęcia (1 lub 2)
static int old_seconds = 111; // Zmienna do filtrowania zbocza sekundy (Edge Detection)

SemaphoreHandle_t fb_mutex = NULL;
volatile uint16_t global_touchX = 0;
volatile uint16_t global_touchY = 0;
volatile bool global_is_touched = false;
volatile bool ntp_read = false;
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// ==========================================
// CZĘŚĆ 2 Z 3: ZADANIE FREE_RTOS I INICJALIZACJA
// ==========================================
void vTaskDisableColon(void *pvParameters)
{
    // uint64_t rtc_start = 0;
    // uint64_t rtc_end = 0;
    // 3. Definiujemy bufory w sposób absolutny i bezpieczny:
    uint16_t *back_buffer = (uint16_t *)fbs[0]; // wolny bufor tła
    while (1)
    {
        // 1. Czekamy na sygnał wybudzenia z funkcji print_RTC()
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (is_editing_time)
        {
            continue;
        } // Jeśli edytujemy , nie ruszaj bufora zegara!
        // 2. Odliczamy dokładnie 500 milisekund (dwukropek świeci)
        vTaskDelay(pdMS_TO_TICKS(500));
        // Drugie sprawdzenie flagi is_editing_time po upływie 500ms
        if (is_editing_time)
        {
            continue;
        }

        // rtc_start = esp_timer_get_time();
        xSemaphoreTake(fb_mutex, portMAX_DELAY);
        // 3. Od razu na nim GASIMY kwadraciki dwukropka
        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 605, 104, 50, 50, 0x0000);
        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 605, 300, 50, 50, 0x0000);
        esp_cache_msync((void *)back_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        xSemaphoreTake(vsync_sem, portMAX_DELAY);

        // 7. Wysłanie klatki bez dwukropka na ekran przez DMA
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)back_buffer);
        xSemaphoreGive(fb_mutex);
        // rtc_end = esp_timer_get_time();
        // printf("[PROFILER] RENDER COLON (vTaskDisableColon) zajął: %llu us\n", (rtc_end - rtc_start));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskHardwareTouchDriver(void *pvParameters)
{
    printf("[DRIVER] Niskopoziomowy wątek sprzętowy I2C (vTaskHardwareTouchDriver) uruchomiony.\n");

    while (1)
    {
        uint16_t tx = 0;
        uint16_t ty = 0;

        // Bezpośrednie próbkowanie kontrolera dotyku na szynie I2C
        if (touch.getTouch(&tx, &ty))
        {
            global_touchX = tx;
            global_touchY = ty;
            global_is_touched = true;
        }
        else
        {
            global_touchX = 0;
            global_touchY = 0;
            global_is_touched = false;
        }

        // Próbkowanie 50Hz (co 20ms) - idealne dla płynnego interfejsu
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskTouch(void *pvParameters)
{
    uint16_t touchX = 0;
    uint16_t touchY = 0;

    printf("[LOG] Wielowątkowe zadanie dotyku vTaskTouch ruszyło z prawidłowym mapowaniem rotacji!\n");

    while (1)
    {
        if (is_editing_time)
        {
            vTaskDelay(pdMS_TO_TICKS(50)); // Śpij i oddaj procesor innym zadaniom
            continue;                      // Przeskocz resztę pętli, nie sprawdzaj ekranu głównego
        }
        bool touched = touch.getTouch(&touchX, &touchY);

        if (touched)
        {
            printf("[TOUCH] Odczyt fizyczny: X = %d, Y = %d\n", touchX, touchY);

            // 1. HIT-BOX: SEKCJA GODZIN (Góra-Lewo)
            if (touchX >= 350 && touchX <= 799)
            {
                if (touchY >= 1 && touchY <= 600)
                {
                    printf("[MENU] -> Wykryto kliknięcie w SEKCJĘ GODZIN!\n");
                    if (hTaskMenuEngine != NULL)
                    {
                        is_editing_time = true;
                        // Dla godzin wysyłamy wartość 1
                        xTaskNotify(hTaskMenuEngine, 2, eSetValueWithOverwrite); // Budzimy silnik menu
                        vTaskSuspend(NULL);                                      // <<< PEWNE SAMOZAWIESZENIE WĄTKU DOTYKU!
                    }
                }
                // 2. HIT-BOX: SEKCJA MINUT (Góra-Prawo) - Łączymy elegancko przez else if na osi Y!
                else if (touchY >= 680 && touchY <= 1279)
                {
                    printf("[MENU] -> vTaskTouch budzi MenuEngine i sam się zawiesza...\n");
                    if (hTaskMenuEngine != NULL)
                    {
                        is_editing_time = true;
                        // Dla godzin wysyłamy wartość 1
                        xTaskNotify(hTaskMenuEngine, 1, eSetValueWithOverwrite); // Budzimy silnik menu
                        vTaskSuspend(NULL);                                      // <<< PEWNE SAMOZAWIESZENIE WĄTKU DOTYKU!
                    }
                }
            }

            // 5. HIT-BOX: WEJŚCIE DO MENU JASNOŚCI (Osobny, czysty warunek)
            if (touchX >= 1 && touchX <= 120 && touchY >= 880 && touchY <= 1279)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w strefę regulacji jasności!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 3, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }

            // 4. HIT-BOX: BŁYSKAWICZNE ZEROWANIE SEKUND (Dodatkowy przycisk na ekranie głównym)
            if (touchX >= 190 && touchX <= 300 && touchY >= 20 && touchY <= 240)
            {
                printf("[TOUCH] -> Wyzwalanie zerowania sekund z ekranu głównego!\n");

                // Wywołujemy funkcję zerującą rejestr sprzętowy i synchronizującą ESP32P4
                reset_seconds_to_zero();

                // Wymuszamy natychmiastowe przerysowanie głównego zegara w kolejnym obiegu
                old_seconds = 111;

                // Anty-rebound (Debouncing) dla przycisku resetu
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            // SEKCJA ROKU
            if (touchX >= 1 && touchX <= 120 && touchY >= 1 && touchY <= 300)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE ROKU!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 6, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }
            // SEKCJA MIESIĘCY
            if (touchX >= 1 && touchX <= 120 && touchY >= 400 && touchY <= 560)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE MIESIĘCY!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 5, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }
            // SEKCJA DNI
            if (touchX >= 1 && touchX <= 120 && touchY >= 570 && touchY <= 870)
            {
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE DNI!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 4, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }
            // --- NTP --- HIT-BOX: WEJŚCIE DO MENU RĘCZNEJ SYNCHRONIZACJI NTP (Pasek stanu na dole) ---
            if (touchX >= 150 && touchX <= 260 && touchY >= 400 && touchY <= 1279)
            {
                printf("[TOUCH] -> Kliknięcie w pasek stanu! Uruchamiam Menu NTP...\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 7, eSetValueWithOverwrite); // Wybudzamy MenuEngine wartością 7
                    vTaskSuspend(NULL);                                      // Samozawieszenie wątku dotyku
                }
                old_seconds = 111;
            }

            // Sprzętowy anty-rebound (Debouncing)
            vTaskDelay(pdMS_TO_TICKS(250));
        }

        // synchronixacja RTC z RX8025T co 4 godziny (g_my_4h_counter jest inkrementowany w vTaskFourHourIncrementer)
        if (g_my_4h_counter != 0)
        {
            sync_internal_rtc_with_rx8025t();

            old_seconds = 111;
            g_my_4h_counter = 0;
            printf("[RESYNC] -> Zsynchronizowano RTC z RX8025T...\n");
        }

        // Wyzwalanie pokazu zdjęcia co 5 minut (g_trigger_auto_photo jest ustawiany w vTaskAutoPhotoTimer)
        if (g_trigger_auto_photo == true)
        {
            printf("[TOUCH] Przechwycono sygnal auto-foto! Blokuje czas i skacze do wyswietlarki...\n");
            is_editing_time = true;

            if (PHOTO_NR == 0)
            {
                wyswietl_jpeg_ze_spiffs("/spiffs/FOTO1.jpg");
            }
            else
            {
                wyswietl_jpeg_ze_spiffs("/spiffs/FOTO2.jpg");
            }
            vTaskDelay(pdMS_TO_TICKS(12 * 1000)); // 12 sec.

            for (int t = BRIGHTNESS; t >= 0; t--)
            {
                set_brightness(uint8_t(t));
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            xSemaphoreTake(fb_mutex, portMAX_DELAY);
            safe_clear_screen((uint16_t *)fbs[0], 0x0000);
            esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
            xSemaphoreGive(fb_mutex);

            old_seconds = 111;
            PHOTO_NR = PHOTO_NR ^ 1; // Przełączanie między FOTO1 a FOTO2 (0 <-> 1)
            g_trigger_auto_photo = false;
            is_editing_time = false;
            printf("[TOUCH] Pokaz zakonczony, wracam do zegara glownego.\n");
            vTaskDelay(pdMS_TO_TICKS(250));

            for (int t = 0; t <= BRIGHTNESS; t++)
            {
                set_brightness((uint8_t)t);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Funkcja wywoływana raz w app_main() w celu rejestracji zadań w jądrze FreeRTOS
void init_RTC_subsystem(void)
{
    char test_ssid[33] = {0};
    char test_pass[65] = {0};
    // Tworzenie wątku na rdzeniu 1 (peryferyjnym)
    xTaskCreatePinnedToCore(vTaskHardwareTouchDriver, "vTaskHwTouch", 4096, NULL, 3, &hTaskHardwareTouchDriver, 1);
    // Usypiamy go na starcie - obudzą go dopiero procedury menu jasności lub NTP
    if (hTaskHardwareTouchDriver != NULL)
    {
        vTaskSuspend(hTaskHardwareTouchDriver);
    }
    // NOWOŚĆ: Montujemy dysk zanim ruszą zadania FreeRTOS
    // tu jest plik z danymi dla WiFi
    init_spiffs_storage();
    // save_wifi_credentials_to_SPIFFS("SSID", "Password"); // Zapisz dane Wi-Fi do SPIFFS (tylko raz, jeśli nie zapisano wcześniej)
    if (load_wifi_credentials_from_SPIFFS(test_ssid, sizeof(test_ssid), test_pass, sizeof(test_pass)))
    {
        printf("[SPIFFS_TEST] SUKCES! Odczytano z SPIFFS -> SSID oraz HASLO");
    }
    else
    {
        printf("[SPIFFS_TEST] BLAD! Brak danych w SPIFFS !\n");
    }

    fb_mutex = xSemaphoreCreateMutex();

    // TESTOWE WYŚWIETLENIE PIERWSZEGO ZDJĘCIA (Zaraz po starcie na chwilę pokaże FOTO1.jpg!)
    // Wygaszamy ekran na start, zanim pokażemy zdjęcie
    set_brightness(0);

    wyswietl_jpeg_ze_spiffs("/spiffs/FOTO1.jpg");
    for (int t = 0; t <= 100; t++)
    {
        set_brightness(uint8_t(t));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(5000)); // Pokazuj zdjęcie przez 3 sekundy, zanim wystartuje główny zegar!

    for (int t = 100; t >= 0; t--)
    {
        set_brightness(uint8_t(t));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    safe_clear_screen(fbs[0], 0x0000);
    esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
    vTaskDelay(pdMS_TO_TICKS(500));
    BRIGHTNESS = load_brightness_from_NVS(); // Wczytanie starej jasności

    xTaskCreate(vTaskAutoPhotoTimer, "vTaskAutoPhoto", 2048, NULL, 2, NULL);
    printf("[LOG] Podsystem RTC: Zadanie auto-foto 5-minutowe zarejestrowane.\n");

    // 1. Rejestracja zadania wygaszania dwukropka (Priorytet 5)
    xTaskCreate(vTaskDisableColon, "TaskDisableColon", 4096, NULL, 5, &hTaskDisableColon);

    // 2. Rejestracja zadania szybkiego skanowania dotyku (Priorytet 4)
    xTaskCreate(vTaskTouch, "vTaskTouch", 4096, NULL, 4, &hTaskTouch);

    xTaskCreate(vTaskMenuEngine, "vTaskMenuEngine", 4096, NULL, 4, &hTaskMenuEngine);

    printf("[LOG] Podsystem RTC: Zadania mrugania i dotyku zostały pomyślnie zarejestrowane.\n");

    // Uruchomienie cyklicznego automatu synchronizacji NTP (raz na dobę / co 4 godziny)
    // Przypinamy go do rdzenia 1 (ekran działa na rdzeniu 0), dajemy 4096 B stosu i priorytet 2
    BaseType_t xReturned4h = xTaskCreatePinnedToCore(
        vTaskFourHourIncrementer, "Task4hNtp", 4096, NULL, 2, NULL, 1);

    if (xReturned4h == pdPASS)
    {
        printf("[LOG] Podsystem RTC: Licznik 4h / 24h NTP został zarejestrowany pomyślnie.\n");
    }
    else
    {
        printf("[ERROR] Błąd uruchomienia vTaskFourHourIncrementer!\n");
    }

    printf("[LOG] Podsystem RTC: Licznik 4h został zarejestrowany.\n");

    print_RTC();
    for (int t = 0; t <= BRIGHTNESS; t++)
    {
        set_brightness(uint8_t(t));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void print_RTC(void)
{
    if (is_editing_time == true)
    {
        return;
    }
    char tekst[32];

    uint16_t *main_buffer = (uint16_t *)fbs[0];

    // uint64_t rtc_start = 0;
    // uint64_t rtc_end = 0;

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
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    char txt_slider_pct[16];
    snprintf(txt_slider_pct, sizeof(txt_slider_pct), "%d%%", BRIGHTNESS);

    snprintf(tekst, sizeof(tekst), "%04d-%02d-%02d", (dane_czasu.tm_year + 1900), (dane_czasu.tm_mon + 1), dane_czasu.tm_mday);
    snprintf(txt_godz, sizeof(txt_godz), "%02d", dane_czasu.tm_hour);
    snprintf(txt_min, sizeof(txt_min), "%02d", dane_czasu.tm_min);
    snprintf(txt_sek, sizeof(txt_sek), "%02d", dane_czasu.tm_sec);

    // 1. Czyszczenie jedynego bufora na czarno
    safe_clear_screen(main_buffer, 0x0000);

    int width_bar = 380 * BRIGHTNESS / 100;
    // 2. Rysowanie bazy zegara (czcionki wektorowe bezpośrednio na nim)
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_godz, 0, 0, 370.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_min, 680, 0, 370.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_sek, 80, 500, 100.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, tekst, 20, 670, 100.0f, 0xFFFF);
    draw_hollow_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 880, 672, 395, 120, 4, 0x07E0);
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 888, 680, width_bar, 104, 0xF100); // 100% = 380 widh
    int week_day = calc_dow((dane_czasu.tm_year + 1900), (dane_czasu.tm_mon + 1), dane_czasu.tm_mday);
    draw_text_90(main_buffer, LCD_H_RES, LCD_V_RES, getDayofweek(week_day), 300, 455, 200.0f, 0xFFFF);
    draw_text_90(main_buffer, LCD_H_RES, LCD_V_RES, txt_slider_pct, 1020, 690, 80.0f, 0xFFFF);

    // 3. Od razu na nim zapalamy kwadraciki dwukropka
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 104, 50, 50, 0xFFFF);
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 300, 50, 50, 0xFFFF);

    if (ntp_read == true)
    {
        draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 1180, 555, 50, 50, 0x07E0);
    }
    else
    {
        draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 1180, 555, 50, 50, 0xF100);
    }

    // 4. Jedno, czyste i pewne czyszczenie Cache dla tego bufora
    esp_cache_msync((void *)main_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // 5. Czekamy na VSYNC i wypychamy DMA na ekran MIPI-DSI

    xSemaphoreTake(vsync_sem, portMAX_DELAY);

    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)main_buffer);
    xSemaphoreGive(fb_mutex);
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
void draw_hollow_rectangle_90(uint16_t *buffer, uint16_t buf_w, uint16_t buf_h,
                              uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                              uint16_t thickness, uint16_t color)
{
    // 1. Górna krawędź ramki
    draw_rectangle_90(buffer, buf_w, buf_h, x, y, w, thickness, color);

    // 2. Dolna krawędź ramki
    draw_rectangle_90(buffer, buf_w, buf_h, x, y + h - thickness, w, thickness, color);

    // 3. Lewa krawędź ramki
    draw_rectangle_90(buffer, buf_w, buf_h, x, y, thickness, h, color);

    // 4. Prawa krawędź ramki
    draw_rectangle_90(buffer, buf_w, buf_h, x + w - thickness, y, thickness, h, color);
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_minutes_menu(void)
{
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t new_min = edit_number_menu("SET MINUTS", 280, t.minute, 0, 59, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.minute = new_min;
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void wait_for_release(void)
{
    // Sprawdzamy stan wątku sprzętowego w tle
    eTaskState state = eTaskGetState(hTaskHardwareTouchDriver);

    // Jeśli wątek sprzętowy NIE jest zawieszony (czyli działa i sam odpytuje I2C)
    if (state != eSuspended && state != eDeleted)
    {
        // Poziom 2: Bezpiecznie czekamy na flagę w pamięci RAM
        while (global_is_touched == true)
        {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
    else
    {
        // Poziom 1: Driver w tle śpi, więc główny wątek może bezpiecznie odpytać I2C
        uint16_t dummyX = 0, dummyY = 0;
        while (touch.getTouch(&dummyX, &dummyY) == true)
        {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_test_cross(void)
{
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    // TYMCZASOWE LINIE POMOCNICZE DO POZYCJONOWANIA (Kolor Czerwony: 0xF800)
    // 1. Linia pionowa dzieląca szerokość (1280) na pół w punkcie X = 640
    draw_line_90(menu_buffer, LCD_H_RES, LCD_V_RES, 640, 0, 799, 0xF800);

    // 2. Linia pozioma dzieląca wysokość (800) na pół w punkcie Y = 400
    // Rysujemy ją jako cienki prostokąt od lewej do prawej krawędzi (od X=0 do 1280)
    draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 0, 400, 1280, 2, 0xF800);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void show_debug_touch(uint16_t touchX, uint16_t touchY)
{
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    char txt_debug_touch[32];
    snprintf(txt_debug_touch, sizeof(txt_debug_touch), "X:%03d Y:%04d", touchX, touchY);
    draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_debug_touch, 8, 8, 30.0f, 0x07E0);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskMenuEngine(void *pvParameters)
{
    uint32_t menu_choice = 0;

    printf("[SYSTEM] Zadanie vTaskMenuEngine gotowe i zablokowane na kolejce powiadomień.\n");
    while (1)
    {
        // Funkcja czeka na powiadomienie i zapisuje przekazaną liczbę do zmiennej
        xTaskNotifyWait(0x00, ULONG_MAX, &menu_choice, portMAX_DELAY);
        xSemaphoreTake(fb_mutex, portMAX_DELAY);
        if (hTaskDisableColon != NULL)
        {
            xTaskNotifyStateClear(hTaskDisableColon);
        }
        if (hTaskDisableColon != NULL)
        {
            vTaskSuspend(hTaskDisableColon);
        }

        // --- TWARDE CZYSZCZENIE SPRZĘTOWEGO POTOKU PRZED MENU ---
        safe_clear_screen((uint16_t *)fbs[0], 0x0000);
        esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        // Czekamy na sprzętowy VSYNC, aby upewnić się, że DMA jest wolne
        xSemaphoreTake(vsync_sem, portMAX_DELAY);
        // Wypychamy fizycznie czysty czarny ekran do kontrolera LCD
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
        xSemaphoreGive(fb_mutex);

        vTaskDelay(pdMS_TO_TICKS(40)); // Dajemy czas na pełne przełączenie ekranu

        if (menu_choice == 1)
        {
            set_minutes_menu();
        }
        else if (menu_choice == 2)
        {
            set_hours_menu();
        }
        else if (menu_choice == 3)
        {
            set_light_menu();
        }
        else if (menu_choice == 4) // NOWOŚĆ: POWIADOMIENIE NR 4
        {
            set_day_menu();
        }
        else if (menu_choice == 5) // NOWOŚĆ: POWIADOMIENIE NR 5
        {
            set_month_menu();
        }
        else if (menu_choice == 6) // USTAWIENIE ROKU
        {
            set_year_menu();
        }
        else if (menu_choice == 7) // NOWOŚĆ: WYWOŁANIE MENU NTP
        {
            set_ntp_and_demo_menu();
        }

        // --- KLUCZOWY FIX PO WCIŚNIĘCIU DONE ---
        // ====================================================================
        // GLOBALNY SPRZĘTOWY RESET POTOKU PO WYJŚCIU Z JAKIEGOKOLWIEK MENU
        // ====================================================================

        // 1. Dajemy chwilę na zakończenie ostatnich transferów DMA z menu
        xSemaphoreTake(fb_mutex, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(20));

        // 2. Czyścimy bufor tła na idealną czerń i synchronizujemy Cache
        safe_clear_screen((uint16_t *)fbs[0], 0x0000);
        esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        // 3. Twardo czekamy na VSYNC i fizycznie zerujemy klatkę na wyświetlaczu
        xSemaphoreTake(vsync_sem, 0);
        xSemaphoreTake(vsync_sem, portMAX_DELAY);
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);

        // 4. Dajemy czas na fizyczne przemiecenie kontrolera i zdejmujemy blokadę czasu
        // 6. Czekamy na kolejne przerwanie VSYNC (potwierdzenie, że DMA skończyło wysyłać powyższą klatkę!)
        xSemaphoreTake(vsync_sem, portMAX_DELAY);
        xSemaphoreGive(fb_mutex);

        old_seconds = 111;
        is_editing_time = false;

        // 5. Oczyszczamy zaległe powiadomienia i budzimy dwukropek w stanie czystym
        if (hTaskDisableColon != NULL)
        {
            uint32_t ulDummy;
            xTaskNotifyStateClear(hTaskDisableColon);
            xTaskNotifyWait(0x00, ULONG_MAX, &ulDummy, 0); // Błyskawiczne opróżnienie kolejki powiadomień bez czekania (tick = 0)
            vTaskResume(hTaskDisableColon);
        }

        // 6. Budzimy skanowanie ekranu głównego zegara
        printf("[ENGINE] Menu zakończone globalnie. Wznawiam vTaskTouch...\n");
        if (hTaskTouch != NULL)
        {
            vTaskResume(hTaskTouch);
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void draw_common_menu_base(void)
{
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    // A. Czyszczenie ekranu na czarno
    safe_clear_screen(menu_buffer, 0x0000);

    // D. Rysowanie elementów interfejsu (Przyciski sterujące: PLUS, MINUS, DONE)
    // Format: draw_hollow_rectangle_90(buffer, buf_w, buf_h, x, y, w, h, grubość, kolor)
    // Rysowanie ramki MINUS (od X=50, Y=220, szerokość=250, wysokość=300)
    draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, "-", 110, 280, 150.0f, 0xFFFF);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 50, 220, 250, 300, 4, 0xFFFF);

    // Rysowanie ramki PLUS (od X=980, Y=220, szerokość=250, wysokość=300)
    draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, "+", 1050, 280, 150.0f, 0xFFFF);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 980, 220, 250, 300, 4, 0xFFFF);

    // Rysowanie ramki DONE (od X=390, Y=600, szerokość=500, wysokość=120)
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "DONE", 500, 580, 150.0f, 0xFFFF);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 390, 600, 500, 120, 4, 0xFFFF);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_hours_menu(void)
{
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t new_h = edit_number_menu("SET HOURS", 300, t.hour, 0, 23, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.hour = new_h;
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void reset_seconds_to_zero(void)
{
    rx8025t_time_t final_time;

    // 1. Pobieramy AKTUALNY pełny stan rejestrów z chipu RX8025T
    if (rx8025t_get_time(&final_time) == ESP_OK)
    {
        // 2. Zerujemy sekundy w strukturze
        final_time.second = 59;

        // 3. Zapisujemy zaktualizowaną strukturę z zerem z powrotem do układu RX8025T
        if (rx8025t_set_time(&final_time) == ESP_OK)
        {
            printf("[RTC] Sekundy wyzerowane w układzie zewnętrznym RX8025T.\n");

            // 4. Synchronizujemy wbudowane, wewnętrzne RTC ESP32P4 z nowymi danymi z kostki
            sync_internal_rtc_with_rx8025t();
            printf("[RTC] Wewnętrzne RTC ESP32P4 zsynchronizowane z nową bazą czasu.\n");
        }
        else
        {
            printf("[ERROR] Nie udało się zapisać wyzerowanych sekund do RX8025T!\n");
        }
    }
    else
    {
        printf("[ERROR] Brak komunikacji z RX8025T przy próbie zerowania sekund!\n");
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_day_menu(void)
{
    wait_for_release();
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t max_d = get_max_days_in_month((t.year < 2000) ? t.year + 2000 : t.year, t.month);
    uint8_t new_d = edit_number_menu("SET DAY", 280, t.day, 1, max_d, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.day = new_d;
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_year_menu(void)
{
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    int start_y = (t.year < 2000) ? t.year + 2000 : t.year;
    int new_y = edit_number_menu("SET YEAR", 280, start_y, 2026, 2099, true);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.year = new_y;
        uint8_t max_d = get_max_days_in_month(t.year, t.month);
        if (t.day > max_d)
        {
            t.day = max_d;
        }
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_month_menu(void)
{
    wait_for_release();
    rx8025t_time_t t;
    if (rx8025t_get_time(&t) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    uint8_t new_m = edit_number_menu("SET MONTH", 280, t.month, 1, 12, false);
    if (rx8025t_get_time(&t) == ESP_OK)
    {
        t.month = new_m;
        uint8_t max_d = get_max_days_in_month((t.year < 2000) ? t.year + 2000 : t.year, t.month);
        if (t.day > max_d)
        {
            t.day = max_d;
        }
        if (rx8025t_set_time(&t) == ESP_OK)
        {
            sync_internal_rtc_with_rx8025t();
        }
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Uniwersalna funkcja do ustawiania jasności (0 - 100%)
void set_backlight_brightness(uint8_t percentage)
{
    if (percentage > 100)
        percentage = 100;
    if (percentage < 5)
        percentage = 5;
    // Przeliczenie procentów (0-100) na wartość 10-bitową (0-1023)
    uint32_t duty = (percentage * 1023) / 100;
    BRIGHTNESS = percentage;

    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);

    // printf("[PWM] Jasność zmieniona na: %d%%\n", percentage);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void reconfig_pin_bl_to_pwm(void)
{
    // 1. Konfiguracja timera LEDC - ZWRÓĆ UWAGĘ NA DOPISANE .deconfigure NA KOŃCU!
    ledc_timer_config_t ledc_timer = {
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
        .timer_num = BACKLIGHT_LEDC_TIMER,
        .freq_hz = BACKLIGHT_LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false // <<< TA LINIA USUNIE WARNING Z LINII 819!
    };
    ledc_timer_config(&ledc_timer);

    // 2. Konfiguracja kanału LEDC - ZWRÓĆ UWAGĘ NA DOPISANE .sleep_mode ORAZ .flags NA KOŃCU!
    ledc_channel_config_t ledc_channel = {
        .gpio_num = BACKLIGHT_GPIO,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel = BACKLIGHT_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BACKLIGHT_LEDC_TIMER,
        .duty = 1023, // Na start ustawiamy 100% jasności (10-bit max)
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD, // <<< TA LINIA USUNIE WARNING O sleep_mode!
        .flags = {.output_invert = 0}                 // <<< TA LINIA USUNIE WARNING O flags!
    };
    ledc_channel_config(&ledc_channel);

    printf("[PWM] Pin 23 został pomyślnie REKONFIGUROWANY na tryb PWM (10-bit, 5kHz).\n");
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Definicja globalnej zmiennej dla jasności (zakres 2 - 100)

void set_light_menu(void)
{
    uint16_t touchX = 0;
    uint16_t touchY = 0;
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    bool content_changed = true;

    safe_clear_screen(menu_buffer, 0x0000);
    esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    // xSemaphoreTake(vsync_sem, portMAX_DELAY);

    // 2. To ZOSTAJE: Napis nagłówkowy menu
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, " SET LIGHT", 300, 20, 150.0f, 0xFFFF);

    // 4. To ZOSTAJE: Przycisk DONE (Ramka i napis) - użytkownik musi jakoś wyjść z menu
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "DONE", 510, 580, 150.0f, 0xFFFF);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 390, 600, 500, 120, 4, 0xFFFF);
    // Rysowanie zewnętrznej ramki suwaka (szerokość 1000, wysokość 120, na Y = 350)
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 132, 342, 1018, 136, 4, 0xFFFF);

    esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);
    vTaskDelay(pdMS_TO_TICKS(10)); // Dajemy czas na fizyczne przemieszczenie kontrolera i zdejmujemy blokadę czasu

    while (1)
    {
        touchX = 0;
        touchY = 0;
        if (!touch.getTouch(&touchX, &touchY))
        {
            touchX = 0;
            touchY = 0;
        }

        if (content_changed == true)
        {
            xSemaphoreTake(fb_mutex, portMAX_DELAY);
            // ==========================================
            // TUTAJ WSTAWIASZ NOWĄ PACZKĘ 1 (RYSOWANIE SUWAKA):
            // ==========================================

            // Wyliczenie szerokości zielonego wypełnienia (1% = 10 pikseli)
            uint16_t len_draw = BRIGHTNESS * 10;

            // Rysowanie zielonego paska jasności wewnątrz ramki
            draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 141, 351, len_draw, 118, 0x07E0);

            // Czyszczenie pozostałej części paska na czarno
            draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 141 + len_draw, 351, 1000 - len_draw, 118, 0x0000);
            // ==========================================
            // 4A. Dynamiczny tekst procentowy na środku suwaka (rysowany PO pasku)
            char txt_slider_pct[16];
            snprintf(txt_slider_pct, sizeof(txt_slider_pct), "%d%%", BRIGHTNESS);
            // Używamy czcionki tekstowej (systemowej) o rozmiarze 100.0f, wyśrodkowanej w pasku
            draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, txt_slider_pct, 600, 360, 100.0f, 0xFFFF);

            // 5. To ZOSTAJE: Rysowanie debugowania pozycji dotyku
            // draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 10, 10, 300, 35, 0x0000);
            // show_debug_touch(touchX, touchY);

            // 6. To ZOSTAJE: Synchronizacja Cache i wypchnięcie klatki przez DMA na MIPI-DSI
            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);

            content_changed = false;
            xSemaphoreGive(fb_mutex);
        }
        // =========================================================================
        // OBSŁUGA DOTYKU I SUWAKA (Kopiuj od tego miejsca aż do samego końca funkcji)
        // =========================================================================
        if (touchX > 0 && touchY > 0)
        {
            if (touchY >= 140 && touchY <= 1140 && touchX >= 347 && touchX <= 473)
            {
                BRIGHTNESS = (touchY - 140) / 10;
                if (BRIGHTNESS > 100)
                    BRIGHTNESS = 100;
                if (BRIGHTNESS < 5)
                    BRIGHTNESS = 5;
                set_backlight_brightness(BRIGHTNESS);
                content_changed = true;
                // printf("[SLIDER] Jasnosc touchY: %d -> %d%%\n", touchY, BRIGHTNESS);
            }
            else if (touchX >= 80 && touchX <= 200 && touchY >= 400 && touchY <= 900)
            {
                save_brightness_to_NVS(BRIGHTNESS);
                break;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1)); // Standardowe 60Hz, gdy ekran odpoczywa

    } // Koniec pętli: while (1)

    printf("[MENU] Done pressed. Czekam na puszczenie ekranu...\n");
    wait_for_release();
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// TWOJA FUNKCJA Z STM32 ZWRACAJĄCA WSKAŹNIK NA STRINGS DNIA TYGODNIA (ZAKRES OTRZYMANY 1-7)
//=====================================================================================================
const char *getDayofweek(uint8_t number)
{
    if (number == 0)
    {
        number = 1;
    }
    if (number > 7)
    {
        number = 7;
    }

    // TABLICA STATYCZNA CONST DLA BEZPIECZEŃSTWA PAMIĘCI W C++
    static const char *weekday[] = {"MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY", "SUNDAY"};
    return weekday[number - 1];
}

//=====================================================================================================
// TWÓJ SPRAWDZONY ALGORYTM LICZENIA DNIA TYGODNIA (ZWRACA 1 = PONIEDZIAŁEK, ..., 7 = NIEDZIELA)
//=====================================================================================================
int calc_dow(int y, int m, int d)
{
    int result;
    y -= m < 3;
    result = (y + y / 4 - y / 100 + y / 400 + "-bed=pen+mad."[m] + d) % 7;
    if (result == 0)
    {
        result = 7;
    }
    return result;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// BEZPIECZNA INICJALIZACJA PAMIĘCI NVS (WYWOŁYWANA RAZ NA STARCIE PROJEKTU)
//=====================================================================================================
void init_NVS_system(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // Jeśli pamięć jest uszkodzona lub ma złą wersję, czyścimy ją i stawiamy od nowa
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    printf("[NVS] Podsystem pamięci nieulotnej został pomyślnie zainicjalizowany.\n");
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
// ZAPIS WARTOŚCI JASNOŚCI DO PAMIĘCI NIEULOTNEJ NVS
//=====================================================================================================
void save_brightness_to_NVS(uint8_t brightness_val)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Otwieramy przestrzeń nazw "storage" w trybie ZAPIS/ODCZYT
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK)
    {
        // Zapisujemy wartość pod kluczem "bl_light" (max 15 znaków!)
        err = nvs_set_u8(my_handle, "bl_light", brightness_val);
        if (err == ESP_OK)
        {
            // Kluczowe! commit wypycha dane fizycznie z bufora ramu do struktury flash
            err = nvs_commit(my_handle);
            if (err == ESP_OK)
            {
                printf("[NVS] Nowa jasność [%d%%] została trwale zapisana we Flashu.\n", brightness_val);
            }
        }
        // Zamykamy uchwyt, zwalniając zasoby systemowe
        nvs_close(my_handle);
    }
    else
    {
        printf("[NVS_ERROR] Nie udało się otworzyć pamięci do zapisu! Kod: %d\n", err);
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
// ODCZYT JASNOŚCI Z PAMIĘCI NVS (JEŚLI BRAK WPISU, DAJE WARTOŚĆ DOMYŚLNĄ)
//=====================================================================================================
uint8_t load_brightness_from_NVS(void)
{
    nvs_handle_t my_handle;
    esp_err_t err;
    uint8_t saved_brightness = 50; // Domyślna wartość, jeśli pamięć jest jeszcze pusta

    err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err == ESP_OK)
    {
        // Próbujemy odczytać wartość zapisaną pod kluczem "bl_light"
        err = nvs_get_u8(my_handle, "bl_light", &saved_brightness);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            printf("[NVS] Brak zapisu jasności. Ustawiono domyślne: 50%%\n");
            saved_brightness = 50;
        }
        else if (err == ESP_OK)
        {
            printf("[NVS] Odczytano zapisaną jasność z Flash: %d%%\n", saved_brightness);
        }
        nvs_close(my_handle);
    }
    else
    {
        printf("[NVS_ERROR] Nie udało się otworzyć pamięci do odczytu. Domyślnie: 50%%\n");
    }
    return saved_brightness;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// ZWRACA MAKSYMALNĄ LICZBĘ DNI DLA PODANEGO MIESIĄCA I ROKU (UWZGLĘDNIA LATA PRZESTĘPNE)
//=====================================================================================================
uint8_t get_max_days_in_month(int year, uint8_t month)
{
    if (month == 2)
    {
        // Rok przestępny: podzielny przez 4, ale nie przez 100, chyba że podzielny przez 400
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))
        {
            return 29;
        }
        return 28;
    }
    // Kwiecień, Czerwiec, Wrzesień, Listopad mają 30 dni
    if (month == 4 || month == 6 || month == 9 || month == 11)
    {
        return 30;
    }
    return 31;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void init_spiffs_storage(void)
{
    printf("[SPIFFS] Inicjalizacja i montowanie partycji 'storage'...\n");

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",        // Ścieżka dostępu w kodzie: /spiffs/
        .partition_label = "storage",  // Musi pasować do partitions.csv
        .max_files = 5,                // Maksymalna liczba otwartych plików
        .format_if_mount_failed = true // Sformatuje przy pierwszym starcie
    };

    // Rejestracja dysku w wirtualnym systemie plików
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            printf("[SPIFFS_ERROR] Nie udalo sie zamontowac ani sformatowac systemu plikow!\n");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            printf("[SPIFFS_ERROR] Nie znaleziono partycji o etykiecie 'storage'!\n");
        }
        else
        {
            printf("[SPIFFS_ERROR] Blad inicjalizacji SPIFFS (%s)\n", esp_err_to_name(ret));
        }
        return;
    }

    // Pobranie informacji o wielkości dysku
    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK)
    {
        printf("[SPIFFS_SUCCESS] Partycja zamontowana! Rozmiar: %d KB, Uzyto: %d KB\n", total / 1024, used / 1024);

        // TWARDE I JAWNE SPRAWDZENIE TWOICH ZDJĘĆ Z FOLDERU FOTOS
        FILE *f1 = fopen("/spiffs/FOTO1.jpg", "r");
        FILE *f2 = fopen("/spiffs/FOTO2.jpg", "r");

        if (f1 != NULL)
        {
            printf("[SPIFFS_TEST] SUKCES: Wykryto plik -> /spiffs/FOTO1.jpg\n");
            fclose(f1);
        }
        else
        {
            printf("[SPIFFS_TEST] BLAD: Brak pliku FOTO1.jpg na partycji Flash!\n");
        }

        if (f2 != NULL)
        {
            printf("[SPIFFS_TEST] SUKCES: Wykryto plik -> /spiffs/FOTO2.jpg\n");
            fclose(f2);
        }
        else
        {
            printf("[SPIFFS_TEST] BLAD: Brak pliku FOTO2.jpg na partycji Flash!\n");
        }
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskAutoPhotoTimer(void *pvParameters)
{
    printf("[SYSTEM] Zadanie auto-foto co 5 minut wystartovalo.\n");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
        g_trigger_auto_photo = true;
        printf("[TIMER] Minelo 5 minut! Wyzwalam pokaz zdjecia.\n");
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
int edit_number_menu(const char *title, int title_x, int start_val, int min_val, int max_val, bool is_year)
{
    uint16_t old_touchX = 9999;
    uint16_t old_touchY = 9999;

    uint16_t touchX = 0, touchY = 0;
    int hold_counter = 0, val = start_val;
    uint16_t *menu_buffer = (uint16_t *)fbs[0]; // dodaj gwiazdke przed menu_buffer i nawiasy z zerem przy fbs
    safe_clear_screen(menu_buffer, 0x0000);
    esp_cache_msync(menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bool content_changed = true;

    draw_common_menu_base();
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, title, title_x, 20, 150.0f, 0xFFFF);
    global_is_touched = false;
    global_touchX = 0;
    global_touchY = 0;
    vTaskResume(hTaskHardwareTouchDriver);

    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(100)); // Dajemy czas na fizyczne przemieszczenie kontrolera i zdejmujemy blokadę czasu

    while (is_editing_time)
    {
        touchX = global_touchX;
        touchY = global_touchY;

        if (content_changed)
        {
            char txt_val[16];
            snprintf(txt_val, sizeof(txt_val), is_year ? "%04d" : "%02d", val);
            draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 305, 200, 674, 350, 0x0000);
            draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_val, is_year ? 350 : 470, 250, is_year ? 180.0f : 220.0f, 0xFFFF);
            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
            content_changed = false;
        }

        if (touchX > 0 && touchY > 0)
        {
            // MINUS button pressed
            // if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 && (hold_counter == 1 || (hold_counter > 25 && hold_counter % 3 == 0)))
            {
                val = (val <= min_val) ? max_val : val - 1;
                content_changed = true;
            }
            // PLUS button pressed
            // else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 && (hold_counter == 1 || (hold_counter > 25 && hold_counter % 3 == 0)))
            {
                val = (val >= max_val) ? min_val : val + 1;
                content_changed = true;
            }
            // DONE button pressed
            else if (touchX >= 50 && touchX <= 250 && touchY >= 450 && touchY <= 850)
            {
                vTaskSuspend(hTaskHardwareTouchDriver);
                break;
            }

            hold_counter++;
        }
        else
        {
            hold_counter = 0;
        }

        // =========================================================================
        // NIEZALEŻNE ODŚWIEŻANIE DEBUGERA DOTYKU (BEZ ZAKŁÓCANIA REPETYCJI!)
        // =========================================================================
        if (touchX > 0 && touchY > 0)
        {
            // Odświeżamy obraz na LCD TYLKO wtedy, gdy palec fizycznie zmienił pozycję
            if (touchX != old_touchX || touchY != old_touchY)
            {
                old_touchX = touchX;
                old_touchY = touchY;

                // Rysujemy debuger dotyku (pole o szerokości 300 idealnie mieści się przed ramką minusa)
                // draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 10, 10, 300, 35, 0x0000);
                // show_debug_touch(touchX, touchY);

                // Wypychamy na LCD wyłącznie gdy stan cyfr nie wymusił odświeżenia klatki
                if (!content_changed)
                {
                    esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                    xSemaphoreTake(vsync_sem, portMAX_DELAY);
                    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
    vTaskSuspend(hTaskHardwareTouchDriver);
    wait_for_release();
    return val;
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void wyswietl_jpeg_ze_spiffs(const char *sciezka_pliku)
{
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    printf("[JPEG] Odczyt pliku: %s\n", sciezka_pliku);
    FILE *f = fopen(sciezka_pliku, "rb");
    if (f == NULL)
    {
        printf("[JPEG_ERROR] Nie mozna otworzyc pliku: %s\n", sciezka_pliku);
        xSemaphoreGive(fb_mutex);
        return;
    }

    // 1. Sprawdzamy rozmiar pliku JPEG
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    const size_t screen_bytes = LCD_H_RES * LCD_V_RES * 2;  // Pierwsze 2 MB: wyjscie RGB565
    const size_t max_jpg_bytes = LCD_H_RES * LCD_V_RES * 1; // Trzeci 1 MB: bufor na plik .jpg

    if (file_size <= 0 || (size_t)file_size > max_jpg_bytes)
    {
        printf("[JPEG_ERROR] Plik za duzy (%ld B) lub pusty! Limit to 1 MB.\n", file_size);
        fclose(f);
        xSemaphoreGive(fb_mutex);
        return;
    }

    // 2. Wczytujemy plik .jpg dokladnie na poczatek trzeciego megabajta (offset +2 MB)
    uint8_t *base_ptr = (uint8_t *)fbs[0];
    uint8_t *jpeg_input_ptr = base_ptr + screen_bytes;

    size_t read_bytes = fread(jpeg_input_ptr, 1, file_size, f);
    fclose(f);

    if (read_bytes != (size_t)file_size)
    {
        printf("[JPEG_ERROR] Blad odczytu pliku ze SPIFFS!\n");
        xSemaphoreGive(fb_mutex);
        return;
    }

    // 3. Konfiguracja dekodera esp_new_jpeg (bezposrednio do RGB565_LE)
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;

    jpeg_dec_handle_t jpeg_dec = NULL;
    if (jpeg_dec_open(&config, &jpeg_dec) != JPEG_ERR_OK)
    {
        printf("[JPEG_ERROR] Nie udalo sie zainicjalizowac dekodera JPEG!\n");
        xSemaphoreGive(fb_mutex);
        return;
    }

    jpeg_dec_io_t jpeg_io = {};
    jpeg_io.inbuf = jpeg_input_ptr;
    jpeg_io.inbuf_len = file_size;

    jpeg_dec_header_info_t out_info = {};
    if (jpeg_dec_parse_header(jpeg_dec, &jpeg_io, &out_info) != JPEG_ERR_OK)
    {
        printf("[JPEG_ERROR] Nieprawidlowy naglowek pliku JPEG!\n");
        jpeg_dec_close(jpeg_dec);
        xSemaphoreGive(fb_mutex);
        return;
    }

    // Dekodujemy z 3. megabajta prosto na pierwsze 2 MB bufora fbs[0]
    jpeg_io.outbuf = base_ptr;

    if (jpeg_dec_process(jpeg_dec, &jpeg_io) != JPEG_ERR_OK)
    {
        printf("[JPEG_ERROR] Blad podczas dekompresji obrazu JPEG!\n");
        jpeg_dec_close(jpeg_dec);
        xSemaphoreGive(fb_mutex);
        return;
    }

    jpeg_dec_close(jpeg_dec);

    // 4. Synchronizacja Cache dla 2 MB obrazu i wyslanie na ekran przez DMA
    esp_cache_msync((void *)fbs[0], screen_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);

    printf("[JPEG_SUCCESS] Obraz %dx%d zdekompresowany i wyslany na LCD!\n", out_info.width, out_info.height);
    xSemaphoreGive(fb_mutex);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// JEDNORAZOWY ZAPIS SSID I HASLA DO PAMIECI NIEULOTNEJ SPIFFS
bool save_wifi_credentials_to_SPIFFS(const char *ssid, const char *password)
{
    FILE *f = fopen("/spiffs/prefs.ini", "w");
    if (f == NULL)
    {
        printf("[SPIFFS_WIFI_ERROR] Nie mozna otworzyc pliku /spiffs/prefs.ini do zapisu!\n");
        return false;
    }

    // Zapisujemy SSID w pierwszej linii oraz hasło w drugiej linii
    fprintf(f, "%s\n%s\n", ssid, password);
    fclose(f); // Zawsze zamykamy plik, aby fizycznie zrzucić dane z bufora do Flash!

    printf("[SPIFFS_WIFI] Zapisano dane Wi-Fi do /spiffs/prefs.ini (SSID: %s)\n", ssid);
    return true;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// ODCZYT SSID I HASLA Z PLIKU /spiffs/prefs.ini
bool load_wifi_credentials_from_SPIFFS(char *ssid_out, size_t ssid_max_len, char *pass_out, size_t pass_max_len)
{
    FILE *f = fopen("/spiffs/prefs.ini", "r");
    if (f == NULL)
    {
        printf("[SPIFFS_WIFI] Brak pliku /spiffs/prefs.ini!\n");
        return false;
    }

    // Czytamy 1. linię (SSID) oraz 2. linię (Hasło)
    bool ok1 = (fgets(ssid_out, ssid_max_len, f) != NULL);
    bool ok2 = (fgets(pass_out, pass_max_len, f) != NULL);
    fclose(f);

    if (!ok1 || !ok2)
    {
        printf("[SPIFFS_WIFI] Plik /spiffs/prefs.ini jest pusty lub niekompletny!\n");
        return false;
    }

    // Bezpieczne obcięcie znaków nowej linii (\r oraz \n) z Windowsa i Linuxa
    ssid_out[strcspn(ssid_out, "\r\n")] = '\0';
    pass_out[strcspn(pass_out, "\r\n")] = '\0';

    if (strlen(ssid_out) == 0)
    {
        printf("[SPIFFS_WIFI] Pusty SSID w pliku prefs.ini!\n");
        return false;
    }

    printf("[SPIFFS_WIFI] Odczytano SSID z SPIFFS: %s\n", ssid_out);
    return true;
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void print_internal_ram_stats(const char *etap)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    printf("[SRAM_STAT - %s] Wolne teraz: %u KB (%u B) | Najmniej w historii: %u KB | Najwiekszy blok: %u KB\n",
           etap,
           free_internal / 1024,
           free_internal,
           min_free_internal / 1024,
           largest_block / 1024);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskFourHourIncrementer(void *pvParameters)
{
    printf("[SYSTEM] Zadanie cykliczne NTP (24h / 4h) wystartowało.\n");

    // Licznik wybudzeń (6 wywołań * 4 godziny = 24 godziny)
    static int licznik_4h = 0;
    static bool ostatnia_sync_udana = false;

    // Po restarcie urządzenia wykonujemy pierwszą natychmiastową próbę synchronizacji
    start_ntp_sync_task_async();

    while (1)
    {
        // Zadanie śpi przez dokładnie 4 godziny
        vTaskDelay(pdMS_TO_TICKS(4 * 60 * 60 * 1000));

        licznik_4h++;

        // Flaga sprawdzająca, czy to czas na dobową synchronizację
        bool czas_na_dobowa_sync = (licznik_4h >= 6);

        // Jeśli minęła doba LUB poprzednia próba zakończyła się błędem:
        if (czas_na_dobowa_sync || !ostatnia_sync_udana)
        {
            printf("[HARMONOGRAM] Uruchamiam cykliczne zadanie NTP (Czas na dobową: %d, Stan poprzedniej: %d)\n",
                   czas_na_dobowa_sync, ostatnia_sync_udana);

            if (!is_ntp_sync_running())
            {
                start_ntp_sync_task_async();
                while (is_ntp_sync_running())
                {
                    vTaskDelay(pdMS_TO_TICKS(200));
                }

                time_t teraz;
                struct tm info;
                time(&teraz);
                localtime_r(&teraz, &info);

                if (ntp_read == true)
                {
                    ostatnia_sync_udana = true;
                    licznik_4h = 0; // Sukces! Resetujemy licznik dobowy
                    printf("[HARMONOGRAM] Cykliczna synchronizacja udana. Następna próba za 24h.\n");
                }
                else
                {
                    ostatnia_sync_udana = false;
                    printf("[HARMONOGRAM] Cykliczna synchronizacja nieudana! Ponowienie nastąpi za 4 godziny.\n");
                }
            }
        }
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_ntp_menu(void)
{
    uint16_t *back_buffer = (uint16_t *)fbs[0]; // wolny bufor ekranu do rysowania
    printf("[MENU] -> Uruchomiono graficzne Menu synchronizacji NTP.\n");
    wait_for_release();            // Czekamy na puszczenie ekranu przed wejściem do menu, aby uniknąć natychmiastowego wyzwolenia przycisku
    vTaskDelay(pdMS_TO_TICKS(50)); // Dajemy czas na fizyczne przemieszczenie kontrolera i zdejmujemy blokadę czasu

    // SYNCHRONIZACJA Z DMA
    xSemaphoreTake(vsync_sem, 0);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    // Czyszczenie i przygotowanie czystego bufora ekranu
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    memset(fbs[0], 0x00, 800 * 1280 * 2); // Czyszczenie na czarno (RGB565)

    // =================================================================================
    // 1. PRZYCISK "START NTP" (WYŻEJ NA OSI Y)
    // =================================================================================
    // Pozycja: X=390, Y=300, Szerokość=500, Wysokość=120, grubość=4, kolor=zielony
    draw_hollow_rectangle_90(fbs[0], LCD_H_RES, LCD_V_RES, 390, 300, 500, 135, 4, 0x07E0);
    draw_text_90(fbs[0], LCD_H_RES, LCD_V_RES, "START NTP", 430, 310, 100, 0x07E0);

    // 4. To ZOSTAJE: Przycisk DONE (Ramka i napis) - użytkownik musi jakoś wyjść z menu
    draw_text_90(back_buffer, LCD_H_RES, LCD_V_RES, "DONE", 510, 580, 150, 0xFFFF);
    draw_hollow_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 390, 600, 500, 120, 4, 0xFFFF);

    esp_cache_msync(fbs[0], 800 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
    xSemaphoreGive(fb_mutex);
    // vTaskResume(hTaskTouch); // Budzimy dotyk dedykowany dla tego menu

    // 👇 WYBUDZENIE PRZED PĘTLĄ WHILE 👇
    if (hTaskHardwareTouchDriver != NULL)
    {
        vTaskResume(hTaskHardwareTouchDriver);
    }

    bool exit_menu = false;
    while (!exit_menu)
    {
        // Sprawdzamy stan z niskopoziomowego wątku sprzętowego
        if (global_is_touched)
        {
            uint16_t tx = global_touchX;
            uint16_t ty = global_touchY;

            // --- DEBUGER DOTYKU NA EKRANIE ---
            // Czyścimy mały pasek pod cyframi w lewym rogu i rysujemy koordynaty
            // draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 10, 10, 300, 35, 0x0000);
            // show_debug_touch(tx, ty);

            // 1. HIT-BOX dla przycisku START NTP (Ramka Y=300, wysokość 120 -> ty od ok. 300 do 450)
            if (tx >= 380 && tx <= 500 && ty >= 390 && ty <= 890)
            {
                if (!is_ntp_sync_running())
                {
                    printf("[MENU NTP] -> Ręczne wywołanie synchronizacji czasu...\n");

                    // Czyścimy przestrzeń pomiędzy przyciskami (Y=450 do 580) na czarno pod status
                    draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 0, 440, 1280, 158, 0);
                    draw_text_90(back_buffer, LCD_H_RES, LCD_V_RES, "SYNCHRONIZACJA...", 360, 480, 70.0f, 0xFfe0); // yellow? napis

                    // Wypychamy status na ekran przed uruchomieniem workera
                    esp_cache_msync(fbs[0], 800 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                    xSemaphoreTake(vsync_sem, portMAX_DELAY);
                    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);

                    start_ntp_sync_task_async(); // Start wątku sieciowego

                    // Czekamy na zakończenie pobierania czasu
                    while (is_ntp_sync_running())
                    {
                        vTaskDelay(pdMS_TO_TICKS(50));
                    }
                    if (ntp_read == true)
                    {
                        time_t now = 0;
                        struct tm timeinfo = {};
                        time(&now);
                        localtime_r(&now, &timeinfo);
                        rx8025t_time_t rtc_time;
                        rtc_time.second = timeinfo.tm_sec;
                        rtc_time.minute = timeinfo.tm_min;
                        rtc_time.hour = timeinfo.tm_hour;
                        rtc_time.day = timeinfo.tm_mday;
                        rtc_time.month = timeinfo.tm_mon + 1;
                        rtc_time.year = timeinfo.tm_year + 1900;
                        rtc_time.weekday = timeinfo.tm_wday;

                        char buf[64]; // Bufor na gotowy tekst (64 bajty w zupełności wystarczą)

                        snprintf(buf, sizeof(buf), "SUKCES NTP! %04d-%02d-%02d %02d:%02d:%02d",
                                 rtc_time.year, rtc_time.month, rtc_time.day,
                                 rtc_time.hour, rtc_time.minute, rtc_time.second);
                        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 0, 440, 1280, 158, 0);
                        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 0, 457, 1280, 120, 0x001f);
                        draw_text_90(back_buffer, LCD_H_RES, LCD_V_RES, &buf[0], 140, 480, 70.0f, 0xffff);
                    }
                    else
                    {
                        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 0, 440, 1280, 158, 0);
                        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 200, 457, 850, 120, 0xF100);
                        draw_text_90(back_buffer, LCD_H_RES, LCD_V_RES, "NIEPOWODZENIE--NTP !", 300, 480, 70.0f, 0xFFFF);
                    }
                }
            }

            // 2. HIT-BOX dla przycisku DONE (Ramka Y=600, wysokość 120 -> ty od ok. 600 do 750)
            if (tx >= 80 && tx <= 200 && ty >= 400 && ty <= 900)
            {
                printf("[MENU NTP] -> Kliknięto DONE. Powrót do zegara.\n");
                wait_for_release(); // Czekamy na puszczenie ekranu przed wyjściem z menu, aby uniknąć natychmiastowego wyzwolenia przycisku
                vTaskDelay(pdMS_TO_TICKS(150));
                exit_menu = true;
            }

            // Synchronizacja klatki po dotknięciu (odświeża suwak i współrzędne debugowania)
            esp_cache_msync(fbs[0], 800 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
        }
        vTaskDelay(pdMS_TO_TICKS(15)); // Częstotliwość odświeżania pętli
    }

    vTaskSuspend(hTaskHardwareTouchDriver);

    wait_for_release();             // Czekamy na puszczenie ekranu przed wejściem do menu, aby uniknąć natychmiastowego wyzwolenia przycisku
    vTaskDelay(pdMS_TO_TICKS(150)); // Dajemy czas na fizyczne przemieszczenie kontrolera i zdejmujemy blokadę czasu

    // Powrót do ekranu głównego
    // is_editing_time = false;
    old_seconds = 111; // Wymuszenie pełnego odświeżenia grafiki ekranu głównego
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void refresh_menu_self(void)
{
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    xSemaphoreTake(fb_mutex, portMAX_DELAY);
    memset(fbs[0], 0x00, 800 * 1280 * 2);
    // Linia pionowa dzielaca ekran na pol przy X=640
    draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 638, 0, 4, 400, 0xFFFF);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 0, 0, 1276, 400, 4, 0xFFFF);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 70, 140, 500, 120, 4, 0x07E0);
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "NTP", 270, 150, 100.0f, 0x07E0);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 710, 140, 500, 120, 4, 0xF100);
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "DEMO", 890, 150, 100.0f, 0xF100);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 390, 600, 500, 120, 4, 0xFFFF);
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "DONE", 510, 580, 150.0f, 0xFFFF);
    esp_cache_msync(fbs[0], 800 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
    xSemaphoreGive(fb_mutex);
    wait_for_release();
}
void set_ntp_and_demo_menu(void)
{
    uint16_t old_tx = 9999, old_ty = 9999;
    bool exit_menu = false;

    refresh_menu_self();

    if (hTaskHardwareTouchDriver != NULL)
    {
        vTaskResume(hTaskHardwareTouchDriver);
    }
    while (!exit_menu)
    {
        uint16_t tx = global_touchX, ty = global_touchY;
        if (global_is_touched && (tx != old_tx || ty != old_ty))
        {
            old_tx = tx;
            old_ty = ty;
            xSemaphoreTake(fb_mutex, portMAX_DELAY);
            // draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 10, 10, 300, 35, 0x0000);
            // show_debug_touch(tx, ty);
            esp_cache_msync(fbs[0], 800 * 1280 * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
            xSemaphoreGive(fb_mutex);
        }
        if (global_is_touched)
        {
            // Obsluga przycisku NTP (lewa gorna cwiartka)
            if (tx >= 540 && tx <= 660 && ty >= 70 && ty <= 570)
            {
                set_ntp_menu();
                vTaskResume(hTaskHardwareTouchDriver);
                vTaskDelay(pdMS_TO_TICKS(150));
                refresh_menu_self(); // Czyste wywołanie odświeżenia
            }

            // Obsluga przycisku DEMO (prawa gorna cwiartka)
            if (tx >= 540 && tx <= 660 && ty >= 680 && ty <= 1279)
            {
                printf("[MENU NTP] -> Uruchamiam Vector Dots Demo...\n");
                run_vector_dots_demo_loop();
            }
            // Obsluga przycisku DONE (wyjscie na dolnym pasku)
            if (tx >= 80 && tx <= 200 && ty >= 400 && ty <= 900)
            {
                printf("[MENU NTP] -> Kliknieto DONE. Powrot.\n");
                exit_menu = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    vTaskSuspend(hTaskHardwareTouchDriver);

    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(150));
    old_seconds = 111;
    // Upewniamy się, że żaden transfer DMA zlecony w menu nie trwa w tle
    xSemaphoreTake(vsync_sem, pdMS_TO_TICKS(50));
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void run_vector_dots_demo_loop(void)
{
    // NEW VECTOR DOTS DEMO
    wait_for_release();
    vTaskDelay(pdMS_TO_TICKS(200));
    start_vectordots_ball();
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_max_briggtness(void)
{
    set_brightness(100);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void restore_brightness(void)
{
    uint32_t duty = 1023 * BRIGHTNESS / 100;
    if (duty > 1023)
    {
        duty = 1023;
    }
    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_brightness(uint8_t dana)
{
    uint32_t duty = 1023 * dana / 100;
    if (duty > 1023)
    {
        duty = 1023;
    }
    ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
    ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
