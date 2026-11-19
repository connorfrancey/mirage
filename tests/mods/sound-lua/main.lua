-- Self-test mod ("Mirage sound"): mirage.sound_play / sound_playing / sound_stop on the mod's own WAV, quiet.
function tone_play(loop) return mirage.sound_play("sounds/tone.wav", { volume = 0.05, loop = loop }) end
function tone_playing(h) return mirage.sound_playing(h) end
function tone_stop(h) mirage.sound_stop(h) end
-- A path out of the mod's folder is refused
function tone_escape() return mirage.sound_play("../sound-lua/sounds/tone.wav") end
