local make = require("sample_playback_range_variant")

return make({
    layer_name = "003 Start 44100 End 88199",
    output_file = "003_start_44100_end_88199.vstpreset",
    sample_start = 44100,
    sample_end = 88199,
})
