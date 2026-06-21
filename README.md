# ESP32-S31 Reference Notes

This directory is reserved for ESP32-S31 bring-up work.

## Project Layout

`platformio.ini` reuses the current OuO firmware sources from `../esp32-s3/src` and `../esp32-s3/include`.

`idf/` is a standalone ESP-IDF bring-up project for ESP32-S31. Use this path first while Arduino/PlatformIO S31 support is incomplete.

ESP-IDF bring-up:

```powershell
cd esp32-s31\idf
. C:\Espressif\tools\Microsoft.master-s31.PowerShell_profile.ps1
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p COM3 flash monitor
```

Helper scripts from `esp32-s31`:

```powershell
.\scripts\build-idf.ps1
.\scripts\preflight-s31.ps1
.\scripts\install-drivers-admin.ps1 -Rescan
.\scripts\flash-monitor-idf.ps1 -Port COM3
.\scripts\smoke-serial-s31.ps1 -Port COM3
.\scripts\smoke-serial-s31.ps1 -Port COM3 -Board korvo-1 -WifiScan
.\scripts\smoke-serial-s31.ps1 -Port COM3 -Board function-coreboard-1 -WifiScan
.\scripts\smoke-ai-home-backend.ps1 -BaseUrl http://127.0.0.1:8787
.\scripts\prepare-ai-home-lan.ps1
.\scripts\smoke-ai-home-device.ps1 -Port COM3 -Ssid <ssid> -Password <password> -ServerUrl http://<pc-ip>:8787
```

If `-Port` is omitted, `flash-monitor-idf.ps1` tries to pick a USB serial/JTAG-looking COM port.
`preflight-s31.ps1` checks the selected EIM/ESP-IDF installation, build artifacts, visible COM ports, USB device hints, and prints the exact helper flash command to use.
If Korvo appears as `CP2102N USB to UART Bridge Controller` with Windows Code 28 / failed install and no COM port, open an Administrator PowerShell in this directory and run `.\scripts\install-drivers-admin.ps1 -Rescan`, then unplug/replug the board.
After flashing, `smoke-serial-s31.ps1` opens the board serial port, runs the serial diagnostics, and saves a log under `logs/`. Add `-WifiScan` to also run the Wi-Fi scan diagnostic. Use `-Board korvo-1` for ESP32-S31-Korvo-1; it enables the Korvo RGB LED smoke step. Use `-Board function-coreboard-1` only on ESP32-S31-Function-CoreBoard-1; it enables the GPIO60 RGB LED test.

The IDF firmware starts a headless serial bring-up app at 115200 baud. Supported commands:

```text
status
diag
help
mac
partitions
storage_test
wifi_scan
wifi_status
wifi_connect <ssid> <password>
wifi_autoconnect on|off
wifi_forget
board_info
korvo_led_test [gpio]
function_led_test
mood smile|grump|angry|surprise|blink|squint|sad|blank|upset|blink2|cheeky|frown
blush on|off
option lock|static|stretch|tilt on|off
reboot
```

This gives us a known-good first target for chip boot, UART, heap reporting, partition-table verification, SPIFFS storage read/write, MAC/eFuse reads, NVS init, persistent boot counting, Wi-Fi scan diagnostics, PSRAM bring-up, and command handling before LCD/audio/camera drivers are added.

The ESP-IDF project uses `idf/partitions.csv` instead of the default 1 MB single-app layout. The factory app partition is 4 MB, with separate NVS, coredump, and storage partitions so the adaptation firmware has room for display/audio/camera diagnostics.

Korvo uses ESP32-S31-WROOM-3 with 16 MB flash and 16 MB PSRAM, so `sdkconfig.defaults` enables Octal PSRAM at 200 MHz. The Korvo smoke test requires a non-zero `psram=` value in `status`.

The active bring-up profile is ESP32-S31-Korvo-1. `korvo_led_test` defaults to GPIO37, matching the Korvo V1.1 pin table entry `WS2812_CTRL`; pass an override such as `korvo_led_test 8` only if a newer schematic confirms a different RGB LED routing. `function_led_test` is intended only for ESP32-S31-Function-CoreBoard-1, whose official board documentation maps its addressable RGB LED to GPIO60.

## AI Home Server

`server/` contains the Rust backend for the AI Home path. It is intentionally separate from the ESP-IDF project so the same API can be reused by the web console now and a mobile frontend later.

Run it:

```powershell
cd esp32-s31\server
$env:OUO_LLM_BASE_URL="http://127.0.0.1:11434/v1/chat/completions"
$env:OUO_LLM_MODEL="qwen2.5:7b"
cargo run
```

Open:

```text
http://127.0.0.1:8787/
```

Primary API:

```text
GET  /health
GET  /api/v1/state
POST /api/v1/device/heartbeat
POST /api/v1/device/command
POST /api/v1/wake
POST /api/v1/emotion/map
POST /api/v1/dialog
POST /api/v1/camera/frame
GET  /api/v1/camera/latest
GET  /api/v1/ota/manifest
POST /api/v1/ota/report
GET  /api/v1/events
```

Backend smoke test:

```powershell
.\scripts\smoke-ai-home-backend.ps1 -BaseUrl http://127.0.0.1:8787
```

LAN backend and OTA manifest preparation before device smoke:

```powershell
.\scripts\prepare-ai-home-lan.ps1
```

Device-to-server smoke test after flashing and starting the backend:

```powershell
.\scripts\smoke-ai-home-device.ps1 -Port COM3 -Ssid <ssid> -Password <password> -ServerUrl http://<pc-ip>:8787
```

Use the PC's LAN IP for `-ServerUrl`; the ESP cannot reach the backend through `127.0.0.1`. `prepare-ai-home-lan.ps1` detects that IP, republishes the OTA manifest with the LAN firmware URL, and prints the matching device smoke command.

Full closed-loop smoke after flashing:

```powershell
.\scripts\smoke-ai-home-closed-loop.ps1 -Port COM5 -Ssid <ssid> -Password <password>
```

The closed-loop script starts the Rust backend if needed, publishes OTA metadata for the detected LAN URL, runs backend HTTP smoke, and then runs the serial device smoke.

The web console at `http://<pc-ip>:8787/` can send dialog text, view the latest camera frame, inspect device state/events, and queue `set_mood` commands for the ESP to receive through `ai_home_poll`.

The ESP firmware exposes matching serial diagnostics:

```text
ai_home_status
ai_home_server <url>
ai_home_ping
ai_home_poll
ai_home_dialog <text>
ai_home_camera_snapshot
ai_home_autostart on|off
wake <phrase> [0.0-1.0]
emotion_map <text>
ota_status
ota_check
ota_update
ota_manifest_url <url>
```

`wifi_connect` stores successful Wi-Fi credentials in NVS and enables `wifi_autoconnect`, so the device can reconnect after reboot. Use `wifi_autoconnect on|off` to control boot reconnection and `wifi_forget` to clear stored credentials.

`ai_home_server` and `ai_home_autostart` are persisted in NVS. After Wi-Fi is connected, `ai_home_ping` posts a heartbeat to the Rust backend, `ai_home_poll` fetches queued server actions from `/api/v1/device/command`, and `ai_home_dialog <text>` posts text to `/api/v1/dialog`, applies the returned `device_mood`, and prints the assistant text. The device currently has no speaker dependency. Assistant audio is handled on the server console with browser speech synthesis, while the device receives mood/actions.

`ai_home_camera_snapshot` captures a 240x240 OV3660 RGB565 frame, posts it as a browser-readable BMP to `/api/v1/camera/frame`, and the web console can render it through `/api/v1/camera/latest`.

`ota_check` downloads and parses the backend OTA manifest without flashing. `ota_update` downloads the manifest, applies the manifest `firmware_url` through `esp_https_ota`, reports OTA state back to `/api/v1/ota/report`, and reboots after a successful update.

Publish the current IDF build as a local OTA artifact:

```powershell
.\scripts\build-idf.ps1
.\scripts\publish-ota.ps1 -Version 0.2.0-ai-home-link -BaseUrl http://<pc-ip>:8787
```

Available environments:

```powershell
pio run -e esp32_s31_function_coreboard_1
pio run -e esp32_s31_korvo_1
```

As of the local verification on 2026-06-09, PlatformIO `espressif32` 7.0.1 recognizes the custom `esp32s31` board manifests, but its Arduino framework package does not yet include `tools/platformio-build-esp32s31.py`. Until PlatformIO/Arduino-ESP32 ships that target support, these environments are prepared but cannot complete an Arduino build locally. ESP-IDF master/latest is the working path for S31 bring-up, and `esp32s31` currently requires the IDF `--preview` flag.

Both ESP32-S31 environments currently run in headless mode:

- `OUO_HAS_LCD=0`
- `OUO_HAS_TOUCH=0`
- `OUO_HAS_IMU=0`

The current renderer targets a GC9A01 240x240 SPI LCD used by the Waveshare ESP32-S3 round LCD board. The official ESP32-S31 Function-CoreBoard-1 has no matching LCD/touch/IMU peripherals, and ESP32-S31-Korvo-1 exposes an RGB LCD connector/LCD expansion board path instead of this SPI LCD. Keep the display disabled until an RGB LCD panel driver is added.

## Official Documents

- ESP32-S31 development kit documentation:
  https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s31/index.html
- ESP32-S31-WROOM-3 module datasheet:
  https://documentation.espressif.com/esp32-s31-wroom-3_datasheet_cn.html
- ESP32-S31 chip datasheet:
  https://documentation.espressif.com/esp32-s31_datasheet_en.html

## Development Boards

The ESP32-S31 dev-kit documentation currently lists these boards:

- ESP32-S31-Function-CoreBoard-1
  - Based on the ESP32-S31-WROOM-3 module.
  - Includes Wi-Fi, Bluetooth Classic, Bluetooth LE, and IEEE 802.15.4 support through the module.
  - Board-level peripherals include Gigabit Ethernet, USB 2.0 OTG, and onboard audio.
- ESP32-S31-Korvo-1 V1.1
  - Multimedia development board based on ESP32-S31 with ESP32-S31-WROOM-3 onboard.
  - The onboard module integrates 16 MB SPI flash and 16 MB PSRAM.
  - Includes dual microphones, LCD, camera, and microSD-related interfaces.
  - Intended for speech, wake-up, audio/video, and graphical UI development.

## Verified Hardware

ESP32-S31-Korvo-1 V1.1 was flashed and smoke-tested over CP210x UART on COM5 on 2026-06-09.

- Flash/write/hash verification passed.
- `status` reported 16 MB flash and PSRAM enabled: `psram=16771340`.
- Partition table, NVS boot counter, SPIFFS storage read/write, MAC/eFuse reads, Wi-Fi scan, and GPIO37 Korvo RGB LED test passed.
- Smoke log: `logs/s31-smoke-20260609-104735.log`.

## Module Summary

ESP32-S31-WROOM-3 is a general-purpose module based on the ESP32-S31 chip.

- CPU: dual-core 32-bit RISC-V, up to 320 MHz.
- Memory: 320 KB ROM, 512 KB shared SRAM, 32 KB low-power SRAM.
- Optional PSRAM: up to 32 MB.
- Wireless: 2.4 GHz Wi-Fi 6, Bluetooth 5.4 LE, Bluetooth Classic, Zigbee, Thread, and Matter support.
- Module variants listed in the preliminary datasheet include 8 MB, 16 MB, and 32 MB Quad SPI flash options with 16 MB Octal SPI PSRAM.

## Bring-Up Notes

- Treat the datasheets as preliminary until Espressif publishes stable revisions.
- Select the target board only after confirming which physical board is being used: Function-CoreBoard-1 and Korvo-1 have different peripherals and pin routing.
- Do not reuse ESP32-S3 LCD/touch/IMU pin assumptions from `../esp32-s3` without checking the S31 board schematic.
- PlatformIO/Arduino support for ESP32-S31 requires target support in the installed Espressif platform and Arduino core. If `pio run` reports a missing `platformio-build-esp32s31.py`, use ESP-IDF for early S31 bring-up or wait for an Arduino/PlatformIO package with S31 support.
