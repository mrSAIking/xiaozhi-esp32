# Project Aura

Xiaozhi 2.6.0 for ESP32-S3 N16R8, 16 MiB flash and 8 MiB octal PSRAM. Build the `anime-companion-s3` variant with ESP-IDF release/v6.1. The dedicated Anime Companion S3 Actions workflow builds the merged image on pushes to `anime-companion`.

| Peripheral | Connections |
| --- | --- |
| ILI9488, 320 × 480 portrait | CS 10, RESET 8, DC 9, MOSI 11, SCK 12, permanently powered backlight |
| INMP441, left slot | SCK 5, WS 4, SD 6 |
| MAX98357A | BCLK 15, LRC 16, DIN 17 |
| WS2812 and BOOT | GPIO48 and GPIO0 |

The write-only display uses RGB666 on the wire. `CONFIG_ANIME_SPI_MHZ` defaults to 10. `CONFIG_ANIME_LED_BRIGHTNESS` defaults to 12 out of 255. The BOOT button uses the breadboard firmware's provisioning and chat behavior.

Xiaozhi owns networking, activation, speech processing and protocols. The setup screen and avatar share one LVGL flush path. Each mode starts with one 320 × 480 RGB565 presentation buffer in PSRAM. The 10,240-byte LVGL strip is also in PSRAM. One 15,360-byte internal DMA strip is reused after its SPI completion interrupt. The immutable bases and patches are linked into the OTA application so an update replaces code and matching art together.

Double-click BOOT to switch between full and half body offline. During startup, provisioning or activation, a double-click shows the selection for five seconds before returning to the setup screen. A single click still performs the usual Xiaozhi action. The `self.avatar.set_mode` MCP tool accepts `{"mode":"half_body"}` or `{"mode":"full_body"}`. Both controls save the mode in NVS, and the device restores it on the next boot. A fresh installation starts in full-body mode.

Breathing defaults to 3 pixels over 3600 ms with a 42% inhale. Left and right arm periods default to 4000 and 4500 ms. Set either mode's arm amplitude independently. The head stays still. Eye and mouth patches update immediately in their manifest rectangles. Body motion samples the selected immutable artwork in short strips with one timestamp per sweep. The scheduler reserves body work even while the mouth changes. The serial console reports completed body poses and total RGB666 payload including overlays.

The board advertises `Project-Aura-XXXX` and uses that name for station DHCP. The Wi-Fi component appends the last two station MAC bytes to the supplied prefix. The provisioning portal identifies Project Aura in its OTA settings. Backend identity and provisioning credentials use the existing Xiaozhi flow.

The green dot indicates listening with voice processing enabled and no pending playback drain. The optional 42-pixel charcoal subtitle strip follows the persisted text-mode setting. STT questions stay on screen until TTS playback starts. Sentence events append to one right-to-left answer; the explicit TTS stop and playback drain release the final text, followed by a 500 ms fade. Scrolling ranges from 45 to 50 px/s. The renderer reuses the device font, keeps a 384-byte answer window, and caps unread UTF-8 at 8192 bytes. On overflow it retains the earliest queued text and truncates new input at a character boundary. Sentence events do not carry word timestamps.

Use the 10 MHz artifact first. The 20, 27 and 40 MHz artifacts use the same GPIO and rendering configuration and request different SPI clocks. Actual bus frequency depends on the peripheral divider. Check colors, noise, reset stability, audio and frame timing on each physical panel. The desktop previews use the same C++ mesh and sampler. Compile `scripts/anime_preview.cc` with a desktop C++17 compiler and the board include directory, then run `python scripts/anime_preview.py <executable> <output-directory>`.

The audio meter timestamps windows of resampled mono PCM immediately before the I2S write and estimates the presentation delay from the codec's DMA configuration. At 24 kHz, five 240-frame descriptors represent 50 ms. The display checks the meter on the manifest's 50 ms cadence. Expired windows return silence. A decoder reset invalidates readings from older packets.

Both OTA slots are 4.5 MiB. The remaining 6.875 MiB stores the usual Xiaozhi assets. CI decodes the compiled partition table, verifies flashed binaries byte-for-byte against the merged image, and reports slot headroom and linker size information.

On the wired unit, check portrait orientation and RGB primary colors at 10 MHz, provisioning and activation, microphone left-channel capture, wake-word response, server audio, interrupted speech, repeated MCP switches and LED brightness. Measure mouth-to-speaker timing and available internal heap under simultaneous playback and animation before testing a faster SPI clock.

## Startup diagnostics

`CONFIG_ANIME_PANEL_SELF_TEST` defaults to disabled. Enable it for the color test and silent provisioning comparison. In that diagnostic build, before LVGL, audio or Wi-Fi starts, the driver fills the entire panel red, green, blue, white and black, with a 700 ms hold on each color. The serial log records reset, panel configuration, each fill, the first LVGL flush and the start of Wi-Fi. A completed SPI transfer confirms the ESP32 sent the pixels. Check the panel itself for full-screen coverage and correct color order.

If the solid colors work but the UI fails, enable `CONFIG_ANIME_SETUP_UI_ONLY` in a diagnostic build. The original Xiaozhi setup UI still runs while the avatar framebuffer and timer are skipped. Capture the complete serial log for each pass. For a brownout at AP startup, measure the board supply at the ESP32-S3 during Wi-Fi transmission and check the backlight and speaker load. Brownout detection stays enabled.

Each firmware artifact includes `merged-binary.bin`, `xiaozhi.elf` and `xiaozhi.map`. Match the panic's ELF SHA-256 prefix against the ELF before using `xtensa-esp32s3-elf-addr2line -aipfC -e xiaozhi.elf` with the reported PCs.

For a provisioning audio comparison, boot into the setup AP with BOOT released and capture the log through prompt playback. On a second boot with the same image and power source, press and hold BOOT after the blue test screen appears. Release it when the serial console reports `Silent provisioning comparison selected`. Both boots render the same setup prompt. The comparison skips its Opus packets for that boot only. A crash before `Provisioning refresh complete` precedes submission of the prompt. If the assertion returns, capture the UART output through `ESP32 CORE DUMP END` and use the ELF packaged with that image.

## Local control

Connect to the Project-Aura hotspot and open http://192.168.4.1. In Advanced Settings, generate and save a home access code and keep a copy. After connecting the device to your home network, open its LAN IP or http://project-aura.local and enter the code. The local HTTP interface is intended for a trusted private Wi-Fi network. Home mode exposes weather, clock and device status. Wi-Fi credentials, OTA, sleep and Tavily configuration stay on the setup hotspot.

Enter an OpenWeather key, city and optional two-letter country. Save offline if necessary. On a connected device, Find cities returns matching localities and their coordinates. Select a result to resolve ambiguous names. A successful test requires an HTTPS current-weather response. Home observations refresh after about 12 minutes. Current minimum and maximum temperatures refer to the observation. No daily forecast is requested.

Clock defaults are Asia/Kolkata, 12-hour display and automatic SNTP. Until SNTP confirms an update the control panel reports pending sync. Voice tools are `self.time.get_current` and `self.weather.get_current`.

The vendored 78/esp-wifi-connect 4.0.0 base is upstream revision 2b7f8926f3e357a5a52ad5cc95f39669710d63c7. Board-specific routes share the upstream provisioning server. Home mode runs its own server and mDNS registration. The original supplied Project Aura PNG was scaled proportionally and encoded for the embedded logo and 320 x 480 RGB565 boot screen.

For desktop visual verification, run `node scripts/preview_aura_portal.mjs` with the `BROWSER` environment variable pointing at Chromium or Edge. The preview server binds to loopback and uses fixture data. The Actions service tests exercise the production parser, cache and settings logic against simulated HTTPS, NVS and SNTP adapters.

## Appearance, volume and settings

Both portals offer **Appearance** and **Audio** settings. Select an original black or white character bank, body mode, subtitles and speaker volume. The device saves the selection. Both banks ship inside each OTA application and use one 307,200-byte active PSRAM framebuffer. Each bank contains 647,310 bytes including facial patches.

Connect one normally open switch between each input and GND. Confirm these pins are exposed and unused on your particular carrier. Internal pull-ups hold the inputs high. Firmware debounces both edges for 40 ms.

| Function | GPIO | Operation |
|---|---:|---|
| Volume up | 13 | +5 on press; hold 500 ms to repeat every 180 ms |
| Volume down | 14 | -5 on press; hold to repeat; release saves the level |
| Settings | 18 | Press once to enter the hotspot, press again to reconnect |
| BOOT | 0 | Existing single-click and double-click controls |

The settings screen displays the actual SSID and URL. Save a network before choosing **Save & Return to Aura**. If the connection times out, the device reopens the hotspot. The system-screen preview PNGs use example credentials and codes. Verify hotspot entry during speech, reconnection, retained mute, both backgrounds and animations on the panel.
