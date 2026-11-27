# GitHub music library

Copy this directory into a public GitHub repository. Keep `songs.json` alongside the `songs/` directory. In Aura → Music, enter `https://raw.githubusercontent.com/OWNER/REPOSITORY/main/songs.json`, enable music, and save. Setup hotspot mode allows saving. Home Wi-Fi enables verification and playback. Refresh after changing the repository.

Each song needs a unique ID, title, artist and MP3 path. An optional whole-number duration in seconds drives the total-time display. Omit it for unknown duration. Relative paths start at the manifest directory. Absolute raw HTTPS URLs must share its owner, repository and first ref segment. Use a simple branch name or commit SHA. Use letters, digits, underscores, periods, hyphens or tildes in paths, and encode spaces as %20. Redirects are rejected. Limits are 256 songs and 64 KiB JSON. Catalog pages default to six tracks.

The two included test recordings are original generated sine tones dedicated to the public domain under CC0. Studio tone is a 4-second 44.1 kHz CBR recording, and Evening test is an 8-second 48 kHz stereo VBR recording. They are repository examples and are excluded from the firmware image.

During music, press BOOT to stop, or use the wake word to interrupt and speak. The physical Settings button stops playback before switching networks. A wake word audible over the speaker depends on volume, room acoustics and microphone placement. Device music tools only access this configured library. Independent cloud-side services follow the backend policy.

The local portal uses visually masked text fields for replacement API keys. Previously stored keys are never filled back in. Browser extensions and password managers can override autofill hints.

### Playback resources

The compressed ring is 128 KiB in PSRAM and the player disc uses 86,528 bytes in PSRAM. Both are released when leaving playback. The decoder worker stack is 24 KiB on demand and the HTTPS producer uses 8 KiB. PCM is downmixed before private rate conversion and queued on the existing 24 kHz speaker task. The serial log reports the observed free-heap delta across MP3 decoder open. Espressif lists 28 KB of decoder heap for a 44.1 kHz stereo MP3 in its [2.5.0 benchmark](https://components.espressif.com/components/espressif/esp_audio_codec/versions/2.5.0/readme). Peak device usage also includes TLS, resampling, queue buffers and LVGL.

Network cancellation is cooperative with a 2.5-second HTTP I/O timeout. BOOT releases speaker ownership immediately and queued PCM is discarded. An in-flight I2S write and DMA tail may still be audible briefly. A confirmed wake-word detection interrupts playback before normal voice handling resumes.
