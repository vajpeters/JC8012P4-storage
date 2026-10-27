# Changelog

## 0.2.3

- Wait until Tessera is ready before initialising SDMMC, so a missing, slow, or unreadable card cannot prevent the plugin and its Home Assistant entities from being announced during startup.

## 0.2.2

- Fix the built-in TF slot pin mapping to use GPIO39-GPIO44, keeping the ESP32-C6 Wi-Fi SD2 bus on GPIO14-GPIO19 free.

## 0.2.1

- Fix ESPHome 2026.9 builds by explicitly enabling the ESP-IDF FATFS and SDMMC components.

## 0.2.0

- Add streamed MP3 decoding from the SD card using ESPHome's `micro-mp3` codec.
- Keep WAV playback and route both formats through Tessera's shared speaker.
- Rename the Home Assistant entity to **SD card media player**.

## 0.1.0

- Mount the JC8012P4 TF/microSD slot over its native four-bit SDMMC bus.
- Report mount state, total space, free space and used percentage to Home Assistant.
- Add optional local 16-bit PCM WAV playback through Tessera's shared speaker.
