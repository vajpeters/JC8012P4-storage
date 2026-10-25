# JC8012P4 storage

A Tessera plugin for the built-in TF/microSD slot of the Guition JC8012P4 panel. It mounts a FAT-formatted card at
`/sdcard` and exposes mounted, total-space, free-space and used-percentage diagnostic entities to Home Assistant.

An optional media-player entity decodes local MP3 or WAV files from the card and sends the audio to Tessera's shared
`ts_speaker`.
Install a speaker provider such as [JC8012P4 audio](https://github.com/vajpeters/JC8012P4-audio) to use it.

## Set up

1. Format a microSD card as FAT32 and copy one or more `.mp3` or `.wav` files to it.
2. Insert the card into the panel's TF slot before booting the screen.
3. Add this plugin to the screen in Tessera. Leave **SD card media player** enabled if a speaker plugin is installed.
4. Build and install the screen firmware.
5. Check the new diagnostic entities in Home Assistant. A missing card is retried every 30 seconds.

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

- The slot uses the panel's native four-bit SDMMC connection: D0-D3 on GPIO14-GPIO17, CLK on GPIO18 and CMD on
  GPIO19.
- Cards are never formatted by the plugin. Remove the card and format or copy files using a computer.
- Playback supports MP3 and WAV. The decoded output must be 16-bit mono or stereo; unsupported files are rejected with
  a log message.
- This first release has been checked statically but has not yet been verified on a physical JC8012P4 panel. Use the
  beta release with that limitation in mind and report boot logs if the card does not mount.

