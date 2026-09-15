# Firmware testing

## Build

Run `./ps/build.ps1` from PowerShell with the pinned dependencies described in [README.md](README.md). This compiles the firmware; physical radio and touch behavior require a CrowPanel. To upload, pass `-Port` with the board's actual serial port. A build without `config.h` validates compilation but shows setup instructions on the panel.

Latest scan-fix validation: the credential-free sketch compiled with ESP32 core 3.3.10, using 1,209,003 bytes of flash and 50,640 bytes of static RAM. No panel was flashed or tested for this fix.

## Wi-Fi selector regression checks

Use a configured statistics endpoint/token and a visible 2.4 GHz network. Keep Serial Monitor at 115200 baud for failure diagnostics.

1. With no saved network and blank fallback credentials, open **Wi-Fi** immediately after boot and again after 30 seconds. Both scans should list nearby networks without starting a connection to an unspecified network.
2. Save a working network, turn that access point off, and restart the panel. Open **Wi-Fi** while it is trying to reconnect. Discovery should still list other nearby networks. Repeat **Scan** several times; touch should remain responsive.
3. While connected to a working network, scan repeatedly. The existing connection should remain available and statistics should continue refreshing after closing the menu.
4. With the saved access point unavailable, scan and then restore the access point. The panel should resume connecting to its saved network without requiring a password again.
5. Select a secured network and enter a wrong password. After the connection failure, scan again. Discovery should work and a failed password must not replace the saved working credentials.
6. Select a network with the correct password. Confirm statistics load and the same network reconnects after a restart. Also check an open network if one is available.
7. With no visible networks, expect **Found 0 network(s)**. If a scan fails, record its on-screen code and the `Wi-Fi scan failed:` serial line. A failed scan must release its busy state and allow another scan; its completion wait is capped at 15 seconds plus radio startup/cleanup.

Hardware results must be recorded separately from compilation results; a successful build does not verify RF discovery or connection recovery.
