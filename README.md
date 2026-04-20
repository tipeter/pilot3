# ESP32-S3 Pilot Firmware

Production-grade ESP-IDF v6.0 firmware. SRP/SSOT elvek mentén felépített moduláris architektúra.

## Modul felelősségek (SRP)

| Modul | Egyetlen felelőssége | SSOT tulajdon |
|---|---|---|
| `main.c` | Boot szekvencia, GPIO ISR reset | — |
| `wifi_manager` | BLE provisioning, reconnect | `device_id`, IP cím |
| `auth` | Bearer token validálás | API token |
| `nvs_config` | NVS R/W, factory partition | App config store |
| `littlefs_mgr` | LittleFS lifecycle | FS state, base path |
| `web_server` | HTTPS server, route tábla | Endpoint regisztráció |
| `static_handler` | Statikus fájlkiszolgálás | MIME type tábla |
| `system_handler` | GET /api/v1/system | — |
| `config_handler` | GET/POST /api/v1/config | — |
| `ota_handler` | OTA upload, rollback, status | OTA progress |
| `log_ws` | WebSocket log stream | WS client fd tábla |

## Flash layout (8 MB)

```
nvs        0x9000   24 KB   App NVS
otadata    0xF000    8 KB   OTA boot selector
fct_data  0x11000   12 KB   Factory NVS (pop, token, device_id)
phy_init  0x14000    4 KB   RF cal data
ota_0     0x20000  2.5 MB   App partition 0
ota_1    0x2A0000  2.5 MB   App partition 1
littlefs 0x520000  2.875 MB  Web UI assets (index.html, app.js, style.css)
```

## Quick start

```bash
# 1. TLS cert generálás (csak egyszer)
chmod +x cert_gen.sh && ./cert_gen.sh

# 2. Build, flash (firmware + LittleFS image egyszerre)
idf.py set-target esp32s3
rm -f sdkconfig && idf.py fullclean
idf.py build flash monitor
```

`idf.py flash` automatikusan flasheli a LittleFS image-et is a `www/` könyvtárból,
a `littlefs_create_partition_image()` CMake direktíva miatt.

## www/ könyvtár – LittleFS tartalom

```
www/
├── index.html   ← Teljes dashboard (shell + hivatkozások)
├── app.js       ← Teljes JS logika (Token, Api, Fmt, panelek)
└── style.css    ← Teljes CSS
```

Módosítás után elegendő csak a LittleFS partíciót újraflashelni:
```bash
idf.py build
parttool.py --port /dev/ttyUSB0 write_partition --partition-name=littlefs \
  --input build/littlefs_image.bin
```

## API végpontok

| Method | URI | Auth | Leírás |
|---|---|---|---|
| GET | / | — | index.html (LittleFS vagy fallback) |
| GET | /* | — | Statikus fájlok LittleFS-ről |
| GET | /api/v1/system | Bearer | Rendszerinfó JSON |
| GET | /api/v1/config | Bearer | NVS config JSON |
| POST | /api/v1/config | Bearer | NVS kulcs írása |
| POST | /api/v1/ota | Bearer | Firmware feltöltés |
| POST | /api/v1/ota/rollback | Bearer | Visszaállítás |
| GET | /api/v1/ota/status | Bearer | OTA státusz JSON |
| GET | /ws/log?token=… | token QP | WebSocket log stream |

## Factory NVS kulcsok (fct_data partíció)

```
device_id        (string) – Egyedi eszközazonosító + BLE név + SRP6a username
device_password  (string) – SRP6a jelszó (PoP)
api_token        (string) – API Bearer token (opcionális)
```

## OTA curl-lel

```bash
curl -k -X POST \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/esp32s3_pilot.bin \
  https://<ip>/api/v1/ota
```
