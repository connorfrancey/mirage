# ALAudio.ALAudioDevice

Class associated with API version `1.0.0`.

Inherits from `Engine.AudioDevice`.

## Runtime functions

### `mirage.sdk.get_vorbis_editor_threshold`

Source member `ALAudio.ALAudioDevice.MinOggVorbisDurationEditor`. Game thread only.

Parameters `object` (`MirageAPIGen.Object`).

Returns `number|nil`.

Reads the editor Ogg Vorbis duration threshold from a live audio device.

### `mirage.sdk.get_vorbis_game_threshold`

Source member `ALAudio.ALAudioDevice.MinOggVorbisDurationGame`. Game thread only.

Parameters `object` (`MirageAPIGen.Object`).

Returns `number|nil`.

Reads the minimum in-game Ogg Vorbis duration from a live audio device.
