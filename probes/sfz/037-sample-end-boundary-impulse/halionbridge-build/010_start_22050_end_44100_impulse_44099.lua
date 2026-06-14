local make = require("sample_end_boundary_variant")

return make({
    layer_name = "010 Start 22050 End 44100 Impulse 44099",
    output_file = "010_start_22050_end_44100_impulse_44099.vstpreset",
    sample_name = "impulse_at_44099",
    sample_start = 22050,
    sample_end = 44100,
})
