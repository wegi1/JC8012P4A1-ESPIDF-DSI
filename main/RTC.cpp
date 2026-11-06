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
#include "driver/ledc.h"
#include "esp_private/periph_ctrl.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_spiffs.h"

#include "esp_cache.h"

// Naglowki Twojego podsystemu graficznego i sprzetu
#include "renderer.hpp"
#include "fonty.h"
#include "jd9365_lcd.h"
#include "gsl3680_touch.h"
#include "pins_config.h"
#include "rx8025t.h"
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

extern uint8_t BRIGHTNESS;

extern uint16_t *fbs[2];
extern volatile int back_buffer_idx;
extern SemaphoreHandle_t vsync_sem;
// Wskaźnik (uchwyt) do zadania gaszenia dwukropka
TaskHandle_t hTaskDisableColon = NULL;
TaskHandle_t hTaskTouch = NULL; // Uchwyt do zadania dotyku
TaskHandle_t hTaskMenuEngine = NULL;

static const size_t BUFFER_SIZE = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);

// Zmienne statyczne do obsługi czasu POSIX
static struct tm dane_czasu;
static time_t sekundy_epoch;

// Bufory tekstowe na sformatowane cyfry
static char txt_godz[4];
static char txt_min[4];
static char txt_sek[4];

volatile bool is_editing_time = false;

volatile int current_day = 1;     // Zakres 1 - 31 (zależnie od miesiąca)
volatile int current_month = 1;   // Zakres 1 - 12
volatile int current_year = 2026; // Zakres np. 2026 - 2099

volatile uint8_t current_brightness = 50;
volatile uint32_t g_my_4h_counter = 0;
volatile bool g_trigger_auto_photo = false;
volatile int PHOTO_NR = 0;    // Zmienna globalna do przechowywania numeru aktualnego zdjęcia (1 lub 2)
static int old_seconds = 111; // Zmienna do filtrowania zbocza sekundy (Edge Detection)

// NOWOŚĆ: Te wskaźniki zostaną idealnie wyrównane matematycznie i podane do dekodera
uint8_t *g_statyczny_bufor_rgb888 = NULL;
uint8_t *g_statyczny_bufor_bmp = NULL;
const size_t ROZMIAR_MAX_BMP = 1280 * 800 * 3 + 64;
const size_t ROZMIAR_RGB888 = 1280 * 800 * 3 + 64; // Zapas 64B dla msync

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
        // 3. Od razu na nim GASIMY kwadraciki dwukropka
        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 605, 104, 50, 50, 0x0000);
        draw_rectangle_90(back_buffer, LCD_H_RES, LCD_V_RES, 605, 300, 50, 50, 0x0000);
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
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE ROKU!\n");
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
                printf("[TOUCH] -> Wykryto kliknięcie w USTAWIENIE ROKU!\n");
                if (hTaskMenuEngine != NULL)
                {
                    is_editing_time = true;
                    xTaskNotify(hTaskMenuEngine, 4, eSetValueWithOverwrite); // Wybudzamy silnik wartością 3
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
                wyswietl_bmp_ze_spiffs("/spiffs/FOTO1.bmp");
            }
            else
            {
                wyswietl_bmp_ze_spiffs("/spiffs/FOTO2.bmp");
            }
            old_seconds = 111;
            vTaskDelay(pdMS_TO_TICKS(5 * 1000));

            // Twardy reset bufora ramki na czarno po zakończeniu zdjęcia, aby wyczyścić potok DMA:
            uint16_t *clear_buf = (uint16_t *)fbs[0];
            safe_clear_screen(clear_buf, 0x0000);
            esp_cache_msync((void *)clear_buf, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)clear_buf);

            old_seconds = 111;
            PHOTO_NR = PHOTO_NR ^ 1; // Przełączanie między FOTO1 a FOTO2 (0 <-> 1)
            g_trigger_auto_photo = false;
            is_editing_time = false;
            printf("[TOUCH] Pokaz zakonczony, wracam do zegara glownego.\n");
            vTaskDelay(pdMS_TO_TICKS(500));
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
    // NOWOŚĆ: Montujemy dysk zanim ruszą zadania FreeRTOS
    init_spiffs_storage();
    printf("[SYSTEM] Alokacja statycznych buforów graficznych w PSRAM...\n");

    printf("[SYSTEM] Alokacja statycznych buforów graficznych w PSRAM z wyrownaniem 64B...\n");

    // Zaokrąglamy rozmiary buforów do wielokrotności 64 bajtów dla Cache
    // TO WSTAWIASZ W ZAMIAN:
    g_statyczny_bufor_bmp = (uint8_t *)heap_caps_malloc(ROZMIAR_MAX_BMP, MALLOC_CAP_SPIRAM);
    if (g_statyczny_bufor_bmp == NULL)
    {
        printf("[KRK] BLAD KRYTYCZNY: Brak pamieci w PSRAM dla bufora BMP!\n");
    }
    else
    {
        printf("[SYSTEM_SUCCESS] Zarezerwowano bufor dla podsystemu graficznego BMP.\n");
    }

    // Drukowanie adresów fizycznych buforów w formacie HEX
    printf("[HEX_DIAG] Adres statycznego bufora BMP:    %p\n", (void *)g_statyczny_bufor_bmp);
    printf("[HEX_DIAG] Adres statycznego bufora RGB888: %p\n", (void *)g_statyczny_bufor_rgb888);
    printf("[HEX_DIAG] Adres sprzętowego bufora ekranu fbs[0]: %p\n", (void *)fbs[0]);

    // POWRÓT DO STAREJ, DZIAŁAJĄCEJ METODY 1:1 (Bez aligned!)
    // g_statyczny_bufor_jpg = (uint8_t *)heap_caps_malloc(ROZMIAR_MAX_JPG, MALLOC_CAP_SPIRAM);
    // g_statyczny_bufor_rgb888 = (uint8_t *)heap_caps_malloc(ROZMIAR_RGB888, MALLOC_CAP_SPIRAM);

    if (g_statyczny_bufor_bmp == NULL || g_statyczny_bufor_rgb888 == NULL)
    {
        printf("[KRK] BLAD KRYTYCZNY: Nie udalo sie zarezerwowac pamieci startowej w PSRAM!\n");
    }
    else
    {
        printf("[SYSTEM_SUCCESS] Zarezerwowano 4.07 MB w PSRAM na stale dla podsystemu BMP.\n");
    }

    // TESTOWE WYŚWIETLENIE PIERWSZEGO ZDJĘCIA (Zaraz po starcie na chwilę pokaże FOTO1.bmp!)
    set_backlight_brightness(100);
    wyswietl_bmp_ze_spiffs("/spiffs/FOTO1.bmp");
    vTaskDelay(pdMS_TO_TICKS(3000)); // Pokazuj zdjęcie przez 3 sekundy, zanim wystartuje główny zegar!
    uint32_t duty;
    for (uint8_t t = 100; t > 0; t--)
    {
        duty = (t * 1023) / 100;
        ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, duty);
        ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    safe_clear_screen(fbs[0], 0x0000);
    esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);
    vTaskDelay(pdMS_TO_TICKS(500));
    BRIGHTNESS = load_brightness_from_NVS(); // Wczytanie starej jasności
    set_backlight_brightness(BRIGHTNESS);

    xTaskCreate(vTaskAutoPhotoTimer, "vTaskAutoPhoto", 2048, NULL, 2, NULL);
    printf("[LOG] Podsystem RTC: Zadanie auto-foto 5-minutowe zarejestrowane.\n");

    // 1. Rejestracja zadania wygaszania dwukropka (Priorytet 5)
    xTaskCreate(vTaskDisableColon, "TaskDisableColon", 4096, NULL, 5, &hTaskDisableColon);

    // 2. Rejestracja zadania szybkiego skanowania dotyku (Priorytet 4)
    xTaskCreate(vTaskTouch, "vTaskTouch", 4096, NULL, 4, &hTaskTouch);

    xTaskCreate(vTaskMenuEngine, "vTaskMenuEngine", 4096, NULL, 4, &hTaskMenuEngine);

    printf("[LOG] Podsystem RTC: Zadania mrugania i dotyku zostały pomyślnie zarejestrowane.\n");
    xTaskCreate(vTaskFourHourIncrementer, "vTask4hInc", 2048, NULL, 2, NULL);

    printf("[LOG] Podsystem RTC: Licznik 4h został zarejestrowany.\n");
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
    // TWARDE WYCZYSZCZENIE CACHE PRZED STARTEM RENDERU ZEGARA
    esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

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
    char txt_slider_pct[16];
    snprintf(txt_slider_pct, sizeof(txt_slider_pct), "%d%%", BRIGHTNESS);

    snprintf(tekst, sizeof(tekst), "%04d-%02d-%02d", (dane_czasu.tm_year + 1900), (dane_czasu.tm_mon + 1), dane_czasu.tm_mday);
    snprintf(txt_godz, sizeof(txt_godz), "%02d", dane_czasu.tm_hour);
    snprintf(txt_min, sizeof(txt_min), "%02d", dane_czasu.tm_min);
    snprintf(txt_sek, sizeof(txt_sek), "%02d", dane_czasu.tm_sec);

    // 1. Czyszczenie jedynego bufora na czarno
    safe_clear_screen(main_buffer, 0x0000);
    if (is_editing_time == true)
    {
        return;
    }
    int width_bar = 380 * BRIGHTNESS / 100;
    // 2. Rysowanie bazy zegara (czcionki wektorowe bezpośrednio na nim)
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_godz, 0, 0, 370.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_min, 680, 0, 370.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_sek, 80, 500, 100.0f, 0xFFFF);
    draw_clock_perfect_monospace_90(main_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, tekst, 20, 670, 100.0f, 0xFFFF);
    draw_hollow_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 880, 672, 395, 120, 4, 0x07E0);
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 888, 680, width_bar, 104, 0xF100); // 100% = 380 widh
    int week_day = calc_dow((dane_czasu.tm_year + 1900), (dane_czasu.tm_mon + 1), dane_czasu.tm_mday);
    draw_text_90(main_buffer, LCD_H_RES, LCD_V_RES, getDayofweek(week_day), 350, 455, 200.0f, 0xFFFF);
    draw_text_90(main_buffer, LCD_H_RES, LCD_V_RES, txt_slider_pct, 1020, 690, 80.0f, 0xFFFF);
    if (is_editing_time == true)
    {
        return;
    }
    // 3. Od razu na nim zapalamy kwadraciki dwukropka
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 104, 50, 50, 0xFFFF);
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 300, 50, 50, 0xFFFF);

    // 4. Jedno, czyste i pewne czyszczenie Cache dla tego bufora
    esp_cache_msync((void *)main_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    // 5. Czekamy na VSYNC i wypychamy DMA na ekran MIPI-DSI
    if (is_editing_time == true)
    {
        return;
    }
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    if (is_editing_time == true)
    {
        return;
    }
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
    uint16_t touchX = 0;
    uint16_t touchY = 0;
    int hold_counter = 0;

    printf("[MENU] -> Uruchomiono WIZUALNY tryb edycji MINUT w wątku dotyku!\n");

    // 1. Blokujemy automatyczne odświeżanie głównego zegara
    is_editing_time = true;

    // 2. Pobieramy aktualny czas z zewnętrznej kostki RX8025T jako bazę do edycji
    rx8025t_time_t setup_time;
    if (rx8025t_get_time(&setup_time) != ESP_OK)
    {
        printf("[ERROR] Brak komunikacji z RX8025T!\n");
        is_editing_time = false;
        return;
    }

    uint8_t current_min = setup_time.minute;

    // Pobieramy wskaźnik na roboczy bufor tła
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    safe_clear_screen(menu_buffer, 0x0000);
    esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    bool content_changed = true; // Na starcie wymuszamy pierwsze rysowanie menu
    // GŁÓWNA PĘTLA EDYCJI (Kręci się w wątku vTaskTouch, dopóki is_editing_time == true)
    while (is_editing_time)
    {
        // 1. Zerujemy zmienne przed każdym sprawdzaniem
        touchX = 0;
        touchY = 0;

        // 2. Jeśli funkcja zwróci false (brak fizycznego dotyku),
        // zmienne pozostaną wyzerowane i żaden warunek na dole się nie wyzwoli
        if (!touch.getTouch(&touchX, &touchY))
        {
            touchX = 0;
            touchY = 0;
        }

        // -------------------------------------------------------------------------
        // SEKCIJA RENDEROWANIA: Rysujemy całe menu od zera na buforze w każdym obiegu
        // -------------------------------------------------------------------------

        // A. Czyszczenie bufora na czarno (czysta karta dla menu)
        // safe_clear_screen(menu_buffer, 0x0000);

        // B. Napisy nagłówkowe (używamy SourceCodePro lub digital_7 zależnie od dostępności)
        // Format: draw_clock_perfect_monospace_90(buffer, buf_w, buf_h, font, text, x, y, size, color)
        //        draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, "SET MINUTS", 100, 50, 100.0f, 0xFFFF);
        if (content_changed == true)
        {

            draw_common_menu_base();

            draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "SET MINUTS", 280, 20, 150.0f, 0xFFFF);

            // C. Rysowanie aktualnie modyfikowanej wartości minut (na środku ekranu)
            char txt_current_min[4];
            snprintf(txt_current_min, sizeof(txt_current_min), "%02d", current_min);
            draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_current_min, 470, 250, 220.0f, 0xFFFF);

            //******************************************************** */
            show_debug_touch(touchX, touchY);
            // draw_test_cross();
            //*********************************************************** */
            //  E. Wypchnięcie gotowej klatki menu na ekran MIPI-DSI z synchronizacją Cache i VSYNC
            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);
            content_changed = false;
        }
        /********************************************** */
        // Obsługa dotyku dopasowana do fizycznego mapowania kontrolera (Natywny pion)
        if (touchX > 0 && touchY > 0)
        {
            // Obsługa przycisków MINUS, PLUS oraz DONE z blokadą stanową touch_busy...
            // 1. MINUS
            if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_min = (current_min == 0) ? 59 : current_min - 1;
                content_changed = true;
                printf("[MENU] Fizyczny odczyt MINUS -> %02d\n", current_min);
            }
            // 2. PLUS
            else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_min = (current_min == 59) ? 0 : current_min + 1;
                content_changed = true;
                printf("[MENU] Fizyczny odczyt PLUS -> %02d\n", current_min);
            }
            // 3. DONE
            else if (touchX >= 50 && touchX <= 250 && touchY >= 450 && touchY <= 850)
            {
                // 1. Pobieramy AKTUALNY pełny stan rejestrów (godziny, sekundy, kalendarz) prosto z chipu
                rx8025t_time_t final_time;
                if (rx8025t_get_time(&final_time) == ESP_OK)
                {
                    // 2. Podmieniamy TYLKO minutę na tę, którą użytkownik wyklikał w menu
                    final_time.minute = current_min;

                    // 3. Zapisujemy zaktualizowaną strukturę z powrotem do układu scalonego RX8025T
                    if (rx8025t_set_time(&final_time) == ESP_OK)
                    {
                        // 4. Synchronizujemy wbudowane, wewnętrzne RTC ESP32 z nowymi danymi z kostki
                        sync_internal_rtc_with_rx8025t();
                    }
                }
                break;
            }
            hold_counter++;
        }
        else
        {
            hold_counter = 0;
        }
    }
    //************************************* */
    // Taktowanie bazy czasu: Jeśli brak dotyku, pętla czeka 16ms (~60Hz),
    // nie obciążając procesora i dbając o natychmiastową reakcję.
    if (touchX == 0 && touchY == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // <<< TUTAJ DOSYPIEMY BLOKADĘ ANTY-POWRACALNĄ >>>
    printf("[MENU] Done pressed. Czekam na puszczenie ekranu...\n");
    wait_for_release();
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void wait_for_release(void)
{
    uint16_t dummyX = 0;
    uint16_t dummyY = 0;
    // Dopóki getTouch zwraca true (ekran wciśnięty), czekaj
    while ((touch.getTouch(&dummyX, &dummyY)) == true)
    {
        vTaskDelay(pdMS_TO_TICKS(15));
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
    snprintf(txt_debug_touch, sizeof(txt_debug_touch), "X:%03d Y:%03d", touchX, touchY);
    draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_debug_touch, 10, 10, 30.0f, 0x07E0);
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
        vTaskDelay(pdMS_TO_TICKS(50));
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
        else if (menu_choice == 6) // NOWOŚĆ: POWIADOMIENIE NR 6
        {
            set_year_menu();
        }

        // --- KLUCZOWY FIX PO WCIŚNIĘCIU DONE ---
        // ====================================================================
        // GLOBALNY SPRZĘTOWY RESET POTOKU PO WYJŚCIU Z JAKIEGOKOLWIEK MENU
        // ====================================================================

        // 1. Dajemy chwilę na zakończenie ostatnich transferów DMA z menu
        vTaskDelay(pdMS_TO_TICKS(20));

        // 2. Czyścimy bufor tła na idealną czerń i synchronizujemy Cache
        safe_clear_screen((uint16_t *)fbs[0], 0x0000);
        esp_cache_msync((void *)fbs[0], BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

        // 3. Twardo czekamy na VSYNC i fizycznie zerujemy klatkę na wyświetlaczu
        xSemaphoreTake(vsync_sem, portMAX_DELAY);
        lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)fbs[0]);

        // 4. Dajemy czas na fizyczne przemiecenie kontrolera i zdejmujemy blokadę czasu
        vTaskDelay(pdMS_TO_TICKS(4));
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
    uint16_t touchX = 0;
    uint16_t touchY = 0;
    int hold_counter = 0;

    printf("[MENU] -> Uruchomiono WIZUALNY tryb edycji MINUT w wątku dotyku!\n");

    // 1. Blokujemy automatyczne odświeżanie głównego zegara
    is_editing_time = true;

    // 2. Pobieramy aktualny czas z zewnętrznej kostki RX8025T jako bazę do edycji
    rx8025t_time_t setup_time;
    if (rx8025t_get_time(&setup_time) != ESP_OK)
    {
        printf("[ERROR] Brak komunikacji z RX8025T!\n");
        is_editing_time = false;
        return;
    }

    uint8_t current_hour = setup_time.hour;

    // Pobieramy wskaźnik na roboczy bufor tła
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    safe_clear_screen(menu_buffer, 0x0000);
    esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bool content_changed = true; // Na starcie wymuszamy pierwsze rysowanie menu

    // GŁÓWNA PĘTLA EDYCJI (Kręci się w wątku vTaskTouch, dopóki is_editing_time == true)
    while (is_editing_time)
    {
        // 1. Zerujemy zmienne przed każdym sprawdzaniem
        touchX = 0;
        touchY = 0;

        // 2. Jeśli funkcja zwróci false (brak fizycznego dotyku),
        // zmienne pozostaną wyzerowane i żaden warunek na dole się nie wyzwoli
        if (!touch.getTouch(&touchX, &touchY))
        {
            touchX = 0;
            touchY = 0;
        }

        // -------------------------------------------------------------------------
        // SEKCIJA RENDEROWANIA: Rysujemy całe menu od zera na buforze w każdym obiegu
        // -------------------------------------------------------------------------

        // A. Czyszczenie bufora na czarno (czysta karta dla menu)
        // safe_clear_screen(menu_buffer, 0x0000);

        // B. Napisy nagłówkowe (używamy SourceCodePro lub digital_7 zależnie od dostępności)
        // Format: draw_clock_perfect_monospace_90(buffer, buf_w, buf_h, font, text, x, y, size, color)
        //        draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, "SET MINUTS", 100, 50, 100.0f, 0xFFFF);
        if (content_changed == true)
        {

            draw_common_menu_base();

            draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "SET HOURS", 300, 20, 150.0f, 0xFFFF);

            // C. Rysowanie aktualnie modyfikowanej wartości minut (na środku ekranu)
            char txt_current_hour[4];
            snprintf(txt_current_hour, sizeof(txt_current_hour), "%02d", current_hour);
            draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_current_hour, 470, 250, 220.0f, 0xFFFF);

            //******************************************************** */
            show_debug_touch(touchX, touchY);
            // draw_test_cross();
            //*********************************************************** */
            //  E. Wypchnięcie gotowej klatki menu na ekran MIPI-DSI z synchronizacją Cache i VSYNC
            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);
            content_changed = false;
        }

        /********************************************** */
        // Obsługa dotyku dopasowana do fizycznego mapowania kontrolera (Natywny pion)
        if (touchX > 0 && touchY > 0)
        {
            // Obsługa przycisków MINUS, PLUS oraz DONE z blokadą stanową touch_busy...
            // 1. MINUS
            if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_hour = (current_hour == 0) ? 23 : current_hour - 1;
                content_changed = true;
                printf("[MENU] Fizyczny odczyt MINUS -> %02d\n", current_hour);
            }
            // 2. PLUS
            else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 && (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_hour = (current_hour == 23) ? 0 : current_hour + 1;
                content_changed = true;
                printf("[MENU] Fizyczny odczyt PLUS -> %02d\n", current_hour);
            }
            // 3. DONE
            else if (touchX >= 50 && touchX <= 250 && touchY >= 450 && touchY <= 850)
            {
                // 1. Pobieramy AKTUALNY pełny stan rejestrów (godziny, sekundy, kalendarz) prosto z chipu
                rx8025t_time_t final_time;
                if (rx8025t_get_time(&final_time) == ESP_OK)
                {
                    // 2. Podmieniamy TYLKO minutę na tę, którą użytkownik wyklikał w menu
                    final_time.hour = current_hour;

                    // 3. Zapisujemy zaktualizowaną strukturę z powrotem do układu scalonego RX8025T
                    if (rx8025t_set_time(&final_time) == ESP_OK)
                    {
                        // 4. Synchronizujemy wbudowane, wewnętrzne RTC ESP32 z nowymi danymi z kostki
                        sync_internal_rtc_with_rx8025t();
                    }
                }
                break;
            }
            hold_counter++;
        }
        else
        {
            hold_counter = 0;
        }
    }
    //************************************* */
    // Taktowanie bazy czasu: Jeśli brak dotyku, pętla czeka 16ms (~60Hz),
    // nie obciążając procesora i dbając o natychmiastową reakcję.
    if (touchX == 0 && touchY == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // <<< TUTAJ DOSYPIEMY BLOKADĘ ANTY-POWRACALNĄ >>>
    printf("[MENU] Done pressed. Czekam na puszczenie ekranu...\n");
    wait_for_release();
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
    uint16_t touchX = 0, touchY = 0;
    int hold_counter = 0;
    is_editing_time = true;
    wait_for_release();

    rx8025t_time_t setup_time;
    if (rx8025t_get_time(&setup_time) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }

    int full_year = setup_time.year;
    if (full_year < 2000)
        full_year += 2000;

    // DYNAMICZNIE USTALAMY MAKSYMALNY DZIEŃ DLA BIEŻĄCEGO MIESIĄCA I ROKU!
    uint8_t max_days = get_max_days_in_month(full_year, setup_time.month);
    int current_edit_day = setup_time.day;

    // POPRAWIONO: Dodano jawny indeks [0] do wskaźnika klatki!
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    bool content_changed = true;

    while (is_editing_time)
    {
        touchX = 0;
        touchY = 0;
        if (!touch.getTouch(&touchX, &touchY))
        {
            touchX = 0;
            touchY = 0;
        }

        if (content_changed)
        {
            draw_common_menu_base();
            draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, " SET DAY", 280, 20, 150.0f, 0xFFFF);
            char txt_current_day[16];
            snprintf(txt_current_day, sizeof(txt_current_day), "%02d", current_edit_day);

            draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_current_day, 470, 250, 220.0f, 0xFFFF);
            show_debug_touch(touchX, touchY);
            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);
            content_changed = false;
        }
        if (touchX > 0 && touchY > 0)
        {
            if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 &&
                (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_edit_day = (current_edit_day == 1) ? max_days : current_edit_day - 1;
                content_changed = true;
            }
            else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 &&
                     (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_edit_day = (current_edit_day == max_days) ? 1 : current_edit_day + 1;
                content_changed = true;
            }
            else if (touchX >= 50 && touchX <= 250 && touchY >= 450 && touchY <= 850)
            {
                setup_time.day = current_edit_day;

                if (rx8025t_set_time(&setup_time) == ESP_OK)
                {
                    sync_internal_rtc_with_rx8025t();
                }
                break;
            }
            hold_counter++;
        }
        else
        {
            hold_counter = 0;
        }
        if (touchX == 0 && touchY == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    printf("[MENU] Done pressed. Czekam na puszczenie ekranu...\n");
    wait_for_release();
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_year_menu(void)
{
    uint16_t touchX = 0, touchY = 0;
    int hold_counter = 0;
    is_editing_time = true;

    rx8025t_time_t setup_time;
    if (rx8025t_get_time(&setup_time) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }
    // PRZELICZAMY DWUCYFROWY ROK Z RTC NA PEŁNY ROK (NP. 26 -> 2026)
    // ODCINAMY baze 2000, poniewaz setup_time.year w ESP-IDF ma juz pelna wartosc kalendarzowa!
    int current_edit_year = setup_time.year;
    if (current_edit_year < 2000)
    {
        current_edit_year += 2000; // Zabezpieczenie na wypadek surowego rejestru z chipu
    }

    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    safe_clear_screen(menu_buffer, 0x0000);

    esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bool content_changed = true;

    while (is_editing_time)
    {
        touchX = 0;
        touchY = 0;
        if (!touch.getTouch(&touchX, &touchY))
        {
            touchX = 0;
            touchY = 0;
        }
        //************************************ */
        if (content_changed)
        {
            draw_common_menu_base();
            draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, " SET YEAR", 280, 20, 150.0f, 0xFFFF);

            // POPRAWIONY BUFOR TEKSTOWY JAKO TABLICA (TERAZ WYŚWIETLI IDEALNIE NP. "2026")
            char txt_current_year[16];
            snprintf(txt_current_year, sizeof(txt_current_year), "%04d", current_edit_year);

            // RYSOWANIE ROKU CZCIONKĄ ZMNIEJSZONĄ DO 180.0f DLA LEPSZEGO DOPASOWANIA
            draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_current_year, 350, 250, 180.0f, 0xFFFF);

            show_debug_touch(touchX, touchY);

            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);
            content_changed = false;
        }

        //******************************* */
        if (touchX > 0 && touchY > 0)
        {
            // OBSŁUGA PRZYCISKU MINUS (ZAKRES OD 2026 ROKU)
            if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 &&
                (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_edit_year = (current_edit_year == 2026) ? 2099 : current_edit_year - 1;
                content_changed = true;
            }
            // OBSŁUGA PRZYCISKU PLUS (ZAKRES DO 2099 ROKU)
            else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 &&
                     (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_edit_year = (current_edit_year == 2099) ? 2026 : current_edit_year + 1;
                content_changed = true;
            }
            // OBSŁUGA PRZYCISKU DONE Z WALIDACJĄ I ZAPISEM DO RTC
            else if (touchX >= 50 && touchX <= 250 && touchY >= 450 && touchY <= 850)
            {
                // SPRAWDZAMY CZY DZIEŃ NIE PRZEKRACZA LIMITU DLA NOWEGO ROKU
                uint8_t max_days = get_max_days_in_month(current_edit_year, setup_time.month);
                if (setup_time.day > max_days)
                {
                    setup_time.day = max_days; // COFAMY DZIEŃ DO KRAWĘDZI (NP. 29 -> 28 LUTY)
                }
                setup_time.year = current_edit_year;

                if (rx8025t_set_time(&setup_time) == ESP_OK)
                {
                    sync_internal_rtc_with_rx8025t();
                }
                break;
            }
            hold_counter++;
        }
        else
        {
            hold_counter = 0;
        }
        if (touchX == 0 && touchY == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    printf("[MENU] Done pressed. Czekam na puszczenie ekranu...\n");
    wait_for_release();
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void set_month_menu(void)
{
    uint16_t touchX = 0, touchY = 0;
    int hold_counter = 0;
    is_editing_time = true;
    wait_for_release();

    rx8025t_time_t setup_time;
    if (rx8025t_get_time(&setup_time) != ESP_OK)
    {
        is_editing_time = false;
        return;
    }

    int current_edit_month = setup_time.month;
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    bool content_changed = true;
    while (is_editing_time)
    {
        touchX = 0;
        touchY = 0;
        if (!touch.getTouch(&touchX, &touchY))
        {
            touchX = 0;
            touchY = 0;
        }

        if (content_changed)
        {
            draw_common_menu_base();
            draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, " SET MONTH", 280, 20, 150.0f, 0xFFFF);

            char txt_current_month[16];
            snprintf(txt_current_month, sizeof(txt_current_month), "%02d", current_edit_month);

            draw_clock_perfect_monospace_90(menu_buffer, LCD_H_RES, LCD_V_RES, digital_7_ttf, txt_current_month, 470, 250, 220.0f, 0xFFFF);
            show_debug_touch(touchX, touchY);
            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);
            content_changed = false;
        }
        if (touchX > 0 && touchY > 0)
        {
            if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 &&
                (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_edit_month = (current_edit_month == 1) ? 12 : current_edit_month - 1;
                content_changed = true;
            }
            else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 &&
                     (hold_counter == 1 || (hold_counter > 500 && hold_counter % 15 == 0)))
            {
                current_edit_month = (current_edit_month == 12) ? 1 : current_edit_month + 1;
                content_changed = true;
            }
            else if (touchX >= 50 && touchX <= 250 && touchY >= 450 && touchY <= 850)
            {
                // Korekta dni miesiąca oraz zapis do RTC
                int full_year = setup_time.year;
                if (full_year < 2000)
                    full_year += 2000;

                uint8_t max_days = get_max_days_in_month(full_year, current_edit_month);
                if (setup_time.day > max_days)
                {
                    setup_time.day = max_days;
                }

                setup_time.month = current_edit_month;
                if (rx8025t_set_time(&setup_time) == ESP_OK)
                {
                    sync_internal_rtc_with_rx8025t();
                }
                break;
            }
            hold_counter++;
        }
        else
        {
            hold_counter = 0;
        }
        if (touchX == 0 && touchY == 0)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    printf("[MENU] Done pressed. Czekam na puszczenie ekranu...\n");
    wait_for_release();
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

// void rekonfiguruj_pin_bl_na_pwm(void)
// {
//     // 1. Konfiguracja timera LEDC
//     ledc_timer_config_t ledc_timer = {
//         .speed_mode = BACKLIGHT_LEDC_MODE,
//         .duty_resolution = BACKLIGHT_LEDC_DUTY_RES,
//         .timer_num = BACKLIGHT_LEDC_TIMER,
//         .freq_hz = BACKLIGHT_LEDC_FREQUENCY,
//         .clk_cfg = LEDC_AUTO_CLK};
//     ledc_timer_config(&ledc_timer);

//     // 2. Konfiguracja kanału LEDC i przejęcie pinu 23
//     ledc_channel_config_t ledc_channel =
//         {
//             .gpio_num = BACKLIGHT_GPIO,
//             .speed_mode = BACKLIGHT_LEDC_MODE,
//             .channel = BACKLIGHT_LEDC_CHANNEL,
//             .intr_type = LEDC_INTR_DISABLE,
//             .timer_sel = BACKLIGHT_LEDC_TIMER,
//             .duty = 1023, // Na start ustawiamy 100% jasności (10-bit max)
//             .hpoint = 0,
//             .sleep_mode = (ledc_sleep_mode_t)0 // <<< Twarde wyzerowanie pola uśpienia
//     };
//     ledc_channel_config(&ledc_channel);

//     printf("[PWM] Pin 23 został pomyślnie REKONFIGUROWANY na tryb PWM (10-bit, 5kHz).\n");
// }

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// Definicja globalnej zmiennej dla jasności (zakres 2 - 100)

void set_light_menu(void)
{
    uint16_t touchX = 0;
    uint16_t touchY = 0;
    uint16_t *menu_buffer = (uint16_t *)fbs[0];
    safe_clear_screen(menu_buffer, 0x0000);
    esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bool content_changed = true;

    // 1. Czyszczenie bufora na czarno (czysta karta pod nową klatkę menu)
    safe_clear_screen(menu_buffer, 0x0000);

    // 2. To ZOSTAJE: Napis nagłówkowy menu
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, " SET LIGHT", 300, 20, 150.0f, 0xFFFF);

    // 4. To ZOSTAJE: Przycisk DONE (Ramka i napis) - użytkownik musi jakoś wyjść z menu
    draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, "DONE", 510, 580, 150.0f, 0xFFFF);
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 390, 600, 500, 120, 4, 0xFFFF);
    // Rysowanie zewnętrznej ramki suwaka (szerokość 1000, wysokość 120, na Y = 350)
    draw_hollow_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 132, 342, 1018, 136, 4, 0xFFFF);

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

            // ==========================================
            // TUTAJ WSTAWIASZ NOWĄ PACZKĘ 1 (RYSOWANIE SUWAKA):
            // ==========================================

            // Wyliczenie szerokości zielonego wypełnienia (1% = 10 pikseli)
            uint16_t len_draw = current_brightness * 10;

            // Rysowanie zielonego paska jasności wewnątrz ramki
            draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 141, 351, len_draw, 118, 0x07E0);

            // Czyszczenie pozostałej części paska na czarno
            draw_rectangle_90(menu_buffer, LCD_H_RES, LCD_V_RES, 141 + len_draw, 351, 1000 - len_draw, 118, 0x0000);
            // ==========================================
            // 4A. Dynamiczny tekst procentowy na środku suwaka (rysowany PO pasku)
            char txt_slider_pct[16];
            snprintf(txt_slider_pct, sizeof(txt_slider_pct), "%d%%", current_brightness);
            // Używamy czcionki tekstowej (systemowej) o rozmiarze 100.0f, wyśrodkowanej w pasku
            draw_text_90(menu_buffer, LCD_H_RES, LCD_V_RES, txt_slider_pct, 600, 360, 100.0f, 0xFFFF);

            // 5. To ZOSTAJE: Rysowanie debugowania pozycji dotyku
            show_debug_touch(touchX, touchY);

            // 6. To ZOSTAJE: Synchronizacja Cache i wypchnięcie klatki przez DMA na MIPI-DSI
            esp_cache_msync((void *)menu_buffer, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            xSemaphoreTake(vsync_sem, portMAX_DELAY);
            lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)menu_buffer);

            content_changed = false;
        }
        // =========================================================================
        // OBSŁUGA DOTYKU I SUWAKA (Kopiuj od tego miejsca aż do samego końca funkcji)
        // =========================================================================
        if (touchX > 0 && touchY > 0)
        {
            if (touchY >= 140 && touchY <= 1140 && touchX >= 347 && touchX <= 473)
            {
                current_brightness = (touchY - 140) / 10;
                if (current_brightness > 100)
                    current_brightness = 100;
                if (current_brightness < 5)
                    current_brightness = 5;
                set_backlight_brightness(current_brightness);
                content_changed = true;
                // printf("[SLIDER] Jasnosc touchY: %d -> %d%%\n", touchY, current_brightness);
            }
            else if (touchX >= 50 && touchX <= 250 && touchY >= 450 && touchY <= 850)
            {
                save_brightness_to_NVS(current_brightness);
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
void vTaskFourHourIncrementer(void *pvParameters)
{
    // Dla testu: 1 minuta (FreeRTOS sam przeliczy to na ticki zależnie od Hz systemu)
    const TickType_t xIncrementInterval = pdMS_TO_TICKS(4 * 60 * 60 * 1000);

    // Zmienna MUSI być zainicjalizowana aktualnym czasem przed pętlą
    TickType_t xLastWakeTime = xTaskGetTickCount();

    printf("[SYSTEM] Zadanie inkrementacji co 4 godziny wystartowało.\n");

    while (1)
    {
        // Pierwszy argument przekazujemy przez wskaźnik – FreeRTOS sam go zaktualizuje o wartość xIncrementInterval
        vTaskDelayUntil(&xLastWakeTime, xIncrementInterval);

        // Sekcja inkrementacji zmiennej
        g_my_4h_counter = g_my_4h_counter + 1;

        // Log do konsoli ESP
        ESP_LOGI("TIMER_TASK", "Minęły 4h. Licznik globalny: %lu", g_my_4h_counter);
    }
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
        FILE *f1 = fopen("/spiffs/FOTO1.bmp", "r");
        FILE *f2 = fopen("/spiffs/FOTO2.bmp", "r");

        if (f1 != NULL)
        {
            printf("[SPIFFS_TEST] SUKCES: Wykryto plik -> /spiffs/FOTO1.bmp\n");
            fclose(f1);
        }
        else
        {
            printf("[SPIFFS_TEST] BLAD: Brak pliku FOTO1.bmp na partycji Flash!\n");
        }

        if (f2 != NULL)
        {
            printf("[SPIFFS_TEST] SUKCES: Wykryto plik -> /spiffs/FOTO2.bmp\n");
            fclose(f2);
        }
        else
        {
            printf("[SPIFFS_TEST] BLAD: Brak pliku FOTO2.bmp na partycji Flash!\n");
        }
    }
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

// void init_spiffs_storage(void)
// {
//     printf("[SPIFFS] Inicjalizacja i montowanie partycji 'storage'...\n");

//     esp_vfs_spiffs_conf_t conf = {
//         .base_path = "/spiffs",         // Wszystkie pliki będą pod ścieżką /spiffs/
//         .partition_label = "storage",   // Ta etykieta MUSI pasować do partitions.csv
//         .max_files = 5,                 // Ile plików można otworzyć naraz
//         .format_if_mount_failed = true  // Automatycznie sformatuje partycję przy pierwszym starcie
//     };

//     // Rejestracja dysku w wirtualnym systemie plików ESP-IDF
//     esp_err_t ret = esp_vfs_spiffs_register(&conf);

//     if (ret != ESP_OK) {
//         if (ret == ESP_FAIL) {
//             printf("[SPIFFS_ERROR] Nie udało się zamontować ani sformatować systemu plików!\n");
//         } else if (ret == ESP_ERR_NOT_FOUND) {
//             printf("[SPIFFS_ERROR] Nie znaleziono partycji o nazwie 'storage'!\n");
//         } else {
//             printf("[SPIFFS_ERROR] Błąd inicjalizacji SPIFFS (%s)\n", esp_err_to_name(ret));
//         }
//         return;
//     }

//     // Pobranie informacji o wolnej przestrzeni (dla pewności w logach)
//     size_t total = 0, used = 0;
//     ret = esp_spiffs_info(conf.partition_label, &total, &used);
//     if (ret == ESP_OK) {
//         printf("[SPIFFS_SUCCESS] Partycja zamontowana! Rozmiar: %d KB, Użyto: %d KB\n", total / 1024, used / 1024);
//     }
// }
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void vTaskAutoPhotoTimer(void *pvParameters)
{
    printf("[SYSTEM] Zadanie auto-foto co 5 minut wystartovalo.\n");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(6 * 1000));
        // vTaskDelay(pdMS_TO_TICKS(5 * 60 * 1000));
        g_trigger_auto_photo = true;
        printf("[TIMER] Minelo 5 minut! Wyzwalam pokaz zdjecia.\n");
    }
}
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// void wyswietl_jpg_ze_spiffs(const char *sciezka_pliku)
// {
//     printf("[JPEG] PSRAM start: %s\n", sciezka_pliku);
//     FILE *f = fopen(sciezka_pliku, "rb");
//     if (f == NULL)
//     {
//         printf("[JPEG_ERROR] Brak pliku %s\n", sciezka_pliku);
//         return;
//     }
//     fseek(f, 0, SEEK_END);
//     size_t rozmiar_jpg = ftell(f);
//     fseek(f, 0, SEEK_SET);

//     uint8_t *bufor_jpg = (uint8_t *)heap_caps_malloc(rozmiar_jpg, MALLOC_CAP_SPIRAM);
//     if (bufor_jpg == NULL)
//     {
//         fclose(f);
//         return;
//     }
//     fread(bufor_jpg, 1, rozmiar_jpg, f);
//     fclose(f);
//     esp_cache_msync((void *)bufor_jpg, rozmiar_jpg, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
//     size_t rozmiar_rgb888 = 1280 * 800 * 3;
//     uint8_t *bufor_rgb888 = (uint8_t *)heap_caps_malloc(rozmiar_rgb888, MALLOC_CAP_SPIRAM);
//     if (bufor_rgb888 == NULL)
//     {
//         heap_caps_free(bufor_jpg);
//         return;
//     }
//     esp_jpeg_image_cfg_t jpeg_cfg = {
//         .indata = bufor_jpg,
//         .indata_size = (uint32_t)rozmiar_jpg,
//         .outbuf = bufor_rgb888,
//         .outbuf_size = (uint32_t)rozmiar_rgb888,
//         .out_format = JPEG_IMAGE_FORMAT_RGB888,
//         .out_scale = JPEG_IMAGE_SCALE_0,
//         .flags = {.swap_color_bytes = 0}};
//     esp_jpeg_image_output_t outimg;
//     esp_err_t res = esp_jpeg_decode(&jpeg_cfg, &outimg);
//     heap_caps_free(bufor_jpg);
//     if (res == ESP_OK)
//     {
//         esp_cache_msync((void *)bufor_rgb888, rozmiar_rgb888, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
//     }
//     uint16_t *bufor_ekranu = (uint16_t *)fbs[0];
//     if (res == ESP_OK)
//     {
//         for (int i = 0; i < 1280 * 800; i++)
//         {
//             uint8_t r = bufor_rgb888[i * 3 + 0];
//             uint8_t g = bufor_rgb888[i * 3 + 1];
//             uint8_t b = bufor_rgb888[i * 3 + 2];
//             bufor_ekranu[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
//         }
//     }
//     heap_caps_free(bufor_rgb888);
//     if (res == ESP_OK)
//     {
//         esp_cache_msync((void *)bufor_ekranu, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
//         xSemaphoreTake(vsync_sem, portMAX_DELAY);
//         lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)bufor_ekranu);
//         printf("[JPEG] Obraz wyrenderowany stabilnie i wyslany przez DMA na LCD!\n");
//     }
// }

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
// void wyswietl_jpg_ze_spiffs(const char *sciezka_pliku)
// {
//     printf("[JPEG] PSRAM start: %s\n", sciezka_pliku);
//     FILE *f = fopen(sciezka_pliku, "rb");
//     if (f == NULL)
//     {
//         printf("[JPEG_ERROR] Brak pliku %s\n", sciezka_pliku);
//         return;
//     }
//     fseek(f, 0, SEEK_END);
//     size_t rozmiar_jpg = ftell(f);
//     fseek(f, 0, SEEK_SET);

//     uint8_t *bufor_jpg = g_statyczny_bufor_jpg;
//     fread(bufor_jpg, 1, rozmiar_jpg, f);
//     fclose(f);

//     periph_module_reset(PERIPH_JPEG_MODULE);
//     vTaskDelay(pdMS_TO_TICKS(10)); // Opóźnienie na stabilizację DMA

//     // Unieważnienie Cache przed dekodowaniem
//     size_t rozmiar_rgb_safe_sync = (ROZMIAR_RGB888 + 63) & ~63;
//     esp_cache_msync((void *)bufor_rgb888, rozmiar_rgb_safe_sync, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

//     esp_err_t res = esp_jpeg_decode(&jpeg_cfg, &outimg);

//     uint8_t *bufor_rgb888 = g_statyczny_bufor_rgb888;

//     esp_jpeg_image_cfg_t jpeg_cfg = {
//         .indata = bufor_jpg,
//         .indata_size = (uint32_t)rozmiar_jpg,
//         .outbuf = bufor_rgb888,
//         .outbuf_size = (uint32_t)ROZMIAR_RGB888,
//         .out_format = JPEG_IMAGE_FORMAT_RGB888,
//         .out_scale = JPEG_IMAGE_SCALE_0,
//         .flags = {.swap_color_bytes = 0}};
//     esp_jpeg_image_output_t outimg;
//     res = esp_jpeg_decode(&jpeg_cfg, &outimg);

//     if (res == ESP_OK)
//     {
//         esp_cache_msync((void *)bufor_rgb888, ROZMIAR_RGB888, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
//     }
//     uint16_t *bufor_ekranu = (uint16_t *)fbs[0];
//     if (res == ESP_OK)
//     {
//         for (int i = 0; i < 1280 * 800; i++)
//         {
//             uint8_t r = bufor_rgb888[i * 3 + 0];
//             uint8_t g = bufor_rgb888[i * 3 + 1];
//             uint8_t b = bufor_rgb888[i * 3 + 2];
//             bufor_ekranu[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
//         }
//     }
//     // esp_err_t res = esp_jpeg_decode(&jpeg_cfg, &outimg);

//     if (res == ESP_OK)
//     {
//         // 1. Synchronizujemy zdekodowany bufor surowych pikseli RGB888 z pamieci Cache do PSRAM
//         size_t rozmiar_rgb_safe_sync = (ROZMIAR_RGB888 + 63) & ~63;
//         esp_cache_msync((void *)bufor_rgb888, rozmiar_rgb_safe_sync, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
//     }

//     if (res == ESP_OK)
//     {
//         // 2. KLUCZOWY KROK: Przepakowanie pikseli 24-bit RGB888 do 16-bit RGB565 bufora ekranu
//         for (int i = 0; i < 1280 * 800; i++)
//         {
//             uint8_t r = bufor_rgb888[i * 3 + 0];
//             uint8_t g = bufor_rgb888[i * 3 + 1];
//             uint8_t b = bufor_rgb888[i * 3 + 2];
//             bufor_ekranu[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
//         }

//         // 3. Wypchniecie nowo zbudowanego obrazu RGB565 z pamieci Cache procesora do fizycznego PSRAM ekranu
//         esp_cache_msync((void *)bufor_ekranu, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

//         // 4. Czekamy na bezpieczny moment VSYNC i rzucamy DMA na LCD
//         xSemaphoreTake(vsync_sem, portMAX_DELAY);
//         lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)bufor_ekranu);
//         printf("[JPEG] Obraz wyrenderowany stabilnie i wyslany przez DMA na LCD!\n");
//     }
// }
//*************************************************** */

//=====================================================================================================
// void wyswietl_jpg_ze_spiffs(const char *sciezka_pliku)
// {
//     printf("[JPEG] PSRAM start: %s\n", sciezka_pliku);
//     FILE *f = fopen(sciezka_pliku, "rb");
//     if (f == NULL)
//     {
//         printf("[JPEG_ERROR] Brak pliku %s\n", sciezka_pliku);
//         return;
//     }
//     fseek(f, 0, SEEK_END);
//     size_t rozmiar_jpg = ftell(f);
//     fseek(f, 0, SEEK_SET);
//     uint8_t *bufor_jpg = g_statyczny_bufor_jpg;
//     fread(bufor_jpg, 1, rozmiar_jpg, f);
//     fclose(f);
//     periph_module_reset(PERIPH_JPEG_MODULE);
//     vTaskDelay(pdMS_TO_TICKS(10));
//     size_t rozmiar_jpg_aligned = (rozmiar_jpg + 63) & ~63;
//     esp_cache_msync((void *)bufor_jpg, rozmiar_jpg_aligned, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
//     uint8_t *bufor_rgb888 = g_statyczny_bufor_rgb888;
//     esp_jpeg_image_cfg_t jpeg_cfg = {0};
//     jpeg_cfg.indata = bufor_jpg;
//     jpeg_cfg.indata_size = (uint32_t)rozmiar_jpg;
//     jpeg_cfg.outbuf = bufor_rgb888;
//     jpeg_cfg.outbuf_size = (uint32_t)ROZMIAR_RGB888;
//     jpeg_cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
//     jpeg_cfg.out_scale = JPEG_IMAGE_SCALE_0;
//     esp_jpeg_image_output_t outimg;
//     esp_err_t res = esp_jpeg_decode(&jpeg_cfg, &outimg);
//     if (res == ESP_OK)
//     {
//         size_t rozmiar_rgb_safe_sync = (ROZMIAR_RGB888 + 63) & ~63;
//         esp_cache_msync((void *)bufor_rgb888, rozmiar_rgb_safe_sync, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
//     }
//     uint16_t *bufor_ekranu = (uint16_t *)fbs[0];
//     if (res == ESP_OK)
//     {
//         for (int i = 0; i < 1280 * 800; i++)
//         {
//             uint8_t r = bufor_rgb888[i * 3 + 0];
//             uint8_t g = bufor_rgb888[i * 3 + 1];
//             uint8_t b = bufor_rgb888[i * 3 + 2];
//             bufor_ekranu[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
//         }
//     }
//     if (res == ESP_OK)
//     {
//         esp_cache_msync((void *)bufor_ekranu, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
//         xSemaphoreTake(vsync_sem, portMAX_DELAY);
//         lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)bufor_ekranu);
//         printf("[JPEG] Obraz udanie wyslany przez DMA na LCD!\n");
//     }
// }
//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
void wyswietl_bmp_ze_spiffs(const char *sciezka_pliku)
{
    if (g_statyczny_bufor_bmp == NULL)
        return;
    printf("[BMP] Odczyt pliku: %s\n", sciezka_pliku);
    FILE *f = fopen(sciezka_pliku, "rb");
    if (f == NULL)
        return;
    fread(g_statyczny_bufor_bmp, 1, ROZMIAR_MAX_BMP, f);
    fclose(f);
    uint16_t *bufor_ekranu = (uint16_t *)fbs[0];

    int idx = 54;

    for (int i = 0; i < 1280 * 800; i++)
    {
        uint8_t b = g_statyczny_bufor_bmp[idx + 0];
        uint8_t g = g_statyczny_bufor_bmp[idx + 1];
        uint8_t r = g_statyczny_bufor_bmp[idx + 2];
        idx += 3;

        bufor_ekranu[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }

    esp_cache_msync((void *)bufor_ekranu, BUFFER_SIZE, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    xSemaphoreTake(vsync_sem, portMAX_DELAY);
    lcd.lcd_draw_bitmap(0, 0, LCD_H_RES, LCD_V_RES, (uint8_t *)bufor_ekranu);
    printf("[BMP_SUCCESS] Obraz bezkompresyjny wyslany na LCD!\n");
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
