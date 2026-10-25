"""Local MP3 and WAV media player backed by the JC8012P4 SD card."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import audio, media_player, speaker
from esphome.const import CONF_ID, CONF_SPEAKER

from .. import Jc8012p4Storage, jc8012p4_storage_ns

AUTO_LOAD = ["audio", "ring_buffer"]
DEPENDENCIES = ["jc8012p4_storage", "speaker"]

CONF_STORAGE_ID = "storage_id"

Jc8012p4StoragePlayer = jc8012p4_storage_ns.class_(
    "Jc8012p4StoragePlayer", cg.Component, media_player.MediaPlayer
)


def _request_codecs(config):
    audio.request_mp3_support()
    audio.request_wav_support()
    return config


CONFIG_SCHEMA = cv.All(
    media_player.media_player_schema(
        Jc8012p4StoragePlayer, icon="mdi:sd"
    ).extend(
        {
            cv.GenerateID(CONF_STORAGE_ID): cv.use_id(Jc8012p4Storage),
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        }
    ),
    cv.only_on_esp32,
    _request_codecs,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)
    cg.add(var.set_storage(await cg.get_variable(config[CONF_STORAGE_ID])))
    cg.add(var.set_speaker(await cg.get_variable(config[CONF_SPEAKER])))

