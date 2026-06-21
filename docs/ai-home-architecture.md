# OuO AI Home Architecture

This project is split into three low-coupling parts:

- `idf/`: ESP32-S31 firmware for display, touch, camera bring-up, serial diagnostics, OTA partition readiness, wake/emotion commands.
- `server/`: Rust AI Home backend for assistant state, local LLM proxying, camera frame ingestion, WebSocket events, browser-based server speech, and OTA manifests.
- `server/static/`: Web console using the same HTTP/WebSocket API that a future mobile client should use.

## Runtime Flow

1. Device connects to Wi-Fi with `wifi_connect <ssid> <password>`; successful credentials are stored in NVS and can reconnect on boot through `wifi_autoconnect`.
2. `ai_home_server <url>` stores the Rust backend URL in NVS.
3. `ai_home_ping` reports device health to `POST /api/v1/device/heartbeat`.
4. Wake events go to `POST /api/v1/wake`; manual wake diagnostics remain available with `wake <phrase> [confidence]`.
5. `ai_home_dialog <text>` posts dialog text to `POST /api/v1/dialog`; the server calls the local OpenAI-compatible model endpoint configured by `OUO_LLM_BASE_URL`.
6. The server maps assistant/user context to a device mood and returns a stable `device_mood`; the device applies it to the OuO renderer.
7. `ai_home_camera_snapshot` captures one OV3660 frame, uploads it to `POST /api/v1/camera/frame`, and the web console renders `GET /api/v1/camera/latest`.
8. OTA metadata is served by `GET /api/v1/ota/manifest`; the device can verify it with `ota_check` and apply it with `ota_update`.

## Stable API Surface

- `GET /health`
- `GET /api/v1/state`
- `POST /api/v1/device/heartbeat`
- `POST /api/v1/wake`
- `POST /api/v1/emotion/map`
- `POST /api/v1/dialog`
- `POST /api/v1/camera/frame`
- `GET /api/v1/camera/latest`
- `GET /api/v1/ota/manifest`
- `POST /api/v1/ota/report`
- `GET /api/v1/events` WebSocket

Future native phone clients should use the API directly and should not depend on web console internals.

## Local Model

Default local LLM endpoint:

```powershell
$env:OUO_LLM_BASE_URL="http://127.0.0.1:11434/v1/chat/completions"
$env:OUO_LLM_MODEL="qwen2.5:7b"
```

Any OpenAI-compatible local server can be used by changing those two variables.

## OTA

The ESP partition table now includes `factory`, `ota_0`, `ota_1`, and `otadata`.

For a dev OTA package:

1. Build `idf/build/ouo_s31_bringup.bin`.
2. Run `scripts/publish-ota.ps1 -Version <version> -BaseUrl http://<pc-ip>:8787`.
3. The script copies the binary to `server/ota/ouo_s31_bringup.bin` and updates `server/ota/manifest.json` with `version`, `sha256`, and `size`.
4. Confirm from the device with `ota_check`.
5. Apply from the device with `ota_update`; successful updates reboot into the new image.
