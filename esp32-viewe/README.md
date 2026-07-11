# ESP32 VIEWE Power Meter Display

PlatformIO Arduino project for an ESP32-S3 VIEWE display module using ESP Display Panel and [LVGL](https://lvgl.io/docs/open) v8.

## Board Settings

- Board: ESP32S3 Dev Module
- USB CDC On Boot: Disabled
- USB DFU On Boot: Disabled
- Flash: 16MB
- Partition scheme: 3MB OTA app slots with LittleFS data partition
- PSRAM: OPI PSRAM
- Monitor speed: 115200

The active ESP Display Panel board is configured in `include/esp/esp_panel_board_supported_conf.h`:

```cpp
#define BOARD_VIEWE_UEDX32480035E_WB_A
```

## Build and Upload

```sh
pio run --target upload
```

## View Serial Logs

```sh
screen /dev/ttyACM0 -s 115200
```

## WSL
Expose Windows USB devices to Linux using [usbipd](https://github.com/dorssel/usbipd-win)

## Layout

- `src/main.cpp`: board, display, LVGL, storage, and network startup
- `src/ui/`: screen navigation and UI screens
- `src/sensors/`: sensor acquisition, simulated source, and ESP32 ADC source
- `src/sensors/sensor_config.h`: source selection and provisional pin mapping
- `src/data/`: minute-level historical storage
- `include/esp/`: ESP Display Panel, ESP utility, and LVGL configuration headers

## Sensor source

Simulation is enabled by default so UI and storage work without sensor
hardware. `src/sensors/sensor_config.h` contains the provisional sequential
mapping: In voltage/current = GPIO 5/6, Out = 7/8, and Aux = 9/10. Set
`POWER_METER_USE_SIMULATED_SENSORS` to `0` when the physical hardware is ready
for validation.
