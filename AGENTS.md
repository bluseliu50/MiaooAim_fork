# Repository Guidelines

Guidance for AI assistants working in this codebase. This is an **ESP32-S3 firmware** project (`epaper_uploader`, `PROJECT_VER` in `CMakeLists.txt`, currently **v2.3.3**) — a tri-color e-paper display terminal with a built-in web management UI. License is **PolyForm Noncommercial** (see `LICENSE`, `COMMERCIAL_LICENSE.md`); not OSI-open-source.

## Project Overview

A low-power ESP32-S3 e-paper terminal that displays images, weather, clocks, calendars, timetables, todos, countdowns, and a Codex/quota dashboard. It is managed entirely through a responsive web UI served from the device (SoftAP `ESP32_EPD_xxxxxx` / STA `http://epdxxxx.local/`). Key capabilities: JPEG/PNG/BMP upload with Floyd–Steinberg dithering, gallery slideshow, QWeather HTTPS integration, deep-sleep with dual boot paths, OTA dual-partition upgrades, HTTP Basic Auth, and a WYSIWYG canvas board editor.

Primary tested panels: **4.2" SSD1619 (400×300 BWR)** and **5.83" UC8179 (648×480 BWR)**. Many experimental panels (ATC/Solum, EPD-nRF5 reference ports) are registered in `main/epd.h` but **not all are hardware-verified** — treat them as experimental.

## Architecture & Data Flow

### Dual boot path (core design)
`main/app_main.c` branches on wake cause:

- **`power_mgr_is_timer_wake()` true → `quick_refresh_and_sleep()`**: minimal init (STA only, no HTTP/SoftAP), refresh the NVS-remembered display mode, return to deep sleep. Cannot serve web during this phase.
- **Otherwise → `full_boot()`**: NVS → power_mgr → battery_mon → SPIFFS mount → WiFi AP+STA → mDNS → `epd_load_panel_from_nvs()` → `fb_reserve_planes_early()` → `http_app_start()` → EPD init → all feature module `*_init()` → `register_display_modes()` → boot display route → button task → `power_mgr_arm()`.

### Display arbitration (`main/display_policy.{c,h}`)
Multiple producers (slideshow, clock, weather, manual HTTP show, canvas, calendar) compete for the single EPD. `display_policy` provides a global **display epoch**: `display_policy_begin_manual_display()` returns a token; long-running renders check `display_policy_epoch_is_current()` to detect cancellation by a newer request. Clock/calendar auto-refresh and background weather fetch are gated by `display_policy_*_may_*` predicates. **Any new full-screen producer must integrate with this epoch or it will race existing renders.**

### Display modes (`main/display_mode.{c,h}`)
Modes are registered via `display_mode_register()` (clock, calendar, timetable, weather, slideshow, todo, countdown, codex_quota). Order in `app_main.c:register_display_modes()` must match the `display_mode_index_t` enum. Physical buttons cycle modes; the active mode persists across deep sleep via `power_mgr_save/load_mode()`.

### Rendering pipeline
`fb_render.{c,h}` owns a two-plane framebuffer (`fb_t` with `black` + `red` byte arrays, MSB-first, packed 1bpp). `fb_reserve_planes_early()` pre-allocates planes before WiFi/HTTP heap fragmentation — **call it before `http_app_start()`**, and only when SPIFFS mounted. Feature modules draw into an `fb_t`, then `epd_display_from_buffer()` / `epd_display_fb_free()` push it via SPI DMA. Native-yellow panels (JD79668/JD79665) use a third plane only in image conversion; built-in UI pages remain two-plane.

### HTTP subsystem
Routes registered in `main/http_app.c` (~63 method+path combos). HTML pages are embedded into firmware via `EMBED_FILES` in `main/CMakeLists.txt` and referenced through `_binary_*_start`/`_end` asm symbols declared in `main/http_internal.h`. Handlers are split: `http_app.c` (core/auth/OTA/system), `http_gallery.c` (images), `http_features.c` (timetable/todo/countdown/calendar), `http_canvas.c` (canvas board). Shared helpers (`http_send_embedded_html`, `http_read_request_body`, `json_escape`, Basic Auth) live in `http_internal.h` as `static inline` or `extern`.

### Persistence
- **NVS** (`nvs` partition, 48KB): PHY, Wi-Fi, per-module config blobs. Use `nvs_throttled_commit()` (from `nvs_utils.h`) instead of raw `nvs_commit()` to protect flash from frequent web auto-saves; call `nvs_flush_all()` before deep sleep.
- **`fontfs` SPIFFS** (2.875MB, built-in): 16/24/32px CJK bitmap fonts generated at build time.
- **`spiffs` SPIFFS** (3.875MB, user): uploaded images, canvas assets, runtime files. Mount failures do **not** auto-format user data — `spiffs_mount_init()` degrades to AP/Web diagnostics.

## Key Directories

| Path | Purpose |
|------|---------|
| `main/` | All firmware C source (~32K lines). Entry: `app_main.c`. |
| `main/epd_stub.c`, `main/epd.h` | EPD drivers (SSD1619/UC8179 + experimental ATC/EPD-nRF5 panels), SPI DMA. |
| `main/fb_render.c`, `main/fb_render.h` | Two-plane framebuffer render engine. |
| `main/http_app.c` + `http_{gallery,features,canvas}.c` | HTTP server (~63 routes) + Basic Auth + OTA. |
| `main/image_convert.c` | JPEG/PNG/BMP decode + Floyd–Steinberg dithering → EPD raw. |
| `main/wifi_manager.c` | AP+STA, provisioning, auto-reconnect. |
| `main/weather.c` | QWeather HTTPS client (large-stack task). |
| `main/power_mgr.c` | Deep sleep, wake-source detection, mode persistence. |
| `main/display_policy.c`, `main/display_mode.c` | Display arbitration + mode registry. |
| `main/{clock,calendar,timetable,todo,countdown,codex_quota,message,canvas}_*.c` | Feature display modules. |
| `main/font_ext.c`, `main/font_data.h` | Font engine + embedded ASCII/CJK bitmaps. |
| `main/lodepng.c` | Vendored PNG decoder (encoder/disk/ancillary compiled out). |
| `web/` | Web UI HTML (embedded into firmware at build via `EMBED_FILES`). |
| `tools/` | Python font-gen scripts (`gen_font.py`, `gen_ext_font.py`, `fetch_open_fonts.py`), BOM/city helpers, `detect_port.sh` (macOS/Linux serial auto-detect for Makefile). |
| `spiffs_image/fonts/` | Generated font assets (`.mef`) — built, gitignored. |
| `test/lunar/` | Host-side Unity unit tests for `main/lunar.c` (ESP-IDF Linux target path, needs `idf.py`). |
| `test/host/` | **ESP-IDF-free** host test runner for `main/lunar.c` (pure `cc`, ships `unity.h`/`sdkconfig.h` shims; `make test-host`). |
| `hardware/` | BOM notes, wiring diagram. |
| `release/` | Pre-built flashing bundle (`epaper_uploader_full_16MB.bin`). |
| `docs/` | README images, font notes, Gitee about text. |
| `Makefile` | macOS/Linux build wrapper around `idf.py` with serial-port auto-detect (`make help`). |
| `pyproject.toml` | Python tooling manifest for uv (font/city-code scripts + host test helpers; `uv sync --group dev`). |

## Development Commands

**Runtime/Toolchain constraints:** ESP-IDF **≥ v5.5.1** (5.5.x series). Target is **`esp32s3` only** — `main/CMakeLists.txt` hard-fails on any other target. Firmware cross-compile needs ESP-IDF (C + CMake + toolchain). Python dev tools (font/city-code gen) and the **ESP-IDF-free host tests** are managed by **[uv](https://docs.astral.sh/uv/)** via `pyproject.toml` (`uv sync --group dev`) — no global Python pollution, no ESP-IDF required for tooling/tests.

**macOS / Linux (Makefile wrapper, recommended):** the repo-root `Makefile` wraps `idf.py` and auto-detects the serial port via `tools/detect_port.sh` (macOS `/dev/cu.*`, Linux `/dev/ttyACM*` / `/dev/ttyUSB*`). Requires ESP-IDF v5.5.1+ activated (`. ~/esp/esp-idf/export.sh`).

```bash
make setup            # first-time target config (esp32s3), once
make fm               # build + flash + monitor (most common; auto-detects port)
make build            # build only
make flash            # flash only
make monitor          # serial monitor
make test             # host-side unit tests (ESP-IDF-free, test/host)
make menuconfig       # config menu
make clean            # clean build artifacts (keeps sdkconfig/target)
make fullclean        # deep clean (deletes build/ + sdkconfig; re-run make setup)
make help             # list all targets

# override port/baud explicitly when auto-detect fails:
make fm PORT=/dev/ttyUSB0 BAUD=921600
```


**Windows / raw `idf.py` (all platforms):**

```powershell
# First-time setup (once)
idf.py fullclean
idf.py set-target esp32s3

# Build, flash, monitor (replace COMx with your port)
idf.py -p COMx build flash monitor

# Clean reconfig after switching chips or stale sdkconfig
idf.py fullclean
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
idf.py set-target esp32s3
idf.py -p COMx build flash monitor

# Full erase (only when Flash is corrupt / partition table changed)
idf.py -p COMx erase-flash
```

**Host-side unit tests** — two paths:
- **ESP-IDF-free (preferred for quick checks):** `make test-host` compiles `main/lunar.c` directly with the system `cc` via `test/host/` (ships minimal `unity.h` + `sdkconfig.h` shims, reuses `test/lunar/main/test_lunar.c` cases). No ESP-IDF, no hardware.
- **ESP-IDF Linux target (full Unity):** via `idf.py --preview set-target linux` (needs ESP-IDF):
```powershell
cd test/lunar
idf.py --preview set-target linux
idf.py build
./build/lunar_unit_test.elf   # Linux/macOS ; .\build\lunar_unit_test.elf on Windows (MinGW)
```

**Python dev tools (uv-managed, ESP-IDF-free):** font generation and city-code export run in a uv venv — no ESP-IDF, no global Python.
```bash
uv sync --group dev                          # create .venv + install Pillow/openpyxl (once)
uv run python tools/gen_ext_font.py --help   # any tool runs via `uv run`
make tools                                   # alias: uv sync --group dev
make tools-fonts                             # regenerate cjk16/24/32.mef (needs OFL fonts)
make tools-city                              # export QWeather city codes to Excel
make ruff-check 2>/dev/null || uv run ruff check tools/   # optional lint
```

## Code Conventions & Common Patterns

- **Error handling**: functions return `esp_err_t`. Use `ESP_ERROR_CHECK()` for fatal/init failures; log + degrade for non-fatal (see how `full_boot()` logs `ESP_LOGW` and continues when a feature `*_init()` fails in diagnostic mode). Each module declares `static const char *TAG = "name";` and logs via `ESP_LOGI/W/E`.
- **Headers**: `#pragma once` guards. Headers expose typed `*_init()` / `*_get_config()` / `*_set_config()` APIs; structs (e.g. `weather_config_t`, `power_config_t`, `fb_t`) defined in headers.
- **Concurrency**: FreeRTOS tasks with large stacks for heavy work (weather fetch, EPD repair). Protect shared state with `portMUX_TYPE` spinlocks (e.g. `scheduler.c:s_cfg_mux`) or mutexes. Long loops must yield to avoid task WDT.
- **Persistence**: NVS for config (`nvs_throttled_commit`), SPIFFS for blobs/files. Writes must tolerate power-loss and corrupt data — read-back-validate, handle `ESP_ERR_NVS_*`.
- **Display integration**: any module that pushes to the EPD must (a) check `epd_is_ready()`, (b) acquire a display epoch via `display_policy_begin_manual_display()`, (c) check `display_policy_epoch_is_current()` during long renders, (d) use `epd_display_fb_free()` to release framebuffer memory early.
- **Web handlers**: validate input length, parse JSON defensively, respect Basic Auth (`http_check_basic_auth(req)`), use `http_read_request_body()` + `json_escape()`. Frontend pages store auth in `localStorage` (`epd_auth_u`/`epd_auth_p`) and attach via `epdFetchOpts()`.
- **Compile flags**: `-Os` size optimization (app partition headroom is tight). `main/CMakeLists.txt` demotes false-positive `-Wformat-truncation`/`-Wformat-overflow` for the app component only; IDF components keep full warnings-as-errors. LodePNG has encoder/disk/ancillary/cpp compiled out to save flash.
- **Localization**: user-facing strings and logs are bilingual (Chinese comments/strings throughout). Keep `display_mode` label fields (`label_cn`) in sync when adding modes.
- **Fonts**: built-in UI text uses `fb_utf8_scaled*()` / `fb_number7()` from `font_ext` (SPIFFS `.mef`) with `font_data.h` fallback. Prefer scaled built-in font APIs for consistency.

## Important Files

| File | Role |
|------|------|
| `main/app_main.c` | Entry point (`app_main`), dual boot path orchestration. |
| `main/CMakeLists.txt` | Source list, `EMBED_FILES` (web HTML), compile flags, target guard. |
| `main/idf_component.yml` | IDF component manifest (`espressif/mdns`, IDF `>=5.5.1`). |
| `CMakeLists.txt` (root) | Font generation custom commands, SPIFFS image, version. |
| `Makefile` | macOS/Linux build wrapper around `idf.py` (setup/build/flash/monitor/fm/test/test-host/clean/fullclean) + uv tool targets (`tools`/`tools-fonts`/`tools-city`). |
| `tools/detect_port.sh` | Serial-port auto-detect script used by the Makefile (macOS `/dev/cu.*`, Linux `/dev/ttyACM*`/`/dev/ttyUSB*`). |
| `pyproject.toml` | Python tooling manifest for uv — dev deps (Pillow, openpyxl), ruff config. `uv sync --group dev` installs. |
| `uv.lock` | Locked Python tool deps for reproducible builds (committed). |
| `test/host/` | ESP-IDF-free host test runner (`unity.h`/`sdkconfig.h` shims + driver); `make test-host`. |
| `partitions.csv` | 16MB partition layout (factory + ota_0/ota_1 3MB each + coredump + fontfs + spiffs). |
| `sdkconfig.defaults` | Project defaults: esp32s3, `-Os`, PSRAM octal 80MHz, coredump-to-flash, HTTPS CA bundle (CMN), lwIP tuning. |
| `main/epd.h` | Panel enum + EPD API (`epd_init`, `epd_display_*`, panel config). |
| `main/fb_render.h` | Framebuffer API + `fb_reserve_planes_early()`. |
| `main/http_internal.h` | Shared HTTP helpers + embedded HTML blob externs. |
| `main/display_policy.h` | Display epoch arbitration API. |
| `main/display_mode.h` | Mode registry (`display_mode_index_t` enum — keep in sync with `app_main.c`). |
| `main/power_mgr.h` | Deep sleep + wake detection + mode persistence. |
| `web/config.html` | Device config page (panel selection, WiFi, weather, OTA, low-power, auth). |

## Testing & QA

- **Host-side unit tests** live in `test/`, run on the ESP-IDF **Linux host target** with the **Unity** framework. Currently only `test/lunar/` covers `main/lunar.c` (pure-C lunar calendar algorithm, no ESP-IDF runtime deps). Tests must **not modify `main/`** — they only add coverage. Extend by adding a subproject that compiles the target `.c` (via relative path `../../../main/xxx.c`) plus `test_xxx.c` with `RUN_TEST(...)`. Only zero-dependency pure-logic modules qualify; modules needing NVS/FreeRTOS/esp_log require mocking or real-device `test_apps`.
- **No automated lint/CI in repo.** Verification = `idf.py build` (CONTRIBUTING.md requires a successful build before PR; run `idf.py -p COMx flash monitor` to smoke on hardware). Frontend changes should be checked in a mobile browser.
- **PR discipline** (`CONTRIBUTING.md`): one concern per PR (don't mix driver/UI/docs); never commit `build/`, `sdkconfig`, `managed_components/`, secrets (WiFi passwords, API keys), or personal assets. Screen-adaptation PRs need panel specs, init sequence, BUSY idle level, refresh timing, and a real-device photo/log.

## Screen Adaptation Notes

When adding/adjusting panels, the typical touch points are `main/epd.h` (panel enum), `main/epd_stub.c` (init sequences, plane order, BUSY polarity), `web/config.html` (panel picker), and docs. All registered panels share one GPIO wiring set (see README pin table: SCK=4, MOSI=5, DC=7, CS=15, RST=6, BUSY=16; buttons SW3/SW4/SW5 = 9/46/3). Changing `panel` via `POST /panel_config` requires a **reboot** for framebuffer/raw-cache dimensions to reinitialize — do not hot-switch. BUSY idle level is configurable per-panel in NVS (`epd_set_busy_idle`).
