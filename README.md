# ESP32-P4 (ECO2) Native ESP-IDF MIPI-DSI 10" Project

🌐 **Language / Język:** [English](#-english-version) | [Wersja Polska](#-wersja-polska)

---

## 🇬🇧 English Version

### 🚀 Status & Hardware Revision
This repository provides a verified, native **ESP-IDF v5.5.3** framework and display driver for a 10-inch MIPI-DSI panel (800x1280, JD9365 controller, GSL3680 touch) designed **exclusively for early revision ESP32-P4 v1.3 (ECO2)** development boards (OLD BATCH). It resolves timing artifacts and bus freezes by enforcing a stable 60MHz pixel clock.

### 🛠 Architecture & Performance
- **Environment:** Pure ESP-IDF v5.5.3 with `-O3` compiler optimizations and Docker devcontainer support.
- **Graphics Strategy:** Implements bare-metal scanlines via direct register access (`0x50120040`) and hardware page flipping in PSRAM for smooth 60 FPS rendering without heavy UI frameworks.
- **Roadmap:** Upcoming integration of GSL3680 touch panel, vector fonts (`stb_truetype.h`), and a vector clock engine.

---

## 🇵🇱 Wersja Polska

### 🚀 Status i rewizja sprzętowa
Repozytorium zawiera zweryfikowaną, natywną konfigurację **ESP-IDF v5.5.3** dla 10-calowego panelu MIPI-DSI (800x1280, kontroler JD9365, dotyk GSL3680) przeznaczoną **wyłącznie dla wczesnej rewizji ESP32-P4 v1.3 (ECO2)** (OLD BATCH). Rozwiązuje problemy z timingami i stabilizuje zegar pikseli na poziomie 60MHz.

### 🛠 Architektura i wydajność
- **Środowisko:** Czyste ESP-IDF v5.5.3 z optymalizacją `-O3` oraz wsparciem dla kontenerów deweloperskich Docker.
- **Strategia graficzna:** Autorski mechanizm synchronizacji z rastrem (odczyt rejestru `0x50120040`) i sprzętowe przełączanie stron w PSRAM zapewniają płynne 60 FPS bez narzutu ciężkich bibliotek UI.
- **Plany rozwoju:** Wdrożenie obsługi dotyku GSL3680, czcionek wektorowych (`stb_truetype.h`) oraz silnika zegara wektorowego.
