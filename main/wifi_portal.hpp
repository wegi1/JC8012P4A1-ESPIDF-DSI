#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief Uruchamia punkt dostępowy SoftAP (ESP32-CLOCK-SETUP) oraz serwer HTTP
     *        z formularzem konfiguracji sieci Wi-Fi.
     */
    void start_wifi_portal(void);

    /**
     * @brief Zatrzymuje serwer HTTP oraz wyłącza tryb SoftAP.
     */
    void stop_wifi_portal(void);

    /**
     * @brief Zwraca informację, czy portal konfiguracyjny jest aktualnie uruchomiony.
     *
     * @return true jeśli AP i serwer HTTP działają, false w przeciwnym razie.
     */
    bool is_wifi_portal_active(void);

    /**
     * @brief Zwraca informację, czy podczas bieżącej lub ostatniej sesji portalu
     *        użytkownik poprawnie zapisał nowe dane logowania Wi-Fi.
     *
     * @return true jeśli dane zostały zapisane, false w przeciwnym razie.
     */
    bool was_wifi_credentials_saved(void);

#ifdef __cplusplus
}
#endif
