local variant = require("pan_basics_variant")

return function(ctx)
    return variant.build(ctx, "007_sampleosc_pan_right_50", "007 SampleOsc Pan Right 50", { sampleOscPan = 50 })
end
