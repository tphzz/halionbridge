local make = require("sample_playback_range_variant")

return make({
    layer_name = "009 Start 22050 End 44100",
    output_file = "009_start_22050_end_44100.vstpreset",
    sample_start = 22050,
    sample_end = 44100,
})
