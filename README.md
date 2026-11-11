# Project JC8012P4A1 - ESP32-P4 Clock Panel (MIPI-DSI & ESP32-C6)

---

## 🇺🇸 English Version

A low-level, highly optimized framework for a vector clock, TrueType font rendering, and both automatic and manual NTP time synchronization designed for the ESP32-P4 paired with an 800x1280 MIPI-DSI display (Portrait). Networking and coprocessor tasks are offloaded to the ESP32-C6 via a high-speed hardware SDIO bus.

### 🛠️ Important Instructions Before First Compilation

If you cloned this repository and want to successfully compile and run the project on your hardware, you must perform two essential configuration steps:

#### 1. Generating the SPIFFS Partition Image (Photos/Resources)
The project stores slideshow graphics on a dedicated Flash partition. To force the build system to automatically generate and flash the SPIFFS image during the project compilation:
* Open the `main/CMakeLists.txt` file.
* Locate and **uncomment** (remove the `#` character) the following line:
  ```cmake
  spiffs_create_partition_image(storage "\${CMAKE_CURRENT_LIST_DIR}/../FOTOS" FLASH_IN_PROJECT)
  ```
* Ensure that you have a folder named `FOTOS` containing the required image files (`FOTO1.jpg`, `FOTO2.jpg`) in the root directory of the project (next to the `main` folder) before starting the build.

#### 2. Initial Wi-Fi Configuration (Saving Credentials to NVS)
The system securely retrieves network credentials directly from the non-volatile storage (NVS) flash memory. Upon flashing the device for the first time, you must store your network credentials into the Flash structure:
* Open the `main/RTC.cpp` file.
* Navigate to the `init_RTC_subsystem(void)` function.
* **Uncomment** the following line for a single run, filling in your actual network credentials (SSID and Password):
  ```cpp
  save_wifi_credentials_to_NVS("YOUR_SSID", "YOUR_PASSWORD");
  ```
* Compile and flash the device. Once the device boots up for the first time and successfully commits the data to NVS, **comment this line out again** to avoid redundant Flash cell overwrites on subsequent reboots.

#### ⚙️ Build Optimization Recommendations
To guarantee native, hardware-level fluidness of the MIPI-DSI bus, eliminate frame tearing, and maintain strict timing for the `vTaskHardwareTouchDriver` background task, **the entire project must be compiled using the `-Og` mode** (Optimization for Debugging) via menuconfig. This mode ensures stable interrupt handling and keeps the rendering engine perfectly synchronized with the display's vertical sync (VSYNC) interrupts.

---

## 🇵🇱 Wersja Polska

Niskopoziomowy, wysoko zoptymalizowany system operacyjny zegara wektorowego, renderowania grafiki TrueType oraz automatycznej i ręcznej synchronizacji czasu NTP dla układu ESP32-P4 z wyświetlaczem MIPI-DSI 800x1280 (Portrait). Sieć i zadania koprocesora są obsługiwane przez układ ESP32-C6 za pośrednictwem szybkiej magistrali sprzętowej SDIO.

### 🛠️ Ważne instrukcje przed pierwszą kompilacją

Jeśli pobrałeś ten projekt i chcesz go pomyślnie skompilować oraz uruchomić na swoim urządzeniu, musisz wykonać dwa kluczowe kroki konfiguracyjne:

#### 1. Generowanie obrazu partycji SPIFFS (Zdjęcia/Zasoby)
Projekt przechowuje grafiki pokazu slajdów na dedykowanej partycji Flash. Aby system automatycznie zbudował i wgrał obraz pamięci SPIFFS podczas kompilacji projektu:
* Otwórz plik `main/CMakeLists.txt`.
* Znajdź i **odkomentuj** (usuń znak `#`) poniższą linię:
  ```cmake
  spiffs_create_partition_image(storage "\${CMAKE_CURRENT_LIST_DIR}/../FOTOS" FLASH_IN_PROJECT)
  ```
* Upewnij się, że w głównym katalogu projektu (obok folderu `main`) posiadasz folder `FOTOS` zawierający pliki graficzne (`FOTO1.jpg`, `FOTO2.jpg`) przed uruchomieniem budowania.

#### 2. Pierwsza konfiguracja sieci Wi-Fi (Zapis w pamięci NVS)
Projekt korzysta z bezpiecznego odczytu danych logowania do sieci bezpośrednio z nieulotnej pamięci flash (NVS). Przy pierwszym uruchomieniu urządzenia na nowej płytce należy fizycznie zapisać te dane w strukturze Flash:
* Otwórz plik `main/RTC.cpp`.
* Przejdź do funkcji `init_RTC_subsystem(void)`.
* **Odkomentuj** na jedno uruchomienie poniższą linię, wpisując swoje rzeczywiste dane sieciowe (SSID oraz Hasło):
  ```cpp
  save_wifi_credentials_to_NVS("TWOJE_SSID", "TWOJE_HASLO");
  ```
* Skompiluj i sflaszuj urządzenie. Po pierwszym uruchomieniu i pomyślnym zapisie danych w pamięci NVS, **ponownie zakomentuj tę linię**, aby nie nadpisywać niepotrzebnie komórek Flash przy każdym kolejnym restarcie urządzenia.

#### ⚙️ Zalecenia dotyczące optymalizacji kompilacji
Dla zapewnienia idealnej, sprzętowej płynności magistrali MIPI-DSI, braku szarpania klatek oraz stabilnego timingu wątku sprzętowego dotyku `vTaskHardwareTouchDriver`, **cały projekt należy kompilować w trybie `-Og`** (Optimization for Debugging) w menuconfig. Tryb ten zapewnia wzorcową stabilność obsługi przerw oraz pełną synchronizację renderera z przerwaniami pionowymi wyświetlacza (VSYNC).
