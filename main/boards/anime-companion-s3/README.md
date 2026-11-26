# Anime Companion S3

Xiaozhi 2.6.0 for ESP32-S3 N16R8, 16 MiB flash and 8 MiB octal PSRAM. Build the `anime-companion-s3` variant with ESP-IDF release/v6.1. The dedicated Anime Companion S3 Actions workflow builds the merged image on pushes to `anime-companion`.

| Peripheral | Connections |
| --- | --- |
| ILI9488, 320 × 480 portrait | CS 10, RESET 8, DC 9, MOSI 11, SCK 12, permanently powered backlight |
| INMP441, left slot | SCK 5, WS 4, SD 6 |
| MAX98357A | BCLK 15, LRC 16, DIN 17 |
| WS2812 and BOOT | GPIO48 and GPIO0 |

The write-only display uses RGB666 on the wire. `CONFIG_ANIME_SPI_MHZ` defaults to 10. `CONFIG_ANIME_LED_BRIGHTNESS` defaults to 12 out of 255. The BOOT button uses the breadboard firmware's provisioning and chat behavior.

Xiaozhi owns networking, activation, speech processing and protocols. The setup screen and avatar share one LVGL flush path. Each mode starts with a pair of 320 × 480 RGB565 buffers in PSRAM for compositing and presentation. The 10,240-byte LVGL strip is also in PSRAM. One 15,360-byte internal DMA strip is reused after its SPI completion interrupt. The immutable bases and patches are linked into the OTA application so an update replaces code and matching art together.

Double-click BOOT to switch between full and half body offline. During startup, provisioning or activation, a double-click shows the selection for five seconds before returning to the setup screen. A single click still performs the usual Xiaozhi action. The `self.avatar.set_mode` MCP tool accepts `{"mode":"half_body"}` or `{"mode":"full_body"}`. Both controls save the mode in NVS, and the device restores it on the next boot. A fresh installation starts in full-body mode.

Breathing defaults to 3 pixels over 4800 ms with a 42% inhale. Left and right arm periods default to 5500 and 6500 ms. Set either mode's arm amplitude independently. Head tilt defaults to 1.5 degrees around the neck with an 8200 ms cycle and slow phase variation. The production mesh samples source pixels bilinearly with eight fractional bits. Eye and mouth patches are composed into the source before the warp. The target is 25 poses/s, configurable up to 30. A costly idle pose yields to blinks and speech. Every five seconds of active animation the serial log reports completed idle poses/s, RGB666 payload bytes/s and recent frame cost.

Use the 10 MHz artifact first. The 20, 27 and 40 MHz artifacts use the same GPIO and rendering configuration and request different SPI clocks. Actual bus frequency depends on the peripheral divider. Check colors, noise, reset stability, audio and frame timing on each physical panel. The desktop previews use the same C++ mesh and sampler. Compile `scripts/anime_preview.cc` with a desktop C++17 compiler and the board include directory, then run `python scripts/anime_preview.py <executable> <output-directory>`.

The audio meter timestamps windows of resampled mono PCM immediately before the I2S write and estimates the presentation delay from the codec's DMA configuration. At 24 kHz, five 240-frame descriptors represent 50 ms. The display checks the meter on the manifest's 50 ms cadence. Expired windows return silence. A decoder reset invalidates readings from older packets.

Both OTA slots are 4.5 MiB. The remaining 6.875 MiB stores the usual Xiaozhi assets. CI decodes the compiled partition table, verifies flashed binaries byte-for-byte against the merged image, and reports slot headroom and linker size information.

On the wired unit, check portrait orientation and RGB primary colors at 10 MHz, provisioning and activation, microphone left-channel capture, wake-word response, server audio, interrupted speech, repeated MCP switches and LED brightness. Measure mouth-to-speaker timing and available internal heap under simultaneous playback and animation before testing a faster SPI clock.

## Startup diagnostics

`CONFIG_ANIME_PANEL_SELF_TEST` defaults to disabled. Enable it for the color test and silent provisioning comparison. In that diagnostic build, before LVGL, audio or Wi-Fi starts, the driver fills the entire panel red, green, blue, white and black, with a 700 ms hold on each color. The serial log records reset, panel configuration, each fill, the first LVGL flush and the start of Wi-Fi. A completed SPI transfer confirms the ESP32 sent the pixels. Check the panel itself for full-screen coverage and correct color order.

If the solid colors work but the UI fails, enable `CONFIG_ANIME_SETUP_UI_ONLY` in a diagnostic build. The original Xiaozhi setup UI still runs while the avatar framebuffer and timer are skipped. Capture the complete serial log for each pass. For a brownout at AP startup, measure the board supply at the ESP32-S3 during Wi-Fi transmission and check the backlight and speaker load. Brownout detection stays enabled.

Each firmware artifact includes `merged-binary.bin`, `xiaozhi.elf` and `xiaozhi.map`. Match the panic's ELF SHA-256 prefix against the ELF before using `xtensa-esp32s3-elf-addr2line -aipfC -e xiaozhi.elf` with the reported PCs.

For a provisioning audio comparison, boot into the setup AP with BOOT released and capture the log through prompt playback. On a second boot with the same image and power source, press and hold BOOT after the blue test screen appears. Release it when the serial console reports `Silent provisioning comparison selected`. Both boots render the same setup prompt. The comparison skips its Opus packets for that boot only. A crash before `Provisioning refresh complete` precedes submission of the prompt. If the assertion returns, capture the UART output through `ESP32 CORE DUMP END` and use the ELF packaged with that image.
