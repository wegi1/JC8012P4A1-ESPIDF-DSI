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
#include "rx8025t.h"
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
volatile bool is_editing_time = false;

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

        // 2. Odliczamy dokładnie 500 milisekund (dwukropek świeci)
        vTaskDelay(pdMS_TO_TICKS(500));
        if (is_editing_time)
        {
            continue;
        } // Jeśli edytujemy , nie ruszaj bufora zegara!
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

    printf("[LOG] Wielowątkowe zadanie dotyku vTaskTouch ruszyło z prawidłowym mapowaniem rotacji!\n");

    while (1)
    {
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
                    // Tutaj wstawimy set_hours_menu(); w przyszłości
                }
                // 2. HIT-BOX: SEKCJA MINUT (Góra-Prawo) - Łączymy elegancko przez else if na osi Y!
                else if (touchY >= 680 && touchY <= 1279)
                {
                    printf("[MENU] -> Wykryto kliknięcie w SEKCJĘ MINUT!\n");
                    set_minutes_menu();
                }
            }
            // 3. HIT-BOX: SEKUNDNIK (Dół ekranu poziomego) - Przechodzi na oddzielny warunek dla osi X
            else if (touchX >= 1 && touchX < 300)
            {
                printf("[MENU] -> Wykryto kliknięcie w DOLNY PANEL / SEKUNDNIK!\n");
            }

            // Sprzętowy anty-rebound (Debouncing)
            vTaskDelay(pdMS_TO_TICKS(250));
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
    if (is_editing_time)
    {
        return;
    } // Jeśli edytujemy , nie ruszaj bufora zegara!

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
void set_minutes_menu(void)
{
    uint16_t touchX = 0;
    uint16_t touchY = 0;

    printf("[MENU] -> Uruchomiono tryb edycji MINUT!\n");
    is_editing_time = true; // Zgłaszamy blokadę dla print_RTC i vTaskDisableColon

    // 1. Pobieramy aktualny czas z zewnętrznej kostki RX8025T jako bazę do edycji
    rx8025t_time_t setup_time;
    if (rx8025t_get_time(&setup_time) != ESP_OK)
    {
        printf("[ERROR] Brak komunikacji z RX8025T!\n");
        is_editing_time = false;
        return;
    }

    uint8_t current_min = setup_time.minute;
    printf("[MENU] Edycja minuty startuje od: %02d. Użyj stref (Góra=PLUS, Dół=MINUS, Środek=DONE)\n", current_min);

    // Główna pętla edycji (działa dopóki nie klikniesz DONE)
    while (is_editing_time)
    {
        if (touch.getTouch(&touchX, &touchY))
        {
            // Pilnujemy, czy palec nadal operuje w pasie pionowym minut (Y od 680 do 1279)
            if (touchY >= 680 && touchY <= 1279)
            {
                // AKCJA: PLUS (Górna część ekranu poziomego, czyli wysoki fizyczny X: 580 - 799)
                if (touchX >= 580 && touchX <= 799)
                {
                    if (current_min == 59)
                        current_min = 0;
                    else
                        current_min++;
                    printf("[MENU] MINUTY ++ -> %02d\n", current_min);
                }
                // AKCJA: MINUS (Dolna część ekranu poziomego, czyli niski fizyczny X: 350 - 450)
                else if (touchX >= 350 && touchX <= 450)
                {
                    if (current_min == 0)
                        current_min = 59;
                    else
                        current_min--;
                    printf("[MENU] MINUTY -- -> %02d\n", current_min);
                }
                // AKCJA: DONE (Środek ekranu poziomego, czyli fizyczny X: 451 - 579)
                else if (touchX > 450 && touchX < 580)
                {
                    printf("[MENU] Zatwierdzono! Zapisuję minutę %02d do sprzętowego RTC...\n", current_min);

                    setup_time.minute = current_min;
                    setup_time.second = 0; // Zerujemy sekundy przy zatwierdzeniu minut [1.11]

                    // 2. Fizyczny zapis do kostki przez magistralę I2C
                    if (rx8025t_set_time(&setup_time) == ESP_OK)
                    {
                        printf("[SUCCESS] Zapis do RX8025T udany!\n");
                        // 3. Re-synchronizacja wewnętrznego zegara procesora z kostki
                        extern void sync_internal_rtc_with_rx8025t(void);
                        sync_internal_rtc_with_rx8025t();
                    }
                    else
                    {
                        printf("[ERROR] Błąd zapisu I2C do kości RTC!\n");
                    }

                    is_editing_time = false; // Wychodzimy z pętli while, odblokowujemy system
                }
            }
            // Anty-rebound (Debouncing) dla kliknięć w menu edycji
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        vTaskDelay(pdMS_TO_TICKS(30)); // Wytchnienie dla FreeRTOS
    }
    printf("[MENU] Wyjście z trybu edycji. Powrót do normalnego zegara.\n");
}

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
