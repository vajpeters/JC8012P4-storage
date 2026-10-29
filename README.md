# JC8012P4 storage

A Tessera plugin for the built-in TF/microSD slot of the Guition JC8012P4 panel. It mounts a FAT-formatted card at
`/sdcard` and exposes detected, mounted, total-space, free-space and used-percentage diagnostic entities to Home Assistant.

An optional media-player entity decodes local MP3 or WAV files from the card and sends the audio to Tessera's shared
`ts_speaker`.
Install a speaker provider such as [JC8012P4 audio](https://github.com/vajpeters/JC8012P4-audio) to use it.

## Set up

1. Format a microSD card as FAT32 and copy one or more `.mp3` or `.wav` files to it.
2. Insert the card into the panel's TF slot before booting the screen.
3. Add this plugin to the screen in Tessera. Leave **SD card media player** enabled if a speaker plugin is installed.
4. Build and install the screen firmware.
5. Turn on **Enable SD card probe** in Home Assistant, then check the diagnostic entities. The switch resets to off on every boot so a faulty card or wiring fault can never block the screen from starting.

To play `/music/song.mp3` from the card, call Home Assistant's `media_player.play_media` action for the new
**SD card media player** entity:

```yaml
action: media_player.play_media
target:
  entity_id: media_player.sd_card_media_player
data:
  media_content_id: sd://music/song.mp3
  media_content_type: music
```

The entity id may include the screen's device name; choose the entity in Home Assistant rather than assuming the exact
id. Paths may use `sd://folder/file.wav`, `/sdcard/folder/file.wav` or `file:///sdcard/folder/file.wav`.

## Good to know

- The plugin uses the card's SPI mode on the panel's TF wiring: MISO/D0 on GPIO39, CS/D3 on GPIO42, CLK on GPIO43
  and MOSI/CMD on GPIO44. It also enables the socket's documented ESP_LDO_VO4 3.3 V power rail while the card is mounted. This avoids a native ESP32-P4 SDMMC driver crash. GPIO14-GPIO19 belong to the separate SD2
  link to the ESP32-C6 Wi-Fi coprocessor.
- Cards are never formatted by the plugin. Remove the card and format or copy files using a computer.
- **SD card detected** reports whether a card answers the SPI initialisation sequence. The TF socket has no separate
  mechanical detection signal, so a physically inserted card that cannot communicate electrically still appears absent.
  **SD card mounted** reports separately whether its filesystem was mounted successfully. Capacity sensors are temporarily unavailable on this ESP32-P4 firmware because its FAT volume-statistics call resets the device.
- Playback supports MP3 and WAV. The decoded output must be 16-bit mono or stereo; unsupported files are rejected with
  a log message.
- The plugin has been boot-tested on a physical JC8012P4A1 V3. When no readable card answers, the screen stays online
  and retries every 30 seconds; the **SD card mounted** entity remains off.
