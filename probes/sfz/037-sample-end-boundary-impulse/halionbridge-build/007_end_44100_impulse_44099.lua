local make = require("sample_end_boundary_variant")

return make({
    layer_name = "007 End 44100 Impulse 44099",
    output_file = "007_end_44100_impulse_44099.vstpreset",
    sample_name = "impulse_at_44099",
    sample_end = 44100,
})
