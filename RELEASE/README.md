# 📦 Pre-compiled Firmware Release (`RELEASE/`) — Guition JC8012P4A1 (10.1" 800×1280)

## 🇺🇸 English Version

### What is inside this folder?
In this folder, you will find a compressed **7-Zip archive (`.7z`)** containing a **complete, pre-compiled, all-in-one binary image (`.bin`)** for the **Guition JC8012P4A1 (ESP32-P4, 10.1" MIPI-DSI 800×1280)** board.

This single merged `.bin` file already includes everything required to run the device out of the box without installing ESP-IDF or compiling the source code:
* **2nd Stage Bootloader** (`0x2000`)
* **Partition Table** (`0x9000` / `0x8000` depending on bootloader size)
* **Main Application Firmware** (`0x10000`)
* **SPIFFS Storage Partition** (`0x190000`) with sample slideshow images and a template `prefs.ini` file

---

### Step 1: Extract the `.7z` Archive
Before flashing, you **must extract** the `.7z` archive directly inside this `RELEASE/` folder (or any folder on your computer) using **7-Zip** (Windows), **Keka / The Unarchiver** (macOS), or `7z x filename.7z` (Linux) to obtain the raw **`.bin`** firmware file (`firmware_JC8012P4A1_v1.0.bin`).

---

### Step 2: Flash the `.bin` File (Start Address: `0x00000000`)

#### Option A: Using a Web Browser (ESPConnect / ESPTool-JS — No Installation Required)
1. Open **[ESPConnect](https://github.io)** or **[Official Espressif ESPTool-JS](https://github.io)** in a Chromium-based browser (Chrome, Edge, Opera).
2. Connect your ESP32-P4 board via USB and click **Connect** (select the corresponding COM / ttyUSB port).
3. *(Optional)* Click **Erase Flash** for a completely clean installation.
4. Set the **Flash Start Address** strictly to **`0x00000000`** (or `0x0`).
5. Select the extracted **`.bin`** file and click **Program / Flash**.
6. Once flashing reaches 100%, press the physical **RESET** button on the board.

#### Option B: Using Standalone `esptool` (Python — Without Full ESP-IDF)
If you don't have ESP-IDF installed, install `esptool` via `pip install esptool`, replace `COMx` with your serial port (e.g., `COM6` or `/dev/ttyACM0`), and run:

* **Optional – Full Flash Erase:**
  ```bash
  python -m esptool --chip esp32p4 -p COMx -b 460800 erase_flash
  ```
* **Flash the Merged Binary:**
  ```bash
  python -m esptool --chip esp32p4 -p COMx -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x00000000 firmware_JC8012P4A1_v1.0.bin
  ```

#### Option C: Using `idf.py` (If You Already Have ESP-IDF Installed)
From the root directory of the cloned repository (inside an active ESP-IDF terminal):

* **Optional – Full Flash Erase:**
  ```bash
  idf.py -p COMx erase-flash
  ```
* **Flash the Merged Binary (`esptool.py` included in ESP-IDF environment):**
  ```bash
  esptool.py --chip esp32p4 -p COMx -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x00000000 RELEASE/firmware_JC8012P4A1_v1.0.bin
  ```

> **Note:** After flashing completes, press the physical **RESET** button on the board if the display does not wake up immediately.

---

## 🇵🇱 Wersja Polska

### Co znajduje się w tym folderze?
W tym katalogu znajduje się skompresowane archiwum **7-Zip (`.7z`)**, które zawiera **kompletny, gotowy do wgrania, połączony obraz binarny (`.bin`)** dla płytki **Guition JC8012P4A1 (ESP32-P4, ekran 10.1" MIPI-DSI 800×1280)**.

Ten pojedynczy plik `.bin` zawiera już wszystkie niezbędne elementy potrzebne do natychmiastowego uruchomienia urządzenia bez konieczności instalowania środowiska ESP-IDF i kompilowania projektu ze źródeł:
* **Bootloader drugiego stopnia** (`0x2000`)
* **Tabelę partycji** (`0x9000` / `0x8000` w zależności od rozmiaru bootloadera)
* **Główny program aplikacji** (`0x10000`)
* **Partycję danych SPIFFS** (`0x190000`) wraz z przykładowymi zdjęciami pokazu slajdów oraz szablonem pliku `prefs.ini`

---

### Krok 1: Rozpakuj archiwum `.7z`
Przed przystąpieniem do wgrywania wsadu należy **rozpakować** plik `.7z` w tym folderze `RELEASE/` (lub dowolnym innym katalogu na dysku) za pomocą programu **7-Zip** (Windows), **Keka / The Unarchiver** (macOS) lub polecenia `7z x nazwa_pliku.7z` (Linux), aby uzyskać właściwy plik wsadowy **`.bin`** (`firmware_JC8012P4A1_v1.0.bin`).

---

### Krok 2: Wgraj plik `.bin` (Adres początkowy: `0x00000000`)

#### Opcja A: Przez przeglądarkę internetową (ESPConnect / ESPTool-JS — bez instalacji programów)
1. Otwórz narzędzie **[ESPConnect](https://github.io)** lub oficjalny **[Espressif ESPTool-JS](https://github.io)** w przeglądarce opartej na silniku Chromium (Chrome, Edge, Opera).
2. Podłącz płytkę ESP32-P4 kablem USB do komputera i kliknij **Connect** (wybierz odpowiedni port COM / ttyUSB).
3. *(Opcjonalnie)* Kliknij **Erase Flash**, aby wyczyścić pamięć przed wgraniem nowego systemu.
4. Ustaw **adres początkowy (Flash Address)** dokładnie na **`0x00000000`** (lub `0x0`).
5. Wskaż rozpakowany plik **`.bin`** i kliknij **Program / Flash**.
6. Po zakończeniu wgrywania naciśnij fizyczny przycisk **RESET** na płytce.

#### Opcja B: Przez samodzielny `esptool` (Python — bez pełnego środowiska ESP-IDF)
Jeśli nie masz zainstalowanego ESP-IDF, zainstaluj sam `esptool` poleceniem `pip install esptool`, zamień `COMx` na swój port szeregowy (np. `COM6` lub `/dev/ttyACM0`) i wykonaj:

* **Opcjonalnie – Pełne czyszczenie pamięci Flash:**
  ```bash
  python -m esptool --chip esp32p4 -p COMx -b 460800 erase_flash
  ```
* **Wgrywanie gotowego wsadu pod adres `0x00000000`:**
  ```bash
  python -m esptool --chip esp32p4 -p COMx -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x00000000 firmware_JC8012P4A1_v1.0.bin
  ```

#### Opcja C: Bezpośrednio spod `idf.py` (Jeśli masz już zainstalowane środowisko ESP-IDF)
Z głównego katalogu sklonowanego repozytorium (w otwartym terminalu ESP-IDF):

* **Opcjonalnie – Pełne czyszczenie pamięci Flash:**
  ```bash
  idf.py -p COMx erase-flash
  ```
* **Wgrywanie gotowego wsadu (`esptool.py` wbudowany w ESP-IDF):**
  ```bash
  esptool.py --chip esp32p4 -p COMx -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x00000000 RELEASE/firmware_JC8012P4A1_v1.0.bin
  ```

> **Uwaga:** Po zakończeniu wgrywania naciśnij fizyczny przycisk **RESET** na płytce, jeśli wyświetlacz nie wybudzi się automatycznie.
