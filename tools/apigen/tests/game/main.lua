mirage.test("reviewed SDK accessors", function()
    assert(mirage.sdk, "build Mirage with MIRAGE_BUILD_APIGEN=ON")
    local devices = mirage.all("ALAudioDevice", true)
    assert(#devices > 0, "expected an audio device or a class default object")
    for _, object in ipairs(devices) do
        local game = mirage.sdk.get_vorbis_game_threshold(object)
        local editor = mirage.sdk.get_vorbis_editor_threshold(object)
        assert(type(game) == "number" and game == object.MinOggVorbisDurationGame)
        assert(type(editor) == "number" and editor == object.MinOggVorbisDurationEditor)
    end
    local world = assert(mirage.world())
    assert(mirage.sdk.get_vorbis_game_threshold(world) == nil)
    assert(not pcall(mirage.sdk.get_vorbis_game_threshold, 42))
end)
