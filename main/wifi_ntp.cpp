#include "wifi_ntp.hpp"
#include "rtc.hpp"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

extern "C"
{
#include "rx8025t.h"
}

// Deklaracja Twojej istniejącej funkcji z main.cpp
extern void sync_internal_rtc_with_rx8025t(void);
extern volatile bool ntp_read;
static const char *TAG = "WIFI_NTP";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_wifi_stack_initialized = false;
static volatile bool s_sync_in_progress = false;
static int s_retry_num = 0;
static const int MAX_RETRY = 3;

static void print_sram_debug(const char *etap)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    printf("[SRAM - %s] Wolne teraz: %u KB | Minimum w historii: %u KB\n",
           etap, free_internal / 1024, min_free_internal / 1024);
}

bool is_ntp_sync_running(void)
{
    return s_sync_in_progress;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAX_RETRY)
        {
            s_retry_num++;
            ESP_LOGW(TAG, "Ponawiam polaczenie z Wi-Fi (%d/%d)...", s_retry_num, MAX_RETRY);
            esp_wifi_connect();
        }
        else
        {
            if (s_wifi_event_group != NULL)
            {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, mac);

        ESP_LOGI(TAG, "=== DANE SIECIOWE ESP32-C6 ===");
        ESP_LOGI(TAG, "Adres MAC : %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Adres IP  : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Maska     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Brama GW  : " IPSTR, IP2STR(&event->ip_info.gw));

        s_retry_num = 0;
        if (s_wifi_event_group != NULL)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}
static volatile bool s_sntp_synced = false;
static void time_sync_notification_cb(struct timeval *tv)
{
    s_sntp_synced = true;
}

void vTaskNtpSyncWorker(void *pvParameters)
{
    print_sram_debug("START ZADANIA NTP");
    ESP_LOGI(TAG, "Start zadania synchronizacji NTP na rdzeniu %d", xPortGetCoreID());

    char ssid[64] = {0};
    char pass[64] = {0};

    // 1. Odczyt danych sieciowych z SPIFFS (Twoja funkcja z RTC.CPP)
    if (!load_wifi_credentials_from_SPIFFS(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        ESP_LOGE(TAG, "Brak pliku /spiffs/prefs.ini lub bledne dane! Przerywam bezpiecznie.");
        s_sync_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    // strcpy(ssid, "BAD_SSID");
    // 2. Jednorazowa inicjalizacja stosu sieciowego (nigdy nie powtarzana przy kolejnych próbach!)
    if (!s_wifi_stack_initialized)
    {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Blad esp_netif_init: %s", esp_err_to_name(err));
            s_sync_in_progress = false;
            vTaskDelete(NULL);
            return;
        }

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Blad event_loop: %s", esp_err_to_name(err));
            s_sync_in_progress = false;
            vTaskDelete(NULL);
            return;
        }

        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Blad esp_wifi_init: %s", esp_err_to_name(err));
            s_sync_in_progress = false;
            vTaskDelete(NULL);
            return;
        }

        s_wifi_stack_initialized = true;
    }

    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    if (esp_wifi_start() == ESP_OK)
    {
        print_sram_debug("PO ESP_WIFI_START");

        // Czekamy maksymalnie 15 sekund na połączenie lub błąd
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "Polaczono z Wi-Fi! Pobieram czas z NTP...");
            s_sntp_synced = false;
            esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pl.pool.ntp.org");
            esp_sntp_setservername(1, "time.google.com");
            sntp_set_time_sync_notification_cb(time_sync_notification_cb);
            esp_sntp_init();

            int sntp_wait = 0;
            while (!s_sntp_synced && sntp_wait < 20)
            {
                vTaskDelay(pdMS_TO_TICKS(500));
                sntp_wait++;
            }

            esp_sntp_stop();

            if (s_sntp_synced)
            {

                time_t now = 0;
                struct tm timeinfo = {};
                time(&now);
                localtime_r(&now, &timeinfo);
                // Zapisujemy pobrany czas do fizycznego zegara RX8025T
                rx8025t_time_t rtc_time;
                rtc_time.second = timeinfo.tm_sec;
                rtc_time.minute = timeinfo.tm_min;
                rtc_time.hour = timeinfo.tm_hour;
                rtc_time.day = timeinfo.tm_mday;
                rtc_time.month = timeinfo.tm_mon + 1;
                rtc_time.year = timeinfo.tm_year + 1900;
                rtc_time.weekday = timeinfo.tm_wday;

                if (rx8025t_set_time(&rtc_time) == ESP_OK)
                {
                    sync_internal_rtc_with_rx8025t();
                    ntp_read = true;
                    ESP_LOGI(TAG, "SUKCES NTP! Zapisano do RX8025T: %04d-%02d-%02d %02d:%02d:%02d",
                             rtc_time.year, rtc_time.month, rtc_time.day,
                             rtc_time.hour, rtc_time.minute, rtc_time.second);
                }
                else
                {
                    ESP_LOGE(TAG, "Blad zapisu czasu NTP do RX8025T!");
                }
            }
            else
            {
                ESP_LOGW(TAG, "Timeout serwera NTP (brak odpowiedzi w 10s). Zegarek pracuje dalej na RX8025T.");
            }
        }
        else
        {
            ESP_LOGW(TAG, "Nie udalo sie polaczyc z Wi-Fi w 15s. Zamykam bezpiecznie bez crasha.");
        }
    }

    // Bezpieczne sprzątanie przed zakończeniem zadania:
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);

    esp_wifi_disconnect();
    esp_wifi_stop();

    if (s_wifi_event_group != NULL)
    {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    print_sram_debug("PO ZAMKNIECIU WIFI");
    ESP_LOGI(TAG, "Zadanie NTP zakonczone czysto. Radio Wi-Fi wylaczone.");
    s_sync_in_progress = false;
    vTaskDelete(NULL);
}

void start_ntp_sync_task_async(void)
{

    if (s_sync_in_progress)
    {
        ESP_LOGW(TAG, "Synchronizacja NTP juz trwa - ignoruje kolejne wywolanie.");
        return;
    }
    ntp_read = false;
    s_sync_in_progress = true;
    BaseType_t res = xTaskCreatePinnedToCore(
        vTaskNtpSyncWorker,
        "NtpSyncTask",
        6144,
        NULL,
        3,
        NULL,
        1);

    if (res != pdPASS)
    {
        ESP_LOGE(TAG, "Nie udalo sie utworzyc zadania NtpSyncTask!");
        s_sync_in_progress = false;
    }
}