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
                    // Tutaj wstawimy set_hours_menu(); w przyszłości
                }
                // 2. HIT-BOX: SEKCJA MINUT (Góra-Prawo) - Łączymy elegancko przez else if na osi Y!
                else if (touchY >= 680 && touchY <= 1279)
                {
                    printf("[MENU] -> vTaskTouch budzi MenuEngine i sam się zawiesza...\n");
                    if (hTaskMenuEngine != NULL)
                    {
                        is_editing_time = true;
                        xTaskNotifyGive(hTaskMenuEngine); // Budzimy silnik menu
                        vTaskSuspend(NULL);               // <<< PEWNE SAMOZAWIESZENIE WĄTKU DOTYKU!
                    }
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
    xTaskCreate(vTaskTouch, "vTaskTouch", 4096, NULL, 4, &hTaskTouch);

    xTaskCreate(vTaskMenuEngine, "vTaskMenuEngine", 4096, NULL, 4, &hTaskMenuEngine);

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
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 104, 50, 50, 0xFFFF);
    draw_rectangle_90(main_buffer, LCD_H_RES, LCD_V_RES, 605, 300, 50, 50, 0xFFFF);

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
        /********************************************** */
        // Obsługa dotyku dopasowana do fizycznego mapowania kontrolera (Natywny pion)
        if (touchX > 0 && touchY > 0)
        {
            // Obsługa przycisków MINUS, PLUS oraz DONE z blokadą stanową touch_busy...
            // 1. MINUS
            if (touchX >= 200 && touchX <= 600 && touchY >= 50 && touchY <= 350 && (hold_counter == 1 || (hold_counter > 6 && hold_counter % 2 == 0)))
            {
                current_min = (current_min == 0) ? 59 : current_min - 1;
                printf("[MENU] Fizyczny odczyt MINUS -> %02d\n", current_min);
            }
            // 2. PLUS
            else if (touchX >= 200 && touchX <= 600 && touchY >= 950 && touchY <= 1250 && (hold_counter == 1 || (hold_counter > 6 && hold_counter % 2 == 0)))
            {
                current_min = (current_min == 59) ? 0 : current_min + 1;
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

                // 5. Opuszczamy pętlę menu edycji
                is_editing_time = false;
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
    printf("[SYSTEM] Zadanie vTaskMenuEngine gotowe i zablokowane na kolejce powiadomień.\n");
    while (1)
    {
        // Czekamy zablokowani w jądru na impuls z vTaskTouch
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Wywołujemy menu (pętla kręci się samodzielnie)
        set_minutes_menu();

        // <<< PO WYJŚCIU Z MENU (PO DONE) BUDZIMY GŁÓWNY DOTYK >>>
        printf("[ENGINE] Menu zakończone. Wznawiam vTaskTouch...\n");
        if (hTaskTouch != NULL)
        {
            vTaskResume(hTaskTouch); // Przywracamy skanowanie ekranu głównego zegara
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

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================

//=====================================================================================================
//=====================================================================================================
//=====================================================================================================
