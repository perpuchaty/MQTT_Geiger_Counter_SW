# ESP32-C6 Geiger Counter

Firmware for a battery-powered ESP32-C6 Geiger counter. The project measures
Geiger-Muller tube pulses, calculates CPM and dose rate, regulates the tube's
high-voltage supply, presents status and configuration on a 128x64 LCD, stores
measurement history, and provides Wi-Fi, MQTT, Home Assistant, and OTA support.

> [!CAUTION]
> The tube power supply generates hazardous high voltage. Disconnect power and
> discharge the high-voltage section before handling the board. The firmware's
> PWM limits are operational safeguards, not a substitute for correctly rated
> components, insulation, current limiting, and safe construction.

## Main Features

- Interrupt-driven Geiger tube pulse counting.
- Configurable sliding-window CPM calculation and dose-rate conversion.
- Closed-loop tube high-voltage regulation with adjustable target, PWM duty,
  and PWM frequency.
- ST7565P 128x64 LCD with main, chart, settings, diagnostics, update, and power
  screens.
- Three-button local interface plus matching virtual buttons in the web UI.
- Animated battery charging indication, battery percentage estimation, charger
  status monitoring, and deep-discharge shutdown.
- Configurable pulse buzzer and indicator LED feedback.
- Wi-Fi station mode with BluFi and SoftAP captive-portal provisioning.
- Embedded responsive web interface with live values, history, settings,
  diagnostics, and firmware update controls.
- MQTT telemetry and Home Assistant MQTT discovery.
- SPIFFS-backed CSV measurement history.
- HTTPS-capable dual-partition OTA updates with rollback support.
- NVS persistence for runtime settings and Wi-Fi credentials.
- Dynamic CPU frequency scaling and automatic light sleep.

## Hardware

### Target

- MCU: ESP32-C6
- Flash: 4 MB
- Display: ST7565P-compatible 128x64 monochrome LCD
- Display transport: hardware SPI through u8g2
- Tube interface: pulse input, high-voltage PWM output, divided voltage ADC, and
  high-voltage comparator input
- Power interface: power latch, charger enable, charger status inputs, and
  battery/latch rail ADC
- Controls: Enter, Left, and Right buttons
- Indicators: LCD backlight, LED, and passive PWM buzzer

### Pin Assignment

| GPIO | Direction | Function |
| ---: | :---: | --- |
| 0 | Output | Power latch |
| 1 | Output | Battery charger enable |
| 2 | Input | Charger `CHRG`, active low |
| 3 | Input | Charger `STBY`, active low |
| 4 | ADC input | Battery/latch rail voltage |
| 5 | PWM output | Geiger tube high-voltage converter |
| 6 | ADC input | Divided tube voltage feedback |
| 7 | Input | Geiger tube pulse counter, falling edge |
| 8 | Input | Tube high-voltage comparator/status |
| 9 | Output | Pulse indicator LED |
| 14 | PWM output | LCD backlight |
| 15 | Input | Enter button, active low |
| 16 | Input | Left button, active low |
| 17 | Input | Right button, active low |
| 18 | PWM output | Passive buzzer |
| 19 | Output | LCD chip select |
| 20 | Output | LCD reset |
| 21 | Output | LCD command/data select |
| 22 | Output | LCD data |
| 23 | Output | LCD clock |

Pin assignments, ADC divider values, PWM channels, and hardware limits are
defined in `main/config.h`.

## Measurement Pipeline

1. Every falling edge on the tube pulse input increments a free-running pulse
   counter in the GPIO ISR.
2. The measurement task samples that counter once per second.
3. A configurable sliding window converts the observed count delta to counts
   per minute (CPM).
4. Dose rate is calculated from the configured tube sensitivity:

   $$\text{dose rate}\;(\mu\text{Sv/h}) =
   \frac{\text{CPM}}{\text{CPM per }\mu\text{Sv/h}}$$

5. One history sample is generated every 60 seconds for the LCD chart, web UI,
   and persistent CSV history.

The default sensitivity is `153.8 CPM/(uSv/h)`, suitable as a starting value
for an SBM-20 tube. It must be changed for tubes with a different conversion
factor. The measurement window is configurable from 5 to 300 seconds and
defaults to 60 seconds.

For bench testing without a connected tube, randomized pulse simulation can be
enabled at compile time with `CONFIG_GEIGER_SIMULATE_TUBE_PULSES`. It is disabled
by default and must remain disabled for real measurements.

## High-Voltage Supply

The tube converter is driven by LEDC PWM and regulated from the divided tube
voltage measured by the ADC.

- Default target: 400 V
- Allowed target range: 200 to 600 V
- Default PWM frequency: 1000 Hz
- Allowed PWM frequency: 100 to 100000 Hz
- Startup duty: 30%
- Hard duty limit: 35%
- Regulation interval: 500 ms
- Regulation step: 1 percentage point
- Regulation deadband: 5000 mV, or 5 V, around the target

At boot, frequency and startup duty are initialized first. The output is then
enabled or kept disabled according to the persisted **Generate high voltage at
startup** setting. Its compiled-in default is enabled. Toggling ENABLE in the
LCD high-voltage menu also saves this startup setting, so the chosen state
survives a restart. The web UI and HTTP API change only the live output; use
the separate **Generate high voltage at startup** setting to persist those changes.

OTA installation disables the high-voltage output. If an update attempt fails,
the previous enabled state is restored.

## LCD Interface

The display system includes these screens:

| Screen | Purpose |
| --- | --- |
| Main | Dose rate, CPM, battery, time, Wi-Fi/BluFi state, and HV state |
| Chart | Recent CPM trend |
| Menu | Entry point for configuration and service screens |
| Wi-Fi | Provisioning and connection information |
| MQTT | Broker and publishing state |
| System | Speaker, LED, charging, display, and power settings |
| Tube | Measurement window, sensitivity, and target voltage |
| Time | Timezone offset and daylight-saving setting |
| High voltage | Live enable, PWM duty/frequency, ADC, and voltage values |
| Firmware Update | Installed/available version and OTA actions |
| Lamp test | LCD and backlight test |
| Power save | Power-save confirmation |
| Shutdown | Shutdown confirmation |
| Deep discharge | Critical-battery warning before power removal |

### Buttons

- **Left / Right on the main view:** switch between the main and chart views.
- **Enter on the main or chart view:** open the menu.
- **Left / Right in menus:** move the selection or adjust the active value.
- **Enter in menus:** open an item, toggle an action, or start/finish editing.
- **Hold Enter for 3 seconds:** request shutdown confirmation.
- **Hold Right for 3 seconds:** request power-save confirmation.
- **Lamp test:** the test remains active while Enter is held.

The source contains a separate three-second hold-to-power-on sequence, but its
call is currently disabled in `app_main()`. The present firmware starts directly
and displays the startup splash screen.

## Battery, Charging, and Power

The charger enable output follows the persisted battery-charging setting.
`CHRG` and `STBY` are active-low charger status inputs. When charging is enabled
and the charger reports active charging, the LCD displays the charging
animation. Other icon states represent battery operation, external power, and
charger fault conditions.

Battery percentage is an estimate derived from measured voltage; it is not a
coulomb counter. The ADC task averages calibrated one-shot samples before the
value is used by the UI and protection logic.

Deep-discharge protection checks the battery once per second. Five consecutive
readings at or below 3300 mV cause the firmware to:

1. Disable tube high voltage.
2. Stop the buzzer and LED.
3. Show the deep-discharge warning.
4. Release the hardware power latch.

Normal shutdown also disables high voltage, stops the buzzer, fades and clears
the display, and releases the latch. Power-save mode disables Wi-Fi and BLE; on
leaving power-save mode, provisioning starts when no Wi-Fi credentials exist.

## Connectivity

### Provisioning

The firmware supports both provisioning methods concurrently:

- **BluFi:** Wi-Fi credentials are supplied over Bluetooth LE.
- **SoftAP captive portal:** the device creates an access point and redirects
  DNS/HTTP traffic to the embedded setup page.

Default SoftAP behavior:

| Setting | Default |
| --- | --- |
| SSID | `esp32` plus the final three station-MAC bytes |
| Password | Empty, creating an open network |
| Channel | 1 |
| Maximum clients | 4 |
| Portal address | `192.168.4.1` |
| Station retry limit | 5 |
| Portal lifetime after station connection | 60000 ms |

Once connected to the local network, the device advertises
`geigercounter.local` through mDNS. The default HTTP port is 80. The SoftAP
password should be set to at least eight characters for WPA2 operation.

### Web Interface

`main/index.html` is embedded into the application binary during the build. Its
tabs provide live readings, Wi-Fi management, history, MQTT configuration,
tube calibration, high-voltage control, device settings, diagnostics, and OTA
controls.

The HTTP server exposes the following routes:

| Method | Route | Purpose |
| :---: | --- | --- |
| GET | `/` | Embedded web interface |
| GET | `/api/live` | Current measurements and hardware state |
| GET | `/api/diagnostics` | Detailed runtime diagnostics |
| GET | `/api/history` | Short-window history for the live chart |
| GET | `/api/history_csv` | Persistent history as CSV |
| GET | `/api/scan` | Nearby Wi-Fi networks |
| GET | `/api/status` | Provisioning and connection status |
| GET | `/api/settings` | Current runtime settings |
| POST | `/api/settings` | Validate, persist, and apply settings |
| POST | `/api/connect` | Save credentials and connect to Wi-Fi |
| POST | `/api/forget_wifi` | Remove saved Wi-Fi credentials |
| POST | `/api/restart` | Restart the device |
| POST | `/api/factory_reset` | Erase NVS/history and restart |
| POST | `/api/button?left` | Simulate a physical button; also accepts `enter` or `right` |
| POST | `/api/hv?...` | Set `freq_hz`, `duty_pct`, and/or `enabled=0|1` |
| GET | `/api/firmware` | OTA configuration, versions, state, and progress |
| POST | `/api/firmware?action=check` | Check the configured OTA image |
| POST | `/api/firmware?action=install` | Install the available OTA image |

### MQTT and Home Assistant

MQTT is disabled by default. When enabled, the device publishes:

- State: `<base-topic>/<device-id>/state`
- Availability: `<base-topic>/<device-id>/status`

The device ID is derived from the configured hostname and a MAC suffix. The
state payload contains dose rate, CPM/count values, tube voltage, battery, and
Wi-Fi information. Optional Home Assistant MQTT discovery creates entities for
dose rate, counts, total counts, tube voltage, battery, and Wi-Fi signal.

| MQTT setting | Default |
| --- | --- |
| Broker URI | `mqtt://homeassistant.local:1883` |
| Base topic | `geiger` |
| Publish interval | 30 seconds |
| Home Assistant discovery | Enabled |
| Discovery prefix | `homeassistant` |

Broker URI, credentials, topics, discovery, and interval can all be changed at
runtime through the web interface.

## History Storage

Measurements are held in memory for the LCD and short live chart and appended
to `/spiffs/history.csv` every 60 seconds. The SPIFFS store is mounted during
startup. When the file approaches approximately 92% of partition capacity, it
is compacted to approximately 75% to retain recent samples.

The live-history span defaults to 300 seconds and is configurable from 60 to
1728000 seconds. Factory reset formats the history partition as well as erasing
NVS.

## Settings and Persistence

Runtime settings are stored in the NVS namespace `geiger`. Startup first loads
compiled defaults and then overlays every key found in NVS, so newly introduced
settings retain their firmware default until explicitly saved.

Persisted groups include:

- Device name.
- MQTT enable, URI, credentials, topic, discovery, prefix, and interval.
- Tube measurement window, sensitivity, and target voltage.
- Speaker volume and click length.
- Battery charging and power-save mode.
- High-voltage-at-startup setting.
- LED enable, LCD brightness, and automatic dimming.
- Timezone offset and daylight-saving setting.
- Live history window.

Values are range-checked before they are saved. Timezone offsets are limited to
UTC-12:00 through UTC+14:00 and rounded to 30-minute increments. The device
synchronizes UTC time with SNTP after receiving a Wi-Fi address, then applies
the configured offset and daylight-saving adjustment locally.

Wi-Fi credentials are managed by the ESP-IDF Wi-Fi stack in NVS. OTA metadata
uses the separate `ota` namespace. A factory reset erases all NVS namespaces,
formats history storage, and restarts the device.

## OTA Updates

The partition table contains two equal application slots and OTA selection
metadata. Bootloader rollback is enabled. On network connection, the firmware
checks the configured direct image URL when that URL is non-empty. Updates can
then be checked and installed from the LCD, web UI, or HTTP API.

The default URL is defined in `sdkconfig.defaults`. Change it for your own
release host before distributing firmware.

Every successful build copies:

```text
build/geiger_counter.bin -> ota/geiger_counter_ota.bin
```

Publish `ota/geiger_counter_ota.bin` as the raw application image at the
configured OTA URL. Do not use a merged full-flash image for this endpoint.

## Build and Flash

### Requirements

- ESP-IDF 6.1 or newer. The current defaults were generated with ESP-IDF 6.1.0.
- ESP32-C6 toolchain installed through ESP-IDF.
- Python and the other tools installed by the ESP-IDF installer.
- A serial connection to the target board.

Managed component dependencies are declared in `main/idf_component.yml`:

- `nixy4/u8g2 ^0.1.4`
- `espressif/cjson ^1.7.19`
- `espressif/mdns ^1.11.3`
- `espressif/mqtt ^1.1.0`

From an ESP-IDF shell in the repository root:

```powershell
idf.py set-target esp32c6
idf.py build
idf.py -p COMx flash monitor
```

Replace `COMx` with the board's serial port. Exit the monitor with
`Ctrl+]`. Subsequent common commands are:

```powershell
idf.py build
idf.py flash
idf.py app-flash
idf.py monitor
idf.py menuconfig
idf.py save-defconfig
idf.py erase-flash
```

`erase-flash` removes firmware, NVS settings, Wi-Fi credentials, OTA state, and
stored history. The `build/` and `managed_components/` directories are generated
by ESP-IDF and its component manager.

### Compile-Time Configuration

Run `idf.py menuconfig` and open **Geiger Counter** to configure defaults for:

- Device identity.
- SoftAP and captive portal.
- mDNS and HTTP.
- OTA image URL.
- MQTT and Home Assistant.
- Tube simulation, sensitivity, window, and voltage target.
- Speaker, charging, LED, and LCD behavior.

Most settings under MQTT, tube, indicators, and display are initial defaults;
saved NVS values take precedence after the user changes them at runtime.

## Partition Layout

| Name | Type | Size | Purpose |
| --- | --- | ---: | --- |
| `nvs` | Data/NVS | 16 KiB | Settings, Wi-Fi data, and OTA metadata |
| `otadata` | Data/OTA | 8 KiB | Active OTA slot selection |
| `phy_init` | Data/PHY | 4 KiB | Radio calibration data |
| `ota_0` | Application | 1792 KiB | Firmware slot A |
| `ota_1` | Application | 1792 KiB | Firmware slot B |
| `spiffs` | Data/SPIFFS | 416 KiB | CSV measurement history |

## Runtime Architecture

`app_main()` initializes subsystems in this order:

1. CPU power management and NVS.
2. Runtime settings, history storage, and OTA state.
3. GPIO, PWM, ADC, SPI/LCD, and persisted hardware settings.
4. Battery safety check and startup display.
5. Tube high voltage and its regulator.
6. Pulse feedback, buttons, and Geiger measurement.
7. Wi-Fi/BluFi provisioning, power-save policy, and MQTT.
8. Main LCD task, battery protection, and OTA image confirmation.

Principal asynchronous workers are:

| Worker | Responsibility |
| --- | --- |
| ADC task | Periodic calibrated, averaged battery and tube voltage samples |
| HV regulator task | Adjust converter duty toward the configured voltage |
| Geiger task | Compute CPM/dose and append history |
| Tube feedback task | Generate short buzzer/LED pulse feedback |
| Button event task | Debounce and dispatch physical/virtual controls |
| LCD task | Render screens and process UI state |
| Backlight task | Fade and auto-dim the LCD backlight |
| Battery task | Confirm sustained undervoltage and shut down safely |
| MQTT task | Manage connection, discovery, and periodic telemetry |
| DNS/HTTP services | Operate the captive portal and web API |

GPIO ISRs only capture input state, count pulses, and notify tasks; display,
networking, and other longer operations run outside interrupt context.

## Project Structure

```text
.
|-- CMakeLists.txt             Project definition and OTA artifact copy
|-- sdkconfig.defaults         ESP32-C6, flash, BLE, network, and app defaults
|-- components/
|   `-- dns_server/            Standalone DNS server component in the tree
|-- main/
|   |-- CMakeLists.txt         Firmware component sources and dependencies
|   |-- idf_component.yml      ESP-IDF and managed component requirements
|   |-- Kconfig.projbuild      Project menuconfig options
|   |-- partitions.csv         Dual-OTA and SPIFFS partition table
|   |-- main.c                 Startup, buttons, shutdown, and battery safety
|   |-- config.c/.h            Board pins, GPIO, ADC, PWM, LCD, and HV regulator
|   |-- geiger.c/.h            Pulse-window measurement and dose calculation
|   |-- history_store.c/.h     SPIFFS CSV history persistence
|   |-- lcd.c/.h               Screen rendering, menus, and backlight control
|   |-- settings.c/.h          Defaults, validation, NVS load/save/reset
|   |-- wifi_prov.c/.h         Wi-Fi, BluFi, SoftAP, DNS, HTTP, mDNS, and SNTP
|   |-- blufi_init.c           BluFi callbacks and provisioning lifecycle
|   |-- blufi_security.c       BluFi DH/AES application-layer security
|   |-- api.c/.h               Root page, virtual buttons, HV, and OTA routes
|   |-- mqtt.c/.h              MQTT telemetry and Home Assistant discovery
|   |-- ota.c/.h               OTA checks, installation, rollback, and status
|   `-- index.html             Embedded web application
|-- managed_components/        Dependencies downloaded by the IDF manager
|-- ota/                       Build-generated OTA application image
`-- build/                     Generated CMake/Ninja build output
```

Although `components/dns_server/` is present, the active captive-portal DNS
implementation is currently contained in `main/wifi_prov.c`.

## Important Limits

- Never exceed the hardware ratings even though firmware limits PWM duty to
  35% and target voltage to 600 V.
- Battery shutdown depends on a correctly scaled and calibrated GPIO 4 ADC
  input. Verify the divider before relying on the 3300 mV cutoff.
- Tube voltage accuracy depends on the configured 80 Mohm / 510 kohm divider,
  ADC calibration, leakage, and component tolerances.
- Dose rate accuracy depends on the tube's real sensitivity and calibration;
  the default SBM-20 factor is not valid for every tube.
- An empty SoftAP password creates an open provisioning network.
- The OTA URL must point directly to a compatible application binary built for
  this ESP32-C6 partition layout and hardware revision.