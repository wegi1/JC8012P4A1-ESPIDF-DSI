#include "wifi_portal.hpp"
#include "RTC.hpp" // Wymagane dla: save_wifi_credentials_to_SPIFFS() i start_ntp_sync_task_async()
#include "wifi_ntp.hpp"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "WIFI_PORTAL";

// Konfiguracja SoftAP
static constexpr const char *AP_SSID = "ESP32-CLOCK-SETUP";
static constexpr const char *AP_PASS = ""; // Puste hasło = sieć otwarta (lub ustaw min. 8 znaków)
static constexpr uint8_t AP_CHANNEL = 1;
static constexpr uint8_t AP_MAX_CONN = 4;

// Flagi stanu (bezpieczne wielowątkowo)
static std::atomic<bool> s_portal_active{false};
static std::atomic<bool> s_credentials_saved{false};

// Uchwyty systemowe
static httpd_handle_t s_server = nullptr;
static esp_netif_t *s_ap_netif = nullptr;

// Strona HTML z formularzem konfiguracji
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="pl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Konfiguracja Wi-Fi</title>
    <style>
        body { font-family: Arial, sans-serif; background-color: #f4f4f9; color: #333; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        .card { background: #fff; padding: 25px; border-radius: 10px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); width: 100%; max-width: 340px; }
        h2 { margin-top: 0; text-align: center; color: #2c3e50; }
        label { display: block; margin-bottom: 5px; font-weight: bold; font-size: 14px; }
        input[type="text"], input[type="password"] { width: 100%; padding: 10px; margin-bottom: 15px; border: 1px solid #ccc; border-radius: 5px; box-sizing: border-box; font-size: 16px; }
        button { width: 100%; padding: 12px; background-color: #2ecc71; border: none; border-radius: 5px; color: white; font-size: 16px; font-weight: bold; cursor: pointer; }
        button:hover { background-color: #27ae60; }
    </style>
</head>
<body>
    <div class="card">
        <h2>Konfiguracja Zegara</h2>
        <form action="/save" method="POST">
            <label for="ssid">Nazwa sieci Wi-Fi (SSID):</label>
            <input type="text" id="ssid" name="ssid" required maxlength="32" placeholder="Wpisz SSID">
            
            <label for="password">Hasło Wi-Fi:</label>
            <input type="password" id="password" name="password" maxlength="64" placeholder="Wpisz hasło">
            
            <button type="submit">Zapisz i połącz</button>
        </form>
    </div>
</body>
</html>
)rawliteral";

static const char SUCCESS_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html lang="pl">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Zapisano</title>
    <style>
        body { font-family: Arial, sans-serif; background-color: #f4f4f9; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; text-align: center; }
        .card { background: #fff; padding: 25px; border-radius: 10px; box-shadow: 0 4px 10px rgba(0,0,0,0.1); max-width: 340px; }
        h2 { color: #27ae60; }
    </style>
</head>
<body>
    <div class="card">
        <h2>Dane zapisane!</h2>
        <p>Urządzenie zamyka punkt dostępowy, łączy się z siecią Wi-Fi i synchronizuje czas NTP.</p>
    </div>
</body>
</html>
)rawliteral";

/**
 * @brief Dekoduje znaki zakodowane w formacie application/x-www-form-urlencoded.
 */
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 1)
    {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2]))
        {
            char hex[3] = {src[1], src[2], '\0'};
            dst[i++] = (char)strtol(hex, nullptr, 16);
            src += 3;
        }
        else if (*src == '+')
        {
            dst[i++] = ' ';
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/**
 * @brief Wyciąga wartość konkretnego pola z ciała żądania POST (URL-encoded).
 */
static bool extract_form_field(const char *body, const char *field_name, char *out_val, size_t out_size)
{
    size_t name_len = strlen(field_name);
    const char *p = body;

    while (p && *p)
    {
        if (strncmp(p, field_name, name_len) == 0 && p[name_len] == '=')
        {
            const char *val_start = p + name_len + 1;
            const char *val_end = strchr(val_start, '&');
            size_t encoded_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);

            char *temp = (char *)malloc(encoded_len + 1);
            if (!temp)
                return false;

            memcpy(temp, val_start, encoded_len);
            temp[encoded_len] = '\0';
            url_decode(out_val, temp, out_size);
            free(temp);
            return true;
        }
        p = strchr(p, '&');
        if (p)
            p++;
    }
    return false;
}

/**
 * @brief Osobne zadanie FreeRTOS zamykające portal z małym opóźnieniem,
 *        aby serwer HTTP zdążył wysłać odpowiedź SUCCESS_HTML do przeglądarki.
 */
static void shutdown_and_sync_task(void *pvParameters)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); // Czas na dostarczenie strony potwierdzenia HTTP

    ESP_LOGI(TAG, "Zamykanie portalu i uruchamianie synchronizacji NTP...");
    stop_wifi_portal();

    // Uruchomienie funkcji z RTC.hpp
    start_ntp_sync_task_async();

    vTaskDelete(nullptr);
}

// Handler GET /
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler POST /save
static esp_err_t save_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 512)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nieprawidlowa dlugosc danych");
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(total_len + 1);
    if (!buf)
    {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    while (received < total_len)
    {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0)
        {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[total_len] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};

    bool has_ssid = extract_form_field(buf, "ssid", ssid, sizeof(ssid));
    extract_form_field(buf, "password", password, sizeof(password)); // Hasło może być puste (sieć otwarta)
    free(buf);

    if (!has_ssid || strlen(ssid) == 0)
    {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Brak nazwy sieci SSID");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Otrzymano dane dla sieci SSID: %s", ssid);

    // Wywołanie funkcji zapisu z RTC.hpp
    if (save_wifi_credentials_to_SPIFFS(ssid, password))
    {
        s_credentials_saved.store(true);

        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, SUCCESS_HTML, HTTPD_RESP_USE_STRLEN);

        // Asynchroniczne zamknięcie AP i start NTP
        xTaskCreate(shutdown_and_sync_task, "portal_shutdown", 4096, nullptr, 5, nullptr);
        return ESP_OK;
    }
    else
    {
        ESP_LOGE(TAG, "Blad zapisu danych Wi-Fi do SPIFFS!");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
}

bool is_wifi_portal_active(void)
{
    return s_portal_active.load();
}

bool was_wifi_credentials_saved(void)
{
    return s_credentials_saved.load();
}

void start_wifi_portal(void)
{
    if (s_portal_active.load())
    {
        ESP_LOGW(TAG, "Portal Wi-Fi jest juz aktywny.");
        return;
    }
    if (!ensure_wifi_stack_initialized())
    {
        return;
    }

    s_credentials_saved.store(false);

    // 1. Utworzenie interfejsu SoftAP (jeśli nie istnieje)
    if (s_ap_netif == nullptr)
    {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    // 2. Ustawienie statycznego IP: 192.168.4.1
    esp_netif_dhcps_stop(s_ap_netif);

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap_netif));

    // 3. Konfiguracja parametrów Wi-Fi AP
    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.ap.ssid, AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(AP_SSID);
    wifi_config.ap.channel = AP_CHANNEL;
    wifi_config.ap.max_connection = AP_MAX_CONN;

    if (strlen(AP_PASS) == 0)
    {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    else
    {
        strncpy((char *)wifi_config.ap.password, AP_PASS, sizeof(wifi_config.ap.password));
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    // Zatrzymaj ewentualne poprzednie połączenia przed zmianą trybu
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 4. Uruchomienie serwera HTTP
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&s_server, &config) == ESP_OK)
    {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = nullptr};
        httpd_register_uri_handler(s_server, &root_uri);

        httpd_uri_t save_uri = {
            .uri = "/save",
            .method = HTTP_POST,
            .handler = save_post_handler,
            .user_ctx = nullptr};
        httpd_register_uri_handler(s_server, &save_uri);
        s_portal_active.store(true);
        ESP_LOGI(TAG, "Portal uruchomiony. SSID: %s, IP: 192.168.4.1", AP_SSID);
    }
    else
    {
        ESP_LOGE(TAG, "Nie udalo sie uruchomic serwera HTTP!");
    }
}
void stop_wifi_portal(void)
{
    if (!s_portal_active.load())
    {
        return;
    }
    if (s_server != nullptr)
    {
        httpd_stop(s_server);
        s_server = nullptr;
    }
    esp_wifi_stop();
    s_portal_active.store(false);
    ESP_LOGI(TAG, "Portal Wi-Fi zostal zamkniety.");
}