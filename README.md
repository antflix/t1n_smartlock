# T1N ESP32 Smart Lock

ESP32-WROOM-32 firmware for the T1N Sprinter proximity smart-lock project.

## Hardware target

- Classic ESP32 / ESP-WROOM-32
- Arduino board: **ESP32 Dev Module**
- Partition scheme: **Minimal SPIFFS (Large APPS with OTA)**
- GPIO23: WT/YL master-lock pulse transistor
- GPIO19: CTM WT/RD comparator
- GPIO21: driver/left lock LED sense
- GPIO18: passenger/cargo LED sense

## Automatic firmware builds

Every relevant push to `main` automatically compiles the sketch with GitHub Actions. The build uses:

`esp32:esp32:esp32:PartitionScheme=min_spiffs`

When the repository Actions secrets are configured, the workflow publishes a moving `latest` release containing:

- `firmware.bin` — OTA application image
- `firmware.sha256` — SHA-256 of the application image
- `manifest.json` — build metadata, download URL, and SHA-256

Stable OTA URL:

`https://github.com/antflix/t1n_smartlock/releases/download/latest/firmware.bin`

Stable manifest URL:

`https://github.com/antflix/t1n_smartlock/releases/download/latest/manifest.json`

## One-time GitHub setup

This repository is public, so Wi-Fi credentials are **not committed to source control**. Add these repository Actions secrets:

- `T1N_WIFI_SSID`
- `T1N_WIFI_PASS`

The workflow generates `firmware/t1n_smartlock/secrets.h` only inside the private Actions runner before compiling. If the secrets are absent, CI still performs a compile test with placeholder credentials but deliberately does **not** publish that build as the latest OTA firmware.

## Sketch layout

The active Arduino sketch is in `firmware/t1n_smartlock/`.

- `t1n_smartlock.ino` — core lock, GPIO and CTM-state logic
- `00_web_assets.ino` — includes the embedded WebUI header after the core declarations
- `web_assets.h` — WebUI HTML/JavaScript assets kept outside Arduino's automatic function-prototype parser
- `01_ble.ino` — BLE pairing, IRK/RPA matching and proximity logic
- `03_ota.ino` — pull-from-URL OTA and SHA-256 verification
- `04_server.ino` — WebServer routes, setup and loop

For local Arduino builds, copy `firmware/t1n_smartlock/secrets.example.h` to `firmware/t1n_smartlock/secrets.h`, enter the Wi-Fi credentials, select **ESP32 Dev Module**, and choose **Minimal SPIFFS (Large APPS with OTA)**.

## Current firmware behavior

- Phone identity is resolved with the bonded iPhone IRK.
- Approach unlock threshold defaults to -80 dBm.
- Departure lock threshold defaults to -95 dBm for 10 seconds, or no matched phone for 10 seconds.
- Vehicle lock state is based on the **driver/left LED only**: solid means locked. CTM sleep does not erase the remembered lock state.
- Right/passenger LED remains available for blink/door detection.
- Desired LOCK/UNLOCK commands use **one 500 ms WT/YL toggle pulse only**. There is no automatic second wake/toggle pulse.
- WebUI supports local `.bin` OTA and pull-from-URL OTA.
