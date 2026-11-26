# Anime Companion S3

Xiaozhi 2.6.0 for ESP32-S3 N16R8, 16 MiB flash and 8 MiB octal PSRAM. Build the `anime-companion-s3` variant with ESP-IDF release/v6.1. The dedicated Anime Companion S3 Actions workflow builds the merged image on pushes to `anime-companion`.

| Peripheral | Connections |
| --- | --- |
| ILI9488, 320 × 480 portrait | CS 10, RESET 8, DC 9, MOSI 11, SCK 12, permanently powered backlight |
| INMP441, left slot | SCK 5, WS 4, SD 6 |
| MAX98357A | BCLK 15, LRC 16, DIN 17 |
| WS2812 and BOOT | GPIO48 and GPIO0 |

The write-only display uses RGB666 on the wire. `CONFIG_ANIME_SPI_MHZ` defaults to 10. `CONFIG_ANIME_LED_BRIGHTNESS` defaults to 12 out of 255. The BOOT button uses the breadboard firmware's provisioning and chat behavior.

Xiaozhi owns networking, activation, speech processing and protocols. The setup screen and avatar share one LVGL flush path. Each mode starts with a single 320 × 480 RGB565 frame in PSRAM. Subsequent frames update the small torso, eyes and mouth areas. The 5,120-byte LVGL strip is also in PSRAM. One 7,680-byte internal DMA strip is reused after its SPI completion interrupt. The immutable bases and patches are linked into the OTA application so an update replaces code and matching art together.

The `self.avatar.set_mode` MCP tool accepts `{"mode":"half_body"}` or `{"mode":"full_body"}`. Full body is the boot default. A 3.6-second cosine drives subpixel torso deformation. The audio meter timestamps windows of resampled mono PCM immediately before the I2S write and estimates the presentation delay from the codec's DMA configuration. At 24 kHz, five 240-frame descriptors represent 50 ms. The display checks the meter on the manifest's 50 ms cadence. Expired windows return silence. A decoder reset invalidates readings from older packets.

Both OTA slots are 4.5 MiB. The remaining 6.875 MiB stores the usual Xiaozhi assets. CI decodes the compiled partition table, verifies flashed binaries byte-for-byte against the merged image, and reports slot headroom and linker size information.

On the wired unit, check portrait orientation and RGB primary colors at 10 MHz, provisioning and activation, microphone left-channel capture, wake-word response, server audio, interrupted speech, repeated MCP switches and LED brightness. Measure mouth-to-speaker timing and available internal heap under simultaneous playback and animation before testing a faster SPI clock.
