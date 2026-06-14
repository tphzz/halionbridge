local make = require("sample_end_boundary_variant")

return make({
    layer_name = "009 Start 22050 End 44099 Impulse 44099",
    output_file = "009_start_22050_end_44099_impulse_44099.vstpreset",
    sample_name = "impulse_at_44099",
    sample_start = 22050,
    sample_end = 44099,
})
