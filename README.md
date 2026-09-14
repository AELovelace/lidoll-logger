# Little Log admin statistics display

Companion for **Elecrow CrowPanel Advance 7-inch ESP32-S3 V1.4**, 800×480 RGB with GT911 capacitive touch. The display reads admin-authorized statistics from Little Log; it cannot record entries or change balances.

## Set up

1. Deploy the updated tracker. In **Admin console → Statistics devices**, create an **Everyone and individual views (admin)** token. Only active administrators can create or use these credentials.
2. Copy `config.example.h` to **`config.h`** in this folder. Set `STATS_API_BASE` and `STATS_TOKEN`, and leave the API base's trailing slash. The current LAN example is `http://10.1.1.23:4173/tracker/api/statistics/v1/`; change it if the tracker is reached at another address. `WIFI_SSID` and `WIFI_PASSWORD` are optional first-boot fallbacks; leave them empty to choose a 2.4 GHz network from the device's **Wi-Fi** menu.
3. For HTTPS with a certificate issued by a public CA, leave `TLS_ROOT_CA` empty: the firmware automatically uses the full public root bundle included with the pinned ESP32 core. Only paste a root CA PEM when the server uses a private CA. Keep NTP reachable so the device can validate certificate dates. HTTP works on a trusted LAN; it sends the credential unencrypted. No redirect following or insecure TLS fallback is enabled.
4. Open `lidoll-logger.ino` in Arduino IDE. Select **ESP32S3 Dev Module**, **16 MB flash**, **OPI PSRAM**, **16M Flash (3MB APP/9.9MB FATFS)** partition, **Hardware CDC and JTAG**, and **USB CDC On Boot: Enabled**. Use the default 240 MHz CPU and QIO 80 MHz flash settings.
5. Compile, connect the CrowPanel's USB port, select its serial port, and upload. If automatic bootloader entry fails, use the board's BOOT/RESET controls as described by Elecrow. Serial diagnostics are 115200 baud and never print credentials or server bodies.

An unconfigured build is valid: it displays configuration instructions. `config.h` and configured firmware binaries contain secrets; they are ignored by `.gitignore`. No Wi-Fi passwords or real admin tokens are supplied with this sketch.

## Dependencies and verified build

This sketch compiled successfully with the versions already installed on this workstation:

- **esp32 by Espressif Systems 3.3.10**
- **LovyanGFX 1.2.26**
- **ArduinoJson 7.4.3**

It draws directly through LovyanGFX, so LVGL and TAMC_GT911 are not required. Stock PSRAM settings are used; no shared Arduino framework files are patched. The verified FQBN is:

```text
esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,USBMode=hwcdc,CDCOnBoot=cdc
```

On this workstation, build from PowerShell:

```powershell
& 'F:\Langley\Documents\Arduino\lidoll-logger\ps\build.ps1'
```

The helper locates the Arduino IDE's CLI and the adjacent sketchbook libraries; it checks the recorded core/library versions and writes output under `build/`. It only compiles. To upload, explicitly supply the actual board port:

```powershell
& 'F:\Langley\Documents\Arduino\lidoll-logger\ps\build.ps1' -Port COM7
```

COM7 is an example, not an automatically detected board. If using another machine, install the versions above first or pass the helper's explicit CLI and library locations. The compile check does not establish hardware operation.

## Screen controls and data

- **People** opens a paged list containing **Everyone** and participant names. Tap a row to switch the dashboard directly; the directory remains server-paged so it supports more people than can fit in device RAM.
- **Wi-Fi** scans for nearby networks. Tap one, enter its password with the on-screen keyboard, and connect. Successful credentials are saved in the ESP32's NVS and automatically reused after restart; selecting another network replaces them. `config.h` credentials remain the fallback until a menu connection has been saved.
- **Today / 7 days / 31 days** selects the inclusive window ending today in Los Angeles.
- **Sync** requests a refresh. Automatic polling is every 60 seconds; connection startup retries every 10 seconds.

Cards show liquid intake, wetting records, diaper changes, bedwetting, potty use and chart stars. Wetting records include potty use, matching the server's category definitions. The daily bars show wetting-record counts, oldest day on the left; they share a scale within the selected window. A caption gives liquid observation count. There are no medical predictions or inferred unrecorded events.

The API sums intake using the existing interval/cumulative rules for each participant's saved local day. All-account totals include disabled accounts' saved records. Deleted records are excluded. LA time determines the default end date; stored dates themselves are not converted to LA. Detailed response fields and endpoints are in `C:\Scripts\omo-trainer\STATISTICS_API.md`.

An optional **My statistics only** token still belongs to an administrator. With that restricted token, the display automatically uses that administrator's individual view. Neither mode is available to ordinary participant accounts.

Networking, Wi-Fi scans and connection attempts run in a FreeRTOS task; the main task owns graphics and touch. Statistics refreshes overwrite only the dashboard's changing regions, and the ten-second age update repaints only the status strip, avoiding a visible full-screen clear. Selection serials discard late responses, queues retain the newest requested selection, response allocations are limited to 32 KiB, and connect/read timeouts prevent a stalled connection from blocking the UI. On transient failures, previously loaded values are marked **STALE**; an observed 401/403 clears them. Only successful Wi-Fi credentials are written to the ESP32's NVS; statistics are not persisted or sent to llama.cpp or Discord. A powered offline device can retain its last displayed values until restarted or refreshed.

## V1.4 hardware provenance

The [official Advance 7-inch wiki](https://media-cdn.elecrow.com/wiki/ESP32_Display-7.0_inch%28Advance_Series%29wiki.html) identifies V1.4 as the V1.3 wiring with a button component change. `CrowPanel14.h` follows Elecrow's [V1.3/V1.4/V1.5 lesson-03 driver](https://github.com/Elecrow-RD/CrowPanel-Advance-7-HMI-ESP32-S3-AI-Powered-IPS-Touch-Screen-800x480/tree/master/example/V1.3_and_V1.4_and_V1.5/Arduino/lesson-03/BigInch_LVGL).

RGB: blue 21/47/48/45/38, green 9/10/11/12/13/14, red 7/17/18/3/46. DE 42, VSYNC 41, HSYNC 40, PCLK 39 at 16 MHz; vendor porch timings are retained. I2C SDA 15 / SCL 16; GT911 address 0x5D; STC controller address 0x30. Startup sends command 250 to enable touch and command 0 for full backlight. It uses bounded retries and GPIO 1's reset sequence from the vendor example. This is not the Basic 7-inch board or the old Advance V1.0 PCA9557 setup.

Elecrow's [environment tutorial](https://media-cdn.elecrow.com/wiki/HMI_Display_course.html) also discusses older pinned examples and a patched 120 MHz PSRAM framework. This sketch was compiled with the stock versions listed above. Verify physical display stability under Wi-Fi load before deciding whether your board needs vendor-specific tuning; do not overwrite an unrelated Arduino installation's framework libraries.

## Hardware acceptance checks

Compilation and server/browser tests passed. No physical panel was flashed or tested during development. On hardware, verify:

1. Correct 800×480 image, colors, backlight and all touch targets after boot.
2. With blank compiled Wi-Fi credentials, scan, select a secured 2.4 GHz network, enter its password, connect, and confirm it reconnects after restart. Confirm a failed attempt does not replace the last working saved network.
3. Valid admin token loads both everyone and a named individual; the paged People list selects the tapped name and counts match the admin records for the same dates.
4. Automatic refresh and rapid participant/window switching do not blink the full screen or show values under the wrong person's name.
5. Disable Wi-Fi or stop the server: touch remains responsive, old values say STALE, and reconnection recovers.
6. Revoke the token or demote its issuing administrator: the next API response clears statistics and reports access denied.
7. Let the panel run across midnight LA time and under sustained Wi-Fi traffic; check date rollover and RGB stability. Use the power supply recommended by Elecrow.
