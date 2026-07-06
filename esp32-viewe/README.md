# ESP32 VIEWE Power Meter Display

PlatformIO Arduino project for an ESP32-S3 VIEWE display module using ESP Display Panel and [LVGL](https://lvgl.io/docs/open) v8.

## Board Settings

- Board: ESP32S3 Dev Module
- USB CDC On Boot: Disabled
- USB DFU On Boot: Disabled
- Flash: 16MB
- Partition scheme: 3MB OTA app slots with FATFS data partition
- PSRAM: OPI PSRAM
- Monitor speed: 115200

The active ESP Display Panel board is configured in `include/esp/esp_panel_board_supported_conf.h`:

```cpp
#define BOARD_VIEWE_UEDX32480035E_WB_A
```

## Build

```sh
PLATFORMIO_CORE_DIR=.platformio pio run
```

## WSL
Expose Windows USB devices to Linux using [usbipd](https://github.com/dorssel/usbipd-win)

## Layout

- `src/main.cpp`: board, display, LVGL, and WiFi startup
- `src/ui/`: screen navigation and UI screens
- `src/sensors/`: sensor task scaffold
- `src/data/`: data store scaffold
- `include/esp/`: ESP Display Panel, ESP utility, and LVGL configuration headers
