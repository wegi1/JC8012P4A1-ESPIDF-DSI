#include "WiFi_NTP.hpp"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

// Deklarujemy Twoją globalną funkcję re-synchronizacji, aby moduł mógł zapisać czas do kości RTC
extern "C"
{
#include "rx8025t.h"
    void sync_internal_rtc_with_rx8025t(void);
}

static const char *TAG = "WIFI_NTP_MODULE";
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

// CALLBACK: Wywoływany automatycznie przez ESP-IDF, gdy czas z internetu zostanie pobrany i ustawiony w jądrze
void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "[NTP] Sukces! Czas systemowy został zsynchronizowany z internetem.");

    // Pobieramy ten świeży czas z jądra i natychmiast aktualizujemy fizyczną kostkę RX8025T!
    time_t now_seconds;
    struct tm time_info;
    time(&now_seconds);
    localtime_r(&now_seconds, &time_info);

    rx8025t_time_t czas_do_zapisu;
    czas_do_zapisu.hour = time_info.tm_hour;
    czas_do_zapisu.minute = time_info.tm_min;
    czas_do_zapisu.second = time_info.tm_sec;
    czas_do_zapisu.day = time_info.tm_mday;
    czas_do_zapisu.month = time_info.tm_mon + 1;
    czas_do_zapisu.year = time_info.tm_year + 1900;
    czas_do_zapisu.weekday = time_info.tm_wday;

    if (rx8025t_set_time(&czas_do_zapisu) == ESP_OK)
    {
        ESP_LOGI(TAG, "[NTP -> RTC] Nowy czas pomyślnie utrwalony w fizycznej kostce RX8025T!");
    }
    else
    {
        ESP_LOGE(TAG, "[NTP -> RTC] Błąd zapisu do magistrali I2C dla RX8025T!");
    }
}
// HANDLER ZDARZEŃ SIECIOWYCH - pilnuje flag w s_wifi_event_group
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
        ESP_LOGI(TAG, "[Wi-Fi] Uruchomiono tryb stacji, nawiązywanie połączenia...");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT); // << UŻYCIE ZMIENNEJ
        ESP_LOGW(TAG, "[Wi-Fi] Rozłączono! Ponowna próba łączenia w tle...");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "[Wi-Fi] Sukces! Uzyskano adres IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT); // << UŻYCIE ZMIENNEJ
    }
}

// GŁÓWNA FUNKCJA INICJALIZACYJNA - PUBLICZNA (BEZ STATIC!)
void init_wifi_and_ntp(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "[INIT] Uruchamianie asynchronicznego podsystemu sieciowego...");

    // 1. Inicjalizacja pamięci nieulotnej NVS (wymagane przez Wi-Fi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Inicjalizacja grupy zdarzeń
    s_wifi_event_group = xEventGroupCreate(); // << UŻYCIE ZMIENNEJ
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    // 3. Konfiguracja sterownika Wi-Fi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 4. Rejestracja callbacków obsługi zdarzeń sieciowych
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    // 5. Ustawienie parametrów sieci użytkownika
    wifi_config_t wifi_config = {};
    strcpy((char *)wifi_config.sta.ssid, ssid);
    strcpy((char *)wifi_config.sta.password, password);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 6. Inicjalizacja klienta SNTP w trybie asynchronicznym (nieblokującym)
    ESP_LOGI(TAG, "[NTP] Konfiguracja serwera pool.ntp.org...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");

    // Ustawiamy strefę czasową dla Polski (obsługuje automatycznie DST)
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    sntp_init();

    ESP_LOGI(TAG, "[SUCCESS] Podsystem sieci i SNTP działa w tle FreeRTOS.");
}
